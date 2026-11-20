/*
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 2 of the License, or (at your option)
 * any later version.
 */
#include <obs-module.h>

#include "video-device.h"
#include "migrate.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("knurbs-video-device-capture", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Video Device Capture: cameras and capture cards on every platform";
}

/* Only registers types: device probing happens on first use, so this is safe
 * to run in parallel with other modules. */
bool obs_module_load(void)
{
	obs_register_source(&video_device_capture_info);
	vd_register_aliases();
	return true;
}

void obs_module_unload(void)
{
	for (size_t i = 0; i < vd_num_backends; i++) {
		if (vd_backends[i]->unload) {
			vd_backends[i]->unload();
		}
	}
}
