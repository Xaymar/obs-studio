# Known libobs bugs

Bugs found while writing the unit tests. Crashes are fixed (each entry names its commit); bugs that only give wrong
results stay listed and unfixed, as decided, because the fix changes behaviour callers may depend on. Each entry names the file and how to trigger it. Bugs that were small enough were fixed in
their own commits with a regression test and are not listed here.

## util/

| File | Bug |
| --- | --- |
| util/cf-lexer.c | Not a crash (corrected): it is reported as "Expected '('". The crash seen in the first test run came from the next input, the open macro call below; stderr ordering made it look like this one. A function-like macro used without `(` (`#define M(a, b) a b` then `M 2`) crashes the preprocessor with an access violation. |
| util/cf-lexer.c | **Fixed** in `9cb523235`: A macro call left open at end of file (`#define M(a) a` then `M(1, 2`) crashes the preprocessor: the token pointer steps past the end token. |
| util/cf-lexer.c | `#define M()` (zero parameters) is rejected with "Expected identifier", so zero-parameter function macros cannot be defined. |
| util/cf-lexer.c | Octal escapes in `cf_literal_to_str()` are decoded from the wrong position: the first digit is consumed by the `switch`, so `"\101"` becomes `"\x01"` instead of `"A"`. `"\0"` followed by digits is also taken as a lone NUL. |
| util/cf-lexer.c | **Fixed** in `cd4eaaad4`: `cf_preprocessor_add_def()` reports a redefinition through `pp->lex` and `pp->ed`, which are NULL until `cf_preprocess()` runs, so redefining before preprocessing crashes. |
| util/cf-lexer.c | Errors in `#include`d files are reported with row/column computed against the including file's text. |
| util/lexer.c | `valid_float_str()` rejects every signed exponent (`1e-3`, `1e+3`): `found_num` is reset at `e`, and the sign branch requires it. |
| util/dstr.c | `dstr_ncopy()` copies `len + 1` bytes from `array`, reading one byte past a non-terminated strref. |
| util/config-file.c | A section header without `]` (`[Broken`) still creates a section; the parse result is ignored. |
| util/config-file.c | Duplicate keys or sections are added to the hash twice instead of replacing the first. |
| util/text-lookup.c | Whitespace around `=` is not skipped: `Key = Value` stores `" "` as the value. |
| util/array-serializer.c | `get_pos` returns the array size instead of the write position after a seek. |
| util/array-serializer.c, util/buffered-file-serializer.c | `SERIALIZE_SEEK_END` moves backwards by `offset`; the buffered writer counts from its current position rather than the end. The file serializer uses `fseek()` semantics. |
| util/platform-windows.c | `os_cpu_usage_info_query()` divides by zero (returns NaN) when called within the same clock tick as `os_cpu_usage_info_start()` or a previous query. |
| util/platform.c | **Fixed** in `295c94d8f`: `os_get_path_extension(NULL)` crashes, unlike most `os_*` path helpers. |
| util/profiler.c | **Fixed** in `435b31de3`. The actual cause: profiler_free() destroyed the static root mutex, and the next profiler call (in the next test) crashed; profiler_free() itself was fine. Originally listed as: `profiler_free()` crashes (access violation) after `profiler_start()`, a snapshot and `profiler_stop()`. |
| util/profiler.c | Not a bug (explained): a call made while the profiler is stopped turns profiling off for that thread until `profile_reenable_thread()`, and the test made one before `profiler_start()`. Originally listed as: A snapshot reports fewer root calls than were made in `test_util_profiler`; not investigated yet. |
| util/profiler.c | **Fixed** in `a4c6474e5`: `profiler_snapshot_filter_roots(NULL, ...)` crashes, unlike every other snapshot accessor. |
| obs.c | **Fixed** in `ebdce7c8d`: `obs_get_video_info()` (and so `os_generate_formatted_filename()`) dereferences `obs` and crashes before `obs_startup()`. |

## media-io/

| File | Bug |
| --- | --- |
| media-io/format-conversion.c | `decompress_420()` packs pixels as `Y << 16 \| U << 8 \| V`, `decompress_nv12()` as `V << 16 \| U << 8 \| Y`: the same "packed 4:4:4" output in opposite byte orders. Neither has callers in the tree. |
| media-io/video-frame.c | **Fixed** in `391adc5d7`: `video_frame_init()` (and `video_frame_create()`) with `VIDEO_FORMAT_NONE` or an unknown format computes a size of 0 and calls `bmalloc(0)`, which crashes. |
| media-io/video-io.c | **Fixed** in `b2ea2c61c`: `video_output_get_skipped_frames(NULL)` and `video_output_get_total_frames(NULL)` crash; every other getter returns a default for NULL. |
| media-io/video-io.c | The getters of a frame-rate divisor output (`video_output_get_frame_rate()` and the rest) read the root output, so a half-rate output reports the full frame rate. |
| media-io/video-io.c | `valid_video_params()` accepts `cache_size == 0`; such an output never locks a frame and keeps counting every frame as skipped. |
| media-io/video-io.c | `video_output_unlock_frame()` without a successful `video_output_lock_frame()` decrements `available_frames` below zero (it is a `size_t`). |
| media-io/media-remux.c | **Fixed** in `5f4062de7`: `media_remux_job_create()` passes `out_filename` to `strcmp()` without a NULL check. |

## graphics/

| File | Bug |
| --- | --- |
| graphics/plane.c | `plane_transform()` transforms the normal as a point (including the translation) before normalizing it, and subtracts the transformed origin from the distance; translating the plane z = 1 by 2 along z gives distance -1 instead of 3, and any translation tilts normals that are not parallel to it. |
| graphics/plane.c | `plane_tri_inside()` and `plane_line_inside()` return a side bitmask as `bool`: false when everything lies on the plane, true when anything is off it, the opposite of what the names say. |
| graphics/math-extra.c | `norm_to_polar()` and `polar_to_norm()` use different angle conventions, so they are not inverses of each other. |
| graphics/graphics.c | `gs_color4v()`, `gs_cubetexture_set_image()`, `gs_set_3d_mode()`, `gs_draw_cube_backdrop()` and `gs_get_input()` are unimplemented `/* TODO */` stubs; `gs_color4v()` silently drops immediate-mode colors. |
| graphics/effect-parser.c | Several malformed effects leak memory on the error path: a uniform of unknown type (4 allocations), a pass calling a missing function (1), a sampler state missing ';' (2), a broken initializer (2) and a shader using undefined names (8). |
| graphics/shader-parser.c | Array initializers (`uniform float v[3] = {1, 2, 3};`) and `bool` defaults (`uniform bool b = false;`) are rejected ("Expected 'numeric value'", "Invalid type 'bool' used for assignment"). |
| graphics/shader-parser.c | `shader_sampler_convert()` reads `BorderColor` with `strtol(value + 1, ...)`: it skips the first character, so `0xff00ff00` becomes 0, and `strtol()` clamps colors above 0x7fffffff (alpha >= 0x80) on Windows, where `long` is 32 bits. |
| graphics/graphics-ffmpeg.c | EXIF orientation is ignored: it is read from the "Orientation" frame metadata, which the bundled FFmpeg no longer sets (it exports a display matrix instead), so rotated JPEGs load unrotated. |

## Core objects: fixes that change behaviour

Fixed while writing the core object tests. These fixes change behaviour that plugins might rely on.

| Area | Change | Commit |
| --- | --- | --- |
| callback/decl.c | A declaration with a missing ',' (`void f(int a b)`) used to be accepted and now fails. `signal_handler_add()` returns false, `proc_handler_add()` adds nothing, and a source, output, encoder or service type whose signal list has such a declaration fails to create. libobs' `audio_sync` and vlc-video's `get_metadata` had one; both are fixed. | `4d56fd267` |
| obs-source.c | `obs_source_filter_add()` refuses a source that is not a filter (it used to add inputs, scenes or transitions to the chain). | `3d46d4d78` |
| obs.c | `obs_shutdown()` stops the threads and destroys the remaining objects before freeing types and unloading modules, so a module's `obs_module_unload()` now runs after them instead of before. | `66803a870` |
| obs-data.c | `obs_data_item_remove()` drops the data's reference as well as the caller's, so the item is destroyed (it used to leak). | `75e67c897` |

Open, not fixed yet: `obs_get_video_sdr_white_level()` returns 0 until `obs_set_video_levels()` is called once video is up (the frontend always calls it).
