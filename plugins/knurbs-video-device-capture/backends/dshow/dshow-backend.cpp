/*
 * DirectShow backend: cameras and capture cards on Windows.
 *
 * Ported from win-dshow's video capture source. The capture core (the
 * DirectShow thread, format matching, decoding of MJPEG/H.264/HEVC and
 * encoded audio) is unchanged; settings and properties follow the common
 * video device keys.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 2 of the License, or (at your option)
 * any later version.
 */
#include <objbase.h>

#include <obs-module.h>
#include <obs.hpp>
#include <util/dstr.hpp>
#include <util/platform.h>
#include <util/windows/WinHandle.hpp>
#include <util/threading.h>
#include <dshowcapture.hpp>

#include "video-device.h"
#include "dshow-formats.h"
#include "ffmpeg-decode.h"
#include "encode-dstr.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#undef min
#undef max

using namespace std;
using namespace DShow;

/* The saved "pixel-format" numbers are DShow::VideoFormat. */
static_assert((int)VideoFormat::ARGB == 100 && (int)VideoFormat::XRGB == 101, "dshow-formats.h is out of date");
static_assert((int)VideoFormat::I420 == 200 && (int)VideoFormat::P010 == 204, "dshow-formats.h is out of date");
static_assert((int)VideoFormat::YVYU == 300 && (int)VideoFormat::HDYC == 303, "dshow-formats.h is out of date");
static_assert((int)VideoFormat::MJPEG == 400 && (int)VideoFormat::HEVC == 402, "dshow-formats.h is out of date");

/* "dshow." settings */
#define S_ACTIVE "dshow.active"
#define S_AUTOROTATION "dshow.autorotation"
#define S_HW_DECODE "dshow.hw-decode"
#define S_AUDIO_OUTPUT_MODE "dshow.audio-output-mode"
#define S_USE_CUSTOM_AUDIO "dshow.use-custom-audio-device"
#define S_AUDIO_DEVICE_ID "dshow.audio-device-id"

/* Buttons, not settings */
#define B_ACTIVATE "dshow.activate"
#define B_CONFIG_VIDEO "dshow.configure-video"
#define B_CONFIG_XBAR "dshow.configure-crossbar"

#define FPS_HIGHEST 0LL
#define FPS_MATCHING -1LL

/* "frame-rate" as a DirectShow frame interval: FPS_HIGHEST for empty or
 * invalid, FPS_MATCHING for "match". */
static long long FrameRateToInterval(const char *rate)
{
	if (!rate || !*rate) {
		return FPS_HIGHEST;
	}
	if (strcmp(rate, VD_FRAME_RATE_MATCH) == 0) {
		return FPS_MATCHING;
	}
	long long num = 0, den = 0;
	if (sscanf(rate, "%lld/%lld", &num, &den) != 2 || num <= 0 || den <= 0) {
		return FPS_HIGHEST;
	}
	return vd_dshow_interval(num, den);
}

static long long GetOBSFPS()
{
	obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		return 0;
	}
	return vd_dshow_interval(ovi.fps_num, ovi.fps_den);
}

static DStr GetVideoFormatName(VideoFormat format)
{
	DStr name;
	if (format == VideoFormat::Any) {
		dstr_copy(name, "Any");
		return name;
	}
	for (const vd_dshow_format &f : vd_dshow_formats) {
		if (f.value == (int)format) {
			dstr_copy(name, f.label);
			return name;
		}
	}
	dstr_printf(name, "Unknown (%d)", (int)format);
	return name;
}

static void ffmpeg_log(void *bla, int level, const char *msg, va_list args)
{
	DStr str;
	if (level == AV_LOG_WARNING) {
		dstr_copy(str, "warning: ");
	} else if (level == AV_LOG_ERROR) {
		/* only print first of this message to avoid spam */
		static bool suppress_app_field_spam = false;
		if (strcmp(msg, "unable to decode APP fields: %s\n") == 0) {
			if (suppress_app_field_spam) {
				return;
			}

			suppress_app_field_spam = true;
		}

		dstr_copy(str, "error:   ");
	} else if (level < AV_LOG_ERROR) {
		dstr_copy(str, "fatal:   ");
	} else {
		return;
	}

	dstr_cat(str, msg);
	if (dstr_end(str) == '\n') {
		dstr_resize(str, str->len - 1);
	}

	blogva(LOG_WARNING, str, args);
	av_log_default_callback(bla, level, msg, args);
}

class Decoder {
	struct ffmpeg_decode decode;

public:
	inline Decoder() { memset(&decode, 0, sizeof(decode)); }
	inline ~Decoder() { ffmpeg_decode_free(&decode); }

	inline operator ffmpeg_decode *() { return &decode; }
	inline ffmpeg_decode *operator->() { return &decode; }
};

class CriticalSection {
	CRITICAL_SECTION mutex;

public:
	inline CriticalSection() { InitializeCriticalSection(&mutex); }
	inline ~CriticalSection() { DeleteCriticalSection(&mutex); }

	inline operator CRITICAL_SECTION *() { return &mutex; }
};

class CriticalScope {
	CriticalSection &mutex;

	CriticalScope() = delete;
	CriticalScope &operator=(CriticalScope &cs) = delete;

public:
	inline CriticalScope(CriticalSection &mutex_) : mutex(mutex_) { EnterCriticalSection(mutex); }

	inline ~CriticalScope() { LeaveCriticalSection(mutex); }
};

enum class Action {
	None,
	Activate,
	ActivateBlock,
	Deactivate,
	Shutdown,
	ConfigVideo,
	ConfigAudio,
	ConfigCrossbar1,
	ConfigCrossbar2,
};

static DWORD CALLBACK DShowThread(LPVOID ptr);

static BOOL CALLBACK CloseDShowDialogs(HWND window, LPARAM)
{
	if (IsWindowVisible(window)) {
		PostMessage(window, WM_CLOSE, 0, 0);
	}
	return TRUE;
}

struct DShowInput {
	obs_source_t *source;
	Device device;
	/* Paused by the source being hidden with "deactivate when not
	 * showing" on. */
	bool hidden = false;
	bool deviceHasAudio = false;
	bool deviceHasSeparateAudioFilter = false;
	bool flip = false;
	bool active = false;
	bool autorotation = true;
	bool hw_decode = false;

	Decoder audio_decoder;
	Decoder video_decoder;

	VideoConfig videoConfig;
	AudioConfig audioConfig;

	enum video_colorspace cs;
	obs_source_frame2 frame;
	obs_source_audio audio;
	long lastRotation = 0;

	WinHandle semaphore;
	WinHandle activated_event;
	WinHandle thread;
	CriticalSection mutex;
	vector<Action> actions;

	inline void QueueAction(Action action)
	{
		CriticalScope scope(mutex);
		actions.push_back(action);
		ReleaseSemaphore(semaphore, 1, nullptr);
	}

	inline void QueueActivate(obs_data_t *settings)
	{
		bool block = obs_data_get_bool(settings, "synchronous_activate");
		QueueAction(block ? Action::ActivateBlock : Action::Activate);
		if (block) {
			obs_data_erase(settings, "synchronous_activate");
			WaitForSingleObject(activated_event, INFINITE);
		}
	}

	inline DShowInput(obs_source_t *source_, obs_data_t *settings) : source(source_), device(InitGraph::False)
	{
		memset(&audio, 0, sizeof(audio));
		memset(&frame, 0, sizeof(frame));

		av_log_set_level(AV_LOG_WARNING);
		av_log_set_callback(ffmpeg_log);

		semaphore = CreateSemaphore(nullptr, 0, 0x7FFFFFFF, nullptr);
		if (!semaphore) {
			throw "Failed to create semaphore";
		}

		activated_event = CreateEvent(nullptr, false, false, nullptr);
		if (!activated_event) {
			throw "Failed to create activated_event";
		}

		thread = CreateThread(nullptr, 0, DShowThread, this, 0, nullptr);
		if (!thread) {
			throw "Failed to create thread";
		}

		/* Sources start out hidden: with "deactivate when not showing"
		 * on, wait for the first show. */
		hidden = obs_data_get_bool(settings, VD_DEACTIVATE_WHEN_HIDDEN) && !obs_source_showing(source);

		if (obs_data_get_bool(settings, S_ACTIVE)) {
			if (!hidden) {
				QueueActivate(settings);
			}
			active = true;
		}
	}

	inline ~DShowInput()
	{
		{
			CriticalScope scope(mutex);
			actions.resize(1);
			actions[0] = Action::Shutdown;
		}

		ReleaseSemaphore(semaphore, 1, nullptr);
		EnumThreadWindows(GetThreadId(thread), CloseDShowDialogs, 0);

		WaitForSingleObject(thread, INFINITE);
	}

	void OnEncodedVideoData(enum AVCodecID id, unsigned char *data, size_t size, long long ts);
	void OnEncodedAudioData(enum AVCodecID id, unsigned char *data, size_t size, long long ts);

	void OnReactivate();
	void OnVideoData(const VideoConfig &config, unsigned char *data, size_t size, long long startTime,
			 long long endTime, long rotation);
	void OnAudioData(const AudioConfig &config, unsigned char *data, size_t size, long long startTime,
			 long long endTime);

	bool UpdateVideoConfig(obs_data_t *settings);
	bool UpdateAudioConfig(obs_data_t *settings);
	void SetActive(bool active);
	inline enum video_colorspace GetColorSpace(obs_data_t *settings) const;
	inline enum video_range_type GetColorRange(obs_data_t *settings) const;
	inline bool Activate(obs_data_t *settings);
	inline void Deactivate();

	inline void SetupBuffering(obs_data_t *settings);

	void DShowLoop();
};

static DWORD CALLBACK DShowThread(LPVOID ptr)
{
	DShowInput *dshowInput = (DShowInput *)ptr;

	os_set_thread_name("video-device: dshow");

	CoInitialize(nullptr);
	dshowInput->DShowLoop();
	CoUninitialize();
	return 0;
}

static inline void ProcessMessages()
{
	MSG msg;
	while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
}

/* Always keep directshow in a single thread for a given device */
void DShowInput::DShowLoop()
{
	while (true) {
		DWORD ret = MsgWaitForMultipleObjects(1, &semaphore, false, INFINITE, QS_ALLINPUT);
		if (ret == (WAIT_OBJECT_0 + 1)) {
			ProcessMessages();
			continue;
		} else if (ret != WAIT_OBJECT_0) {
			break;
		}

		Action action = Action::None;
		{
			CriticalScope scope(mutex);
			if (actions.size()) {
				action = actions.front();
				actions.erase(actions.begin());
			}
		}

		switch (action) {
		case Action::Activate:
		case Action::ActivateBlock: {
			bool block = action == Action::ActivateBlock;

			obs_data_t *settings;
			settings = obs_source_get_settings(source);
			if (!Activate(settings)) {
				obs_source_output_video2(source, nullptr);
			}
			if (block) {
				SetEvent(activated_event);
			}
			obs_data_release(settings);
			break;
		}

		case Action::Deactivate:
			Deactivate();
			break;

		case Action::Shutdown:
			device.ShutdownGraph();
			return;

		case Action::ConfigVideo:
			device.OpenDialog(nullptr, DialogType::ConfigVideo);
			break;

		case Action::ConfigAudio:
			device.OpenDialog(nullptr, DialogType::ConfigAudio);
			break;

		case Action::ConfigCrossbar1:
			device.OpenDialog(nullptr, DialogType::ConfigCrossbar);
			break;

		case Action::ConfigCrossbar2:
			device.OpenDialog(nullptr, DialogType::ConfigCrossbar2);
			break;

		case Action::None:;
		}
	}
}

template<typename T, typename U, typename V> static bool between(T &&lower, U &&value, V &&upper)
{
	return value >= lower && value <= upper;
}

static bool ResolutionAvailable(const VideoInfo &cap, int cx, int cy)
{
	return between(cap.minCX, cx, cap.maxCX) && between(cap.minCY, cy, cap.maxCY);
}

#define DEVICE_INTERVAL_DIFF_LIMIT VD_DSHOW_INTERVAL_TOLERANCE

static bool FrameRateAvailable(const VideoInfo &cap, long long interval)
{
	return interval == FPS_HIGHEST || interval == FPS_MATCHING ||
	       between(cap.minInterval - DEVICE_INTERVAL_DIFF_LIMIT, interval,
		       cap.maxInterval + DEVICE_INTERVAL_DIFF_LIMIT);
}

static long long FrameRateInterval(const VideoInfo &cap, long long desired_interval)
{
	return desired_interval < cap.minInterval ? cap.minInterval : min(desired_interval, cap.maxInterval);
}

static inline video_format ConvertVideoFormat(VideoFormat format)
{
	switch (format) {
	case VideoFormat::ARGB:
		return VIDEO_FORMAT_BGRA;
	case VideoFormat::XRGB:
		return VIDEO_FORMAT_BGRX;
	case VideoFormat::I420:
		return VIDEO_FORMAT_I420;
	case VideoFormat::YV12:
		return VIDEO_FORMAT_I420;
	case VideoFormat::NV12:
		return VIDEO_FORMAT_NV12;
	case VideoFormat::Y800:
		return VIDEO_FORMAT_Y800;
	case VideoFormat::YVYU:
		return VIDEO_FORMAT_YVYU;
	case VideoFormat::YUY2:
		return VIDEO_FORMAT_YUY2;
	case VideoFormat::UYVY:
		return VIDEO_FORMAT_UYVY;
	case VideoFormat::HDYC:
		return VIDEO_FORMAT_UYVY;
	case VideoFormat::P010:
		return VIDEO_FORMAT_P010;
	default:
		return VIDEO_FORMAT_NONE;
	}
}

static inline audio_format ConvertAudioFormat(AudioFormat format)
{
	switch (format) {
	case AudioFormat::Wave16bit:
		return AUDIO_FORMAT_16BIT;
	case AudioFormat::WaveFloat:
		return AUDIO_FORMAT_FLOAT;
	default:
		return AUDIO_FORMAT_UNKNOWN;
	}
}

static inline enum speaker_layout convert_speaker_layout(uint8_t channels)
{
	switch (channels) {
	case 0:
		return SPEAKERS_UNKNOWN;
	case 1:
		return SPEAKERS_MONO;
	case 2:
		return SPEAKERS_STEREO;
	case 3:
		return SPEAKERS_2POINT1;
	case 4:
		return SPEAKERS_4POINT0;
	case 5:
		return SPEAKERS_4POINT1;
	case 6:
		return SPEAKERS_5POINT1;
	case 8:
		return SPEAKERS_7POINT1;
	default:
		return SPEAKERS_UNKNOWN;
	}
}

//#define LOG_ENCODED_VIDEO_TS 1
//#define LOG_ENCODED_AUDIO_TS 1

void DShowInput::OnEncodedVideoData(enum AVCodecID id, unsigned char *data, size_t size, long long ts)
{
	/* If format or hw decode changes, recreate the decoder */
	if (ffmpeg_decode_valid(video_decoder) &&
	    ((video_decoder->codec->id != id) || (video_decoder->hw != hw_decode))) {
		ffmpeg_decode_free(video_decoder);
	}

	if (!ffmpeg_decode_valid(video_decoder)) {
		if (ffmpeg_decode_init(video_decoder, id, hw_decode) < 0) {
			blog(LOG_WARNING, "%s: Could not initialize video decoder", obs_source_get_name(source));
			return;
		}
	}

	bool got_output;
	bool success = ffmpeg_decode_video(video_decoder, data, size, &ts, cs, frame.range, &frame, &got_output);
	if (!success) {
		blog(LOG_WARNING, "%s: Error decoding video", obs_source_get_name(source));
		return;
	}

	if (got_output) {
		frame.timestamp = (uint64_t)ts * 100;
		if (flip) {
			frame.flip = !frame.flip;
		}
#if LOG_ENCODED_VIDEO_TS
		blog(LOG_DEBUG, "video ts: %llu", frame.timestamp);
#endif
		obs_source_output_video2(source, &frame);
	}
}

void DShowInput::OnReactivate()
{
	SetActive(true);
}

void DShowInput::OnVideoData(const VideoConfig &config, unsigned char *data, size_t size, long long startTime,
			     long long endTime, long rotation)
{
	if (autorotation && rotation != lastRotation) {
		lastRotation = rotation;
		obs_source_set_async_rotation(source, rotation);
	}

	if (videoConfig.format == VideoFormat::H264) {
		OnEncodedVideoData(AV_CODEC_ID_H264, data, size, startTime);
		return;
	}

#ifdef ENABLE_HEVC
	if (videoConfig.format == VideoFormat::HEVC) {
		OnEncodedVideoData(AV_CODEC_ID_HEVC, data, size, startTime);
		return;
	}
#endif

	if (videoConfig.format == VideoFormat::MJPEG) {
		OnEncodedVideoData(AV_CODEC_ID_MJPEG, data, size, startTime);
		return;
	}

	const int cx = config.cx;
	const int cy_abs = config.cy_abs;

	frame.timestamp = (uint64_t)startTime * 100;
	frame.width = config.cx;
	frame.height = cy_abs;
	frame.format = ConvertVideoFormat(config.format);
	frame.flip = flip;
	frame.flags = OBS_SOURCE_FRAME_LINEAR_ALPHA;

	/* YUV DIBS are always top-down */
	if (config.format == VideoFormat::XRGB || config.format == VideoFormat::ARGB) {
		/* RGB DIBs are bottom-up by default */
		if (!config.cy_flip) {
			frame.flip = !frame.flip;
		}
	}

	if (videoConfig.format == VideoFormat::XRGB || videoConfig.format == VideoFormat::ARGB) {
		frame.data[0] = data;
		frame.linesize[0] = cx * 4;

	} else if (videoConfig.format == VideoFormat::YVYU || videoConfig.format == VideoFormat::YUY2 ||
		   videoConfig.format == VideoFormat::HDYC || videoConfig.format == VideoFormat::UYVY) {
		frame.data[0] = data;
		frame.linesize[0] = cx * 2;

	} else if (videoConfig.format == VideoFormat::I420) {
		frame.data[0] = data;
		frame.data[1] = frame.data[0] + (cx * cy_abs);
		frame.data[2] = frame.data[1] + (cx * cy_abs / 4);
		frame.linesize[0] = cx;
		frame.linesize[1] = cx / 2;
		frame.linesize[2] = cx / 2;

	} else if (videoConfig.format == VideoFormat::YV12) {
		frame.data[0] = data;
		frame.data[2] = frame.data[0] + (cx * cy_abs);
		frame.data[1] = frame.data[2] + (cx * cy_abs / 4);
		frame.linesize[0] = cx;
		frame.linesize[1] = cx / 2;
		frame.linesize[2] = cx / 2;

	} else if (videoConfig.format == VideoFormat::NV12) {
		frame.data[0] = data;
		frame.data[1] = frame.data[0] + (cx * cy_abs);
		frame.linesize[0] = cx;
		frame.linesize[1] = cx;

	} else if (videoConfig.format == VideoFormat::Y800) {
		frame.data[0] = data;
		frame.linesize[0] = cx;

	} else if (videoConfig.format == VideoFormat::P010) {
		frame.data[0] = data;
		frame.data[1] = frame.data[0] + (cx * cy_abs) * 2;
		frame.linesize[0] = cx * 2;
		frame.linesize[1] = cx * 2;

	} else {
		/* TODO: other formats */
		return;
	}

	obs_source_output_video2(source, &frame);

	UNUSED_PARAMETER(endTime); /* it's the enndd tiimmes! */
}

void DShowInput::OnEncodedAudioData(enum AVCodecID id, unsigned char *data, size_t size, long long ts)
{
	if (!ffmpeg_decode_valid(audio_decoder)) {
		if (ffmpeg_decode_init(audio_decoder, id, false) < 0) {
			blog(LOG_WARNING, "%s: Could not initialize audio decoder", obs_source_get_name(source));
			return;
		}
	}

	bool got_output = false;
	do {
		bool success = ffmpeg_decode_audio(audio_decoder, data, size, &audio, &got_output);
		if (!success) {
			blog(LOG_WARNING, "%s: Error decoding audio", obs_source_get_name(source));
			return;
		}

		if (got_output) {
			audio.timestamp = (uint64_t)ts * 100;
#if LOG_ENCODED_AUDIO_TS
			blog(LOG_DEBUG, "audio ts: %llu", audio.timestamp);
#endif
			obs_source_output_audio(source, &audio);
		} else {
			break;
		}

		ts += int64_t(audio_decoder->frame->nb_samples) * 10000000LL /
		      int64_t(audio_decoder->frame->sample_rate);
		size = 0;
		data = nullptr;
	} while (got_output);
}

void DShowInput::OnAudioData(const AudioConfig &config, unsigned char *data, size_t size, long long startTime,
			     long long endTime)
{
	size_t block_size;

	if (config.format == AudioFormat::AAC) {
		OnEncodedAudioData(AV_CODEC_ID_AAC, data, size, startTime);
		return;
	} else if (config.format == AudioFormat::AC3) {
		OnEncodedAudioData(AV_CODEC_ID_AC3, data, size, startTime);
		return;
	} else if (config.format == AudioFormat::MPGA) {
		OnEncodedAudioData(AV_CODEC_ID_MP2, data, size, startTime);
		return;
	}

	audio.speakers = convert_speaker_layout((uint8_t)config.channels);
	audio.format = ConvertAudioFormat(config.format);
	audio.samples_per_sec = (uint32_t)config.sampleRate;
	audio.data[0] = data;

	block_size = get_audio_bytes_per_channel(audio.format) * get_audio_channels(audio.speakers);

	audio.frames = (uint32_t)(size / block_size);
	audio.timestamp = (uint64_t)startTime * 100;

	if (audio.format != AUDIO_FORMAT_UNKNOWN) {
		obs_source_output_audio(source, &audio);
	}

	UNUSED_PARAMETER(endTime);
}

struct DeviceList {
	vector<VideoDevice> devices;

	bool GetDevice(VideoDevice &device, const char *encoded_id) const
	{
		DeviceId deviceId;
		DecodeDeviceId(deviceId, encoded_id);

		for (const VideoDevice &curDevice : devices) {
			if (deviceId.name == curDevice.name && deviceId.path == curDevice.path) {
				device = curDevice;
				return true;
			}
		}

		return false;
	}
};

static inline bool ConvertRes(int &cx, int &cy, const char *res)
{
	return sscanf(res, "%dx%d", &cx, &cy) == 2;
}

static inline bool FormatMatches(VideoFormat left, VideoFormat right)
{
	return left == VideoFormat::Any || right == VideoFormat::Any || left == right;
}

static inline bool ResolutionValid(const string &res, int &cx, int &cy)
{
	if (!res.size()) {
		return false;
	}

	return ConvertRes(cx, cy, res.c_str());
}

static inline bool CapsMatch(const VideoInfo &)
{
	return true;
}

template<typename... F> static bool CapsMatch(const VideoDevice &dev, F... fs);

template<typename F, typename... Fs> static inline bool CapsMatch(const VideoInfo &info, F &&f, Fs... fs)
{
	return f(info) && CapsMatch(info, fs...);
}

template<typename... F> static bool CapsMatch(const VideoDevice &dev, F... fs)
{
	// no early exit, trigger all side effects.
	bool match = false;
	for (const VideoInfo &info : dev.caps) {
		if (CapsMatch(info, fs...)) {
			match = true;
		}
	}
	return match;
}

static inline bool MatcherMatchVideoFormat(VideoFormat format, bool &did_match, const VideoInfo &info)
{
	bool match = FormatMatches(format, info.format);
	did_match = did_match || match;
	return match;
}

static inline bool MatcherClosestFrameRateSelector(long long interval, long long &best_match, const VideoInfo &info)
{
	long long current = FrameRateInterval(info, interval);
	if (llabs(interval - best_match) > llabs(interval - current)) {
		best_match = current;
	}
	return true;
}

#if 0
auto ResolutionMatcher = [](int cx, int cy)
{
	return [cx, cy](const VideoInfo &info)
	{
		return ResolutionAvailable(info, cx, cy);
	};
};

auto FrameRateMatcher = [](long long interval)
{
	return [interval](const VideoInfo &info)
	{
		return FrameRateAvailable(info, interval);
	};
};

auto VideoFormatMatcher = [](VideoFormat format, bool &did_match)
{
	return [format, &did_match](const VideoInfo &info)
	{
		return MatcherMatchVideoFormat(format, did_match, info);
	};
};

auto ClosestFrameRateSelector = [](long long interval, long long &best_match)
{
	return [interval, &best_match](const VideoInfo &info) mutable -> bool
	{
		MatcherClosestFrameRateSelector(interval, best_match, info);
	};
}
#else
#define ResolutionMatcher(cx, cy)                         \
	[cx, cy](const VideoInfo &info) -> bool {         \
		return ResolutionAvailable(info, cx, cy); \
	}
#define FrameRateMatcher(interval)                         \
	[interval](const VideoInfo &info) -> bool {        \
		return FrameRateAvailable(info, interval); \
	}
#define VideoFormatMatcher(format, did_match)                            \
	[format, &did_match](const VideoInfo &info) mutable -> bool {    \
		return MatcherMatchVideoFormat(format, did_match, info); \
	}
#define ClosestFrameRateSelector(interval, best_match)                              \
	[interval, &best_match](const VideoInfo &info) mutable -> bool {            \
		return MatcherClosestFrameRateSelector(interval, best_match, info); \
	}
#endif

static inline bool IsDelayedDevice(const VideoConfig &config)
{
	return config.format > VideoFormat::MJPEG ||
	       wstrstri(config.name.c_str(), L"elgato game capture hd") != nullptr ||
	       wstrstri(config.name.c_str(), L"stream engine") != nullptr;
}

static inline bool IsDecoupled(const VideoConfig &config)
{
	return wstrstri(config.name.c_str(), L"GV-USB2") != NULL;
}

inline void DShowInput::SetupBuffering(obs_data_t *settings)
{
	const char *buffering = obs_data_get_string(settings, VD_BUFFERING);
	bool useBuffering;

	if (strcmp(buffering, "on") == 0) {
		useBuffering = true;
	} else if (strcmp(buffering, "off") == 0) {
		useBuffering = false;
	} else {
		useBuffering = IsDelayedDevice(videoConfig);
	}

	obs_source_set_async_unbuffered(source, !useBuffering);
	obs_source_set_async_decoupled(source, IsDecoupled(videoConfig));
}

bool DShowInput::UpdateVideoConfig(obs_data_t *settings)
{
	string video_device_id = obs_data_get_string(settings, VD_DEVICE_ID);
	flip = obs_data_get_bool(settings, VD_FLIP_VERTICAL);
	autorotation = obs_data_get_bool(settings, S_AUTOROTATION);
	hw_decode = obs_data_get_bool(settings, S_HW_DECODE);

	DeviceId id;
	if (!DecodeDeviceId(id, video_device_id.c_str())) {
		blog(LOG_WARNING, "%s: DecodeDeviceId failed", obs_source_get_name(source));
		return false;
	}

	DeviceList data;
	Device::EnumVideoDevices(data.devices);
	VideoDevice dev;
	if (!data.GetDevice(dev, video_device_id.c_str())) {
		blog(LOG_WARNING, "%s: data.GetDevice failed", obs_source_get_name(source));
		return false;
	}

	/* An empty resolution is the device's own default configuration;
	 * frame rate and format only apply with a resolution. */
	const char *resolution = obs_data_get_string(settings, VD_RESOLUTION);
	bool useDefault = !resolution || !*resolution;
	int cx = 0, cy = 0;
	long long interval = 0;
	VideoFormat format = VideoFormat::Any;

	if (!useDefault) {
		if (!ConvertRes(cx, cy, resolution)) {
			blog(LOG_WARNING, "%s: invalid resolution '%s'", obs_source_get_name(source), resolution);
			return false;
		}

		interval = FrameRateToInterval(obs_data_get_string(settings, VD_FRAME_RATE));
		if (interval == FPS_MATCHING) {
			interval = GetOBSFPS();
		}

		format = (VideoFormat)vd_dshow_format_value(obs_data_get_string(settings, VD_PIXEL_FORMAT));

		long long best_interval = numeric_limits<long long>::max();
		bool video_format_match = false;
		bool caps_match =
			CapsMatch(dev, ResolutionMatcher(cx, cy), VideoFormatMatcher(format, video_format_match),
				  ClosestFrameRateSelector(interval, best_interval), FrameRateMatcher(interval));

		if (!caps_match && !video_format_match) {
			blog(LOG_WARNING, "%s: Video format match failed", obs_source_get_name(source));
			return false;
		}

		interval = best_interval;
	}

	videoConfig.name = id.name;
	videoConfig.path = id.path;
	videoConfig.useDefaultConfig = useDefault;
	videoConfig.cx = cx;
	videoConfig.cy_abs = abs(cy);
	videoConfig.cy_flip = cy < 0;
	videoConfig.frameInterval = interval;
	videoConfig.internalFormat = format;

	deviceHasAudio = dev.audioAttached;
	deviceHasSeparateAudioFilter = dev.separateAudioFilter;

	videoConfig.callback = std::bind(&DShowInput::OnVideoData, this, placeholders::_1, placeholders::_2,
					 placeholders::_3, placeholders::_4, placeholders::_5, placeholders::_6);
	videoConfig.reactivateCallback = std::bind(&DShowInput::OnReactivate, this);

	videoConfig.format = videoConfig.internalFormat;

	if (!device.SetVideoConfig(&videoConfig)) {
		blog(LOG_WARNING, "%s: device.SetVideoConfig failed", obs_source_get_name(source));
		return false;
	}

	if (!videoConfig.cx || !videoConfig.cy_abs) {
		blog(LOG_ERROR, "%s: Frame width or height are zero (%" PRIu32 "x%" PRIu32 ")",
		     obs_source_get_name(source), videoConfig.cx, videoConfig.cy_abs);
		return false;
	}

	DStr formatName = GetVideoFormatName(videoConfig.internalFormat);

	double fps = 0.0;

	if (videoConfig.frameInterval) {
		fps = 10000000.0 / double(videoConfig.frameInterval);
	}

	BPtr<char> name_utf8;
	BPtr<char> path_utf8;
	os_wcs_to_utf8_ptr(videoConfig.name.c_str(), videoConfig.name.size(), &name_utf8);
	os_wcs_to_utf8_ptr(videoConfig.path.c_str(), videoConfig.path.size(), &path_utf8);

	SetupBuffering(settings);

	blog(LOG_INFO, "---------------------------------");
	blog(LOG_INFO,
	     "[DShow Device: '%s'] settings updated: \n"
	     "\tvideo device: %s\n"
	     "\tvideo path: %s\n"
	     "\tresolution: %dx%d\n"
	     "\tflip: %d\n"
	     "\tfps: %0.2f (interval: %lld)\n"
	     "\tformat: %s\n"
	     "\tbuffering: %s\n"
	     "\thardware decode: %s",
	     obs_source_get_name(source), (const char *)name_utf8, (const char *)path_utf8, videoConfig.cx,
	     videoConfig.cy_abs, (int)videoConfig.cy_flip, fps, videoConfig.frameInterval, formatName->array,
	     obs_source_async_unbuffered(source) ? "disabled" : "enabled", hw_decode ? "enabled" : "disabled");

	return true;
}

bool DShowInput::UpdateAudioConfig(obs_data_t *settings)
{
	if (!obs_data_get_bool(settings, VD_CAPTURE_AUDIO)) {
		obs_source_set_audio_active(source, false);
		return true;
	}

	string audio_device_id = obs_data_get_string(settings, S_AUDIO_DEVICE_ID);
	bool useCustomAudio = obs_data_get_bool(settings, S_USE_CUSTOM_AUDIO);

	if (useCustomAudio) {
		DeviceId id;
		if (!DecodeDeviceId(id, audio_device_id.c_str())) {
			obs_source_set_audio_active(source, false);
			return false;
		}

		audioConfig.name = id.name;
		audioConfig.path = id.path;

	} else if (!deviceHasAudio) {
		obs_source_set_audio_active(source, false);
		return true;
	}

	audioConfig.useVideoDevice = !useCustomAudio && !deviceHasSeparateAudioFilter;
	audioConfig.useSeparateAudioFilter = deviceHasSeparateAudioFilter;

	audioConfig.callback = std::bind(&DShowInput::OnAudioData, this, placeholders::_1, placeholders::_2,
					 placeholders::_3, placeholders::_4, placeholders::_5);

	audioConfig.mode = (AudioMode)obs_data_get_int(settings, S_AUDIO_OUTPUT_MODE);

	bool success = device.SetAudioConfig(&audioConfig);
	if (!success) {
		obs_source_set_audio_active(source, false);
		return false;
	}

	obs_source_set_audio_active(source, true);

	BPtr<char> name_utf8;
	os_wcs_to_utf8_ptr(audioConfig.name.c_str(), audioConfig.name.size(), &name_utf8);

	blog(LOG_INFO, "\tusing video device audio: %s", audioConfig.useVideoDevice ? "yes" : "no");

	if (!audioConfig.useVideoDevice) {
		if (audioConfig.useSeparateAudioFilter) {
			blog(LOG_INFO, "\tseparate audio filter");
		} else {
			blog(LOG_INFO, "\taudio device: %s", (const char *)name_utf8);
		}
	}

	const char *mode = "";

	switch (audioConfig.mode) {
	case AudioMode::Capture:
		mode = "Capture";
		break;
	case AudioMode::DirectSound:
		mode = "DirectSound";
		break;
	case AudioMode::WaveOut:
		mode = "WaveOut";
		break;
	}

	blog(LOG_INFO,
	     "\tsample rate: %d\n"
	     "\tchannels: %d\n"
	     "\taudio type: %s",
	     audioConfig.sampleRate, audioConfig.channels, mode);
	return true;
}

void DShowInput::SetActive(bool active_)
{
	obs_data_t *settings = obs_source_get_settings(source);
	/* While hidden the device stays closed; showing opens it. */
	if (!active_ || !hidden) {
		QueueAction(active_ ? Action::Activate : Action::Deactivate);
	}
	obs_data_set_bool(settings, S_ACTIVE, active_);
	active = active_;
	obs_data_release(settings);
}

inline enum video_colorspace DShowInput::GetColorSpace(obs_data_t *settings) const
{
	const char *space = obs_data_get_string(settings, VD_COLOR_SPACE);

	/* P010 devices are HDR10 unless told otherwise. */
	if (vd_is_auto(space) && videoConfig.format == VideoFormat::P010) {
		return VIDEO_CS_2100_PQ;
	}
	return vd_parse_color_space(space);
}

inline enum video_range_type DShowInput::GetColorRange(obs_data_t *settings) const
{
	return vd_parse_color_range(obs_data_get_string(settings, VD_COLOR_RANGE));
}

inline bool DShowInput::Activate(obs_data_t *settings)
{
	if (!device.ResetGraph()) {
		obs_source_set_audio_active(source, false);
		return false;
	}

	if (!UpdateVideoConfig(settings)) {
		blog(LOG_WARNING, "%s: Video configuration failed", obs_source_get_name(source));
		obs_source_set_audio_active(source, false);
		return false;
	}

	if (!UpdateAudioConfig(settings)) {
		blog(LOG_WARNING,
		     "%s: Audio configuration failed, ignoring "
		     "audio",
		     obs_source_get_name(source));
	}

	if (!device.ConnectFilters()) {
		return false;
	}

	if (device.Start() != Result::Success) {
		return false;
	}

	cs = GetColorSpace(settings);
	const enum video_range_type range = GetColorRange(settings);

	enum video_trc trc = VIDEO_TRC_DEFAULT;
	switch (cs) {
	case VIDEO_CS_DEFAULT:
	case VIDEO_CS_601:
	case VIDEO_CS_709:
	case VIDEO_CS_SRGB:
		trc = VIDEO_TRC_SRGB;
		break;
	case VIDEO_CS_2100_PQ:
		trc = VIDEO_TRC_PQ;
		break;
	case VIDEO_CS_2100_HLG:
		trc = VIDEO_TRC_HLG;
	}

	frame.range = range;
	frame.trc = trc;

	bool success = video_format_get_parameters_for_format(cs, range, ConvertVideoFormat(videoConfig.format),
							      frame.color_matrix, frame.color_range_min,
							      frame.color_range_max);
	if (!success) {
		blog(LOG_ERROR,
		     "Failed to get video format parameters for "
		     "video format %u",
		     cs);
	}

	return true;
}

inline void DShowInput::Deactivate()
{
	device.ResetGraph();
	obs_source_output_video2(source, nullptr);
}

/* ------------------------------------------------------------------------- */
/* Devices                                                                   */

static once_flag init_flag;

static void DShowModuleLogCallback(LogType type, const wchar_t *msg, void *param)
{
	int obs_type = LOG_DEBUG;

	switch (type) {
	case LogType::Error:
		obs_type = LOG_ERROR;
		break;
	case LogType::Warning:
		obs_type = LOG_WARNING;
		break;
	case LogType::Info:
		obs_type = LOG_INFO;
		break;
	case LogType::Debug:
		obs_type = LOG_DEBUG;
		break;
	}

	DStr dmsg;

	dstr_from_wcs(dmsg, msg);
	blog(obs_type, "DShow: %s", dmsg->array);

	UNUSED_PARAMETER(param);
}

static void EnsureInit()
{
	call_once(init_flag, [] { SetLogCallback(DShowModuleLogCallback, nullptr); });
}

/* The devices from the last listing, for the property callbacks: the core
 * lists devices every time it builds the properties. */
static mutex devices_mutex;
static DeviceList listed_devices;

static bool ListedDevice(VideoDevice &device, const char *encoded_id)
{
	lock_guard<mutex> lock(devices_mutex);
	return listed_devices.GetDevice(device, encoded_id);
}

static void DShowEnumDevices(vd_device_cb cb, void *param)
{
	EnsureInit();

	DeviceList list;
	Device::EnumVideoDevices(list.devices);

	for (const VideoDevice &device : list.devices) {
		DStr id, name;
		EncodeDeviceId(id, device.name.c_str(), device.path.c_str());
		dstr_from_wcs(name, device.name.c_str());
		cb(param, id->array, name->array);
	}

	lock_guard<mutex> lock(devices_mutex);
	listed_devices = std::move(list);
}

/* ------------------------------------------------------------------------- */
/* Properties                                                                */

struct Resolution {
	int cx, cy;

	inline Resolution(int cx, int cy) : cx(cx), cy(cy) {}
};

static void InsertResolution(vector<Resolution> &resolutions, int cx, int cy)
{
	int bestCY = 0;
	size_t idx = 0;

	for (; idx < resolutions.size(); idx++) {
		const Resolution &res = resolutions[idx];
		if (res.cx > cx) {
			break;
		}

		if (res.cx == cx) {
			if (res.cy == cy) {
				return;
			}

			if (!bestCY) {
				bestCY = res.cy;
			} else if (res.cy > bestCY) {
				break;
			}
		}
	}

	resolutions.insert(resolutions.begin() + idx, Resolution(cx, cy));
}

static inline void AddCap(vector<Resolution> &resolutions, const VideoInfo &cap)
{
	InsertResolution(resolutions, cap.minCX, cap.minCY);
	InsertResolution(resolutions, cap.maxCX, cap.maxCY);
}

static void FillResolutions(obs_property_t *list, const VideoDevice *device, const char *current)
{
	obs_property_list_clear(list);
	obs_property_list_add_string(list, obs_module_text("resolution.device-default"), "");

	bool current_listed = !current || !*current;
	if (device) {
		vector<Resolution> resolutions;
		for (const VideoInfo &cap : device->caps) {
			AddCap(resolutions, cap);
		}

		for (size_t idx = resolutions.size(); idx > 0; idx--) {
			const Resolution &res = resolutions[idx - 1];
			string str = to_string(res.cx) + "x" + to_string(res.cy);
			obs_property_list_add_string(list, str.c_str(), str.c_str());
			if (!current_listed && str == current) {
				current_listed = true;
			}
		}
	}

	/* The list is editable, so an unlisted saved resolution is kept. */
	if (!current_listed) {
		obs_property_list_add_string(list, current, current);
	}
}

static void FillFrameRates(obs_property_t *list, const VideoDevice *device, bool resolution_ok, int cx, int cy,
			   VideoFormat format, const char *current)
{
	obs_property_list_clear(list);
	obs_property_list_add_string(list, obs_module_text("frame-rate.highest"), "");
	obs_property_list_add_string(list, obs_module_text("frame-rate.match-output"), VD_FRAME_RATE_MATCH);

	bool current_listed = !current || !*current || strcmp(current, VD_FRAME_RATE_MATCH) == 0;
	for (const vd_dshow_frame_rate &rate : vd_dshow_frame_rates) {
		char value[48];
		snprintf(value, sizeof(value), "%lld/%lld", rate.num, rate.den);
		bool selected = !current_listed && strcmp(value, current) == 0;

		long long interval = vd_dshow_interval(rate.num, rate.den);
		bool format_match = false;
		bool available = device && resolution_ok &&
				 CapsMatch(*device, ResolutionMatcher(cx, cy), VideoFormatMatcher(format, format_match),
					   FrameRateMatcher(interval));

		if (!available && !selected) {
			continue;
		}

		size_t idx = obs_property_list_add_string(list, rate.label, value);
		obs_property_list_item_disable(list, idx, !available);
		current_listed = current_listed || selected;
	}

	if (!current_listed) {
		long long num = 0, den = 0;
		char label[32];
		if (sscanf(current, "%lld/%lld", &num, &den) == 2 && num > 0 && den > 0) {
			snprintf(label, sizeof(label), "%.2f", (double)num / (double)den);
		} else {
			snprintf(label, sizeof(label), "%s", current);
		}
		size_t idx = obs_property_list_add_string(list, label, current);
		obs_property_list_item_disable(list, idx, true);
	}
}

static void FillFormats(obs_property_t *list, const VideoDevice *device, bool resolution_ok, int cx, int cy,
			long long interval, const char *current)
{
	set<VideoFormat> formats;
	if (device && resolution_ok) {
		auto format_gatherer = [&formats](const VideoInfo &info) mutable -> bool {
			formats.insert(info.format);
			return false;
		};
		CapsMatch(*device, ResolutionMatcher(cx, cy), FrameRateMatcher(interval), format_gatherer);
	}

	obs_property_list_clear(list);
	obs_property_list_add_string(list, obs_module_text("auto"), "");

	bool current_listed = !current || !*current;
	for (const vd_dshow_format &format : vd_dshow_formats) {
		bool available = formats.count((VideoFormat)format.value) != 0;
		bool selected = !current_listed && strcmp(format.fourcc, current) == 0;
		if (!available && !selected) {
			continue;
		}

		size_t idx = obs_property_list_add_string(list, format.label, format.fourcc);
		obs_property_list_item_disable(list, idx, !available);
		current_listed = current_listed || selected;
	}

	if (!current_listed) {
		size_t idx = obs_property_list_add_string(list, current, current);
		obs_property_list_item_disable(list, idx, true);
	}
}

/* Frame rates and formats depend on the device and resolution, and on each
 * other. */
static void RefreshLists(obs_properties_t *props, obs_data_t *settings, bool resolutions)
{
	VideoDevice device;
	bool have_device = ListedDevice(device, obs_data_get_string(settings, VD_DEVICE_ID));
	const VideoDevice *dev = have_device ? &device : nullptr;

	const char *res = obs_data_get_string(settings, VD_RESOLUTION);
	const char *rate = obs_data_get_string(settings, VD_FRAME_RATE);
	const char *fourcc = obs_data_get_string(settings, VD_PIXEL_FORMAT);

	obs_property_t *res_list = obs_properties_get(props, VD_RESOLUTION);
	obs_property_t *rate_list = obs_properties_get(props, VD_FRAME_RATE);
	obs_property_t *format_list = obs_properties_get(props, VD_PIXEL_FORMAT);

	if (resolutions && res_list) {
		FillResolutions(res_list, dev, res);
	}

	int cx = 0, cy = 0;
	bool custom = res && *res;
	bool resolution_ok = custom && ConvertRes(cx, cy, res);

	long long interval = FrameRateToInterval(rate);
	if (interval == FPS_MATCHING) {
		interval = GetOBSFPS();
	}
	VideoFormat format = (VideoFormat)vd_dshow_format_value(fourcc);

	if (rate_list) {
		FillFrameRates(rate_list, dev, resolution_ok, cx, cy, format, rate);
		obs_property_set_enabled(rate_list, custom);
	}
	if (format_list) {
		FillFormats(format_list, dev, resolution_ok, cx, cy, interval, fourcc);
		obs_property_set_enabled(format_list, custom);
	}
}

static bool DeviceChanged(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	RefreshLists(props, settings, true);
	return true;
}

static bool ModeChanged(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	RefreshLists(props, settings, false);
	return true;
}

static bool CustomAudioClicked(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	obs_property_t *p = obs_properties_get(props, S_AUDIO_DEVICE_ID);
	obs_property_set_visible(p, obs_data_get_bool(settings, S_USE_CUSTOM_AUDIO));
	return true;
}

static bool ActivateClicked(obs_properties_t *, obs_property_t *p, void *data)
{
	DShowInput *input = reinterpret_cast<DShowInput *>(data);

	input->SetActive(!input->active);
	obs_property_set_description(p, obs_module_text(input->active ? "dshow.deactivate" : "dshow.activate"));
	return true;
}

static bool VideoConfigClicked(obs_properties_t *, obs_property_t *, void *data)
{
	reinterpret_cast<DShowInput *>(data)->QueueAction(Action::ConfigVideo);
	return false;
}

static bool CrossbarConfigClicked(obs_properties_t *, obs_property_t *, void *data)
{
	reinterpret_cast<DShowInput *>(data)->QueueAction(Action::ConfigCrossbar1);
	return false;
}

static void DShowAddProperties(void *data, obs_properties_t *props, obs_data_t *settings)
{
	DShowInput *input = reinterpret_cast<DShowInput *>(data);
	obs_property_t *p;

	p = obs_properties_get(props, VD_DEVICE_ID);
	if (p) {
		obs_property_set_modified_callback(p, DeviceChanged);
	}

	/* Device dialogs and the manual switch need a running instance. */
	if (input) {
		const char *text = obs_module_text(input->active ? "dshow.deactivate" : "dshow.activate");
		obs_properties_add_button2(props, B_ACTIVATE, text, ActivateClicked, input);
		obs_properties_add_button2(props, B_CONFIG_VIDEO, obs_module_text("dshow.configure-video"),
					   VideoConfigClicked, input);
		obs_properties_add_button2(props, B_CONFIG_XBAR, obs_module_text("dshow.configure-crossbar"),
					   CrossbarConfigClicked, input);
	}

	/* Editable: a capture card may take a resolution it doesn't list. */
	p = obs_properties_add_list(props, VD_RESOLUTION, obs_module_text("resolution"), OBS_COMBO_TYPE_EDITABLE,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_set_modified_callback(p, ModeChanged);

	p = obs_properties_add_list(props, VD_FRAME_RATE, obs_module_text("frame-rate"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_set_modified_callback(p, ModeChanged);

	p = obs_properties_add_list(props, VD_PIXEL_FORMAT, obs_module_text("pixel-format"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_set_modified_callback(p, ModeChanged);

	RefreshLists(props, settings, true);

	obs_properties_add_bool(props, S_AUTOROTATION, obs_module_text("dshow.autorotation"));
	obs_properties_add_bool(props, S_HW_DECODE, obs_module_text("dshow.hw-decode"));

	/* Audio */
	p = obs_properties_add_list(props, S_AUDIO_OUTPUT_MODE, obs_module_text("dshow.audio-output-mode"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("dshow.audio-output-mode.capture"), (int64_t)AudioMode::Capture);
	obs_property_list_add_int(p, obs_module_text("dshow.audio-output-mode.directsound"),
				  (int64_t)AudioMode::DirectSound);
	obs_property_list_add_int(p, obs_module_text("dshow.audio-output-mode.waveout"), (int64_t)AudioMode::WaveOut);

	vector<AudioDevice> audioDevices;
	Device::EnumAudioDevices(audioDevices);
	if (audioDevices.empty()) {
		return;
	}

	p = obs_properties_add_bool(props, S_USE_CUSTOM_AUDIO, obs_module_text("dshow.use-custom-audio-device"));
	obs_property_set_modified_callback(p, CustomAudioClicked);

	p = obs_properties_add_list(props, S_AUDIO_DEVICE_ID, obs_module_text("dshow.audio-device"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	for (const AudioDevice &device : audioDevices) {
		DStr id, name;
		EncodeDeviceId(id, device.name.c_str(), device.path.c_str());
		dstr_from_wcs(name, device.name.c_str());
		obs_property_list_add_string(p, name, id);
	}
	obs_property_set_visible(p, obs_data_get_bool(settings, S_USE_CUSTOM_AUDIO));
}

/* ------------------------------------------------------------------------- */
/* Backend                                                                   */

static void proc_activate(void *data, calldata_t *cd)
{
	bool activate = calldata_bool(cd, "active");
	DShowInput *input = reinterpret_cast<DShowInput *>(data);
	input->SetActive(activate);
}

static bool DShowAvailable(void)
{
	return true;
}

static void *DShowCreate(obs_source_t *source, obs_data_t *settings)
{
	EnsureInit();

	DShowInput *dshow = nullptr;
	try {
		dshow = new DShowInput(source, settings);
		proc_handler_t *ph = obs_source_get_proc_handler(source);
		proc_handler_add(ph, "void activate(bool active)", proc_activate, dshow);
	} catch (const char *error) {
		blog(LOG_ERROR, "Could not create device '%s': %s", obs_source_get_name(source), error);
	}
	return dshow;
}

static void DShowDestroy(void *data)
{
	delete reinterpret_cast<DShowInput *>(data);
}

static void DShowUpdate(void *data, obs_data_t *settings)
{
	DShowInput *input = reinterpret_cast<DShowInput *>(data);
	if (input->active && !input->hidden) {
		input->QueueActivate(settings);
	}
}

static void DShowDefaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, S_ACTIVE, true);
	obs_data_set_default_int(settings, S_AUDIO_OUTPUT_MODE, (int)AudioMode::Capture);
	obs_data_set_default_bool(settings, S_AUTOROTATION, true);
	obs_data_set_default_bool(settings, S_HW_DECODE, false);
}

static void DShowShow(void *data)
{
	DShowInput *input = reinterpret_cast<DShowInput *>(data);
	if (input->hidden) {
		input->hidden = false;
		if (input->active) {
			input->QueueAction(Action::Activate);
		}
	}
}

static void DShowHide(void *data)
{
	DShowInput *input = reinterpret_cast<DShowInput *>(data);
	if (!input->hidden) {
		input->hidden = true;
		if (input->active) {
			input->QueueAction(Action::Deactivate);
		}
	}
}

static void DShowUnload(void)
{
	lock_guard<mutex> lock(devices_mutex);
	listed_devices.devices.clear();
}

extern "C" {
extern const struct vd_backend vd_dshow_backend;
}

/* In field order: C++17 has no designated initializers. */
const struct vd_backend vd_dshow_backend = {
	"dshow",            /* name */
	"backend.dshow",    /* display_name */
	DShowAvailable,     /* available */
	DShowEnumDevices,   /* enum_devices */
	DShowCreate,        /* create */
	DShowDestroy,       /* destroy */
	DShowUpdate,        /* update */
	DShowDefaults,      /* get_defaults */
	DShowAddProperties, /* add_properties */
	nullptr,            /* activate */
	nullptr,            /* deactivate */
	DShowShow,          /* show */
	DShowHide,          /* hide */
	DShowUnload,        /* unload */
	true,               /* has_audio */
};
