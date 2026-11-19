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

#ifdef __cplusplus
}
#endif
