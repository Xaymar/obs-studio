#include <obs-module.h>

#include "qsv-migrate.h"

/*
 * Aliases from the old OBS encoder IDs to the current encoders.
 *
 * - The texture and system memory variants both map to the one public ID per
 *   codec, which picks its input path itself.
 * - obs_qsv11 and obs_qsv11_soft are the deprecated v1 H.264 encoder, whose
 *   settings also get converted, see qsv_migrate_v1_settings.
 */

static void migrate_v1(void *param, const char *old_id, obs_data_t *settings)
{
	UNUSED_PARAMETER(param);
	UNUSED_PARAMETER(old_id);
	qsv_migrate_v1_settings(settings);
}

void qsv_register_encoder_aliases(void)
{
	obs_register_encoder_alias("obs_qsv11", QSV_ID_H264, migrate_v1, NULL);
	obs_register_encoder_alias("obs_qsv11_soft", QSV_ID_H264, migrate_v1, NULL);
	obs_register_encoder_alias("obs_qsv11_v2", QSV_ID_H264, NULL, NULL);
	obs_register_encoder_alias("obs_qsv11_soft_v2", QSV_ID_H264, NULL, NULL);

	obs_register_encoder_alias("obs_qsv11_av1", QSV_ID_AV1, NULL, NULL);
	obs_register_encoder_alias("obs_qsv11_av1_soft", QSV_ID_AV1, NULL, NULL);

#if ENABLE_HEVC
	obs_register_encoder_alias("obs_qsv11_hevc", QSV_ID_HEVC, NULL, NULL);
	obs_register_encoder_alias("obs_qsv11_hevc_soft", QSV_ID_HEVC, NULL, NULL);
#endif
}
