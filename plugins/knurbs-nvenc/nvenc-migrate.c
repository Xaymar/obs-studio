#include "nvenc-migrate.h"

static void fill_int(obs_data_t *settings, const char *key, long long value)
{
	if (!obs_data_has_user_value(settings, key))
		obs_data_set_int(settings, key, value);
}

static void fill_string(obs_data_t *settings, const char *key, const char *value)
{
	if (!obs_data_has_user_value(settings, key))
		obs_data_set_string(settings, key, value);
}

static void fill_bool(obs_data_t *settings, const char *key, bool value)
{
	if (!obs_data_has_user_value(settings, key))
		obs_data_set_bool(settings, key, value);
}

void nvenc_migrate_legacy_settings(obs_data_t *settings, enum nvenc_legacy_codec codec, int num_devices)
{
	if (!settings)
		return;

	/* Defaults of the legacy (FFmpeg based) encoders. */
	fill_int(settings, "bitrate", 2500);
	fill_int(settings, "max_bitrate", 5000);
	fill_int(settings, "keyint_sec", 0);
	fill_int(settings, "cqp", 20);
	fill_string(settings, "rate_control", "CBR");
	fill_string(settings, "multipass", "qres");
	fill_string(settings, "tune", "hq");
	fill_string(settings, "profile", codec == NVENC_LEGACY_H264 ? "high" : "main");
	fill_int(settings, "bf", 2);
	fill_bool(settings, "repeat_headers", false);

	/* Keys that were renamed. */
	const char *preset = obs_data_has_user_value(settings, "preset2") ? obs_data_get_string(settings, "preset2")
									  : "p5";
	obs_data_set_string(settings, "preset", preset);
	obs_data_erase(settings, "preset2");

	const bool psycho_aq = obs_data_has_user_value(settings, "psycho_aq") ? obs_data_get_bool(settings, "psycho_aq")
									      : true;
	obs_data_set_bool(settings, "adaptive_quantization", psycho_aq);
	obs_data_erase(settings, "psycho_aq");

	if (obs_data_has_user_value(settings, "gpu")) {
		if (num_devices > 1)
			obs_data_set_int(settings, "device", obs_data_get_int(settings, "gpu"));
		obs_data_erase(settings, "gpu");
	}
}
