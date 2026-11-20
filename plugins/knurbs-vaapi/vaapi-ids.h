#pragma once

/* Encoder IDs. One public ID per codec: the encoder takes textures and
 * reroutes to the internal ".raw" system memory variant when it can't. */
#define VAAPI_ID_H264 "com.xaymar.knurbs.vaapi.h264"
#define VAAPI_ID_HEVC "com.xaymar.knurbs.vaapi.hevc"
#define VAAPI_ID_AV1 "com.xaymar.knurbs.vaapi.av1"
#define VAAPI_ID_H264_RAW VAAPI_ID_H264 ".raw"
#define VAAPI_ID_HEVC_RAW VAAPI_ID_HEVC ".raw"
#define VAAPI_ID_AV1_RAW VAAPI_ID_AV1 ".raw"
