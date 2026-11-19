#include "settings.hpp"

#include <util/platform.h>

namespace knurbs::device_output {

namespace {

constexpr int kVersion = 1;

constexpr Slot kSlots[] = {Slot::Program, Slot::Preview, Slot::Audio};

SlotConfig slot_from_data(obs_data_t *data, Slot slot)
{
	SlotConfig config;

	OBSDataAutoRelease entry = data ? obs_data_get_obj(data, slot_key(slot)) : nullptr;
	if (entry) {
		const char *type = obs_data_get_string(entry, "type");
		const OutputType *known = find_type(type);
		if (known && supports(*known, slot)) {
			config.type = known->id;
		}
		config.autoStart = obs_data_get_bool(entry, "auto-start");

		OBSDataAutoRelease settings = obs_data_get_obj(entry, "settings");
		if (settings && !config.type.empty()) {
			config.settings = copy_data(settings);
		}
	}

	if (!config.settings) {
		OBSDataAutoRelease empty = obs_data_create();
		config.settings = empty.Get();
	}
	return config;
}

} // namespace

SlotConfig &Config::at(Slot slot)
{
	switch (slot) {
	case Slot::Program:
		return program;
	case Slot::Preview:
		return preview;
	case Slot::Audio:
		return audio;
	}
	return program;
}

const SlotConfig &Config::at(Slot slot) const
{
	return const_cast<Config *>(this)->at(slot);
}

OBSData copy_data(obs_data_t *data)
{
	OBSDataAutoRelease copy = obs_data_create();
	if (data) {
		obs_data_apply(copy, data);
	}
	return copy.Get();
}

Config config_from_data(obs_data_t *data)
{
	Config config;
	for (Slot slot : kSlots) {
		config.at(slot) = slot_from_data(data, slot);
	}
	return config;
}

OBSData config_to_data(const Config &config)
{
	OBSDataAutoRelease data = obs_data_create();
	obs_data_set_int(data, "version", kVersion);

	for (Slot slot : kSlots) {
		const SlotConfig &slot_config = config.at(slot);

		OBSDataAutoRelease entry = obs_data_create();
		obs_data_set_string(entry, "type", slot_config.type.c_str());
		obs_data_set_bool(entry, "auto-start", slot_config.autoStart);
		OBSData settings = copy_data(slot_config.settings);
		obs_data_set_obj(entry, "settings", settings);

		obs_data_set_obj(data, slot_key(slot), entry);
	}
	return data.Get();
}

Config load_config(const std::string &path)
{
	OBSDataAutoRelease data = path.empty() ? nullptr : obs_data_create_from_json_file_safe(path.c_str(), "bak");
	return config_from_data(data);
}

bool save_config(const Config &config, const std::string &path)
{
	if (path.empty()) {
		return false;
	}
	OBSData data = config_to_data(config);
	return obs_data_save_json_pretty_safe(data, path.c_str(), "tmp", "bak");
}

} // namespace knurbs::device_output
