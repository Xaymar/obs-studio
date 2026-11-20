# Code the unit tests cannot reach

Regions of libobs that stay uncovered on purpose, because no test can reach them through the public API without
changing libobs. Each entry gives the file, the line range (as of the commit that added or last updated it) and the
reason. Line numbers drift; search for the quoted code when they do.

Reasons:

- **OOM**: allocation failure. `bmalloc()`/`brealloc()` never return NULL, they `bcrash()`.
- **OS failure**: a Win32 or CRT call that only fails when the OS is out of resources or broken.
- **Exit**: the code ends the process, so it cannot run inside a test process that reports results.
- **Once per process**: `bcrash()` refuses to run twice, so only one crash path per test executable is testable.
- **Dead**: no input reaches it.
- **Crash**: reaching it crashed libobs. All of these are fixed now; their rows say so.

## Allocation failure checks (dead)

`bmalloc()`, `bzalloc()` and `brealloc()` never return NULL: they `bcrash()` when the allocation fails
(util/bmem.c), so an `if (!ptr)` right after one of them is dead. The checks in libobs:

| File | Lines | Allocation |
| --- | --- | --- |
| audio-monitoring/monitoring-mix.c | 210, 384 | `bzalloc()` of the mix and of a mix source |
| media-io/audio-io.c | 362 (`goto fail0`) | `bzalloc()` of the audio output |
| media-io/video-io.c | 250 (`goto fail0`) | `bzalloc()` of the video output |
| obs-audio-controls.c | 553, 776 | `bzalloc()` of a fader and of a volume meter |
| util/buffered-file-serializer.c | 70-72 | `bmalloc()` of the I/O thread's chunk (also listed below) |

Out of this build: obs-nix.c 196 (`bmalloc()`). `bstrdup()` checks are not dead (it returns NULL for NULL).
Allocation failures inside FFmpeg, the CRT and Win32 calls stay under OOM in the tables below.

## util/

| File | Lines | Code | Reason |
| --- | --- | --- | --- |
| util/base.c | 58-63 | `def_crash_handler()`: `vfprintf(...); exit(0);` | Exit |
| util/base.c | 103-105 | `bcrash()`: `"Crashed in the crash handler"`, `exit(2)` | Exit (second `bcrash()`) |
| util/bmem.c | 111-113, 132-134 | `bmalloc()`/`brealloc()`: `os_oom(); bcrash("Out of memory ...")` | OOM |
| util/bmem.c | 125-127 | `brealloc()` with size 0: `os_breakpoint(); bcrash(...)` | Once per process (`test_util_bmem` uses its crash for `bmalloc(0)`) |
| util/bmem.c | `bmemdup()` | `if (size) memcpy(...)` false branch | Dead: `bmalloc(0)` already crashed |
| util/darray.h | `darray_move_item()`, `darray_swap()` | `if (!temp) bcrash(...)` after `malloc()` | OOM |
| util/dstr.c | 552, 574 | `dstr_vprintf()`/`dstr_vcatf()`: `if (len < 0) len = 4095;` | OS failure: `vsnprintf(NULL, 0, ...)` only fails on an encoding error the MSVC CRT does not produce for valid formats |
| util/dstr.c | 755, 778 | `dstr_to_upper()`/`dstr_to_lower()`: `if (!wstr) return;` | Dead: `dstr_to_wcs()` of a non-empty dstr always returns a string |
| util/dstr.h | 256 | `dstr_is_empty()`: `return true` for a non-NULL array with `*array == 0` | Dead: every dstr function frees instead of keeping an empty array |
| util/lexer.c | 168 | `valid_float_str()`: sign branch `return false` | Reachable only for signed exponents, which the function rejects earlier (see known-bugs.md) |
| util/cf-lexer.c | 75-77 | octal escape in `cf_convert_from_escape_literal()` | Reachable, but wrong (see known-bugs.md); not asserted |
| util/cf-lexer.c | 591-593, 1126, 1157-1159 | macro call without `(` or left open at end of file | Now covered: the open call is fixed (`9cb523235`), the call without `(` never crashed |
| util/cf-lexer.c | 1178-1182 | zero-parameter macro call `M()` | Dead: `#define M()` is rejected, so no zero-parameter macro exists (see known-bugs.md) |
| util/cf-lexer.c | `next_token()` | `preprocessor == false` loop condition `SPACETAB && NEWLINE` | Dead: the condition can never be true; every caller passes `true` anyway |
| util/cf-lexer.c | 1350 | `cf_preprocess()`: `if (!token) return false;` | Dead: `cf_lexer_lex()` always leaves at least the end token |
| util/config-file.c | 82-84, 281-283, 309-311 | `pthread_mutex_init_recursive()` failure | OS failure |
| util/config-file.c | 380, 384 | `fwrite()` failure | OS failure (disk full) |
| util/config-file.c | 439 | `os_safe_replace()` failure | OS failure |
| util/pipe-windows.c | 40-41, 101-106, 112-118 | `CreatePipe()`/`SetHandleInformation()` failure | OS failure |
| util/pipe-windows.c | 280 | `os_process_pipe_read_err()` when `handle_err` is NULL | Dead: always set on success |
| util/buffered-file-serializer.c | 69-72 | `bmalloc()` returned NULL | Dead: `bmalloc()` never returns NULL |
| util/buffered-file-serializer.c | 182-187, 215, 262, 317 | `fwrite()` failure and the `output_error` paths it enables | OS failure (disk full) |
| util/buffered-file-serializer.c | 273-279 | writer waits for the I/O thread | Timing: only when the I/O thread falls behind; flaky to force |
| util/profiler.c | `profiler_free()` | freeing after a start/snapshot/stop cycle | Now covered: the crash was a later call after the free (fixed in `435b31de3`) |
| util/utf8.c | `wchar_to_utf8()`: `insize == 0` | the `wcslen()` fallback | Dead: `wchar_to_utf8()` is not exported, and its only callers (`os_wcs_to_utf8()` and friends) compute the length first |

## media-io/

| File | Lines | Code | Reason |
| --- | --- | --- | --- |
| media-io/audio-io.c | 363-390 | `audio_output_open()`: `bzalloc()`, mutex, event or thread creation failed | OOM / OS failure |
| media-io/audio-io.c | 278-281 | `audio_input_init()`: resampler creation failed | Dead: every format/layout pair the API accepts creates a resampler |
| media-io/audio-resampler-ffmpeg.c | 49-50, 70 | `AUDIO_FORMAT_UNKNOWN` / unknown layout fallbacks | Dead: `audio_output_open()` rejects unknown formats and layouts |
| media-io/audio-resampler-ffmpeg.c | 134-137, 161-164, 209-211 | `swr_alloc_set_opts2()`, `swr_init()`, `swr_convert()` failures | OS failure (swresample allocation) |
| media-io/audio-resampler-ffmpeg.c | 156 | `swr_set_matrix()` failure for the mono upmix | Dead for the layouts libobs passes |
| media-io/media-remux.c | 56-59, 81-91 | stream info, stream allocation and parameter copy failures | OS failure (libavformat allocation) / needs a corrupt file libavformat opens but cannot probe |
| media-io/media-remux.c | 186-189, 207-215, 231-233, 243-245 | read, mux, header and trailer errors | Needs inputs that libavformat opens but fails to read or mux; not constructible from a test-generated file |
| media-io/video-frame.c | 37-39, 127-129 | `VIDEO_FORMAT_NONE` in the layout switches | Now covered: `video_frame_init()` no longer crashes on it (`391adc5d7`) |
| media-io/video-io.c | 248-276 | `video_output_open()`: mutex, semaphore or thread creation failed | OOM / OS failure |
| media-io/video-io.c | 119-120, 357-359 | scaling a frame failed / scaler creation failed for a reason other than a bad conversion | OS failure (swscale allocation) |
| media-io/video-io.c | 617-679 | `video_output_inc/dec_texture_*()`, `video_output_create_with_frame_rate_divisor()` | Not exported: reachable only through the encoder code (track with obs-encoder tests) |
| media-io/video-scaler-ffmpeg.c | 181-190, 202-218, 242-244 | `av_image_alloc()`, `sws_alloc_context()`, `sws_init_context()`, `sws_scale()` failures | OS failure (swscale allocation) |
| media-io/video-scaler-ffmpeg.c | 209-211 | `sws_setColorspaceDetails()` failure | Dead for the coefficients libobs passes |
| media-io/video-matrices.c | 188, 190 | `bpc < 8` / `bpc > 16` clamps | Dead: every format maps to 8, 10, 12 or 16 bits |
| audio-monitoring/monitoring-mix.c | all | monitoring mixer | Not exported: reachable only through libobs audio monitoring with live sources (track with obs-audio tests) |
| audio-monitoring/win32/* | all | WASAPI monitoring output and device enumeration | Needs a real audio device |

## graphics/

| File | Lines | Code | Reason |
| --- | --- | --- | --- |
| graphics/graphics.c | `gs_create()` error paths | module missing exports, `device_create()` failure | Needs a broken graphics module; libobs-null always loads |
| graphics/graphics.c | Apple and Linux branches (`gs_texture_create_from_iosurface`, dmabuf, sync objects) | platform-specific wrappers | Not compiled on Windows |
| graphics/graphics-ffmpeg.c | WIC paths (`wic_image_init`, PQ conversion) | HDR images (JXR) through WIC | Needs HDR test images and WIC codecs; reachable, not covered yet |
| graphics/graphics-ffmpeg.c | `avcodec_*` / `av_frame_*` / swscale allocation failures | decoder setup and conversion failures | OOM / OS failure |
| graphics/image-file.c | `alloc_mem()` limits, "overflowed maximum pointer size" | GIFs whose decoded size overflows | Needs a GIF that claims more than 4096x4096 or overflows; reachable with a crafted file, not covered yet |
| graphics/libnsgif/* | interlacing, local color tables, extension blocks, LZW error paths | parts of the GIF format the test images do not use | Reachable with more crafted GIFs, not covered yet |
| graphics/half.h | all | half-float conversion | No exported caller in libobs |

Reachable, but no test yet (not unreachable; tracked here until covered):

- util/util_uint64.h (the `util_mul_div64()` portable path)
