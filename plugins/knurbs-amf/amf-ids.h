#pragma once

/* Encoder IDs. One public ID per codec: the encoder takes textures and
 * reroutes to the internal ".raw" system memory variant when it can't. */
#define AMF_ID_H264 "com.xaymar.knurbs.amf.h264"
#define AMF_ID_HEVC "com.xaymar.knurbs.amf.hevc"
#define AMF_ID_AV1 "com.xaymar.knurbs.amf.av1"
#define AMF_ID_H264_RAW AMF_ID_H264 ".raw"
#define AMF_ID_HEVC_RAW AMF_ID_HEVC ".raw"
#define AMF_ID_AV1_RAW AMF_ID_AV1 ".raw"
