/* Builds against the installed libobs and obs-frontend-api packages. */
#include <obs-module.h>
#include <obs-frontend-api.h>

OBS_DECLARE_MODULE()

bool obs_module_load(void)
{
	(void)obs_frontend_get_main_window();
	return true;
}
