#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <obs.h>
#include <util/dstr.h>
#include <util/platform.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "vaapi-vendor.h"

/* VA-API leaves Intel devices to QSV, which needs the PCI vendor of a DRM
 * render node. The tests build a fake /dev and /sys tree. */

struct tree {
	char root[64];
	struct dstr dev;
	struct dstr sys;
};

static void write_file(const char *path, const char *text)
{
	FILE *file = fopen(path, "w");
	assert_non_null(file);
	fputs(text, file);
	fclose(file);
}

static void add_node(struct tree *t, const char *node, const char *vendor)
{
	struct dstr path = {0};
	dstr_printf(&path, "%s/%s", t->dev.array, node);
	write_file(path.array, "");

	if (vendor) {
		dstr_printf(&path, "%s/class/drm/%s/device", t->sys.array, node);
		assert_int_equal(os_mkdirs(path.array), MKDIR_SUCCESS);
		dstr_cat(&path, "/vendor");
		write_file(path.array, vendor);
	}
	dstr_free(&path);
}

static int setup(void **state)
{
	struct tree *t = bzalloc(sizeof(*t));
	snprintf(t->root, sizeof(t->root), "/tmp/knurbs-vaapi-XXXXXX");
	assert_non_null(mkdtemp(t->root));
	dstr_printf(&t->dev, "%s/dev", t->root);
	dstr_printf(&t->sys, "%s/sys", t->root);
	assert_int_equal(os_mkdirs(t->dev.array), MKDIR_SUCCESS);
	assert_int_equal(os_mkdirs(t->sys.array), MKDIR_SUCCESS);
	*state = t;
	return 0;
}

static int teardown(void **state)
{
	struct tree *t = *state;
	char cmd[128];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", t->root);
	assert_int_equal(system(cmd), 0);
	dstr_free(&t->dev);
	dstr_free(&t->sys);
	bfree(t);
	return 0;
}

static void reads_vendor_of_render_node(void **state)
{
	struct tree *t = *state;
	add_node(t, "renderD128", "0x8086\n");
	add_node(t, "renderD129", "0x1002\n");

	struct dstr path = {0};
	dstr_printf(&path, "%s/renderD128", t->dev.array);
	assert_int_equal(vaapi_device_pci_vendor(t->sys.array, path.array), VAAPI_PCI_VENDOR_INTEL);
	dstr_printf(&path, "%s/renderD129", t->dev.array);
	assert_int_equal(vaapi_device_pci_vendor(t->sys.array, path.array), 0x1002);
	dstr_free(&path);
}

static void follows_by_path_links(void **state)
{
	struct tree *t = *state;
	add_node(t, "renderD128", "0x10de\n");

	struct dstr target = {0}, link = {0};
	dstr_printf(&target, "%s/renderD128", t->dev.array);
	dstr_printf(&link, "%s/pci-0000:01:00.0-render", t->dev.array);
	assert_int_equal(symlink(target.array, link.array), 0);

	assert_int_equal(vaapi_device_pci_vendor(t->sys.array, link.array), 0x10de);
	dstr_free(&target);
	dstr_free(&link);
}

static void unknown_vendor_is_zero(void **state)
{
	struct tree *t = *state;
	add_node(t, "renderD128", NULL);
	add_node(t, "renderD129", "garbage\n");

	struct dstr path = {0};
	dstr_printf(&path, "%s/renderD128", t->dev.array);
	assert_int_equal(vaapi_device_pci_vendor(t->sys.array, path.array), 0);
	dstr_printf(&path, "%s/renderD129", t->dev.array);
	assert_int_equal(vaapi_device_pci_vendor(t->sys.array, path.array), 0);
	dstr_printf(&path, "%s/missing", t->dev.array);
	assert_int_equal(vaapi_device_pci_vendor(t->sys.array, path.array), 0);
	dstr_free(&path);

	assert_int_equal(vaapi_device_pci_vendor(NULL, "/dev/null"), 0);
	assert_int_equal(vaapi_device_pci_vendor("/sys", NULL), 0);
}

static void missing_device_is_not_skipped(void **state)
{
	UNUSED_PARAMETER(state);
	/* Without a vendor there is nothing to hand to QSV. */
	assert_false(vaapi_device_skipped("/nonexistent/renderD128"));
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test_setup_teardown(reads_vendor_of_render_node, setup, teardown),
		cmocka_unit_test_setup_teardown(follows_by_path_links, setup, teardown),
		cmocka_unit_test_setup_teardown(unknown_vendor_is_zero, setup, teardown),
		cmocka_unit_test(missing_device_is_not_skipped),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
