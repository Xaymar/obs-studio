#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"

#include <stdio.h>
#include <string.h>

#include <media-io/media-remux.h>
#include <util/platform.h>

static void put32(FILE *f, uint32_t v)
{
	fputc(v & 0xff, f);
	fputc((v >> 8) & 0xff, f);
	fputc((v >> 16) & 0xff, f);
	fputc((v >> 24) & 0xff, f);
}

static void put16(FILE *f, uint16_t v)
{
	fputc(v & 0xff, f);
	fputc((v >> 8) & 0xff, f);
}

/* one second of 16-bit stereo PCM at 8 kHz */
static void write_wav(const char *path, int channels)
{
	const uint32_t rate = 8000, frames = 8000;
	const uint32_t data_size = frames * channels * 2;
	FILE *f = os_fopen(path, "wb");
	assert_non_null(f);
	fwrite("RIFF", 1, 4, f);
	put32(f, 36 + data_size);
	fwrite("WAVEfmt ", 1, 8, f);
	put32(f, 16);
	put16(f, 1);
	put16(f, (uint16_t)channels);
	put32(f, rate);
	put32(f, rate * channels * 2);
	put16(f, (uint16_t)(channels * 2));
	put16(f, 16);
	fwrite("data", 1, 4, f);
	put32(f, data_size);
	for (uint32_t i = 0; i < frames * channels; i++)
		put16(f, (uint16_t)(i * 37));
	fclose(f);
}

struct progress {
	int calls;
	float last;
	bool cancel;
};

static bool on_progress(void *data, float percent)
{
	struct progress *p = data;
	p->calls++;
	p->last = percent;
	return !p->cancel;
}

static void remux_test(void **state)
{
	UNUSED_PARAMETER(state);

	write_wav("in.wav", 2);

	media_remux_job_t job = NULL;
	assert_true(media_remux_job_create(&job, "in.wav", "out.mka"));
	struct progress p = {0};
	assert_true(media_remux_job_process(job, on_progress, &p));
	assert_true(p.calls >= 1);
	media_remux_job_destroy(job);
	assert_true(os_get_file_size("out.mka") > 1000);

	/* without a progress callback */
	assert_true(media_remux_job_create(&job, "in.wav", "out2.mka"));
	assert_true(media_remux_job_process(job, NULL, NULL));
	media_remux_job_destroy(job);

	/* five channels are written as 4.1 */
	write_wav("five.wav", 5);
	assert_true(media_remux_job_create(&job, "five.wav", "five.mka"));
	assert_true(media_remux_job_process(job, NULL, NULL));
	media_remux_job_destroy(job);

	os_unlink("in.wav");
	os_unlink("five.wav");
	os_unlink("out.mka");
	os_unlink("out2.mka");
	os_unlink("five.mka");
}

static void cancel_test(void **state)
{
	UNUSED_PARAMETER(state);

	write_wav("cancel.wav", 2);
	media_remux_job_t job = NULL;
	assert_true(media_remux_job_create(&job, "cancel.wav", "cancel.mka"));
	struct progress p = {.cancel = true};
	media_remux_job_process(job, on_progress, &p);
	assert_true(p.calls >= 1);
	media_remux_job_destroy(job);
	os_unlink("cancel.wav");
	os_unlink("cancel.mka");
}

static void error_test(void **state)
{
	UNUSED_PARAMETER(state);

	media_remux_job_t job = (media_remux_job_t)1;
	assert_false(media_remux_job_create(NULL, "in.wav", "out.mka"));
	assert_false(media_remux_job_create(&job, "missing.wav", "out.mka"));
	write_wav("null-out.wav", 2);
	/* Regression: a NULL output name went to strcmp() */
	assert_false(media_remux_job_create(&job, "null-out.wav", NULL));
	assert_false(media_remux_job_create(&job, NULL, "out.mka"));
	os_unlink("null-out.wav");
	assert_null(job);

	write_wav("same.wav", 2);
	assert_false(media_remux_job_create(&job, "same.wav", "same.wav"));

	/* not a media file */
	FILE *f = os_fopen("junk.wav", "wb");
	fputs("not media", f);
	fclose(f);
	assert_false(media_remux_job_create(&job, "junk.wav", "out.mka"));

	/* no muxer for the extension, and an output that cannot be opened */
	assert_false(media_remux_job_create(&job, "same.wav", "out.unknown-extension"));
	assert_false(media_remux_job_create(&job, "same.wav", "no-such-dir/out.mka"));

	assert_false(media_remux_job_process(NULL, NULL, NULL));
	media_remux_job_destroy(NULL);

	os_unlink("same.wav");
	os_unlink("junk.wav");
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		obs_unit_test(remux_test),
		obs_unit_test(cancel_test),
		obs_unit_test(error_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
