/* Builds against the installed libobs package using the include lines plugins use. */
#include <obs-module.h>
#include <util/bmem.h>
#include <util/threading.h>
#include <graphics/graphics.h>

OBS_DECLARE_MODULE()

bool obs_module_load(void)
{
	char *text = bstrdup("knurbs");
	bfree(text);
	return true;
}
