/*
 * Thread attributes.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version. See COPYING.LIB.
 */

#include "internal.h"

static BOOL attr_invalid(const pthread_attr_t *attr)
{
	return attr == NULL || *attr == NULL || (*attr)->valid != PTW_ATTR_VALID;
}

int PTW32_CDECL pthread_attr_init(pthread_attr_t *attr)
{
	struct pthread_attr_t_ *a;

	if (!attr) {
		return EINVAL;
	}
	a = (struct pthread_attr_t_ *)calloc(1, sizeof(*a));
	if (!a) {
		return ENOMEM;
	}

	a->stacksize = 0; /* use the executable's default */
	a->stackaddr = NULL;
	a->detachstate = PTHREAD_CREATE_JOINABLE;
	/* Windows starts new threads at normal priority rather than the
	 * creator's, and pthreads-win32 defaulted to the same. */
	a->param.sched_priority = THREAD_PRIORITY_NORMAL;
	a->inheritsched = PTHREAD_EXPLICIT_SCHED;
	a->contentionscope = PTHREAD_SCOPE_SYSTEM;
	a->valid = PTW_ATTR_VALID;
	*attr = a;
	return 0;
}

int PTW32_CDECL pthread_attr_destroy(pthread_attr_t *attr)
{
	if (attr_invalid(attr)) {
		return EINVAL;
	}
	(*attr)->valid = 0;
	free(*attr);
	*attr = NULL;
	return 0;
}

int PTW32_CDECL pthread_attr_getdetachstate(const pthread_attr_t *attr, int *detachstate)
{
	if (attr_invalid(attr) || !detachstate) {
		return EINVAL;
	}
	*detachstate = (*attr)->detachstate;
	return 0;
}

int PTW32_CDECL pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate)
{
	if (attr_invalid(attr)) {
		return EINVAL;
	}
	if (detachstate != PTHREAD_CREATE_JOINABLE && detachstate != PTHREAD_CREATE_DETACHED) {
		return EINVAL;
	}
	(*attr)->detachstate = detachstate;
	return 0;
}

int PTW32_CDECL pthread_attr_getstackaddr(const pthread_attr_t *attr, void **stackaddr)
{
	(void)attr;
	(void)stackaddr;
	return ENOSYS;
}

int PTW32_CDECL pthread_attr_setstackaddr(pthread_attr_t *attr, void *stackaddr)
{
	(void)attr;
	(void)stackaddr;
	return ENOSYS;
}

int PTW32_CDECL pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *stacksize)
{
	if (attr_invalid(attr) || !stacksize) {
		return EINVAL;
	}
	*stacksize = (*attr)->stacksize;
	return 0;
}

int PTW32_CDECL pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize)
{
	if (attr_invalid(attr)) {
		return EINVAL;
	}
	if (stacksize > 0xFFFFFFFFu) {
		return EINVAL; /* _beginthreadex takes an unsigned */
	}
	(*attr)->stacksize = stacksize;
	return 0;
}

int PTW32_CDECL pthread_attr_getschedparam(const pthread_attr_t *attr, struct sched_param *param)
{
	if (attr_invalid(attr) || !param) {
		return EINVAL;
	}
	*param = (*attr)->param;
	return 0;
}

int PTW32_CDECL pthread_attr_setschedparam(pthread_attr_t *attr, const struct sched_param *param)
{
	if (attr_invalid(attr) || !param) {
		return EINVAL;
	}
	if (param->sched_priority < THREAD_PRIORITY_IDLE || param->sched_priority > THREAD_PRIORITY_TIME_CRITICAL) {
		return EINVAL;
	}
	(*attr)->param = *param;
	return 0;
}

int PTW32_CDECL pthread_attr_setschedpolicy(pthread_attr_t *attr, int policy)
{
	if (attr_invalid(attr)) {
		return EINVAL;
	}
	return policy == SCHED_OTHER ? 0 : ENOTSUP;
}

int PTW32_CDECL pthread_attr_getschedpolicy(const pthread_attr_t *attr, int *policy)
{
	if (attr_invalid(attr) || !policy) {
		return EINVAL;
	}
	*policy = SCHED_OTHER;
	return 0;
}

int PTW32_CDECL pthread_attr_setinheritsched(pthread_attr_t *attr, int inheritsched)
{
	if (attr_invalid(attr)) {
		return EINVAL;
	}
	if (inheritsched != PTHREAD_INHERIT_SCHED && inheritsched != PTHREAD_EXPLICIT_SCHED) {
		return EINVAL;
	}
	(*attr)->inheritsched = inheritsched;
	return 0;
}

int PTW32_CDECL pthread_attr_getinheritsched(const pthread_attr_t *attr, int *inheritsched)
{
	if (attr_invalid(attr) || !inheritsched) {
		return EINVAL;
	}
	*inheritsched = (*attr)->inheritsched;
	return 0;
}

int PTW32_CDECL pthread_attr_setscope(pthread_attr_t *attr, int contentionscope)
{
	if (attr_invalid(attr)) {
		return EINVAL;
	}
	switch (contentionscope) {
	case PTHREAD_SCOPE_SYSTEM:
		(*attr)->contentionscope = contentionscope;
		return 0;
	case PTHREAD_SCOPE_PROCESS:
		return ENOTSUP;
	default:
		return EINVAL;
	}
}

int PTW32_CDECL pthread_attr_getscope(const pthread_attr_t *attr, int *contentionscope)
{
	if (attr_invalid(attr) || !contentionscope) {
		return EINVAL;
	}
	*contentionscope = (*attr)->contentionscope;
	return 0;
}
