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

#include "obs-mem.h"
#include "util/bmem.h"
#include "util/platform.h"
#include "util/threading.h"

struct obs_mem_weak {
	volatile long weak_refs; /* starts at 1: the object's own reference */
	volatile long pins;      /* upgrades in progress */
	volatile long dead;      /* set once the object is destroyed */
	void *object;            /* never cleared, so identity checks keep working */
};

#define OBS_MEM_WEAK_DYING ((struct obs_mem_weak *)(uintptr_t)1)

#ifdef _MSC_VER
static_assert(sizeof(struct obs_mem_header) == 32, "obs_mem_header must be 32 bytes");
#else
_Static_assert(sizeof(struct obs_mem_header) == 32, "obs_mem_header must be 32 bytes");
#endif

static inline struct obs_mem_header *hdr_of(void *obj)
{
	return obj;
}

void obs_mem_init(void *obj, obs_mem_destroy_cb destroy)
{
	if (!obj)
		return;

	struct obs_mem_header *hdr = hdr_of(obj);
	hdr->refs = 1;
	hdr->weak = NULL;
	hdr->destroy = destroy;
	hdr->flags = 0;
}

void *obs_mem_acquire(void *obj)
{
	if (obj)
		os_atomic_inc_long(&hdr_of(obj)->refs);
	return obj;
}

void *obs_mem_get_ref(void *obj)
{
	if (!obj)
		return NULL;

	struct obs_mem_header *hdr = hdr_of(obj);
	long refs = os_atomic_load_long(&hdr->refs);
	while (refs > 0) {
		if (os_atomic_compare_exchange_long(&hdr->refs, &refs, refs + 1))
			return obj;
	}
	return NULL;
}

/*
 * Marks the object's weak block dead and waits for in-flight upgrades; returns the block, if any.
 * Unless forced, an existing block stays attached and the marker is only installed when none exists.
 */
static struct obs_mem_weak *mark_dead(struct obs_mem_header *hdr, bool force_dying)
{
	struct obs_mem_weak *w = NULL;

	if (force_dying) {
		w = os_atomic_set_ptr((void *volatile *)&hdr->weak, OBS_MEM_WEAK_DYING);
	} else if (os_atomic_compare_exchange_ptr((void *volatile *)&hdr->weak, (void **)&w, OBS_MEM_WEAK_DYING)) {
		return NULL;
	}

	if (!w || w == OBS_MEM_WEAK_DYING)
		return NULL;

	os_atomic_store_long(&w->dead, 1);
	while (os_atomic_load_long(&w->pins))
		os_sleep_ms(0);
	return w;
}

bool obs_mem_release(void *obj)
{
	if (!obj)
		return false;

	struct obs_mem_header *hdr = hdr_of(obj);
	if (os_atomic_dec_long(&hdr->refs) != 0)
		return false;

	mark_dead(hdr, false);
	hdr->destroy(obj);
	return true;
}

obs_mem_weak_t *obs_mem_get_weak(void *obj)
{
	if (!obj)
		return NULL;

	struct obs_mem_header *hdr = hdr_of(obj);
	struct obs_mem_weak *w = os_atomic_load_ptr((void *const volatile *)&hdr->weak);

	if (!w) {
		struct obs_mem_weak *created = bzalloc(sizeof(*created));
		created->weak_refs = 1; /* the object's own reference */
		created->object = obj;

		if (os_atomic_compare_exchange_ptr((void *volatile *)&hdr->weak, (void **)&w, created))
			w = created;
		else
			bfree(created);
	}

	if (w == OBS_MEM_WEAK_DYING) {
		/* Already dying: hand out an unattached block that is expired from the start. */
		w = bzalloc(sizeof(*w));
		w->weak_refs = 1;
		w->dead = 1;
		w->object = obj;
		return w;
	}

	os_atomic_inc_long(&w->weak_refs);
	return w;
}

void obs_mem_weak_acquire(obs_mem_weak_t *weak)
{
	if (weak)
		os_atomic_inc_long(&weak->weak_refs);
}

void obs_mem_weak_release(obs_mem_weak_t *weak)
{
	if (weak && os_atomic_dec_long(&weak->weak_refs) == 0)
		bfree(weak);
}

void *obs_mem_weak_get_strong(obs_mem_weak_t *weak)
{
	if (!weak)
		return NULL;

	void *ret = NULL;

	/* The pin keeps the object's memory alive while its refs are touched. */
	os_atomic_inc_long(&weak->pins);
	if (!os_atomic_load_long(&weak->dead))
		ret = obs_mem_get_ref(weak->object);
	os_atomic_dec_long(&weak->pins);
	return ret;
}

bool obs_mem_weak_expired(obs_mem_weak_t *weak)
{
	return !weak || os_atomic_load_long(&weak->dead);
}

bool obs_mem_weak_references(obs_mem_weak_t *weak, void *obj)
{
	return weak && weak->object == obj;
}

void obs_mem_finalize(void *obj)
{
	if (!obj)
		return;

	/* Also covers forced destroys where refs never reached 0. */
	struct obs_mem_weak *w = mark_dead(hdr_of(obj), true);
	if (w)
		obs_mem_weak_release(w); /* the object's own reference */
}
