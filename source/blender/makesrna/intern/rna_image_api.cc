/* SPDX-FileCopyrightText: 2009 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 */

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>

#include "BLI_path_utils.hh"

#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "BKE_packedFile.hh"

#include "rna_internal.hh" /* own include */

#ifdef RNA_RUNTIME

#  include "BLI_listbase.h"
#  include "BLI_math_base.h"
#  include "BLI_string.h"
#  include "BLI_vector.hh"

#  include "BKE_context.hh"
#  include "BKE_image.hh"
#  include "BKE_image_format.hh"
#  include "BKE_image_paint_selection.hh"
#  include "BKE_image_save.hh"
#  include "BKE_library.hh"
#  include "BKE_main.hh"
#  include "BKE_report.hh"
#  include "BKE_scene.hh"

#  include "IMB_imbuf.hh"

#  include "DNA_image_types.h"
#  include "DNA_scene_types.h"

#  include "MEM_guardedalloc.h"

#  include "WM_api.hh"

namespace blender {

static void rna_ImagePackedFile_save(ImagePackedFile *imapf, Main *bmain, ReportList *reports)
{
  if (BKE_packedfile_write_to_file(
          reports, BKE_main_blendfile_path(bmain), imapf->filepath, imapf->packedfile) != RET_OK)
  {
    BKE_reportf(reports, RPT_ERROR, "Could not save packed file to disk as '%s'", imapf->filepath);
  }
}

static void rna_Image_save_render(Image *image,
                                  bContext *C,
                                  ReportList *reports,
                                  const char *path,
                                  Scene *scene,
                                  const int quality)
{
  Main *bmain = CTX_data_main(C);

  if (scene == nullptr) {
    scene = CTX_data_scene(C);
  }

  ImageSaveOptions opts;

  if (BKE_image_save_options_init(&opts, bmain, scene, image, nullptr, false, true)) {
    opts.save_copy = true;
    STRNCPY(opts.filepath, path);
    if (quality != 0) {
      opts.im_format.quality = clamp_i(quality, 0, 100);
    }

    if (!BKE_image_save(reports, bmain, image, nullptr, &opts)) {
      BKE_reportf(
          reports, RPT_ERROR, "Image '%s' could not be saved to '%s'", image->id.name + 2, path);
    }
  }
  else {
    BKE_reportf(reports, RPT_ERROR, "Image '%s' does not have any image data", image->id.name + 2);
  }

  BKE_image_save_options_free(&opts);

  WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, image);
}

static void rna_Image_save(Image *image,
                           Main *bmain,
                           bContext *C,
                           ReportList *reports,
                           const char *path,
                           const int quality,
                           const bool save_copy)
{
  Scene *scene = CTX_data_scene(C);
  ImageSaveOptions opts;

  if (BKE_image_save_options_init(&opts, bmain, scene, image, nullptr, false, false)) {
    if (path && path[0]) {
      STRNCPY(opts.filepath, path);
    }
    if (quality != 0) {
      opts.im_format.quality = clamp_i(quality, 0, 100);
    }
    opts.save_copy = save_copy;
    if (!BKE_image_save(reports, bmain, image, nullptr, &opts)) {
      BKE_reportf(reports,
                  RPT_ERROR,
                  "Image '%s' could not be saved to '%s'",
                  image->id.name + 2,
                  image->filepath);
    }
  }
  else {
    BKE_reportf(reports, RPT_ERROR, "Image '%s' does not have any image data", image->id.name + 2);
  }

  BKE_image_save_options_free(&opts);

  WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, image);
}

static void rna_Image_pack(
    Image *image, Main *bmain, bContext *C, ReportList *reports, const char *data, int data_len)
{
  BKE_image_packfile_ensure(bmain, image, reports, data, data_len);
  WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, image);
}

static void rna_Image_unpack(Image *image, Main *bmain, ReportList *reports, int method)
{
  if (!BKE_image_has_packedfile(image)) {
    BKE_report(reports, RPT_ERROR, "Image not packed");
    return;
  }

  if (!ID_IS_EDITABLE(&image->id)) {
    BKE_report(reports, RPT_ERROR, "Image is not editable");
    return;
  }

  if (ELEM(image->source, IMA_SRC_MOVIE, IMA_SRC_SEQUENCE)) {
    BKE_report(reports, RPT_ERROR, "Unpacking movies or image sequences not supported");
    return;
  }

  /* reports its own error on failure */
  BKE_packedfile_unpack_image(bmain, reports, image, ePF_FileStatus(method));
}

static void rna_Image_reload(Image *image, Main *bmain)
{
  BKE_image_signal(bmain, image, nullptr, IMA_SIGNAL_RELOAD);
  WM_main_add_notifier(NC_IMAGE | NA_EDITED, image);
}

static void rna_Image_update(Image *image, ReportList *reports)
{
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, nullptr);

  if (ibuf == nullptr) {
    BKE_reportf(reports, RPT_ERROR, "Image '%s' does not have any image data", image->id.name + 2);
    return;
  }

  if (ibuf->byte_data()) {
    IMB_byte_from_float(ibuf);
  }

  ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
  BKE_image_partial_update_mark_full_update(image);

  BKE_image_release_ibuf(image, ibuf, nullptr);
}

static void rna_Image_scale(
    Image *image, ReportList *reports, int width, int height, int frame, int tile_index)
{
  ImageUser iuser{};
  BKE_imageuser_default(&iuser);
  iuser.framenr = frame;
  if (image->source == IMA_SRC_TILED) {
    const ImageTile *tile = static_cast<ImageTile *>(BLI_findlink(&image->tiles, tile_index));
    if (tile != nullptr) {
      iuser.tile = tile->tile_number;
    }
  }

  if (!BKE_image_scale(image, width, height, &iuser)) {
    BKE_reportf(reports, RPT_ERROR, "Image '%s' failed to load image buffer", image->id.name + 2);
    return;
  }
  BKE_image_partial_update_mark_full_update(image);
  WM_main_add_notifier(NC_IMAGE | NA_EDITED, image);
}

static int rna_Image_gl_load(
    Image *image, ReportList *reports, int frame, int layer_index, int pass_index)
{
  ImageUser iuser;
  BKE_imageuser_default(&iuser);
  iuser.framenr = frame;
  iuser.layer = layer_index;
  iuser.pass = pass_index;
  if (image->rr != nullptr) {
    BKE_image_multilayer_index(image->rr, &iuser);
  }

  gpu::Texture *tex = BKE_image_get_gpu_texture(image, &iuser);

  if (tex == nullptr) {
    BKE_reportf(reports, RPT_ERROR, "Failed to load image texture '%s'", image->id.name + 2);
    /* TODO(fclem): this error code makes no sense for vulkan. */
    return 0x0502; /* GL_INVALID_OPERATION */
  }

  return 0; /* GL_NO_ERROR */
}

static int rna_Image_gl_touch(
    Image *image, ReportList *reports, int frame, int layer_index, int pass_index)
{
  int error = 0; /* GL_NO_ERROR */

  BKE_image_tag_time(image);

  if (image->runtime->gputexture[TEXTARGET_2D][0] == nullptr) {
    error = rna_Image_gl_load(image, reports, frame, layer_index, pass_index);
  }

  return error;
}

static void rna_Image_gl_free(Image *image)
{
  BKE_image_free_gputextures(image);

  /* Remove the no-collect flag, image is available for garbage collection again. */
  image->flag &= ~IMA_NOCOLLECT;
}

static void rna_Image_filepath_from_user(Image *image, ImageUser *image_user, char *filepath)
{
  BKE_image_user_file_path(image_user, image, filepath);
}

static void rna_Image_buffers_free(Image *image)
{
  BKE_image_free_buffers_ex(image, true);
}

static void rna_Image_get_selection_bounding_box(Image *image,
                                                 ReportList * /*reports*/,
                                                 int tile_number,
                                                 int r_bbox[4],
                                                 bool *r_has_selection)
{
  if (!image->runtime) {
    *r_has_selection = false;
    r_bbox[0] = r_bbox[1] = r_bbox[2] = r_bbox[3] = 0;
    return;
  }

  int r_min[2], r_max[2];
  if (BKE_image_paint_selection_mask_bounds(image, tile_number, r_min, r_max)) {
    *r_has_selection = true;
    r_bbox[0] = r_min[0];
    r_bbox[1] = r_min[1];
    r_bbox[2] = r_max[0];
    r_bbox[3] = r_max[1];
  }
  else {
    *r_has_selection = false;
    r_bbox[0] = r_bbox[1] = r_bbox[2] = r_bbox[3] = 0;
  }
}

/* -------------------------------------------------------------------- */
/** \name Paint Selection Mask Python API (runtime-only, no undo)
 * \{ */

static void rna_Image_get_selection_tiles(Image *image,
                                          ReportList * /*reports*/,
                                          int **r_tiles,
                                          int *r_tiles_len)
{
  blender::Vector<int> tile_nums;
  for (const ImageTile &tile : image->tiles) {
    /* Presence check only: the const overload keeps the query from invalidating the mask caches. */
    if (BKE_image_paint_selection_mask_lookup(const_cast<const Image *>(image), tile.tile_number) !=
        nullptr)
    {
      tile_nums.append(tile.tile_number);
    }
  }
  *r_tiles_len = int(tile_nums.size());
  if (tile_nums.is_empty()) {
    *r_tiles = nullptr;
    return;
  }
  *r_tiles = MEM_new_array_uninitialized<int>(tile_nums.size(), __func__);
  memcpy(*r_tiles, tile_nums.data(), tile_nums.size() * sizeof(int));
}

static void rna_Image_get_selection_mask(Image *image,
                                         ReportList *reports,
                                         int tile_number,
                                         float **r_mask,
                                         int *r_mask_len,
                                         int *r_width,
                                         int *r_height)
{
  *r_mask = nullptr;
  *r_mask_len = 0;
  *r_width = 0;
  *r_height = 0;

  /* The mask is only copied out, so the const overload avoids advancing the revision. */
  const ImBuf *mask = BKE_image_paint_selection_mask_lookup(const_cast<const Image *>(image),
                                                            tile_number);
  if (mask == nullptr) {
    return;
  }

  const float *src = mask->float_buffer.data;
  if (src == nullptr) {
    BKE_reportf(reports, RPT_ERROR, "Selection mask for tile %d has no float buffer", tile_number);
    return;
  }

  const int w = mask->x;
  const int h = mask->y;
  const int total = w * h;
  float *out = MEM_new_array_uninitialized<float>(total, __func__);
  memcpy(out, src, sizeof(float) * total);
  *r_mask = out;
  *r_mask_len = total;
  *r_width = w;
  *r_height = h;
}

static void rna_Image_get_selection_pixels(Image *image,
                                           ReportList *reports,
                                           int tile_number,
                                           float **r_pixels,
                                           int *r_pixels_len,
                                           int *r_x,
                                           int *r_y,
                                           int *r_width,
                                           int *r_height,
                                           int *r_channels)
{
  *r_pixels = nullptr;
  *r_pixels_len = 0;
  *r_x = *r_y = *r_width = *r_height = *r_channels = 0;

  int origin[2] = {0, 0};
  int size[2] = {0, 0};
  ImBuf *fragment = BKE_image_paint_selection_extract_pixels(
      image, tile_number, nullptr, origin, size, nullptr);

  if (fragment == nullptr) {
    return;
  }

  const int w = fragment->x;
  const int h = fragment->y;
  float *out = nullptr;
  int actual_channels = 0;

  if (fragment->float_buffer.data != nullptr) {
    actual_channels = fragment->channels ? fragment->channels : 4;
    const int total = w * h * actual_channels;
    out = MEM_new_array_uninitialized<float>(total, __func__);
    memcpy(out, fragment->float_buffer.data, sizeof(float) * total);
    *r_pixels_len = total;
  }
  else if (fragment->byte_buffer.data != nullptr) {
    /* Byte buffer is always 4-channel RGBA. */
    actual_channels = 4;
    const int total = w * h * 4;
    out = MEM_new_array_uninitialized<float>(total, __func__);
    IMB_buffer_float_from_byte(out, fragment->byte_buffer.data, w, h, w, w);
    *r_pixels_len = total;
  }
  else {
    BKE_reportf(reports, RPT_ERROR, "Image tile %d has no pixel buffer", tile_number);
    IMB_freeImBuf(fragment);
    return;
  }

  *r_pixels = out;
  *r_x = origin[0];
  *r_y = origin[1];
  *r_width = w;
  *r_height = h;
  *r_channels = actual_channels;
  IMB_freeImBuf(fragment);
}

static void rna_Image_set_selection_mask(Image *image,
                                         ReportList *reports,
                                         int tile_number,
                                         const float *mask,
                                         int mask_len,
                                         int width,
                                         int height)
{
  if (mask == nullptr || mask_len <= 0) {
    BKE_report(reports, RPT_ERROR, "No mask data provided");
    return;
  }
  if (width <= 0 || height <= 0) {
    BKE_report(reports, RPT_ERROR, "Invalid mask dimensions");
    return;
  }
  if (mask_len != width * height) {
    BKE_reportf(reports,
                RPT_ERROR,
                "Mask length %d does not match width * height = %d",
                mask_len,
                width * height);
    return;
  }
  if (!image->runtime) {
    BKE_report(reports, RPT_ERROR, "Image has no runtime data");
    return;
  }

  /* BKE_image_paint_selection_mask_get creates or resizes the mask to the requested dimensions. */
  ImBuf *mask_ibuf = BKE_image_paint_selection_mask_get(image, tile_number, width, height);
  if (mask_ibuf == nullptr) {
    BKE_reportf(reports, RPT_ERROR, "Failed to get selection mask for tile %d", tile_number);
    return;
  }

  float *dst = mask_ibuf->float_data_for_write();
  if (dst == nullptr) {
    BKE_report(reports, RPT_ERROR, "Selection mask has no float buffer");
    return;
  }

  memcpy(dst, mask, sizeof(float) * mask_len);
  WM_main_add_notifier(NC_IMAGE | NA_EDITED, image);
}

static void rna_Image_write_region(Image *image,
                                   ReportList *reports,
                                   int tile_number,
                                   int x,
                                   int y,
                                   int width,
                                   int height,
                                   const float *pixels,
                                   int pixels_len,
                                   int channels,
                                   const float *mask,
                                   int mask_len)
{
  if (pixels == nullptr || pixels_len <= 0) {
    BKE_report(reports, RPT_ERROR, "No pixel data provided");
    return;
  }
  if (width <= 0 || height <= 0) {
    BKE_report(reports, RPT_ERROR, "Invalid region dimensions");
    return;
  }
  if (pixels_len < width * height * channels) {
    BKE_reportf(reports,
                RPT_ERROR,
                "Pixel array too small: expected %d elements, got %d",
                width * height * channels,
                pixels_len);
    return;
  }
  if (mask != nullptr && mask_len > 0 && mask_len < width * height) {
    BKE_reportf(reports,
                RPT_ERROR,
                "Mask array too small: expected %d elements, got %d",
                width * height,
                mask_len);
    return;
  }

  const float *mask_ptr = (mask != nullptr && mask_len > 0) ? mask : nullptr;
  if (!BKE_image_paint_selection_write_region(
          image, tile_number, nullptr, pixels, channels, x, y, width, height, mask_ptr))
  {
    BKE_reportf(reports, RPT_ERROR, "Failed to write region to image tile %d", tile_number);
    return;
  }
  WM_main_add_notifier(NC_IMAGE | NA_EDITED, image);
}

static void rna_Image_clear_selection_mask(Image *image,
                                           ReportList * /*reports*/,
                                           int tile_number,
                                           bool all_tiles)
{
  if (all_tiles) {
    BKE_image_paint_selection_mask_free(image);
  }
  else {
    BKE_image_paint_selection_mask_tile_free(image, tile_number);
  }
  WM_main_add_notifier(NC_IMAGE | NA_EDITED, image);
}

/** \} */

}  // namespace blender

#else

namespace blender {

void RNA_api_image_packed_file(StructRNA *srna)
{
  FunctionRNA *func;

  func = RNA_def_function(srna, "save", "rna_ImagePackedFile_save");
  RNA_def_function_ui_description(func, "Save the packed file to its filepath");
  RNA_def_function_flag(func, FUNC_USE_MAIN | FUNC_USE_REPORTS);
}

void RNA_api_image(StructRNA *srna)
{
  FunctionRNA *func;
  PropertyRNA *parm;

  func = RNA_def_function(srna, "save_render", "rna_Image_save_render");
  RNA_def_function_ui_description(func,
                                  "Save image to a specific path using a scenes render settings");
  RNA_def_function_flag(func, FUNC_USE_CONTEXT | FUNC_USE_REPORTS);
  parm = RNA_def_string_file_path(func, "filepath", nullptr, 0, "", "Output path");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  RNA_def_pointer(func, "scene", "Scene", "", "Scene to take image parameters from");
  RNA_def_int(func,
              "quality",
              0,
              0,
              100,
              "Quality",
              "Quality for image formats that support lossy compression, uses default quality if "
              "not specified",
              0,
              100);

  func = RNA_def_function(srna, "save", "rna_Image_save");
  RNA_def_function_ui_description(func, "Save image");
  RNA_def_function_flag(func, FUNC_USE_MAIN | FUNC_USE_CONTEXT | FUNC_USE_REPORTS);
  RNA_def_string_file_path(func,
                           "filepath",
                           nullptr,
                           0,
                           "",
                           "Output path, uses image data-block filepath if not specified");
  RNA_def_int(func,
              "quality",
              0,
              0,
              100,
              "Quality",
              "Quality for image formats that support lossy compression, uses default quality if "
              "not specified",
              0,
              100);
  RNA_def_boolean(func,
                  "save_copy",
                  false,
                  "Save Copy",
                  "Save the image as a copy, without updating current image's filepath");

  func = RNA_def_function(srna, "pack", "rna_Image_pack");
  RNA_def_function_ui_description(func, "Pack an image as embedded data into the .blend file");
  RNA_def_function_flag(func, FUNC_USE_MAIN | FUNC_USE_CONTEXT | FUNC_USE_REPORTS);
  parm = RNA_def_property(func, "data", PROP_STRING, PROP_BYTESTRING);
  RNA_def_property_ui_text(parm, "data", "Raw data (bytes, exact content of the embedded file)");
  RNA_def_int(func,
              "data_len",
              0,
              0,
              INT_MAX,
              "data_len",
              "length of given data (mandatory if data is provided)",
              0,
              INT_MAX);

  func = RNA_def_function(srna, "unpack", "rna_Image_unpack");
  RNA_def_function_ui_description(func, "Save an image packed in the .blend file to disk");
  RNA_def_function_flag(func, FUNC_USE_MAIN | FUNC_USE_REPORTS);
  RNA_def_enum(
      func, "method", rna_enum_unpack_method_items, PF_USE_LOCAL, "method", "How to unpack");

  func = RNA_def_function(srna, "reload", "rna_Image_reload");
  RNA_def_function_flag(func, FUNC_USE_MAIN);
  RNA_def_function_ui_description(func, "Reload the image from its source path");

  func = RNA_def_function(srna, "update", "rna_Image_update");
  RNA_def_function_ui_description(func, "Update the display image from the floating-point buffer");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);

  func = RNA_def_function(srna, "scale", "rna_Image_scale");
  RNA_def_function_ui_description(func, "Scale the buffer of the image, in pixels");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  parm = RNA_def_int(func, "width", 1, 1, INT_MAX, "", "Width", 1, INT_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_int(func, "height", 1, 1, INT_MAX, "", "Height", 1, INT_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  RNA_def_int(func, "frame", 0, 0, INT_MAX, "Frame", "Frame (for image sequences)", 0, INT_MAX);
  RNA_def_int(
      func, "tile_index", 0, 0, INT_MAX, "Tile", "Tile index (for tiled images)", 0, INT_MAX);

  func = RNA_def_function(
      srna, "get_selection_bounding_box", "rna_Image_get_selection_bounding_box");
  RNA_def_function_ui_description(
      func,
      "Get the bounding box in pixels of the active paint selection mask for the given UDIM tile");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  RNA_def_int(func, "tile", 1001, 1001, 2000, "Tile", "UDIM tile number", 1001, 2000);
  parm = RNA_def_int_array(func, "bbox", 4, nullptr, 0, INT_MAX, "Bounding Box",
                           "Pixel bounding box [x_min, y_min, x_max, y_max]", 0, INT_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_OUTPUT);
  parm = RNA_def_boolean(func, "has_selection", false, "Has Selection",
                         "True if there is an active selection on the tile");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_OUTPUT);

  func = RNA_def_function(srna, "gl_touch", "rna_Image_gl_touch");
  RNA_def_function_ui_description(
      func, "Delay the image from being cleaned from the cache due inactivity");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  RNA_def_int(
      func, "frame", 0, 0, INT_MAX, "Frame", "Frame of image sequence or movie", 0, INT_MAX);
  RNA_def_int(func,
              "layer_index",
              0,
              0,
              INT_MAX,
              "Layer",
              "Index of layer that should be loaded",
              0,
              INT_MAX);
  RNA_def_int(func,
              "pass_index",
              0,
              0,
              INT_MAX,
              "Pass",
              "Index of pass that should be loaded",
              0,
              INT_MAX);
  /* return value */
  parm = RNA_def_int(
      func, "error", 0, INT_MIN, INT_MAX, "Error", "OpenGL error value", INT_MIN, INT_MAX);
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "gl_load", "rna_Image_gl_load");
  RNA_def_function_ui_description(
      func,
      "Load the image into an OpenGL texture. On success, image.bindcode will contain the "
      "OpenGL texture bindcode. Colors read from the texture will be in scene linear color space "
      "and have premultiplied or straight alpha matching the image alpha mode.");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  RNA_def_int(
      func, "frame", 0, 0, INT_MAX, "Frame", "Frame of image sequence or movie", 0, INT_MAX);
  RNA_def_int(func,
              "layer_index",
              0,
              0,
              INT_MAX,
              "Layer",
              "Index of layer that should be loaded",
              0,
              INT_MAX);
  RNA_def_int(func,
              "pass_index",
              0,
              0,
              INT_MAX,
              "Pass",
              "Index of pass that should be loaded",
              0,
              INT_MAX);
  /* return value */
  parm = RNA_def_int(
      func, "error", 0, INT_MIN, INT_MAX, "Error", "OpenGL error value", INT_MIN, INT_MAX);
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "gl_free", "rna_Image_gl_free");
  RNA_def_function_ui_description(func, "Free the image from OpenGL graphics memory");

  /* path to an frame specified by image user */
  func = RNA_def_function(srna, "filepath_from_user", "rna_Image_filepath_from_user");
  RNA_def_function_ui_description(
      func,
      "Return the absolute path to the filepath of an image frame specified by the image user");
  RNA_def_pointer(
      func, "image_user", "ImageUser", "", "Image user of the image to get filepath for");
  parm = RNA_def_string_file_path(func,
                                  "filepath",
                                  nullptr,
                                  FILE_MAX,
                                  "File Path",
                                  "The resulting filepath from the image and its user");
  RNA_def_parameter_flags(
      parm, PROP_THICK_WRAP, ParameterFlag(0)); /* needed for string return value */
  RNA_def_function_output(func, parm);

  func = RNA_def_function(srna, "buffers_free", "rna_Image_buffers_free");
  RNA_def_function_ui_description(func, "Free the image buffers from memory");

  /* Paint selection mask API (runtime-only, no undo). */

  func = RNA_def_function(srna, "get_selection_tiles", "rna_Image_get_selection_tiles");
  RNA_def_function_ui_description(
      func, "Get the UDIM tile numbers that currently have a paint selection mask");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  parm = RNA_def_int_array(
      func, "tiles", 1, nullptr, 1001, 2000, "Tiles", "Tile numbers with a selection mask", 1001, 2000);
  RNA_def_parameter_flags(parm, PROP_DYNAMIC, PARM_OUTPUT);

  func = RNA_def_function(srna, "get_selection_mask", "rna_Image_get_selection_mask");
  RNA_def_function_ui_description(
      func, "Get the full selection mask buffer for the given UDIM tile as a flat float array");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  RNA_def_int(func, "tile", 1001, 1001, 2000, "Tile", "UDIM tile number", 1001, 2000);
  parm = RNA_def_float_array(func,
                             "mask",
                             1,
                             nullptr,
                             0.0f,
                             1.0f,
                             "Mask",
                             "Flat float array of mask values [0..1], row-major (bottom to top)",
                             0.0f,
                             1.0f);
  RNA_def_parameter_flags(parm, PROP_DYNAMIC, PARM_OUTPUT);
  parm = RNA_def_int(func, "width", 0, 0, INT_MAX, "Width", "Mask width in pixels", 0, INT_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_OUTPUT);
  parm = RNA_def_int(
      func, "height", 0, 0, INT_MAX, "Height", "Mask height in pixels", 0, INT_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_OUTPUT);

  func = RNA_def_function(srna, "get_selection_pixels", "rna_Image_get_selection_pixels");
  RNA_def_function_ui_description(
      func,
      "Extract pixel data from the bounding box of the active paint selection on the given tile");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  RNA_def_int(func, "tile", 1001, 1001, 2000, "Tile", "UDIM tile number", 1001, 2000);
  parm = RNA_def_float_array(func,
                             "pixels",
                             1,
                             nullptr,
                             -FLT_MAX,
                             FLT_MAX,
                             "Pixels",
                             "Flat float pixel array, row-major (bottom to top)",
                             -FLT_MAX,
                             FLT_MAX);
  RNA_def_parameter_flags(parm, PROP_DYNAMIC, PARM_OUTPUT);
  parm = RNA_def_int(
      func, "x", 0, 0, INT_MAX, "X", "Left edge of the extracted region in tile space", 0, INT_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_OUTPUT);
  parm = RNA_def_int(func,
                     "y",
                     0,
                     0,
                     INT_MAX,
                     "Y",
                     "Bottom edge of the extracted region in tile space",
                     0,
                     INT_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_OUTPUT);
  parm = RNA_def_int(func,
                     "width",
                     0,
                     0,
                     INT_MAX,
                     "Width",
                     "Width of the extracted region in pixels",
                     0,
                     INT_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_OUTPUT);
  parm = RNA_def_int(func,
                     "height",
                     0,
                     0,
                     INT_MAX,
                     "Height",
                     "Height of the extracted region in pixels",
                     0,
                     INT_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_OUTPUT);
  parm = RNA_def_int(
      func, "channels", 0, 0, 4, "Channels", "Number of channels per pixel", 0, 4);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_OUTPUT);

  func = RNA_def_function(srna, "set_selection_mask", "rna_Image_set_selection_mask");
  RNA_def_function_ui_description(
      func,
      "Write a new selection mask buffer for the given UDIM tile. "
      "Does not push an undo step. The mask must contain width * height float values");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  RNA_def_int(func, "tile", 1001, 1001, 2000, "Tile", "UDIM tile number", 1001, 2000);
  parm = RNA_def_float_array(func,
                             "mask",
                             1,
                             nullptr,
                             0.0f,
                             1.0f,
                             "Mask",
                             "Flat float array of mask values [0..1], row-major; "
                             "size must equal width * height",
                             0.0f,
                             1.0f);
  RNA_def_parameter_flags(parm, PROP_DYNAMIC, PARM_REQUIRED);
  parm = RNA_def_int(
      func, "width", 1, 1, INT_MAX, "Width", "Mask width in pixels", 1, INT_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_int(
      func, "height", 1, 1, INT_MAX, "Height", "Mask height in pixels", 1, INT_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);

  func = RNA_def_function(srna, "write_region", "rna_Image_write_region");
  RNA_def_function_ui_description(
      func,
      "Write pixel data into a rectangular region of the given UDIM tile. "
      "Does not push an undo step");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  RNA_def_int(func, "tile", 1001, 1001, 2000, "Tile", "UDIM tile number", 1001, 2000);
  parm = RNA_def_int(
      func, "x", 0, 0, INT_MAX, "X", "Left edge of the destination region in tile space", 0, INT_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_int(func,
                     "y",
                     0,
                     0,
                     INT_MAX,
                     "Y",
                     "Bottom edge of the destination region in tile space",
                     0,
                     INT_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_int(
      func, "width", 1, 1, INT_MAX, "Width", "Region width in pixels", 1, INT_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_int(
      func, "height", 1, 1, INT_MAX, "Height", "Region height in pixels", 1, INT_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_float_array(func,
                             "pixels",
                             1,
                             nullptr,
                             -FLT_MAX,
                             FLT_MAX,
                             "Pixels",
                             "Flat float pixel array, row-major; size = width * height * channels",
                             -FLT_MAX,
                             FLT_MAX);
  RNA_def_parameter_flags(parm, PROP_DYNAMIC, PARM_REQUIRED);
  parm = RNA_def_int(func,
                     "channels",
                     4,
                     1,
                     4,
                     "Channels",
                     "Number of channels per pixel; must match the image buffer",
                     1,
                     4);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_float_array(func,
                             "mask",
                             1,
                             nullptr,
                             0.0f,
                             1.0f,
                             "Mask",
                             "Optional 1-channel blend mask [0..1], same width * height size. "
                             "When provided: dst = dst * (1 - mask) + pixels * mask",
                             0.0f,
                             1.0f);
  RNA_def_parameter_flags(parm, PROP_DYNAMIC, ParameterFlag(0));

  func = RNA_def_function(srna, "clear_selection_mask", "rna_Image_clear_selection_mask");
  RNA_def_function_ui_description(
      func, "Clear the paint selection mask for one tile or all tiles");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  RNA_def_int(func, "tile", 1001, 1001, 2000, "Tile", "UDIM tile number (ignored when all_tiles is true)", 1001, 2000);
  RNA_def_boolean(func,
                  "all_tiles",
                  false,
                  "All Tiles",
                  "Clear the selection mask on all UDIM tiles instead of just the given tile");

  /* TODO: pack/unpack, maybe should be generic functions? */
}

}  // namespace blender

#endif
