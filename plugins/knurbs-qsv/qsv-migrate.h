#pragma once

#include <obs-data.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Encoder IDs. One public ID per codec: the encoder picks texture or system
 * memory input itself and reroutes to the internal ".raw" ID when it can't
 * use textures. */
#define QSV_ID_H264 "com.xaymar.knurbs.qsv.h264"
#define QSV_ID_HEVC "com.xaymar.knurbs.qsv.hevc"
#define QSV_ID_AV1 "com.xaymar.knurbs.qsv.av1"
#define QSV_ID_H264_RAW QSV_ID_H264 ".raw"
#define QSV_ID_HEVC_RAW QSV_ID_HEVC ".raw"
#define QSV_ID_AV1_RAW QSV_ID_AV1 ".raw"

/* Converts settings saved by the deprecated v1 H.264 encoder (obs_qsv11,
 * obs_qsv11_soft) to the current encoder's settings.
 *
 * v1 had separate I, P and B frame quantizers (qpi, qpp, qpb) for CQP; the
 * current encoder has one (cqp). The P frame value carries over, since most
 * frames are P frames. Both used the same default, so untouched settings stay
 * untouched. */
void qsv_migrate_v1_settings(obs_data_t *settings);

/* Registers the old OBS encoder IDs as aliases of the current ones. */
void qsv_register_encoder_aliases(void);

#ifdef __cplusplus
}
#endif
