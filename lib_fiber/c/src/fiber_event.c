#include "stdafx.h"
#include "common.h"

#ifdef SYS_UNIX

#include "fiber/libfiber.h"
#include "fiber.h"

struct ACL_FIBER_EVENT {
	FIBER_BASE   *owner;
	ATOMIC       *atomic;
	long long     value;
	RING          waiters;
	unsigned long tid;
	unsigned int  flag;

	union {
		pthread_mutex_t   tlock;
		struct {
			ATOMIC   *alock;
			long long value;
		} atomic;
	} lock;
};

ACL_FIBER_EVENT *acl_fiber_event_create(unsigned flag)
{
	ACL_FIBER_EVENT *event = (ACL_FIBER_EVENT *)
		calloc(1, sizeof(ACL_FIBER_EVENT));

	event->owner = NULL;
	event->tid   = 0;
	event->flag  = flag;

	event->atomic = atomic_new();
	atomic_set(event->atomic, &event->value);
	atomic_int64_set(event->atomic, 0);

	if ((flag & FIBER_FLAG_USE_MUTEX)) {
		pthread_mutexattr_t attr;

		pthread_mutexattr_init(&attr);
		pthread_mutex_init(&event->lock.tlock, &attr);
		pthread_mutexattr_destroy(&attr);
	} else {
		event->lock.atomic.alock = atomic_new();
		atomic_set(event->lock.atomic.alock, &event->lock.atomic.value);
		atomic_int64_set(event->lock.atomic.alock, 0);
	}

	ring_init(&event->waiters);
	return event;
}

void acl_fiber_event_free(ACL_FIBER_EVENT *event)
{
	atomic_free(event->atomic);
	if ((event->flag & FIBER_FLAG_USE_MUTEX)) {
		pthread_mutex_destroy(&event->lock.tlock);
	} else {
		atomic_free(event->lock.atomic.alock);
	}

	free(event);
}

static inline void __ll_lock(ACL_FIBER_EVENT *event)
{
	if ((event->flag & FIBER_FLAG_USE_MUTEX)) {
		assert(pthread_mutex_lock(&event->lock.tlock) == 0);
	} else {
		while (atomic_int64_cas(event->lock.atomic.alock, 0, 1)) {}
	}
}

static inline void __ll_unlock(ACL_FIBER_EVENT *event)
{
	if ((event->flag & FIBER_FLAG_USE_MUTEX)) {
		assert(pthread_mutex_unlock(&event->lock.tlock) == 0);
	} else if (atomic_int64_cas(event->lock.atomic.alock, 1, 0) != 1) {
		msg_fatal("%s(%d), %s: lock corrupt",
			__FILE__, __LINE__, __FUNCTION__);
	}
}

int acl_fiber_event_wait(ACL_FIBER_EVENT *event)
{
	ACL_FIBER  *fiber = acl_fiber_running();
	FIBER_BASE *fbase;
	unsigned    wakeup;

	if (LIKELY(atomic_int64_cas(event->atomic, 0, 1) == 0)) {
		event->owner = fiber ? &fiber->base : NULL;
		event->tid   = __pthread_self();
		return 0;
	}

	// FIBER_BASE obj will be created if is not in fiber scheduled
	fbase = fiber ? &fiber->base : fbase_alloc();
	fbase_event_open(fbase);

	wakeup = 0;

	__ll_lock(event);

	ring_prepend(&event->waiters, &fbase->event_waiter);

	while (1) {
		if (atomic_int64_cas(event->atomic, 0, 1) == 0) {
			if (!wakeup) {
				ring_detach(&fbase->event_waiter);
			}

			__ll_unlock(event);

			event->owner = fbase;
			event->tid   = __pthread_self();
			break;
		}

		if (wakeup) {
			ring_prepend(&event->waiters, &fbase->event_waiter);
		}

		__ll_unlock(event);

		if (fbase_event_wait(fbase) == -1) {
			fbase_event_close(fbase);
			if (fbase->flag & FBASE_F_BASE) {
				fbase_free(fbase);
			}

			if ((event->flag & FIBER_FLAG_USE_FATAL)) {
				msg_fatal("%s(%d), %s: event wait error %s",
					__FILE__, __LINE__, __FUNCTION__,
					last_serror());
			}

			msg_error("%s(%d), %s: event wait error %s",
				__FILE__, __LINE__, __FUNCTION__,
				last_serror());

			fbase_event_close(fbase);
			if (fbase->flag & FBASE_F_BASE) {
				event->owner = NULL;
				fbase_free(fbase);
			}
			return -1;
		}

		// overflow ?
		if (++wakeup == 0) {
			wakeup = 1;
		}

		__ll_lock(event);
	}

	fbase_event_close(fbase);
	if (fbase->flag & FBASE_F_BASE) {
		event->owner = NULL;
		fbase_free(fbase);
	}
	return 0;
}

int acl_fiber_event_trywait(ACL_FIBER_EVENT *event)
{
	if (atomic_int64_cas(event->atomic, 0, 1) == 0) {
		ACL_FIBER *fiber = acl_fiber_running();
		event->owner     = fiber ? &fiber->base : NULL;
		event->tid       = __pthread_self();
		return 0;
	}
	return -1;
}

int acl_fiber_event_notify(ACL_FIBER_EVENT *event)
{
	ACL_FIBER  *curr  = acl_fiber_running();
	FIBER_BASE *owner = curr ? &curr->base : NULL, *waiter;
	RING       *head;

	if (UNLIKELY(event->owner != owner)) {
		msg_error("%s(%d), %s: fiber(%p) is not the owner(%p)",
			__FILE__, __LINE__, __FUNCTION__, owner, event->owner);
		return -1;
	} else if (UNLIKELY(event->owner == NULL
		&& event->tid != __pthread_self())) {

		msg_error("%s(%d), %s: tid(%lu) is not the owner(%lu)",
			__FILE__, __LINE__, __FUNCTION__,
			event->tid, __pthread_self());
		return -1;
	}

	__ll_lock(event);

	head = ring_pop_head(&event->waiters);
	if (head) {
		waiter = RING_TO_APPL(head, FIBER_BASE, event_waiter);
	} else {
		waiter = NULL;
	}

	if (atomic_int64_cas(event->atomic, 1, 0) != 1) {
		msg_fatal("%s(%d), %s: atomic corrupt",
			__FILE__, __LINE__, __FUNCTION__);
	}

	__ll_unlock(event);

	if (waiter && fbase_event_wakeup(waiter) == -1) {
		if ((event->flag & FIBER_FLAG_USE_FATAL)) {
			msg_fatal("%s(%d), %s: wakup waiter error=%s",
				__FILE__, __LINE__, __FUNCTION__,
				last_serror());
		}

		msg_error("%s(%d), %s: wakup waiter error=%s",
			__FILE__, __LINE__, __FUNCTION__, last_serror());
		return -1;
	}

	return 0;
}

#endif // SYS_UNIX
