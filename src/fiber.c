/********************************************************************

   Copyright 2013 Konstantin Olkhovskiy <lupus@oxnull.net>

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

 ********************************************************************/

#include <evfibers/config.h>

#include <sys/mman.h>
#include <fcntl.h>
#include <libgen.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <err.h>
#ifdef HAVE_VALGRIND_H
#include <valgrind/valgrind.h>
#else
#define RUNNING_ON_VALGRIND (0)
#define VALGRIND_STACK_REGISTER(a,b) (void)0
#endif

#undef FBR_EIO_ENABLED
#include <evfibers_private/fiber.h>

#ifndef LIST_FOREACH_SAFE
#define LIST_FOREACH_SAFE(var, head, field, next_var)              \
	for ((var) = ((head)->lh_first);                           \
		(var) && ((next_var) = ((var)->field.le_next), 1); \
		(var) = (next_var))

#endif

#ifndef TAILQ_FOREACH_SAFE
#define TAILQ_FOREACH_SAFE(var, head, field, next_var)                 \
	for ((var) = ((head)->tqh_first);                              \
		(var) ? ({ (next_var) = ((var)->field.tqe_next); 1; }) \
		: 0;                                                   \
		(var) = (next_var))
#endif


#define ENSURE_ROOT_FIBER do {                            \
	assert(fctx->__p->sp->fiber == &fctx->__p->root); \
} while (0)

#define CURRENT_FIBER (fctx->__p->sp->fiber)
#define CURRENT_FIBER_ID (fbr_id_pack(CURRENT_FIBER))
#define CALLED_BY_ROOT ((fctx->__p->sp - 1)->fiber == &fctx->__p->root)

#define unpack_transfer_errno(value, ptr, id)           \
	do {                                            \
		if (-1 == fbr_id_unpack(fctx, ptr, id)) \
			return (value);                 \
	} while (0)

#define return_success(value)                \
	do {                                 \
		fctx->f_errno = FBR_SUCCESS; \
		return (value);              \
	} while (0)

#define return_error(value, code)       \
	do {                            \
		fctx->f_errno = (code); \
		return (value);         \
	} while (0)


const fbr_id_t FBR_ID_NULL = {0, NULL};
static const char default_buffer_pattern[] = "/dev/shm/fbr_buffer.XXXXXXXXX";

static fbr_id_t fbr_id_pack(struct fbr_fiber *fiber)
{
	return (struct fbr_id_s){.g = fiber->id, .p = fiber};
}

static int fbr_id_unpack(FBR_P_ struct fbr_fiber **ptr, fbr_id_t id)
{
	struct fbr_fiber *fiber = id.p;
	if (fiber->id != id.g)
		return_error(-1, FBR_ENOFIBER);
	if (ptr)
		*ptr = id.p;
	return 0;
}

static void pending_async_cb(uv_async_t *w)
{
	struct fbr_context *fctx;
	struct fbr_id_tailq_i *item;
	fctx = (struct fbr_context *)w->data;
	int retval;

	ENSURE_ROOT_FIBER;

	if (TAILQ_EMPTY(&fctx->__p->pending_fibers)) {
		uv_unref((uv_handle_t *)&fctx->__p->pending_async);
		return;
	}

	item = TAILQ_FIRST(&fctx->__p->pending_fibers);
	assert(item->head == &fctx->__p->pending_fibers);
	/* item shall be removed from the queue by a destructor, which shall be
	 * set by the procedure demanding delayed execution. Destructor
	 * guarantees removal upon the reclaim of fiber. */
	uv_async_send(&fctx->__p->pending_async);

	retval = fbr_transfer(FBR_A_ item->id);
	if (-1 == retval && FBR_ENOFIBER != fctx->f_errno) {
		fbr_log_e(FBR_A_ "libevfibers: unexpected error trying to call"
				" a fiber by id: %s",
				fbr_strerror(FBR_A_ fctx->f_errno));
	}
}

static void *allocate_in_fiber(FBR_P_ size_t size, struct fbr_fiber *in)
{
	struct mem_pool *pool_entry;
	pool_entry = malloc(size + sizeof(struct mem_pool));
	if (NULL == pool_entry) {
		fbr_log_e(FBR_A_ "libevfibers: unable to allocate %zu bytes\n",
				size + sizeof(struct mem_pool));
		abort();
	}
	pool_entry->ptr = pool_entry;
	pool_entry->destructor = NULL;
	pool_entry->destructor_context = NULL;
	LIST_INSERT_HEAD(&in->pool, pool_entry, entries);
	return pool_entry + 1;
}

static void stdio_logger(FBR_P_ struct fbr_logger *logger,
		enum fbr_log_level level, const char *format, va_list ap)
{
	struct fbr_fiber *fiber;
	FILE* stream;
	char *str_level;
	double tstamp;

	if (level > logger->level)
		return;

	fiber = CURRENT_FIBER;

	switch (level) {
		case FBR_LOG_ERROR:
			str_level = "ERROR";
			stream = stderr;
			break;
		case FBR_LOG_WARNING:
			str_level = "WARNING";
			stream = stdout;
			break;
		case FBR_LOG_NOTICE:
			str_level = "NOTICE";
			stream = stdout;
			break;
		case FBR_LOG_INFO:
			str_level = "INFO";
			stream = stdout;
			break;
		case FBR_LOG_DEBUG:
			str_level = "DEBUG";
			stream = stdout;
			break;
		default:
			str_level = "?????";
			stream = stdout;
			break;
	}
	tstamp = uv_now(fctx->__p->loop)/1e3;
	fprintf(stream, "%.6f  %-7s %-16s ", tstamp, str_level, fiber->name);
	vfprintf(stream, format, ap);
	fprintf(stream, "\n");
}

void fbr_init(FBR_P_ uv_loop_t *loop)
{
	struct fbr_fiber *root;
	struct fbr_logger *logger;
	char *buffer_pattern;

	fctx->__p = malloc(sizeof(struct fbr_context_private));
	LIST_INIT(&fctx->__p->reclaimed);
	LIST_INIT(&fctx->__p->root.children);
	LIST_INIT(&fctx->__p->root.pool);
	TAILQ_INIT(&fctx->__p->root.destructors);
	TAILQ_INIT(&fctx->__p->pending_fibers);

	root = &fctx->__p->root;
	strncpy(root->name, "root", FBR_MAX_FIBER_NAME - 1);
	fctx->__p->last_id = 0;
	root->id = fctx->__p->last_id++;
	coro_create(&root->ctx, NULL, NULL, NULL, 0);

	logger = allocate_in_fiber(FBR_A_ sizeof(struct fbr_logger), root);
	logger->logv = stdio_logger;
	logger->level = FBR_LOG_NOTICE;
	fctx->logger = logger;

	fctx->__p->sp = fctx->__p->stack;
	fctx->__p->sp->fiber = root;
	fctx->__p->backtraces_enabled = 1;
	fill_trace_info(FBR_A_ &fctx->__p->sp->tinfo);
	fctx->__p->loop = loop;
	fctx->__p->pending_async.data = fctx;
	fctx->__p->backtraces_enabled = 0;
	memset(&fctx->__p->key_free_mask, 0xFF,
			sizeof(fctx->__p->key_free_mask));
	uv_async_init(UV_A_ &fctx->__p->pending_async, pending_async_cb);
	uv_unref((uv_handle_t *)&fctx->__p->pending_async);

	buffer_pattern = getenv("FBR_BUFFER_FILE_PATTERN");
	if (buffer_pattern)
		fctx->__p->buffer_file_pattern = buffer_pattern;
	else
		fctx->__p->buffer_file_pattern = default_buffer_pattern;
}

const char *fbr_strerror(_unused_ FBR_P_ enum fbr_error_code code)
{
	switch (code) {
		case FBR_SUCCESS:
			return "Success";
		case FBR_EINVAL:
			return "Invalid argument";
		case FBR_ENOFIBER:
			return "No such fiber";
		case FBR_ESYSTEM:
			return "System error, consult system errno";
		case FBR_EBUFFERMMAP:
			return "Failed to mmap two adjacent regions";
		case FBR_ENOKEY:
			return "Fiber-local key does not exist";
		case FBR_EPROTOBUF:
			return "Protobuf unpacking error";
		case FBR_EBUFFERNOSPACE:
			return "Not enough space in the buffer";
		case FBR_EEIO:
			return "libeio request error";
	}
	return "Unknown error";
}

void fbr_log_e(FBR_P_ const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	(*fctx->logger->logv)(FBR_A_ fctx->logger, FBR_LOG_ERROR, format, ap);
	va_end(ap);
}

void fbr_log_w(FBR_P_ const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	(*fctx->logger->logv)(FBR_A_ fctx->logger, FBR_LOG_WARNING, format, ap);
	va_end(ap);
}

void fbr_log_n(FBR_P_ const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	(*fctx->logger->logv)(FBR_A_ fctx->logger, FBR_LOG_NOTICE, format, ap);
	va_end(ap);
}

void fbr_log_i(FBR_P_ const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	(*fctx->logger->logv)(FBR_A_ fctx->logger, FBR_LOG_INFO, format, ap);
	va_end(ap);
}

void fbr_log_d(FBR_P_ const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	(*fctx->logger->logv)(FBR_A_ fctx->logger, FBR_LOG_DEBUG, format, ap);
	va_end(ap);
}

void id_tailq_i_set(_unused_ FBR_P_
		struct fbr_id_tailq_i *item,
		struct fbr_fiber *fiber)
{
	item->id = fbr_id_pack(fiber);
	item->ev = NULL;
}

static void reclaim_children(FBR_P_ struct fbr_fiber *fiber)
{
	struct fbr_fiber *f;
	LIST_FOREACH(f, &fiber->children, entries.children) {
		fbr_reclaim(FBR_A_ fbr_id_pack(f));
	}
}

static void fbr_free_in_fiber(_unused_ FBR_P_ _unused_ struct fbr_fiber *fiber,
		void *ptr, int destructor);

void fbr_destroy(FBR_P)
{
	struct fbr_fiber *fiber, *x;
	struct mem_pool *p, *x2;

	reclaim_children(FBR_A_ &fctx->__p->root);

	LIST_FOREACH_SAFE(p, &fctx->__p->root.pool, entries, x2) {
		fbr_free_in_fiber(FBR_A_ &fctx->__p->root, p + 1, 1);
	}

	LIST_FOREACH_SAFE(fiber, &fctx->__p->reclaimed, entries.reclaimed, x) {
		free(fiber->stack);
		free(fiber);
	}

	free(fctx->__p);
}

void fbr_enable_backtraces(FBR_P_ int enabled)
{
	if (enabled)
		fctx->__p->backtraces_enabled = 1;
	else
		fctx->__p->backtraces_enabled = 0;

}

static void cancel_ev(_unused_ FBR_P_ struct fbr_ev_base *ev)
{
	fbr_destructor_remove(FBR_A_ &ev->item.dtor, 1 /* call it */);
}

static void post_ev(_unused_ FBR_P_ struct fbr_fiber *fiber,
		struct fbr_ev_base *ev)
{
	assert(NULL != fiber->ev.waiting);

	fiber->ev.arrived = 1;
	ev->arrived = 1;
}

static struct fbr_ev_watcher ev_watcher_invalid;

static void bad_watcher_abort_cb(uv_handle_t *w)
{
	fprintf(stderr, "libevfibers: libev callback called for pending "
			"watcher (%p), which is no longer being awaited via "
			"fbr_ev_wait()", w);
	abort();
}

void fbr_uv_handler_cb(uv_handle_t *w)
{
	struct fbr_fiber *fiber;
	struct fbr_ev_watcher *ev = w->data;
	struct fbr_context *fctx = ev->ev_base.fctx;
	int retval;

	ENSURE_ROOT_FIBER;

	if (ev == &ev_watcher_invalid)
		bad_watcher_abort_cb(w);

	retval = fbr_id_unpack(FBR_A_ &fiber, ev->ev_base.id);
	if (-1 == retval) {
		fbr_log_e(FBR_A_ "libevfibers: fiber is about to be called by"
			" the watcher callback, but it's id is not valid: %s",
			fbr_strerror(FBR_A_ fctx->f_errno));
		abort();
	}

	post_ev(FBR_A_ fiber, &ev->ev_base);

	retval = fbr_transfer(FBR_A_ fbr_id_pack(fiber));
	assert(0 == retval);
}

static void fbr_free_in_fiber(_unused_ FBR_P_ _unused_ struct fbr_fiber *fiber,
		void *ptr, int destructor)
{
	struct mem_pool *pool_entry = NULL;
	if (NULL == ptr)
		return;
	pool_entry = (struct mem_pool *)ptr - 1;
	if (pool_entry->ptr != pool_entry) {
		fbr_log_e(FBR_A_ "libevfibers: address %p does not look like "
				"fiber memory pool entry", ptr);
		if (!RUNNING_ON_VALGRIND)
			abort();
	}
	LIST_REMOVE(pool_entry, entries);
	if (destructor && pool_entry->destructor)
		pool_entry->destructor(FBR_A_ ptr, pool_entry->destructor_context);
	free(pool_entry);
}

static void fiber_cleanup(FBR_P_ struct fbr_fiber *fiber)
{
	struct mem_pool *p, *x;
	struct fbr_destructor *dtor;
	/* coro_destroy(&fiber->ctx); */
	LIST_REMOVE(fiber, entries.children);
	TAILQ_FOREACH(dtor, &fiber->destructors, entries) {
		dtor->func(FBR_A_ dtor->arg);
	}
	LIST_FOREACH_SAFE(p, &fiber->pool, entries, x) {
		fbr_free_in_fiber(FBR_A_ fiber, p + 1, 1);
	}
}

static void filter_fiber_stack(FBR_P_ struct fbr_fiber *fiber)
{
	struct fbr_stack_item *sp;
	for (sp = fctx->__p->stack; sp < fctx->__p->sp; sp++) {
		if (sp->fiber == fiber) {
			memmove(sp, sp + 1, (fctx->__p->sp - sp) * sizeof(*sp));
			fctx->__p->sp--;
		}
	}
}

static int do_reclaim(FBR_P_ struct fbr_fiber *fiber)
{
#if 0
	struct fbr_fiber *f;
#endif

	fill_trace_info(FBR_A_ &fiber->reclaim_tinfo);
	reclaim_children(FBR_A_ fiber);
	fiber_cleanup(FBR_A_ fiber);
	fiber->id = fctx->__p->last_id++;
#if 0
	LIST_FOREACH(f, &fctx->__p->reclaimed, entries.reclaimed) {
		assert(f != fiber);
	}
#endif
	LIST_INSERT_HEAD(&fctx->__p->reclaimed, fiber, entries.reclaimed);

	filter_fiber_stack(FBR_A_ fiber);

	if (CURRENT_FIBER == fiber)
		fbr_yield(FBR_A);

	return_success(0);
}

int fbr_reclaim(FBR_P_ fbr_id_t id)
{
	struct fbr_fiber *fiber;
	struct fbr_mutex mutex;
	int retval;

	unpack_transfer_errno(-1, &fiber, id);

	fbr_mutex_init(FBR_A_ &mutex);
	fbr_mutex_lock(FBR_A_ &mutex);
	while (fiber->no_reclaim > 0) {
		fiber->want_reclaim = 1;
		assert("Attempt to reclaim self while no_reclaim is set would"
				" block forever" && fiber != CURRENT_FIBER);
		if (-1 == fbr_id_unpack(FBR_A_ NULL, id) &&
				FBR_ENOFIBER == fctx->f_errno)
			return_success(0);
		retval = fbr_cond_wait(FBR_A_ &fiber->reclaim_cond, &mutex);
		assert(0 == retval);
		(void)retval;
	}
	fbr_mutex_unlock(FBR_A_ &mutex);
	fbr_mutex_destroy(FBR_A_ &mutex);

	if (-1 == fbr_id_unpack(FBR_A_ NULL, id) &&
			FBR_ENOFIBER == fctx->f_errno)
		return_success(0);

	return do_reclaim(FBR_A_ fiber);
}

int fbr_set_reclaim(FBR_P_ fbr_id_t id)
{
	struct fbr_fiber *fiber;

	unpack_transfer_errno(-1, &fiber, id);
	fiber->no_reclaim--;
	if (0 == fiber->no_reclaim)
		fbr_cond_broadcast(FBR_A_ &fiber->reclaim_cond);
	return_success(0);
}

int fbr_set_noreclaim(FBR_P_ fbr_id_t id)
{
	struct fbr_fiber *fiber;

	unpack_transfer_errno(-1, &fiber, id);
	fiber->no_reclaim++;
	return_success(0);
}

int fbr_want_reclaim(FBR_P_ fbr_id_t id)
{
	struct fbr_fiber *fiber;

	unpack_transfer_errno(-1, &fiber, id);
	if (fiber->no_reclaim > 0)
		/* If we're in noreclaim block of any depth, always return 0 */
		return 0;
	return_success(fiber->want_reclaim);
}

int fbr_is_reclaimed(_unused_ FBR_P_ fbr_id_t id)
{
	if (0 == fbr_id_unpack(FBR_A_ NULL, id))
		return 0;
	return 1;
}

fbr_id_t fbr_self(FBR_P)
{
	return CURRENT_FIBER_ID;
}

static int do_reclaim(FBR_P_ struct fbr_fiber *fiber);

static void call_wrapper(FBR_P)
{
	int retval;
	struct fbr_fiber *fiber = CURRENT_FIBER;

	fiber->func(FBR_A_ fiber->func_arg);

	retval = do_reclaim(FBR_A_ fiber);
	assert(0 == retval);
	(void)retval;
	fbr_yield(FBR_A);
	assert(NULL);
}

enum ev_action_hint {
	EV_AH_OK = 0,
	EV_AH_ARRIVED,
	EV_AH_EINVAL
};

static void item_dtor(_unused_ FBR_P_ void *arg)
{
	struct fbr_id_tailq_i *item = arg;

	if (item->head) {
		TAILQ_REMOVE(item->head, item, entries);
	}
}

static enum ev_action_hint prepare_ev(FBR_P_ struct fbr_ev_base *ev)
{
	struct fbr_ev_watcher *e_watcher;
	struct fbr_ev_mutex *e_mutex;
	struct fbr_ev_cond_var *e_cond;
	struct fbr_id_tailq_i *item = &ev->item;

	ev->arrived = 0;
	ev->item.dtor.func = item_dtor;
	ev->item.dtor.arg = item;
	fbr_destructor_add(FBR_A_ &ev->item.dtor);

	switch (ev->type) {
	case FBR_EV_WATCHER:
		e_watcher = fbr_ev_upcast(ev, fbr_ev_watcher);
		if (!uv_is_active(e_watcher->w)) {
			fbr_destructor_remove(FBR_A_ &ev->item.dtor,
					0 /* call it */);
			return EV_AH_EINVAL;
		}
		e_watcher->w->data = e_watcher;
		//ev_set_cb(e_watcher->w, ev_watcher_cb);
		//Not needed any more, client is expected to set cb
		break;
	case FBR_EV_MUTEX:
		e_mutex = fbr_ev_upcast(ev, fbr_ev_mutex);
		if (fbr_id_isnull(e_mutex->mutex->locked_by)) {
			e_mutex->mutex->locked_by = CURRENT_FIBER_ID;
			return EV_AH_ARRIVED;
		}
		id_tailq_i_set(FBR_A_ item, CURRENT_FIBER);
		item->ev = ev;
		ev->data = item;
		TAILQ_INSERT_TAIL(&e_mutex->mutex->pending, item, entries);
		item->head = &e_mutex->mutex->pending;
		break;
	case FBR_EV_COND_VAR:
		e_cond = fbr_ev_upcast(ev, fbr_ev_cond_var);
		if (e_cond->mutex && fbr_id_isnull(e_cond->mutex->locked_by)) {
			fbr_destructor_remove(FBR_A_ &ev->item.dtor,
					0 /* call it */);
			return EV_AH_EINVAL;
		}
		id_tailq_i_set(FBR_A_ item, CURRENT_FIBER);
		item->ev = ev;
		ev->data = item;
		TAILQ_INSERT_TAIL(&e_cond->cond->waiting, item, entries);
		item->head = &e_cond->cond->waiting;
		if (e_cond->mutex)
			fbr_mutex_unlock(FBR_A_ e_cond->mutex);
		break;
	case FBR_EV_EIO:
#ifdef FBR_EIO_ENABLED
		/* NOP */
#else
		fbr_log_e(FBR_A_ "libevfibers: libeio support is not compiled");
		abort();
#endif
		break;
	}
	return EV_AH_OK;
}

static void finish_ev(FBR_P_ struct fbr_ev_base *ev)
{
	struct fbr_ev_cond_var *e_cond;
	struct fbr_ev_watcher *e_watcher;
	fbr_destructor_remove(FBR_A_ &ev->item.dtor, 1 /* call it */);
	switch (ev->type) {
	case FBR_EV_COND_VAR:
		e_cond = fbr_ev_upcast(ev, fbr_ev_cond_var);
		if (e_cond->mutex)
			fbr_mutex_lock(FBR_A_ e_cond->mutex);
		break;
	case FBR_EV_WATCHER:
		e_watcher = fbr_ev_upcast(ev, fbr_ev_watcher);
		//ev_set_cb(e_watcher->w, ev_abort_cb);
		e_watcher->w->data = &ev_watcher_invalid;
		break;
	case FBR_EV_MUTEX:
		/* NOP */
		break;
	case FBR_EV_EIO:
#ifdef FBR_EIO_ENABLED
		/* NOP */
#else
		fbr_log_e(FBR_A_ "libevfibers: libeio support is not compiled");
		abort();
#endif
		break;
	}
}

static void watcher_timer_dtor(_unused_ FBR_P_ void *_arg)
{
	uv_timer_t *w = _arg;
	uv_timer_stop(w);
}

int fbr_ev_wait_to(FBR_P_ struct fbr_ev_base *events[], double timeout)
{
	size_t size;
	uv_timer_t timer;
	struct fbr_ev_watcher watcher;
	struct fbr_destructor dtor = FBR_DESTRUCTOR_INITIALIZER;
	struct fbr_ev_base **new_events;
	struct fbr_ev_base **ev_pptr;
	int n_events;

	uv_timer_init(fctx->__p->loop, &timer);
	uv_timer_start(&timer, fbr_uv_timer_cb, timeout*1e3, 0);
	fbr_ev_watcher_init(FBR_A_ &watcher,
			(uv_handle_t *)&timer);
	dtor.func = watcher_timer_dtor;
	dtor.arg = &timer;
	fbr_destructor_add(FBR_A_ &dtor);
	size = 0;
	for (ev_pptr = events; NULL != *ev_pptr; ev_pptr++)
		size++;
	new_events = alloca((size + 2) * sizeof(void *));
	memcpy(new_events, events, size * sizeof(void *));
	new_events[size] = &watcher.ev_base;
	new_events[size + 1] = NULL;
	n_events = fbr_ev_wait(FBR_A_ new_events);
	fbr_destructor_remove(FBR_A_ &dtor, 1 /* Call it? */);
	if (n_events < 0)
		return n_events;
	if (watcher.ev_base.arrived)
		n_events--;
	return n_events;
}

int fbr_ev_wait(FBR_P_ struct fbr_ev_base *events[])
{
	struct fbr_fiber *fiber = CURRENT_FIBER;
	enum ev_action_hint hint;
	int num = 0;
	int i;

	fiber->ev.arrived = 0;
	fiber->ev.waiting = events;

	for (i = 0; NULL != events[i]; i++) {
		hint = prepare_ev(FBR_A_ events[i]);
		switch (hint) {
		case EV_AH_OK:
			break;
		case EV_AH_ARRIVED:
			fiber->ev.arrived = 1;
			events[i]->arrived = 1;
			break;
		case EV_AH_EINVAL:
			return_error(-1, FBR_EINVAL);
		}
	}

	while (0 == fiber->ev.arrived)
		fbr_yield(FBR_A);

	for (i = 0; NULL != events[i]; i++) {
		if (events[i]->arrived) {
			num++;
			finish_ev(FBR_A_ events[i]);
		} else
			cancel_ev(FBR_A_ events[i]);
	}
	return_success(num);
}

int fbr_ev_wait_one(FBR_P_ struct fbr_ev_base *one)
{
	struct fbr_fiber *fiber = CURRENT_FIBER;
	enum ev_action_hint hint;
	struct fbr_ev_base *events[] = {one, NULL};

	fiber->ev.arrived = 0;
	fiber->ev.waiting = events;

	hint = prepare_ev(FBR_A_ one);
	switch (hint) {
	case EV_AH_OK:
		break;
	case EV_AH_ARRIVED:
		goto finish;
	case EV_AH_EINVAL:
		return_error(-1, FBR_EINVAL);
	}

	while (0 == fiber->ev.arrived)
		fbr_yield(FBR_A);

finish:
	finish_ev(FBR_A_ one);
	return 0;
}

int fbr_ev_wait_one_wto(FBR_P_ struct fbr_ev_base *one, double timeout)
{
	int n_events;
	struct fbr_ev_base *events[] = {one, NULL, NULL};
	uv_timer_t timer;
	struct fbr_ev_watcher twatcher;
	struct fbr_destructor dtor = FBR_DESTRUCTOR_INITIALIZER;

	uv_timer_init(fctx->__p->loop, &timer);
	uv_timer_start(&timer, fbr_uv_timer_cb, timeout*1e3, 0);

	fbr_ev_watcher_init(FBR_A_ &twatcher,
			(uv_handle_t *)&timer);
	dtor.func = watcher_timer_dtor;
	dtor.arg = &timer;
	fbr_destructor_add(FBR_A_ &dtor);
	events[1] = &twatcher.ev_base;

	n_events = fbr_ev_wait(FBR_A_ events);
	fbr_destructor_remove(FBR_A_ &dtor, 1 /* Call it? */);

	if (n_events > 0 && events[0]->arrived)
		return 0;
	errno = ETIMEDOUT;
	return -1;
}


int fbr_transfer(FBR_P_ fbr_id_t to)
{
	struct fbr_fiber *callee;
	struct fbr_fiber *caller = fctx->__p->sp->fiber;

	unpack_transfer_errno(-1, &callee, to);

	fctx->__p->sp++;

	fctx->__p->sp->fiber = callee;
	fill_trace_info(FBR_A_ &fctx->__p->sp->tinfo);

	coro_transfer(&caller->ctx, &callee->ctx);

	return_success(0);
}

void fbr_yield(FBR_P)
{
	struct fbr_fiber *callee;
	struct fbr_fiber *caller;
	assert("Attemp to yield in a root fiber" &&
			fctx->__p->sp->fiber != &fctx->__p->root);
	callee = fctx->__p->sp->fiber;
	caller = (--fctx->__p->sp)->fiber;
	coro_transfer(&callee->ctx, &caller->ctx);
}

int fbr_fd_nonblock(FBR_P_ int fd)
{
	int flags, s;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1)
		return_error(-1, FBR_ESYSTEM);

	flags |= O_NONBLOCK;
	s = fcntl(fd, F_SETFL, flags);
	if (s == -1)
		return_error(-1, FBR_ESYSTEM);

	return_success(0);
}

static void ev_base_init(FBR_P_ struct fbr_ev_base *ev,
		enum fbr_ev_type type)
{
	memset(ev, 0x00, sizeof(*ev));
	ev->type = type;
	ev->id = CURRENT_FIBER_ID;
	ev->fctx = fctx;
}

void fbr_ev_watcher_init(FBR_P_ struct fbr_ev_watcher *ev, uv_handle_t *w)
{
	ev_base_init(FBR_A_ &ev->ev_base, FBR_EV_WATCHER);
	ev->w = w;
}

double fbr_sleep(FBR_P_ double seconds)
{
	uv_timer_t timer;
	struct fbr_ev_watcher watcher;
	struct fbr_destructor dtor = FBR_DESTRUCTOR_INITIALIZER;
	double expected = uv_now(fctx->__p->loop)/1e3 + seconds;

	uv_timer_init(fctx->__p->loop, &timer);
	uv_timer_start(&timer, fbr_uv_timer_cb, seconds*1e3, 0);
	dtor.func = watcher_timer_dtor;
	dtor.arg = &timer;
	fbr_destructor_add(FBR_A_ &dtor);

	fbr_ev_watcher_init(FBR_A_ &watcher, (uv_handle_t *)&timer);
	fbr_ev_wait_one(FBR_A_ &watcher.ev_base);

	fbr_destructor_remove(FBR_A_ &dtor, 0 /* Call it? */);
	uv_timer_stop(&timer);

	return max(0., expected - uv_now(fctx->__p->loop)/1e3);
}

static unsigned get_page_size()
{
	static unsigned sz;
	long retval;
	if (sz > 0)
		return sz;
	retval = sysconf(_SC_PAGESIZE);
	if (0 > retval) {
		fprintf(stderr, "libevfibers: sysconf(_SC_PAGESIZE): %s",
				strerror(errno));
		abort();
	}
	sz = retval;
	return sz;
}

static size_t round_up_to_page_size(size_t size)
{
	unsigned sz = get_page_size();
	size_t remainder;
	remainder = size % sz;
	if (remainder == 0)
		return size;
	return size + sz - remainder;
}

fbr_id_t fbr_create(FBR_P_ const char *name, fbr_fiber_func_t func, void *arg,
		size_t stack_size)
{
	struct fbr_fiber *fiber;
	if (!LIST_EMPTY(&fctx->__p->reclaimed)) {
		fiber = LIST_FIRST(&fctx->__p->reclaimed);
		LIST_REMOVE(fiber, entries.reclaimed);
	} else {
		fiber = malloc(sizeof(struct fbr_fiber));
		memset(fiber, 0x00, sizeof(struct fbr_fiber));
		if (0 == stack_size)
			stack_size = FBR_STACK_SIZE;
		stack_size = round_up_to_page_size(stack_size);
		fiber->stack = malloc(stack_size);
		if (NULL == fiber->stack)
			err(EXIT_FAILURE, "malloc failed");
		fiber->stack_size = stack_size;
		(void)VALGRIND_STACK_REGISTER(fiber->stack, fiber->stack +
				stack_size);
		fbr_cond_init(FBR_A_ &fiber->reclaim_cond);
		fiber->id = fctx->__p->last_id++;
	}
	coro_create(&fiber->ctx, (coro_func)call_wrapper, FBR_A, fiber->stack,
			fiber->stack_size);
	LIST_INIT(&fiber->children);
	LIST_INIT(&fiber->pool);
	TAILQ_INIT(&fiber->destructors);
	strncpy(fiber->name, name, FBR_MAX_FIBER_NAME - 1);
	fiber->func = func;
	fiber->func_arg = arg;
	LIST_INSERT_HEAD(&CURRENT_FIBER->children, fiber, entries.children);
	fiber->parent = CURRENT_FIBER;
	fiber->no_reclaim = 0;
	fiber->want_reclaim = 0;
	return fbr_id_pack(fiber);
}

int fbr_disown(FBR_P_ fbr_id_t parent_id)
{
	struct fbr_fiber *fiber, *parent;
	if (!fbr_id_isnull(parent_id))
		unpack_transfer_errno(-1, &parent, parent_id);
	else
		parent = &fctx->__p->root;
	fiber = CURRENT_FIBER;
	LIST_REMOVE(fiber, entries.children);
	LIST_INSERT_HEAD(&parent->children, fiber, entries.children);
	fiber->parent = parent;
	return_success(0);
}

fbr_id_t fbr_parent(FBR_P)
{
	struct fbr_fiber *fiber = CURRENT_FIBER;
	if (fiber->parent == &fctx->__p->root)
		return FBR_ID_NULL;
	return fbr_id_pack(fiber->parent);
}

void *fbr_calloc(FBR_P_ unsigned int nmemb, size_t size)
{
	void *ptr;
	fprintf(stderr, "libevfibers: fbr_calloc is deprecated\n");
	ptr = allocate_in_fiber(FBR_A_ nmemb * size, CURRENT_FIBER);
	memset(ptr, 0x00, nmemb * size);
	return ptr;
}

void *fbr_alloc(FBR_P_ size_t size)
{
	fprintf(stderr, "libevfibers: fbr_alloc is deprecated\n");
	return allocate_in_fiber(FBR_A_ size, CURRENT_FIBER);
}

void fbr_alloc_set_destructor(_unused_ FBR_P_ void *ptr,
		fbr_alloc_destructor_func_t func, void *context)
{
	struct mem_pool *pool_entry;
	fprintf(stderr, "libevfibers:"
		       " fbr_alloc_set_destructor is deprecated\n");
	pool_entry = (struct mem_pool *)ptr - 1;
	pool_entry->destructor = func;
	pool_entry->destructor_context = context;
}

void fbr_free(FBR_P_ void *ptr)
{
	fprintf(stderr, "libevfibers: fbr_free is deprecated\n");
	fbr_free_in_fiber(FBR_A_ CURRENT_FIBER, ptr, 1);
}

void fbr_free_nd(FBR_P_ void *ptr)
{
	fprintf(stderr, "libevfibers: fbr_free_nd is deprecated\n");
	fbr_free_in_fiber(FBR_A_ CURRENT_FIBER, ptr, 0);
}

void fbr_dump_stack(FBR_P_ fbr_logutil_func_t log)
{
	struct fbr_stack_item *ptr = fctx->__p->sp;
	(*log)(FBR_A_ "%s", "Fiber call stack:");
	(*log)(FBR_A_ "%s", "-------------------------------");
	while (ptr >= fctx->__p->stack) {
		(*log)(FBR_A_ "fiber_call: %p\t%s",
				ptr->fiber,
				ptr->fiber->name);
		print_trace_info(FBR_A_ &ptr->tinfo, log);
		(*log)(FBR_A_ "%s", "-------------------------------");
		ptr--;
	}
}

static void transfer_later(FBR_P_ struct fbr_id_tailq_i *item)
{
	int was_empty;
	was_empty = TAILQ_EMPTY(&fctx->__p->pending_fibers);
	TAILQ_INSERT_TAIL(&fctx->__p->pending_fibers, item, entries);
	item->head = &fctx->__p->pending_fibers;
	if (was_empty && !TAILQ_EMPTY(&fctx->__p->pending_fibers)) {
		uv_ref((uv_handle_t *)&fctx->__p->pending_async);
	}
	uv_async_send(&fctx->__p->pending_async);
}

static void transfer_later_tailq(FBR_P_ struct fbr_id_tailq *tailq)
{
	int was_empty;
	struct fbr_id_tailq_i *item;
	TAILQ_FOREACH(item, tailq, entries) {
		item->head = &fctx->__p->pending_fibers;
	}
	was_empty = TAILQ_EMPTY(&fctx->__p->pending_fibers);
	TAILQ_CONCAT(&fctx->__p->pending_fibers, tailq, entries);
	if (was_empty && !TAILQ_EMPTY(&fctx->__p->pending_fibers)) {
		uv_ref((uv_handle_t *)&fctx->__p->pending_async);
	}
	uv_async_send(&fctx->__p->pending_async);
}

void fbr_ev_mutex_init(FBR_P_ struct fbr_ev_mutex *ev,
		struct fbr_mutex *mutex)
{
	ev_base_init(FBR_A_ &ev->ev_base, FBR_EV_MUTEX);
	ev->mutex = mutex;
}

void fbr_mutex_init(_unused_ FBR_P_ struct fbr_mutex *mutex)
{
	mutex->locked_by = FBR_ID_NULL;
	TAILQ_INIT(&mutex->pending);
}

void fbr_mutex_lock(FBR_P_ struct fbr_mutex *mutex)
{
	struct fbr_ev_mutex ev;

	assert(!fbr_id_eq(mutex->locked_by, CURRENT_FIBER_ID) &&
			"Mutex is already locked by current fiber");
	fbr_ev_mutex_init(FBR_A_ &ev, mutex);
	fbr_ev_wait_one(FBR_A_ &ev.ev_base);
	assert(fbr_id_eq(mutex->locked_by, CURRENT_FIBER_ID));
}

int fbr_mutex_trylock(FBR_P_ struct fbr_mutex *mutex)
{
	if (fbr_id_isnull(mutex->locked_by)) {
		mutex->locked_by = CURRENT_FIBER_ID;
		return 1;
	}
	return 0;
}

void fbr_mutex_unlock(FBR_P_ struct fbr_mutex *mutex)
{
	struct fbr_id_tailq_i *item, *x;
	struct fbr_fiber *fiber = NULL;
	assert(fbr_id_eq(mutex->locked_by, CURRENT_FIBER_ID) &&
			"Can't unlock the mutex, locked by another fiber");

	if (TAILQ_EMPTY(&mutex->pending)) {
		mutex->locked_by = FBR_ID_NULL;
		return;
	}

	TAILQ_FOREACH_SAFE(item, &mutex->pending, entries, x) {
		assert(item->head == &mutex->pending);
		TAILQ_REMOVE(&mutex->pending, item, entries);
		if (-1 == fbr_id_unpack(FBR_A_ &fiber, item->id)) {
			fbr_log_e(FBR_A_ "libevfibers: unexpected error trying"
					" to find a fiber by id: %s",
					fbr_strerror(FBR_A_ fctx->f_errno));
			continue;
		}
		break;
	}

	mutex->locked_by = item->id;
	assert(!fbr_id_isnull(mutex->locked_by));
	post_ev(FBR_A_ fiber, item->ev);

	transfer_later(FBR_A_ item);
}

void fbr_mutex_destroy(_unused_ FBR_P_ _unused_ struct fbr_mutex *mutex)
{
	/* Since mutex is stack allocated now, this efffeectively turns into
	 * NOOP. But we might consider adding some cleanup in the future.
	 */
}

void fbr_ev_cond_var_init(FBR_P_ struct fbr_ev_cond_var *ev,
		struct fbr_cond_var *cond, struct fbr_mutex *mutex)
{
	ev_base_init(FBR_A_ &ev->ev_base, FBR_EV_COND_VAR);
	ev->cond = cond;
	ev->mutex = mutex;
}

void fbr_cond_init(_unused_ FBR_P_ struct fbr_cond_var *cond)
{
	cond->mutex = NULL;
	TAILQ_INIT(&cond->waiting);
}

void fbr_cond_destroy(_unused_ FBR_P_ _unused_ struct fbr_cond_var *cond)
{
	/* Since condvar is stack allocated now, this efffeectively turns into
	 * NOOP. But we might consider adding some cleanup in the future.
	 */
}

int fbr_cond_wait(FBR_P_ struct fbr_cond_var *cond, struct fbr_mutex *mutex)
{
	struct fbr_ev_cond_var ev;

	if (mutex && fbr_id_isnull(mutex->locked_by))
		return_error(-1, FBR_EINVAL);

	fbr_ev_cond_var_init(FBR_A_ &ev, cond, mutex);
	fbr_ev_wait_one(FBR_A_ &ev.ev_base);
	return_success(0);
}

void fbr_cond_broadcast(FBR_P_ struct fbr_cond_var *cond)
{
	struct fbr_id_tailq_i *item;
	struct fbr_fiber *fiber;
	if (TAILQ_EMPTY(&cond->waiting))
		return;
	TAILQ_FOREACH(item, &cond->waiting, entries) {
		if(-1 == fbr_id_unpack(FBR_A_ &fiber, item->id)) {
			assert(FBR_ENOFIBER == fctx->f_errno);
			continue;
		}
		post_ev(FBR_A_ fiber, item->ev);
	}
	transfer_later_tailq(FBR_A_ &cond->waiting);
}

void fbr_cond_signal(FBR_P_ struct fbr_cond_var *cond)
{
	struct fbr_id_tailq_i *item;
	struct fbr_fiber *fiber;
	if (TAILQ_EMPTY(&cond->waiting))
		return;
	item = TAILQ_FIRST(&cond->waiting);
	if(-1 == fbr_id_unpack(FBR_A_ &fiber, item->id)) {
		assert(FBR_ENOFIBER == fctx->f_errno);
		return;
	}
	post_ev(FBR_A_ fiber, item->ev);

	assert(item->head == &cond->waiting);
	TAILQ_REMOVE(&cond->waiting, item, entries);
	transfer_later(FBR_A_ item);
}

int fbr_vrb_init(struct fbr_vrb *vrb, size_t size, const char *file_pattern)
{
	int fd = -1;
	size_t sz = get_page_size();
	size = (size ? round_up_to_page_size(size) : sz);
	void *ptr = MAP_FAILED;
	char *temp_name = NULL;
	mode_t old_umask;
	const mode_t secure_umask = 077;

	temp_name = strdup(file_pattern);
	if (!temp_name)
		return -1;
	//fctx->__p->vrb_file_pattern);
	vrb->mem_ptr_size = size * 2 + sz * 2;
	vrb->mem_ptr = mmap(NULL, vrb->mem_ptr_size, PROT_NONE,
			FBR_MAP_ANON_FLAG | MAP_PRIVATE, -1, 0);
	if (MAP_FAILED == vrb->mem_ptr)
		goto error;
	vrb->lower_ptr = vrb->mem_ptr + sz;
	vrb->upper_ptr = vrb->lower_ptr + size;
	vrb->ptr_size = size;
	vrb->data_ptr = vrb->lower_ptr;
	vrb->space_ptr = vrb->lower_ptr;

	old_umask = umask(0);
	umask(secure_umask);
	fd = mkstemp(temp_name);
	umask(old_umask);
	if (0 >= fd)
		goto error;

	if (0 > unlink(temp_name))
		goto error;
	free(temp_name);
	temp_name = NULL;

	if (0 > ftruncate(fd, size))
		goto error;

	ptr = mmap(vrb->lower_ptr, vrb->ptr_size, PROT_READ | PROT_WRITE,
			MAP_FIXED | MAP_SHARED, fd, 0);
	if (MAP_FAILED == ptr)
		goto error;
	if (ptr != vrb->lower_ptr)
		goto error;

	ptr = mmap(vrb->upper_ptr, vrb->ptr_size, PROT_READ | PROT_WRITE,
			MAP_FIXED | MAP_SHARED, fd, 0);
	if (MAP_FAILED == ptr)
		goto error;
	if (ptr != vrb->upper_ptr)
		goto error;

	close(fd);
	return 0;

error:
	if (MAP_FAILED != ptr)
		munmap(ptr, size);
	if (0 < fd)
		close(fd);
	if (vrb->mem_ptr)
		munmap(vrb->mem_ptr, vrb->mem_ptr_size);
	if (temp_name)
		free(temp_name);
	return -1;
}

int fbr_buffer_init(FBR_P_ struct fbr_buffer *buffer, size_t size)
{
	int rv;
	rv = fbr_vrb_init(&buffer->vrb, size, fctx->__p->buffer_file_pattern);
	if (rv)
		return_error(-1, FBR_EBUFFERMMAP);

	buffer->prepared_bytes = 0;
	buffer->waiting_bytes = 0;
	fbr_cond_init(FBR_A_ &buffer->committed_cond);
	fbr_cond_init(FBR_A_ &buffer->bytes_freed_cond);
	fbr_mutex_init(FBR_A_ &buffer->write_mutex);
	fbr_mutex_init(FBR_A_ &buffer->read_mutex);
	return_success(0);
}

void fbr_vrb_destroy(struct fbr_vrb *vrb)
{
	munmap(vrb->upper_ptr, vrb->ptr_size);
	munmap(vrb->lower_ptr, vrb->ptr_size);
	munmap(vrb->mem_ptr, vrb->mem_ptr_size);
}

void fbr_buffer_destroy(FBR_P_ struct fbr_buffer *buffer)
{
	fbr_vrb_destroy(&buffer->vrb);

	fbr_mutex_destroy(FBR_A_ &buffer->read_mutex);
	fbr_mutex_destroy(FBR_A_ &buffer->write_mutex);
	fbr_cond_destroy(FBR_A_ &buffer->committed_cond);
	fbr_cond_destroy(FBR_A_ &buffer->bytes_freed_cond);
}

void *fbr_buffer_alloc_prepare(FBR_P_ struct fbr_buffer *buffer, size_t size)
{
	if (size > fbr_buffer_size(FBR_A_ buffer))
		return_error(NULL, FBR_EINVAL);

	fbr_mutex_lock(FBR_A_ &buffer->write_mutex);

	while (buffer->prepared_bytes > 0)
		fbr_cond_wait(FBR_A_ &buffer->committed_cond,
				&buffer->write_mutex);

	assert(0 == buffer->prepared_bytes);

	buffer->prepared_bytes = size;

	while (fbr_buffer_free_bytes(FBR_A_ buffer) < size)
		fbr_cond_wait(FBR_A_ &buffer->bytes_freed_cond,
				&buffer->write_mutex);

	return fbr_buffer_space_ptr(FBR_A_ buffer);
}

void fbr_buffer_alloc_commit(FBR_P_ struct fbr_buffer *buffer)
{
	fbr_vrb_give(&buffer->vrb, buffer->prepared_bytes);
	buffer->prepared_bytes = 0;
	fbr_cond_signal(FBR_A_ &buffer->committed_cond);
	fbr_mutex_unlock(FBR_A_ &buffer->write_mutex);
}

void fbr_buffer_alloc_abort(FBR_P_ struct fbr_buffer *buffer)
{
	buffer->prepared_bytes = 0;
	fbr_cond_signal(FBR_A_ &buffer->committed_cond);
	fbr_mutex_unlock(FBR_A_ &buffer->write_mutex);
}

void *fbr_buffer_read_address(FBR_P_ struct fbr_buffer *buffer, size_t size)
{
	int retval;
	if (size > fbr_buffer_size(FBR_A_ buffer))
		return_error(NULL, FBR_EINVAL);

	fbr_mutex_lock(FBR_A_ &buffer->read_mutex);

	while (fbr_buffer_bytes(FBR_A_ buffer) < size) {
		retval = fbr_cond_wait(FBR_A_ &buffer->committed_cond,
				&buffer->read_mutex);
		assert(0 == retval);
		(void)retval;
	}

	buffer->waiting_bytes = size;

	return_success(fbr_buffer_data_ptr(FBR_A_ buffer));
}

void fbr_buffer_read_advance(FBR_P_ struct fbr_buffer *buffer)
{
	fbr_vrb_take(&buffer->vrb, buffer->waiting_bytes);

	fbr_cond_signal(FBR_A_ &buffer->bytes_freed_cond);
	fbr_mutex_unlock(FBR_A_ &buffer->read_mutex);
}

void fbr_buffer_read_discard(FBR_P_ struct fbr_buffer *buffer)
{
	fbr_mutex_unlock(FBR_A_ &buffer->read_mutex);
}

int fbr_buffer_resize(FBR_P_ struct fbr_buffer *buffer, size_t size)
{
	int rv;
	fbr_mutex_lock(FBR_A_ &buffer->read_mutex);
	fbr_mutex_lock(FBR_A_ &buffer->write_mutex);
	rv = fbr_vrb_resize(&buffer->vrb, size, fctx->__p->buffer_file_pattern);
	fbr_mutex_unlock(FBR_A_ &buffer->write_mutex);
	fbr_mutex_unlock(FBR_A_ &buffer->read_mutex);
	if (rv)
		return_error(-1, FBR_EBUFFERMMAP);
	return_success(0);
}

struct fbr_mq *fbr_mq_create(FBR_P_ size_t size, int flags)
{
	struct fbr_mq *mq;

	mq = calloc(1, sizeof(*mq));
	mq->fctx = fctx;
	mq->max = size + 1; /* One element is always unused */
	mq->rb = calloc(mq->max, sizeof(void *));
	mq->flags = flags;

	fbr_cond_init(FBR_A_ &mq->bytes_available_cond);
	fbr_cond_init(FBR_A_ &mq->bytes_freed_cond);

	return mq;
}

void fbr_mq_clear(struct fbr_mq *mq, int wake_up_writers)
{
	memset(mq->rb, 0x00, mq->max * sizeof(void *));
	mq->head = 0;
	mq->tail = 0;

	if (wake_up_writers)
		fbr_cond_signal(mq->fctx, &mq->bytes_available_cond);
}

void fbr_mq_push(struct fbr_mq *mq, void *obj)
{
	unsigned next;

	while ((next = ((mq->head + 1) % mq->max )) == mq->tail)
		fbr_cond_wait(mq->fctx, &mq->bytes_freed_cond, NULL);

	mq->rb[mq->head] = obj;
	mq->head = next;

	fbr_cond_signal(mq->fctx, &mq->bytes_available_cond);
}

int fbr_mq_try_push(struct fbr_mq *mq, void *obj)
{
	unsigned next = mq->head + 1;
	if (next >= mq->max)
		next = 0;

	/* Cicular buffer is full */
	if (next == mq->tail)
		return -1;

	mq->rb[mq->head] = obj;
	mq->head = next;

	fbr_cond_signal(mq->fctx, &mq->bytes_available_cond);
	return 0;
}

void fbr_mq_wait_push(struct fbr_mq *mq)
{
	while (((mq->head + 1) % mq->max) == mq->tail)
		fbr_cond_wait(mq->fctx, &mq->bytes_freed_cond, NULL);
}

static void *mq_do_pop(struct fbr_mq *mq)
{
	void *obj;
	unsigned next;

	obj = mq->rb[mq->tail];
	mq->rb[mq->tail] = NULL;

	next = mq->tail + 1;
	if (next >= mq->max)
		next = 0;

	mq->tail = next;

	fbr_cond_signal(mq->fctx, &mq->bytes_freed_cond);
	return obj;
}

void *fbr_mq_pop(struct fbr_mq *mq)
{

	/* if the head isn't ahead of the tail, we don't have any emelemnts */
	while (mq->head == mq->tail)
		fbr_cond_wait(mq->fctx, &mq->bytes_available_cond, NULL);

	return mq_do_pop(mq);
}

int fbr_mq_try_pop(struct fbr_mq *mq, void **obj)
{
	/* if the head isn't ahead of the tail, we don't have any emelemnts */
	if (mq->head == mq->tail)
		return -1;

	*obj = mq_do_pop(mq);
	return 0;
}

void fbr_mq_wait_pop(struct fbr_mq *mq)
{
	/* if the head isn't ahead of the tail, we don't have any emelemnts */
	while (mq->head == mq->tail)
		fbr_cond_wait(mq->fctx, &mq->bytes_available_cond, NULL);
}

void fbr_mq_destroy(struct fbr_mq *mq)
{
	fbr_cond_destroy(mq->fctx, &mq->bytes_freed_cond);
	fbr_cond_destroy(mq->fctx, &mq->bytes_available_cond);
	free(mq->rb);
	free(mq);
}

void *fbr_get_user_data(FBR_P_ fbr_id_t id)
{
	struct fbr_fiber *fiber;
	unpack_transfer_errno(NULL, &fiber, id);
	return_success(fiber->user_data);
}

int fbr_set_user_data(FBR_P_ fbr_id_t id, void *data)
{
	struct fbr_fiber *fiber;
	unpack_transfer_errno(-1, &fiber, id);
	fiber->user_data = data;
	return_success(0);
}

void fbr_destructor_add(FBR_P_ struct fbr_destructor *dtor)
{
	struct fbr_fiber *fiber = CURRENT_FIBER;
	TAILQ_INSERT_TAIL(&fiber->destructors, dtor, entries);
	dtor->active = 1;
}

void fbr_destructor_remove(FBR_P_ struct fbr_destructor *dtor,
		int call)
{
	struct fbr_fiber *fiber = CURRENT_FIBER;

	if (0 == dtor->active)
		return;

	TAILQ_REMOVE(&fiber->destructors, dtor, entries);
	if (call)
		dtor->func(FBR_A_ dtor->arg);
	dtor->active = 0;
}

static inline int wrap_ffsll(uint64_t val)
{
	/* TODO: Add some check for the existance of this builtin */
	return __builtin_ffsll(val);
}

static inline int is_key_registered(FBR_P_ fbr_key_t key)
{
	return 0 == (fctx->__p->key_free_mask & (1 << key));
}

static inline void register_key(FBR_P_ fbr_key_t key)
{

	fctx->__p->key_free_mask &= ~(1ULL << key);
}

static inline void unregister_key(FBR_P_ fbr_key_t key)
{
	fctx->__p->key_free_mask |= (1 << key);
}

int fbr_key_create(FBR_P_ fbr_key_t *key_ptr)
{
	fbr_key_t key = wrap_ffsll(fctx->__p->key_free_mask) - 1;
	assert(key < FBR_MAX_KEY);
	register_key(FBR_A_ key);
	*key_ptr = key;
	return_success(0);
}

int fbr_key_delete(FBR_P_ fbr_key_t key)
{
	if (!is_key_registered(FBR_A_ key))
		return_error(-1, FBR_ENOKEY);

	unregister_key(FBR_A_ key);

	return_success(0);
}

int fbr_key_set(FBR_P_ fbr_id_t id, fbr_key_t key, void *value)
{
	struct fbr_fiber *fiber;

	unpack_transfer_errno(-1, &fiber, id);

	if (!is_key_registered(FBR_A_ key))
		return_error(-1, FBR_ENOKEY);

	fiber->key_data[key] = value;
	return_success(0);
}

void *fbr_key_get(FBR_P_ fbr_id_t id, fbr_key_t key)
{
	struct fbr_fiber *fiber;

	unpack_transfer_errno(NULL, &fiber, id);

	if (!is_key_registered(FBR_A_ key))
		return_error(NULL, FBR_ENOKEY);

	return fiber->key_data[key];
}

const char *fbr_get_name(FBR_P_ fbr_id_t id)
{
	struct fbr_fiber *fiber;
	unpack_transfer_errno(NULL, &fiber, id);
	return_success(fiber->name);
}

int fbr_set_name(FBR_P_ fbr_id_t id, const char *name)
{
	struct fbr_fiber *fiber;
	unpack_transfer_errno(-1, &fiber, id);
	strncpy(fiber->name, name, FBR_MAX_FIBER_NAME - 1);
	return_success(0);
}
