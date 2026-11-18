/******************************************************************************
    Copyright (C) 2019 by Dillon Pentz <dillon@vodbox.io>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "util/threading.h"
#include "util/dstr.h"
#include "obs-missing-files.h"
#include "obs.h"
#include "obs-mem.h"

struct obs_missing_file {
	struct obs_mem_header hdr;
	char *file_path;
	obs_missing_file_cb callback;
	int src_type;
	void *src;
	char *src_name;
	void *data;
};

struct obs_missing_files {
	DARRAY(struct obs_missing_file *) files;
};

obs_missing_files_t *obs_missing_files_create()
{
	struct obs_missing_files *files = bzalloc(sizeof(struct obs_missing_files));

	return files;
}

void obs_missing_files_destroy(obs_missing_files_t *files)
{
	for (size_t i = 0; i < files->files.num; i++) {
		obs_missing_file_release(files->files.array[i]);
	}

	da_free(files->files);
	bfree(files);
}

void obs_missing_files_add_file(obs_missing_files_t *files, obs_missing_file_t *file)
{
	da_insert(files->files, files->files.num, &file);
}

size_t obs_missing_files_count(obs_missing_files_t *files)
{
	return files->files.num;
}

obs_missing_file_t *obs_missing_files_get_file(obs_missing_files_t *files, int idx)
{
	return files->files.array[idx];
}

void obs_missing_files_append(obs_missing_files_t *dst, obs_missing_files_t *src)
{
	for (size_t i = 0; i < src->files.num; i++) {
		obs_missing_file_t *file = src->files.array[i];
		obs_missing_files_add_file(dst, file);
		obs_mem_acquire(file);
	}
}

static void obs_missing_file_destroy_cb(void *file)
{
	obs_missing_file_destroy(file);
}

obs_missing_file_t *obs_missing_file_create(const char *path, obs_missing_file_cb callback, int src_type, void *src,
					    void *data)
{
	struct obs_missing_file *file = bzalloc(sizeof(struct obs_missing_file));

	file->file_path = bstrdup(path);
	file->callback = callback;
	file->src_type = src_type;
	file->src = src;
	file->data = data;
	obs_mem_init(file, obs_missing_file_destroy_cb);

	switch (src_type) {
	case OBS_MISSING_FILE_SOURCE:
		file->src_name = bstrdup(obs_source_get_name(src));
		break;
	case OBS_MISSING_FILE_SCRIPT:
		break;
	}

	return file;
}

void obs_missing_file_release(obs_missing_file_t *file)
{
	if (!file)
		return;

	obs_mem_release(file);
}

void obs_missing_file_destroy(obs_missing_file_t *file)
{
	switch (file->src_type) {
	case OBS_MISSING_FILE_SOURCE:
		bfree(file->src_name);
		break;
	case OBS_MISSING_FILE_SCRIPT:
		break;
	}
	bfree(file->file_path);
	obs_mem_finalize(file);
	bfree(file);
}

void obs_missing_file_issue_callback(obs_missing_file_t *file, const char *new_path)
{
	switch (file->src_type) {
	case OBS_MISSING_FILE_SOURCE:
		obs_source_replace_missing_file(file->callback, (obs_source_t *)file->src, new_path, file->data);
		break;
	case OBS_MISSING_FILE_SCRIPT:
		break;
	}
}

const char *obs_missing_file_get_path(obs_missing_file_t *file)
{
	return file->file_path;
}

const char *obs_missing_file_get_source_name(obs_missing_file_t *file)
{
	return file->src_name;
}
