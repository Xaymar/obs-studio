/*
 * Thread-specific data (pthread_key_*) on top of Win32 TLS slots.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version. See COPYING.LIB.
 */

#include "internal.h"

/* Keys with destructors, so they can be run when a thread exits. */
static SRWLOCK keys_lock = SRWLOCK_INIT;
static struct pthread_key_t_ *keys_head = NULL;

int PTW32_CDECL pthread_key_create(pthread_key_t *key, void(PTW32_CDECL *destructor)(void *))
{
	struct pthread_key_t_ *k;

	if (!key) {
		return EINVAL;
	}

	k = (struct pthread_key_t_ *)calloc(1, sizeof(*k));
	if (!k) {
		return ENOMEM;
	}

	k->tls = TlsAlloc();
	if (k->tls == TLS_OUT_OF_INDEXES) {
		free(k);
		return EAGAIN;
	}
	k->destructor = destructor;

	if (destructor) {
		AcquireSRWLockExclusive(&keys_lock);
		k->next = keys_head;
		if (keys_head) {
			keys_head->prev = k;
		}
		keys_head = k;
		ReleaseSRWLockExclusive(&keys_lock);
	}

	*key = k;
	return 0;
}

int PTW32_CDECL pthread_key_delete(pthread_key_t key)
{
	if (!key) {
		return EINVAL;
	}

	if (key->destructor) {
		AcquireSRWLockExclusive(&keys_lock);
		if (key->prev) {
			key->prev->next = key->next;
		} else {
			keys_head = key->next;
		}
		if (key->next) {
			key->next->prev = key->prev;
		}
		ReleaseSRWLockExclusive(&keys_lock);
	}

	/* POSIX: destructors are not called for values still set. */
	TlsFree(key->tls);
	free(key);
	return 0;
}

int PTW32_CDECL pthread_setspecific(pthread_key_t key, const void *value)
{
	if (!key) {
		return EINVAL;
	}

	/* Make sure the thread has a descriptor, so a non-pthread thread
	 * still gets its destructors run when it exits. */
	if (key->destructor && value) {
		(void)ptw_self();
	}

	return TlsSetValue(key->tls, (LPVOID)value) ? 0 : EINVAL;
}

void *PTW32_CDECL pthread_getspecific(pthread_key_t key)
{
	DWORD err;
	void *v;

	if (!key) {
		return NULL;
	}

	/* Like pthreads-win32, leave GetLastError() untouched. */
	err = GetLastError();
	v = TlsGetValue(key->tls);
	SetLastError(err);
	return v;
}

struct pending_dtor {
	void(PTW32_CDECL *destructor)(void *);
	void *value;
};

void ptw_run_key_destructors(void)
{
	int round;

	for (round = 0; round < PTHREAD_DESTRUCTOR_ITERATIONS; round++) {
		struct pending_dtor stack_buf[32];
		struct pending_dtor *pending = stack_buf;
		size_t count = 0, capacity = 32, i;
		struct pthread_key_t_ *k;

		/* Collect values under the lock, run destructors without it:
		 * destructors may create, delete or set keys themselves. */
		AcquireSRWLockShared(&keys_lock);
		for (k = keys_head; k; k = k->next) {
			void *v = TlsGetValue(k->tls);
			if (!v) {
				continue;
			}

			if (count == capacity) {
				size_t new_cap = capacity * 2;
				struct pending_dtor *grown = (struct pending_dtor *)malloc(new_cap * sizeof(*grown));
				if (!grown) {
					break;
				}
				memcpy(grown, pending, count * sizeof(*grown));
				if (pending != stack_buf) {
					free(pending);
				}
				pending = grown;
				capacity = new_cap;
			}

			TlsSetValue(k->tls, NULL);
			pending[count].destructor = k->destructor;
			pending[count].value = v;
			count++;
		}
		ReleaseSRWLockShared(&keys_lock);

		for (i = 0; i < count; i++) {
			pending[i].destructor(pending[i].value);
		}

		if (pending != stack_buf) {
			free(pending);
		}

		if (count == 0) {
			break;
		}
	}
}
