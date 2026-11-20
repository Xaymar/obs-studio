#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-core-test.h"
#include "null-fail.h"

#include <obs-missing-files.h>
#include <util/threading.h>

static volatile long draws;
static void on_draw(void *param, uint32_t cx, uint32_t cy)
{
	UNUSED_PARAMETER(param);
	UNUSED_PARAMETER(cx);
	UNUSED_PARAMETER(cy);
	os_atomic_inc_long(&draws);
}

static volatile long ticks, renders, rendereds;
static void on_tick(void *param, float seconds)
{
	UNUSED_PARAMETER(param);
	UNUSED_PARAMETER(seconds);
	os_atomic_inc_long(&ticks);
}

static void on_render(void *param, uint32_t cx, uint32_t cy)
{
	UNUSED_PARAMETER(param);
	UNUSED_PARAMETER(cx);
	UNUSED_PARAMETER(cy);
	os_atomic_inc_long(&renders);
}

static void on_rendered(void *param)
{
	UNUSED_PARAMETER(param);
	os_atomic_inc_long(&rendereds);
}

static bool wait_long(volatile long *v, long count)
{
	for (int i = 0; i < 400 && os_atomic_load_long(v) < count; i++)
		os_sleep_ms(5);
	return os_atomic_load_long(v) >= count;
}

static obs_canvas_t *projected_canvas;
static volatile long projector_draws;

/* what a projector draws: the main texture and a canvas texture, both ways */
static void on_projector_draw(void *param, uint32_t cx, uint32_t cy)
{
	UNUSED_PARAMETER(param);
	UNUSED_PARAMETER(cx);
	UNUSED_PARAMETER(cy);
	obs_render_main_texture();
	obs_render_main_texture_src_color_only();
	obs_render_canvas_texture(projected_canvas);
	obs_render_canvas_texture_src_color_only(projected_canvas);
	assert_non_null(obs_get_main_texture());
	os_atomic_inc_long(&projector_draws);
}

static void projector_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();

	obs_source_t *src = obs_source_create("fake_input", "src", NULL, NULL);
	obs_set_output_source(0, src);
	struct obs_video_info ovi;
	obs_get_video_info(&ovi);
	ovi.base_width = ovi.output_width = 320;
	ovi.base_height = ovi.output_height = 180;
	projected_canvas = obs_canvas_create("projected", &ovi, PROGRAM);
	obs_canvas_set_channel(projected_canvas, 0, src);

	/* an SDR, an sRGB 16-bit float and a scRGB display */
	static const enum gs_color_format formats[] = {GS_BGRA, GS_RGBA16F, GS_RGBA16F};
	for (int i = 0; i < 3; i++) {
		struct gs_init_data init = {.cx = 160, .cy = 90, .num_backbuffers = 1, .format = formats[i]};
		obs_display_t *display = obs_display_create(&init, 0);
		assert_non_null(display);
		os_atomic_set_long(&projector_draws, 0);
		obs_display_add_draw_callback(display, on_projector_draw, NULL);
		assert_true(wait_long(&projector_draws, 3));
		obs_display_remove_draw_callback(display, on_projector_draw, NULL);
		obs_display_destroy(display);
	}

	/* a display whose swap chain cannot be created */
	struct gs_init_data init = {.cx = 160, .cy = 90, .num_backbuffers = 1, .format = GS_BGRA};
	null_fail_next("device_swapchain_create", 1);
	assert_null(obs_display_create(&init, 0));

	obs_canvas_set_channel(projected_canvas, 0, NULL);
	obs_canvas_remove(projected_canvas);
	obs_canvas_release(projected_canvas);
	obs_set_output_source(0, NULL);
	obs_source_release(src);
	core_stop();
}

static void display_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();

	struct gs_init_data init = {.cx = 320, .cy = 240, .num_backbuffers = 1, .format = GS_BGRA};
	obs_display_t *display = obs_display_create(&init, 0x112233);
	assert_non_null(display);
	uint32_t cx, cy;
	obs_display_size(display, &cx, &cy);
	assert_int_equal(cx, 320);
	assert_int_equal(cy, 240);
	/* a resize applies when the display next renders */
	obs_display_resize(display, 200, 100);
	core_wait_frames(3);
	obs_display_size(display, &cx, &cy);
	assert_int_equal(cx, 200);
	obs_display_set_background_color(display, 0xff0000);
	obs_display_update_color_space(display);

	draws = 0;
	obs_display_add_draw_callback(display, on_draw, NULL);
	assert_true(wait_long(&draws, 2));
	obs_display_set_enabled(display, false);
	assert_false(obs_display_enabled(display));
	obs_display_set_enabled(display, true);
	assert_true(obs_display_enabled(display));
	obs_display_remove_draw_callback(display, on_draw, NULL);

	obs_display_destroy(display);
	obs_display_destroy(NULL);
	assert_false(obs_display_enabled(NULL));

	/* main loop callbacks */
	ticks = renders = rendereds = 0;
	obs_add_tick_callback(on_tick, NULL);
	obs_add_main_render_callback(on_render, NULL);
	obs_add_main_rendered_callback(on_rendered, NULL);
	assert_true(wait_long(&ticks, 2));
	assert_true(wait_long(&renders, 2));
	assert_true(wait_long(&rendereds, 2));
	obs_remove_tick_callback(on_tick, NULL);
	obs_remove_main_render_callback(on_render, NULL);
	obs_remove_main_rendered_callback(on_rendered, NULL);
	assert_true(obs_get_video_frame_time() > 0);
	/* Regression: both levels were 0 once graphics existed, until obs_set_video_levels() */
	assert_true(obs_get_video_sdr_white_level() == 300.0f);
	assert_true(obs_get_video_hdr_nominal_peak_level() == 1000.0f);
	obs_set_video_levels(200.0f, 800.0f);
	assert_true(obs_get_video_sdr_white_level() == 200.0f);
	assert_true(obs_get_video_hdr_nominal_peak_level() == 800.0f);

	core_stop();
}

static bool count_info(void *param, struct obs_video_info *ovi)
{
	UNUSED_PARAMETER(ovi);
	(*(int *)param)++;
	return true;
}

static void view_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();

	obs_view_t *view = obs_view_create();
	obs_source_t *src = obs_source_create("fake_input", "src", NULL, NULL);
	obs_view_set_source(view, 0, src);
	obs_source_t *got = obs_view_get_source(view, 0);
	assert_ptr_equal(got, src);
	obs_source_release(got);
	assert_null(obs_view_get_source(view, 1));

	/* a view with its own video output renders its source */
	video_t *video = obs_view_add(view);
	assert_non_null(video);
	int infos = 0;
	obs_view_enum_video_info(view, count_info, &infos);
	assert_int_equal(infos, 1);
	core_wait_frames(4);
	assert_true(fake_of(src)->renders > 0);
	obs_view_remove(view);

	/* a view at another size */
	struct obs_video_info ovi;
	obs_get_video_info(&ovi);
	ovi.base_width = ovi.output_width = 320;
	ovi.base_height = ovi.output_height = 180;
	video = obs_view_add2(view, &ovi);
	assert_non_null(video);
	assert_int_equal(video_output_get_width(video), 320);
	core_wait_frames(3);
	obs_view_remove(view);

	obs_view_set_source(view, 0, NULL);
	obs_view_destroy(view);
	obs_view_destroy(NULL);
	obs_source_release(src);
	core_stop();
}

static bool count_scene(void *param, obs_source_t *scene)
{
	UNUSED_PARAMETER(scene);
	(*(int *)param)++;
	return true;
}

static bool count_canvas(void *param, obs_canvas_t *canvas)
{
	UNUSED_PARAMETER(canvas);
	(*(int *)param)++;
	return true;
}

static int canvas_renames;
static void on_canvas_rename(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(cd);
	canvas_renames++;
}

static void canvas_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();

	obs_canvas_t *main = obs_get_main_canvas();
	assert_non_null(main);
	assert_true(obs_canvas_get_flags(main) & MAIN);
	assert_true(obs_canvas_has_video(main));
	/* the main canvas cannot be renamed */
	const char *main_name = obs_canvas_get_name(main);
	obs_canvas_set_name(main, "other");
	assert_string_equal(obs_canvas_get_name(main), main_name);
	obs_canvas_release(main);

	struct obs_video_info ovi;
	obs_get_video_info(&ovi);
	ovi.base_width = ovi.output_width = 320;
	ovi.base_height = ovi.output_height = 180;
	obs_canvas_t *canvas = obs_canvas_create("second", &ovi, PROGRAM);
	assert_non_null(canvas);
	assert_string_equal(obs_canvas_get_name(canvas), "second");
	assert_non_null(obs_canvas_get_uuid(canvas));
	assert_true(obs_canvas_has_video(canvas));
	assert_non_null(obs_canvas_get_video(canvas));
	struct obs_video_info got;
	assert_true(obs_canvas_get_video_info(canvas, &got));
	assert_int_equal(got.base_width, 320);

	obs_canvas_t *found = obs_get_canvas_by_name("second");
	assert_ptr_equal(found, canvas);
	obs_canvas_release(found);
	found = obs_get_canvas_by_uuid(obs_canvas_get_uuid(canvas));
	assert_ptr_equal(found, canvas);
	obs_canvas_release(found);
	int canvases = 0;
	obs_enum_canvases(count_canvas, &canvases);
	assert_int_equal(canvases, 2);

	canvas_renames = 0;
	signal_handler_connect(obs_canvas_get_signal_handler(canvas), "rename", on_canvas_rename, NULL);
	obs_canvas_set_name(canvas, "renamed");
	assert_string_equal(obs_canvas_get_name(canvas), "renamed");
	assert_int_equal(canvas_renames, 1);
	obs_canvas_set_name(canvas, "");
	assert_string_equal(obs_canvas_get_name(canvas), "renamed");
	signal_handler_disconnect(obs_canvas_get_signal_handler(canvas), "rename", on_canvas_rename, NULL);

	/* weak references */
	obs_weak_canvas_t *weak = obs_canvas_get_weak_canvas(canvas);
	assert_true(obs_weak_canvas_references_canvas(weak, canvas));
	obs_canvas_t *strong = obs_weak_canvas_get_canvas(weak);
	assert_ptr_equal(strong, canvas);
	obs_canvas_release(strong);

	/* scenes on the canvas */
	obs_scene_t *scene = obs_canvas_scene_create(canvas, "canvas scene");
	assert_ptr_equal(obs_source_get_canvas(obs_scene_get_source(scene)), canvas);
	obs_canvas_release(canvas); /* the getter returned a reference */
	obs_source_t *byname = obs_canvas_get_source_by_name(canvas, "canvas scene");
	assert_ptr_equal(byname, obs_scene_get_source(scene));
	obs_source_release(byname);
	obs_scene_t *sbyname = obs_canvas_get_scene_by_name(canvas, "canvas scene");
	assert_ptr_equal(sbyname, scene);
	obs_scene_release(sbyname);
	int scenes = 0;
	obs_canvas_enum_scenes(canvas, count_scene, &scenes);
	assert_int_equal(scenes, 1);
	/* the main canvas does not list it */
	assert_null(obs_get_source_by_name("canvas scene"));

	/* channels render on the canvas's own video */
	obs_source_t *src = obs_source_create("fake_input", "src", NULL, NULL);
	obs_scene_add(scene, src);
	obs_canvas_set_channel(canvas, 0, obs_scene_get_source(scene));
	obs_source_t *channel = obs_canvas_get_channel(canvas, 0);
	assert_ptr_equal(channel, obs_scene_get_source(scene));
	obs_source_release(channel);
	core_wait_frames(4);
	assert_true(fake_of(src)->renders > 0);
	assert_false(obs_canvas_video_active(canvas));

	/* resetting the canvas video */
	ovi.base_width = ovi.output_width = 640;
	ovi.base_height = ovi.output_height = 360;
	assert_true(obs_canvas_reset_video(canvas, &ovi));
	assert_true(obs_canvas_get_video_info(canvas, &got));
	assert_int_equal(got.base_width, 640);

	/* saving and loading */
	obs_data_t *saved = obs_save_canvas(canvas);
	assert_non_null(saved);
	assert_string_equal(obs_data_get_string(saved, "name"), "renamed");

	/* moving the scene to the main canvas */
	main = obs_get_main_canvas();
	obs_canvas_move_scene(scene, main);
	scenes = 0;
	obs_canvas_enum_scenes(canvas, count_scene, &scenes);
	assert_int_equal(scenes, 0);
	obs_source_t *moved = obs_get_source_by_name("canvas scene");
	assert_ptr_equal(moved, obs_scene_get_source(scene));
	obs_source_release(moved);
	obs_canvas_release(main);

	obs_canvas_set_channel(canvas, 0, NULL);
	obs_source_remove(obs_scene_get_source(scene));
	obs_scene_release(scene);
	obs_source_release(src);

	obs_canvas_remove(canvas);
	assert_true(obs_canvas_removed(canvas));
	obs_canvas_release(canvas);
	assert_null(obs_weak_canvas_get_canvas(weak));
	obs_weak_canvas_release(weak);

	obs_canvas_t *loaded = obs_load_canvas(saved);
	assert_non_null(loaded);
	assert_string_equal(obs_canvas_get_name(loaded), "renamed");
	obs_canvas_remove(loaded);
	obs_canvas_release(loaded);
	obs_data_release(saved);

	/* private canvases are not listed */
	obs_canvas_t *priv = obs_canvas_create_private("private", NULL, PREVIEW);
	assert_null(obs_get_canvas_by_name("private"));
	assert_false(obs_canvas_has_video(priv));
	obs_canvas_release(priv);

	core_stop();
}

static int replaced;
static char replaced_path[64];
static void on_replace(void *src, const char *new_path, void *data)
{
	UNUSED_PARAMETER(src);
	UNUSED_PARAMETER(data);
	replaced++;
	snprintf(replaced_path, sizeof(replaced_path), "%s", new_path);
}

static obs_missing_files_t *fake_missing(void *data)
{
	struct fake_source *fs = data;
	obs_missing_files_t *files = obs_missing_files_create();
	obs_missing_files_add_file(files, obs_missing_file_create("C:/missing.png", on_replace, OBS_MISSING_FILE_SOURCE,
								  fs->source, NULL));
	return files;
}

static void missing_files_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();

	obs_missing_files_t *files = obs_missing_files_create();
	assert_int_equal(obs_missing_files_count(files), 0);
	obs_missing_file_t *file = obs_missing_file_create("C:/a.png", on_replace, OBS_MISSING_FILE_SCRIPT, NULL, NULL);
	obs_missing_files_add_file(files, file);
	assert_int_equal(obs_missing_files_count(files), 1);
	assert_ptr_equal(obs_missing_files_get_file(files, 0), file);
	assert_string_equal(obs_missing_file_get_path(file), "C:/a.png");
	/* script files are handled by the scripting module, not here */
	replaced = 0;
	obs_missing_file_issue_callback(file, "C:/b.png");
	assert_int_equal(replaced, 0);

	obs_missing_files_t *more = obs_missing_files_create();
	obs_missing_files_add_file(more, obs_missing_file_create("C:/c.png", on_replace, OBS_MISSING_FILE_SCRIPT, NULL,
								 NULL));
	obs_missing_files_append(files, more);
	assert_int_equal(obs_missing_files_count(files), 2);
	obs_missing_files_destroy(more);
	obs_missing_files_destroy(files);
	obs_missing_files_destroy(NULL);

	/* a source type that reports missing files */
	struct obs_source_info info = {
		.id = "fake_missing",
		.type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_VIDEO,
		.get_name = fake_get_name,
		.create = fake_create,
		.destroy = fake_destroy,
		.get_width = fake_width,
		.get_height = fake_height,
		.missing_files = fake_missing,
	};
	obs_register_source(&info);
	obs_source_t *src = obs_source_create("fake_missing", "missing", NULL, NULL);
	files = obs_source_get_missing_files(src);
	assert_int_equal(obs_missing_files_count(files), 1);
	file = obs_missing_files_get_file(files, 0);
	assert_string_equal(obs_missing_file_get_source_name(file), "missing");
	obs_missing_file_issue_callback(file, "C:/found.png");
	assert_int_equal(replaced, 1);
	assert_string_equal(replaced_path, "C:/found.png");
	obs_missing_files_destroy(files);
	/* sources without the callback have none */
	obs_source_t *plain = obs_source_create("fake_input", "plain", NULL, NULL);
	files = obs_source_get_missing_files(plain);
	assert_int_equal(obs_missing_files_count(files), 0);
	obs_missing_files_destroy(files);
	obs_source_release(plain);
	obs_source_release(src);

	core_stop();
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(display_test), cmocka_unit_test(projector_test),
		cmocka_unit_test(view_test),
		cmocka_unit_test(canvas_test),
		cmocka_unit_test(missing_files_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
