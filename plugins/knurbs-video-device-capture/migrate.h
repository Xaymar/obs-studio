/*
 * Settings migration from the per-platform camera sources to Video Device
 * Capture. Each function is an obs_source_alias_migrate_t.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 2 of the License, or (at your option)
 * any later version.
 */
#pragma once

#include <obs.h>

#ifdef __cplusplus
extern "C" {
#endif

void vd_migrate_dshow(void *param, const char *old_id, obs_data_t *settings);
void vd_migrate_v4l2(void *param, const char *old_id, obs_data_t *settings);
void vd_migrate_pipewire_camera(void *param, const char *old_id, obs_data_t *settings);
void vd_migrate_avfoundation(void *param, const char *old_id, obs_data_t *settings);

/* Registers every old camera source ID as an alias of Video Device Capture.
 * While an old plugin is still loaded, its own source type wins over the
 * alias, so this only takes effect once that plugin is retired. */
void vd_register_aliases(void);

#ifdef __cplusplus
}
#endif
