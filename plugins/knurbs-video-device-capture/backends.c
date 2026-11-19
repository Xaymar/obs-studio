/*
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 2 of the License, or (at your option)
 * any later version.
 */
#include "video-device.h"

/* Backends are added here per platform, in Auto preference order, as they
 * are ported from the old per-platform plugins. */

#ifdef HAVE_DSHOW
extern const struct vd_backend vd_dshow_backend;
#endif
#ifdef HAVE_PIPEWIRE_CAMERA
extern const struct vd_backend vd_pipewire_backend;
#endif
#ifdef HAVE_V4L2
extern const struct vd_backend vd_v4l2_backend;
#endif

const struct vd_backend *const vd_backends[] = {
#ifdef HAVE_DSHOW
	&vd_dshow_backend,
#endif
#ifdef HAVE_PIPEWIRE_CAMERA
	/* Webcams through the portal first; V4L2 for capture cards. */
	&vd_pipewire_backend,
#endif
#ifdef HAVE_V4L2
	&vd_v4l2_backend,
#endif
	NULL, /* terminator, and keeps the array non-empty */
};

const size_t vd_num_backends = sizeof(vd_backends) / sizeof(vd_backends[0]) - 1;
