/* A small module for test_obs_module: it registers one source type and counts its lifecycle calls. */

#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("fake-module", "en-US")
OBS_MODULE_AUTHOR("Knurbs tests")

static int loads, post_loads, unloads;

MODULE_EXPORT int fake_module_loads(void)
{
	return loads;
}

MODULE_EXPORT int fake_module_post_loads(void)
{
	return post_loads;
}

MODULE_EXPORT int fake_module_unloads(void)
{
	return unloads;
}

static const char *module_source_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("FakeModule.Name");
}

static void *module_source_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	return source;
}

static void module_source_destroy(void *data)
{
	UNUSED_PARAMETER(data);
}

static struct obs_source_info module_source = {
	.id = "fake_module_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = 0,
	.get_name = module_source_name,
	.create = module_source_create,
	.destroy = module_source_destroy,
};

const char *obs_module_name(void)
{
	return "Fake module";
}

const char *obs_module_description(void)
{
	return "A module for the libobs tests";
}

bool obs_module_load(void)
{
	loads++;
	obs_register_source(&module_source);

	/* rejected: composite sources cannot be audio sources */
	struct obs_source_info invalid = module_source;
	invalid.id = "fake_module_invalid_source";
	invalid.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_COMPOSITE | OBS_SOURCE_AUDIO;
	obs_register_source(&invalid);
	return true;
}

void obs_module_post_load(void)
{
	post_loads++;
}

void obs_module_unload(void)
{
	unloads++;
}
