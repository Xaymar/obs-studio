/*
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 2 of the License, or (at your option)
 * any later version.
 */
#include "video-device.h"

/* Backends are added here per platform, in Auto preference order, as they
 * are ported from the old per-platform plugins. */

const struct vd_backend *const vd_backends[] = {
	NULL, /* keeps the array non-empty while no backend is ported yet */
};

const size_t vd_num_backends = sizeof(vd_backends) / sizeof(vd_backends[0]) - 1;
