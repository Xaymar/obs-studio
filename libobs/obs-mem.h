/******************************************************************************
    Copyright (C) 2026 by OBS Project

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#pragma once

#include "util/c99defs.h"

/*
 * Unified reference counting. Embed struct obs_mem_header as the first member
 * of an object allocated with bzalloc, call obs_mem_init on it, and free its
 * memory with bfree from inside the destroy callback, after calling
 * obs_mem_finalize.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*obs_mem_destroy_cb)(void *obj);
typedef struct obs_mem_weak obs_mem_weak_t;

#ifdef _MSC_VER
#define OBS_MEM_ALIGN32 __declspec(align(32))
#else
#define OBS_MEM_ALIGN32 __attribute__((aligned(32)))
#endif

/* 32 bytes, 32-byte aligned; must be the first member of the object. */
struct OBS_MEM_ALIGN32 obs_mem_header {
	volatile long refs;                 /* strong count, destroyed at 0 */
	struct obs_mem_weak *volatile weak; /* NULL, a weak block or a "dying" marker */
	obs_mem_destroy_cb destroy;
	volatile long flags; /* spare */
};

/* Sets refs to 1 and stores the destroy callback. */
EXPORT void obs_mem_init(void *obj, obs_mem_destroy_cb destroy);

/* Unconditional increment; the caller must already hold a reference. NULL-safe. */
EXPORT void *obs_mem_acquire(void *obj);

/* Increments only if the object is not yet dying, otherwise returns NULL. */
EXPORT void *obs_mem_get_ref(void *obj);

/* Returns true if this call destroyed the object. May run on any thread. */
EXPORT bool obs_mem_release(void *obj);

EXPORT obs_mem_weak_t *obs_mem_get_weak(void *obj);
EXPORT void obs_mem_weak_acquire(obs_mem_weak_t *weak);
EXPORT void obs_mem_weak_release(obs_mem_weak_t *weak);
EXPORT void *obs_mem_weak_get_strong(obs_mem_weak_t *weak);
EXPORT bool obs_mem_weak_expired(obs_mem_weak_t *weak);
EXPORT bool obs_mem_weak_references(obs_mem_weak_t *weak, void *obj);

/* Call right before the final bfree of the object. */
EXPORT void obs_mem_finalize(void *obj);

#ifdef __cplusplus
}
#endif
