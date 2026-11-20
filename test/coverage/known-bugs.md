# Known libobs bugs

Bugs found while writing the unit tests. Each entry names the file, how to trigger it and, once fixed, the commit
that fixed it with a regression test. Fixes that change behaviour plugins could rely on are marked
**Behaviour change** here and in the commit message; the last section collects them. Entries without a commit are
still open, with the reason.

## util/

| File | Bug |
| --- | --- |
| util/cf-lexer.c | Not a bug: a function-like macro used without `(` (`#define M(a, b) a b` then `M 2`) is reported as "Expected '('". It was listed as a crash because of stderr ordering; the crash came from the open macro call below. |
| util/cf-lexer.c | **Fixed** in `9cb523235`: A macro call left open at end of file (`#define M(a) a` then `M(1, 2`) crashes the preprocessor: the token pointer steps past the end token. |
| util/cf-lexer.c | **Fixed** in `3cf1f7a20`: `#define M()` (zero parameters) is rejected with "Expected identifier", and calling such a macro reported "Mismatching number of macro parameters". |
| util/cf-lexer.c | **Fixed** in `4fefeecf0`: Octal escapes in `cf_literal_to_str()` are decoded from the wrong position, so `"\101"` becomes `"\x01"` instead of `"A"`, and `"\0"` followed by digits is a lone NUL. Hex escapes read any number of digits and then skipped two characters. |
| util/cf-lexer.c | **Fixed** in `cd4eaaad4`: `cf_preprocessor_add_def()` reports a redefinition through `pp->lex` and `pp->ed`, which are NULL until `cf_preprocess()` runs, so redefining before preprocessing crashes. |
| util/cf-lexer.c | **Fixed** in `c9573ed79`: Errors in `#include`d files are reported with row/column computed against the including file's text. |
| util/cf-lexer.c | **Fixed** in `e629d8773`: The tokens of an `#include`d file point at a lexer on the stack of `cf_include_file()`, so the second include of a file and the effect/shader parsers' error messages for its tokens read freed stack memory. |
| util/lexer.c | **Fixed** in `a0b87467c`: `valid_float_str()` rejects every signed exponent (`1e-3`, `1e+3`): `found_num` is reset at `e`, and the sign branch requires it. |
| util/dstr.c | **Fixed** in `44789e46c`: `dstr_ncopy()` copies `len + 1` bytes from `array`, reading one byte past a non-terminated strref. |
| util/config-file.c | **Fixed** in `59a02be43`, **behaviour change**: A section header without `]` (`[Broken`) still creates a section; the parse result is ignored. A last line without its `]` or `=` was also accepted. |
| util/config-file.c | **Fixed** in `77915b1ec`, **behaviour change**: Duplicate keys or sections are added to the hash twice instead of replacing the first. |
| util/text-lookup.c | **Fixed** in `dc9cc505b`, **behaviour change**: Whitespace around `=` is not skipped: `Key = Value` stores `" "` as the value. |
| util/array-serializer.c | **Fixed** in `9dfcdf37d`: `get_pos` returns the array size instead of the write position after a seek. |
| util/array-serializer.c, util/buffered-file-serializer.c | **Fixed** in `723919330`, **behaviour change**: `SERIALIZE_SEEK_END` moves backwards by `offset`; the buffered writer counts from its current position rather than the end. The file serializer uses `fseek()` semantics, which all three now share. |
| util/platform-windows.c | **Fixed** in `35bf48eeb`: `os_cpu_usage_info_query()` divides by zero (returns NaN) when called within the same clock tick as `os_cpu_usage_info_start()` or a previous query. |
| util/platform.c | **Fixed** in `295c94d8f`: `os_get_path_extension(NULL)` crashes, unlike most `os_*` path helpers. |
| util/profiler.c | **Fixed** in `435b31de3`. The actual cause: profiler_free() destroyed the static root mutex, and the next profiler call (in the next test) crashed; profiler_free() itself was fine. Originally listed as: `profiler_free()` crashes (access violation) after `profiler_start()`, a snapshot and `profiler_stop()`. |
| util/profiler.c | Not a bug (explained): a call made while the profiler is stopped turns profiling off for that thread until `profile_reenable_thread()`, and the test made one before `profiler_start()`. Originally listed as: A snapshot reports fewer root calls than were made in `test_util_profiler`; not investigated yet. |
| util/profiler.c | **Fixed** in `a4c6474e5`: `profiler_snapshot_filter_roots(NULL, ...)` crashes, unlike every other snapshot accessor. |
| util/task.c | **Fixed** in `b782f6ba3`: `os_task_queue_inside(NULL)` dereferences NULL, unlike every other `os_task_queue_*` function. |
| util/source-profiler.c | **Fixed** in `ffcec25de`: When GPU profiling is turned off, the render timers of the last profiled frame are destroyed outside a graphics context, where `gs_timer_destroy()` does nothing, so they leak. |
| util/source-profiler.c | **Fixed** in `90313d3e3`: `task_delete_source()` frees a source's entry while holding `hm_rwlock` only for reading, racing with `source_profiler_fill_result()` readers. No deterministic test. |
| util/windows/window-helpers.c | **Fixed** in `e20f80668`: `ms_get_window_exe()` compares an uninitialized process id for a window that no longer exists. |
| obs.c | **Fixed** in `ebdce7c8d`: `obs_get_video_info()` (and so `os_generate_formatted_filename()`) dereferences `obs` and crashes before `obs_startup()`. |
| obs.c | **Fixed** in `686380e63`: `obs_get_video_sdr_white_level()` and `obs_get_video_hdr_nominal_peak_level()` return 0 once video is up until `obs_set_video_levels()` is called (the frontend always calls it), so every SDR/HDR multiplier divided by 0. They now start at the 300 and 1000 nits reported before video is up. |
| obs.c | **Fixed** in `396380014`: At shutdown, sources that a canvas's scene still holds are destroyed before the scene, whose destruction on the destruction thread then releases them again ("Double destroy just occurred", possibly on freed memory). Order depended on the uuids. |
| obs-encoder.c | **Fixed** in `978f83c9b` (committed by mistake together with the dotted module folder entry): `obs_encoder_set_group(encoder, NULL)` keeps `encoder->encoder_group`, so leaving again or destroying the encoder releases it twice and uses a group that may be freed. |
| obs.c | **Fixed** in `77b5adbe8`: When setting up a video mix fails part way (a texture or stage surface the device cannot create), `obs_create_video_mix()` frees it with `bfree()` and leaks the open video output and the textures created so far. |
| obs.c, obs-video.c | **Fixed** in `cafe2319e`, **behaviour change**: Output formats without a GPU conversion (Y800, I412, I210, YA2L, ...) and YUV formats with `gpu_conversion` off are accepted, and `copy_rgbx_frame()` then copies the 4-byte RGBA texture into their smaller frames: heap corruption on every frame. `obs_reset_video()` now rejects them. |
| obs-service.c | **Fixed** in `bed0899ab`: `obs_service_get_supported_video_codecs(NULL)` and `obs_service_get_supported_audio_codecs(NULL)` crash; every other service getter checks the service. |
| obs-encoder.c | **Fixed** in `cf549e547`: An encoder destroyed after an encode error leaks its paired encoder array and weak reference: they are released only in `obs_encoder_shutdown()`, which the error path skips. |
| obs-module.c | **Fixed** in `ac24f6a61`: A source a module registers but that is rejected is still added to the module's source list, so `obs_source_get_module()` and `obs_source_load_state()` report a source that does not exist. |
| obs-module.c | **Fixed** in `73eb68b67`: A module in a folder whose name has dots (`plugins/com.example.plugin/`) is looked up under the folder name cut at its last dot (`com.example`), so it is not found. Reported upstream. |
| obs-hotkey.c | **Fixed** in `2a2670793`: `obs_hotkeys_save_encoder/output/service/source(NULL)` crash; the load and register functions check the object. |
| obs-video-gpu-encode.c | **Fixed** in `7b96f0216`: An encode error in a texture encoder hangs the GPU encode thread for good: `full_stop()` runs on that thread and waits for it to go idle (and joins it for the last encoder), so its encoders and textures are never freed. |
| obs-service.c | **Fixed** in `bc440c43f`, **behaviour change**: `obs_service_create()` never applies the type's `get_defaults`, unlike outputs and encoders, so unset keys read as 0 or "". |

## media-io/

| File | Bug |
| --- | --- |
| media-io/format-conversion.c | Open, the right behaviour is unclear: `decompress_420()` packs pixels as `Y << 16 \| U << 8 \| V`, `decompress_nv12()` as `V << 16 \| U << 8 \| Y`, and `decompress_422()` keeps its input's byte order, while `compress_uyvx_to_*()` read `V << 16 \| Y << 8 \| U`. There is no documented packed 4:4:4 layout and no caller in the tree, so any choice breaks some plugin that relies on one of them. |
| media-io/video-frame.c | **Fixed** in `391adc5d7`: `video_frame_init()` (and `video_frame_create()`) with `VIDEO_FORMAT_NONE` or an unknown format computes a size of 0 and calls `bmalloc(0)`, which crashes. |
| media-io/video-io.c | **Fixed** in `b2ea2c61c`: `video_output_get_skipped_frames(NULL)` and `video_output_get_total_frames(NULL)` crash; every other getter returns a default for NULL. |
| media-io/video-io.c | **Fixed** in `1d79916d9`, **behaviour change**: The getters of a frame-rate divisor output (`video_output_get_frame_rate()`, `video_output_get_frame_time()`) read the root output, so a half-rate output reports the full frame rate. Format, size and frame counters still come from the root, which is right for them. |
| media-io/video-io.c | **Fixed** in `e41de0eef`: `valid_video_params()` accepts `cache_size == 0`; such an output never locks a frame and keeps counting every frame as skipped. |
| media-io/video-io.c | **Fixed** in `59c0b93e0`: `video_output_unlock_frame()` without a successful `video_output_lock_frame()` decrements `available_frames` below zero (it is a `size_t`). |
| media-io/video-io.c | **Fixed** in `32d00e649`: The video thread holds `input_mutex` while CPU encoders handle a frame and takes it again right away when an encoder is slower than the frame rate, so the output's stop thread never gets the lock to disconnect the encoder. The output never stops, and the next start, release or `obs_shutdown()` hangs its caller. This is the "session freezes when the encoder is overloaded" bug known since OBS 21.x. |
| media-io/media-remux.c | **Fixed** in `5f4062de7`: `media_remux_job_create()` passes `out_filename` to `strcmp()` without a NULL check. |

## graphics/

| File | Bug |
| --- | --- |
| graphics/texture-render.c | **Fixed** in `82f0af00a`: After its render target could not be created once, a texrender stays without one: the next `gs_texrender_begin()` with the same size skips creating it and fails, until the size changes. |
| graphics/graphics.c | **Fixed** in `46470fb49`: When `graphics_init()` fails (a vertex buffer the device cannot create), the context stays current with no reference, and `gs_destroy()` counts the reference down from 0 until the `long` wraps around: about ten seconds per failed `gs_create()`. |
| graphics/plane.c | **Fixed** in `c37e79238`, **behaviour change**: `plane_transform()` transforms the normal as a point (including the translation) before normalizing it, and subtracts the transformed origin from the distance; translating the plane z = 1 by 2 along z gives distance -1 instead of 3, and any translation tilts normals that are not parallel to it. `plane_transform3x4()` had the same problems. Non-uniform scale is still not handled (the normal needs the inverse transpose). |
| graphics/plane.c | **Fixed** in `04515d185`, **behaviour change**: `plane_tri_inside()` and `plane_line_inside()` return a side bitmask as `bool`: false when everything lies on the plane, true when anything is off it, the opposite of what the names say. |
| graphics/math-extra.c | **Fixed** in `622ef4024`, **behaviour change**: `norm_to_polar()` and `polar_to_norm()` use different angle conventions, so they are not inverses of each other. `polar_to_norm()` now uses the (yaw, pitch) convention of `norm_to_polar()`. |
| graphics/graphics.c | **Fixed** in `7cf55011e`, **behaviour change**: `gs_color4v()` is an empty stub and silently drops immediate-mode colors. |
| graphics/graphics.c | Open, unimplemented features rather than bugs: `gs_cubetexture_set_image()`, `gs_set_3d_mode()`, `gs_draw_cube_backdrop()` and `gs_get_input()` are `/* TODO */` stubs. There is no device export for updating a cube texture face, and nothing in the tree calls the other three; implementing them is new API work. |
| graphics/effect-parser.c | **Fixed** in `ee5b9185b`: A pass calling a missing function leaks the call string, and a sampler state cut off by the end of the effect (or with an empty value) leaks its name and value. The other inputs listed here before (a uniform of unknown type, a broken initializer, a shader using undefined names) parse successfully; their effects stay in the graphics context's effect cache until `gs_destroy()`, which the old per-effect count reported as a leak. |
| graphics/shader-parser.c | **Fixed** in `4b55e0353`: Array initializers (`uniform float v[3] = {1, 2, 3};`), `bool` defaults (`uniform bool b = false;`) and `intN` defaults are rejected ("Expected 'numeric value'", "Invalid type 'bool' used for assignment"). |
| graphics/shader-parser.c | **Fixed** in `d1858d906`, **behaviour change**: `shader_sampler_convert()` reads `BorderColor` with `strtol(value + 1, ...)`: it skips the first character, so `0xff00ff00` becomes 0, and `strtol()` clamps colors above 0x7fffffff (alpha >= 0x80) on Windows, where `long` is 32 bits. |
| graphics/graphics-ffmpeg.c | **Fixed** in `ea51c4f7e`, **behaviour change**: EXIF orientation is ignored: it is read from the "Orientation" frame metadata, which the bundled FFmpeg no longer sets (it exports a display matrix instead), so rotated JPEGs load unrotated. |

## Fixes that change behaviour

These fixes change behaviour that plugins might rely on.

| Area | Change | Commit |
| --- | --- | --- |
| callback/decl.c | A declaration with a missing ',' (`void f(int a b)`) used to be accepted and now fails. `signal_handler_add()` returns false, `proc_handler_add()` adds nothing, and a source, output, encoder or service type whose signal list has such a declaration fails to create. libobs' `audio_sync` and vlc-video's `get_metadata` had one; both are fixed. | `4d56fd267` |
| obs-source.c | `obs_source_filter_add()` refuses a source that is not a filter (it used to add inputs, scenes or transitions to the chain). | `3d46d4d78` |
| obs.c | `obs_shutdown()` stops the threads and destroys the remaining objects before freeing types and unloading modules, so a module's `obs_module_unload()` now runs after them instead of before. | `66803a870` |
| obs-data.c | `obs_data_item_remove()` drops the data's reference as well as the caller's, so the item is destroyed (it used to leak). | `75e67c897` |
| util/config-file.c | A section header without `]` and the keys under it are skipped instead of forming a section; a last line without `]` or `=` is ignored. | `59a02be43` |
| util/config-file.c | A repeated key replaces the earlier value and a repeated section is merged into the first; both used to be listed and saved twice, with only one copy readable. | `77915b1ec` |
| util/text-lookup.c | Spaces and tabs around the name and `=` are skipped (`Key = Value` gives `Value`, not `" "`), and a CRLF line with a name but no value no longer defines it. | `dc9cc505b` |
| util/array-serializer.c, util/buffered-file-serializer.c | `SERIALIZE_SEEK_END` adds the offset to the end like `fseek()`; a positive offset used to move back. | `723919330` |
| media-io/video-io.c | `video_output_get_frame_rate()` and `video_output_get_frame_time()` of an encoder's frame rate divisor output give the divided rate; flv-mux's `framerate` metadata changes with it. | `1d79916d9` |
| obs-service.c | A service's settings include its type's defaults (`get_defaults`) for keys the caller did not set. No service in the tree has a `get_defaults`. | `bc440c43f` |
| obs.c | `obs_reset_video()` returns `OBS_VIDEO_INVALID_PARAM` for output formats that are neither RGBA/BGRA/BGRX nor I420/NV12/I444/I010/P010/P216/P416 with GPU conversion. The frontend only uses supported ones. | `cafe2319e` |
| graphics/plane.c | `plane_transform()` and `plane_transform3x4()` give different planes for every matrix with a translation or rotation; `plane_tri_inside()` and `plane_line_inside()` return the inverted result. No callers in the tree. | `c37e79238`, `04515d185` |
| graphics/math-extra.c | `polar_to_norm()` returns a different normal for every input except (0, 0). No callers in the tree. | `622ef4024` |
| graphics/graphics.c | `gs_color4v()` adds the color; with fewer colors than vertices, the draw is shortened like it is for `gs_color()`. | `7cf55011e` |
| graphics/shader-parser.c | Sampler `BorderColor` values are used as written; most used to become 0 or 0x7fffffff. | `d1858d906` |
| graphics/graphics-ffmpeg.c | JPEGs with an EXIF orientation load rotated or mirrored as displayed, and orientations 5 to 8 swap width and height. | `ea51c4f7e` |
