#include "vaapi-vendor.h"

#include <util/base.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t vaapi_device_pci_vendor(const char *sysfs_root, const char *device_path)
{
	if (!sysfs_root || !device_path)
		return 0;

	char resolved[PATH_MAX];
	if (!realpath(device_path, resolved))
		return 0;

	const char *node = strrchr(resolved, '/');
	node = node ? node + 1 : resolved;
	if (!*node)
		return 0;

	char vendor_path[PATH_MAX];
	int len = snprintf(vendor_path, sizeof(vendor_path), "%s/class/drm/%s/device/vendor", sysfs_root, node);
	if (len < 0 || (size_t)len >= sizeof(vendor_path))
		return 0;

	FILE *file = fopen(vendor_path, "r");
	if (!file)
		return 0;

	unsigned int vendor = 0;
	if (fscanf(file, "%x", &vendor) != 1)
		vendor = 0;
	fclose(file);
	return (uint32_t)vendor;
}

static bool intel_allowed(void)
{
	const char *allow = getenv("KNURBS_VAAPI_ALLOW_INTEL");
	return allow && strcmp(allow, "1") == 0;
}

bool vaapi_device_skipped(const char *device_path)
{
	if (vaapi_device_pci_vendor("/sys", device_path) != VAAPI_PCI_VENDOR_INTEL || intel_allowed())
		return false;

	blog(LOG_DEBUG, "VAAPI: Skipping Intel device '%s', it is handled by QSV", device_path);
	return true;
}
