/* Test-only API of the libobs-null graphics module. */

#pragma once

#include <util/c99defs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Number of calls to the named graphics export since load or the last reset; -1 for unknown names. */
EXPORT long null_gs_get_call_count(const char *name);
EXPORT void null_gs_reset_call_counts(void);

/* Number of live objects the module has created and not yet destroyed. */
EXPORT long null_gs_get_live_objects(void);

/* Makes the next `count` calls of the named export fail (return NULL, false or GS_ERROR_FAIL). Works for
 * device_create, the device_*_create functions and gs_texture_map/gs_stagesurface_map. Returns false for unknown
 * names. */
EXPORT bool null_gs_fail_next(const char *name, long count);
EXPORT void null_gs_reset_failures(void);

#ifdef __cplusplus
}
#endif
