/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spfile
 */

#include "DNA_asset_types.h"
#include "DNA_ID.h"

#include "BKE_asset.hh"

#include "AS_asset_library.hh"
#include "AS_asset_representation.hh"

#include "BLI_listbase.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"

#include "ED_asset_image_library.hh"

#include <cstdio>

#include "filelist_intern.hh"
#include "filelist_readjob.hh"

namespace blender {

struct ImageFilelistReadData {
  FileListReadJob *job_params;
  const bool *stop;
  ListBaseT<FileListInternEntry> entries = {nullptr};
  int entries_num = 0;
};

static bool filelist_readjob_image_file_callback(void *userdata,
                                                 const char * /*library_root*/,
                                                 const char *relative_image_path,
                                                 const char *image_name,
                                                 const bUUID &catalog_id)
{
  ImageFilelistReadData *data = static_cast<ImageFilelistReadData *>(userdata);
  if (*data->stop) {
    return false;
  }

  FileListReadJob *job_params = data->job_params;
  if (!job_params->load_asset_library) {
    return false;
  }

  auto metadata = std::make_unique<AssetMetaData>();
  metadata->catalog_id = catalog_id;
  metadata->preferred_import_method = ASSET_IMPORT_APPEND_REUSE;
  metadata->flag |= ASSETDATA_USE_OWN_IMPORT_METHOD;

  FileListInternEntry *entry = MEM_new<FileListInternEntry>(__func__);
  /* Paths in the image index are relative to the library root, not #FileListReadJob::cur_relbase. */
  entry->relpath = BLI_strdup(relative_image_path);
  entry->typeflag = FILE_TYPE_BLENDERLIB | FILE_TYPE_ASSET | FILE_TYPE_IMAGE;
  entry->blentype = ID_IM;
  entry->asset = job_params->load_asset_library->add_external_on_disk_asset(
      relative_image_path, image_name, ID_IM, std::move(metadata));

  if (job_params->on_asset_added) {
    if (asset_system::AssetRepresentation *asset = entry->get_asset()) {
      (*job_params->on_asset_added)(*asset);
    }
  }

  BLI_addtail(&data->entries, entry);
  data->entries_num++;
  return true;
}

void filelist_readjob_ensure_image_library_indexed(FileListReadJob *job_params)
{
  FileList *filelist = job_params->tmp_filelist;
  const char *root = filelist->filelist.root;
  if (!root[0]) {
    return;
  }
  if (!ed::asset::image_library_needs_reindex(root)) {
    return;
  }

  const int indexed = ed::asset::image_library_scan_and_index(
      root, job_params->load_asset_library);
  if (indexed <= 0) {
    return;
  }

  job_params->reload_asset_library = true;
  bool dummy_update = false;
  filelist_readjob_load_asset_library_data(job_params, &dummy_update);
}

void filelist_readjob_image_files_add_items(FileListReadJob *job_params,
                                            const bool *stop,
                                            bool *do_update,
                                            float * /*progress*/)
{
  if (!job_params->load_asset_library) {
    printf("[IMG_ASSET_DROP] image_files_add_items: no load_asset_library\n");
    fflush(stdout);
    return;
  }
  if (job_params->load_asset_library->library_type() == ASSET_LIBRARY_ALL) {
    printf("[IMG_ASSET_DROP] image_files_add_items: skip ALL library\n");
    fflush(stdout);
    return;
  }

  FileList *filelist = job_params->tmp_filelist;
  const char *root = filelist->filelist.root;
  if (!root[0]) {
    printf("[IMG_ASSET_DROP] image_files_add_items: empty root\n");
    fflush(stdout);
    return;
  }

  printf("[IMG_ASSET_DROP] image_files_add_items: root=\"%s\"\n", root);
  fflush(stdout);

  ImageFilelistReadData data{};
  data.job_params = job_params;
  data.stop = stop;

  if (!ed::asset::image_library_foreach_image(
          root, filelist_readjob_image_file_callback, &data))
  {
    printf("[IMG_ASSET_DROP] image_files_add_items: image_library_foreach_image FAILED\n");
    fflush(stdout);
    return;
  }

  printf("[IMG_ASSET_DROP] image_files_add_items: indexed image count=%d\n", data.entries_num);
  fflush(stdout);

  if (data.entries_num == 0) {
    return;
  }

  char name_buff[FILE_MAX];
  for (FileListInternEntry &entry : data.entries) {
    entry.uid = filelist_uid_generate(filelist);
    if (!entry.name) {
      entry.name = fileentry_uiname(root, &entry, name_buff);
      entry.free_name = true;
    }
  }

  if (filelist_readjob_append_entries(job_params, &data.entries, data.entries_num)) {
    *do_update = true;
  }
}

}  // namespace blender
