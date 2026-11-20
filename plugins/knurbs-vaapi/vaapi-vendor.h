#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VAAPI_PCI_VENDOR_INTEL 0x8086

/* Returns the PCI vendor ID of a DRM device node such as /dev/dri/renderD128
 * or a /dev/dri/by-path link to one, or 0 if it can't be found. The vendor is
 * read from <sysfs_root>/class/drm/<node>/device/vendor; sysfs_root is "/sys"
 * outside of tests. */
uint32_t vaapi_device_pci_vendor(const char *sysfs_root, const char *device_path);

/* Intel devices are left to knurbs-qsv, so VA-API skips them unless the
 * KNURBS_VAAPI_ALLOW_INTEL environment variable is set to 1. */
bool vaapi_device_skipped(const char *device_path);

#ifdef __cplusplus
}
#endif
