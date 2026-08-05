/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup blenloader
 */
#include "BLO_readfile.hh"

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math_matrix.h"
#include "BLI_set.hh"
#include "BLI_string.h"

#include "BKE_main.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"

#include "DNA_ID.h"
#include "DNA_object_types.h"

namespace blender {

/**
 * Recursively resolves `ob`'s world matrix, computing its parent first if needed.
 * #BKE_object_where_is_calc_mat4() reads the parent's #Object::object_to_world() to solve
 * parenting, so parents must be resolved before their children or the child's matrix would be
 * missing the parent's contribution. `visiting` guards against cyclic parenting in corrupt data.
 *
 * NOTE: temp-loaded objects are never depsgraph-evaluated, so this only resolves #PAROBJECT
 * parenting correctly. Bone (#PARBONE), vertex (#PARVERT1/#PARVERT3), and curve-path (path
 * parenting via #ob_parcurve) contributions read pose-channel/mesh-deform/curve-path data that is
 * never populated on an un-evaluated temp object; those cases fall back to an identity
 * contribution for the parent link instead of the deformed one.
 */
static void temp_object_world_compute(Object &ob, Set<Object *> &done, Set<Object *> &visiting)
{
  if (done.contains(&ob)) {
    return;
  }
  if (!visiting.add(&ob)) {
    /* Already being resolved higher up the call stack: cyclic parenting. Bail out and leave this
     * object's contribution out rather than recursing forever. */
    return;
  }

  if (ob.parent) {
    temp_object_world_compute(*ob.parent, done, visiting);
  }

  float mat[4][4];
  BKE_object_where_is_calc_mat4(&ob, mat);
  copy_m4_m4(ob.runtime->object_to_world.ptr(), mat);
  done.add(&ob);
  visiting.remove(&ob);
}

/**
 * Library-linked objects only get their DNA loc/rot/scale restored; #Object::object_to_world()
 * reads a runtime-only cache that is never serialized and defaults to identity (see
 * #object_blend_read_data). Without this, every temp-loaded object reports the same identity
 * world matrix, collapsing collection-asset previews onto a single origin.
 */
static void temp_main_update_object_transforms(Main &bmain)
{
  Set<Object *> done;
  Set<Object *> visiting;
  for (Object &ob : bmain.objects) {
    temp_object_world_compute(ob, done, visiting);
  }
}

TempLibraryContext *BLO_library_temp_load_id(Main *real_main,
                                             const char *blend_file_path,
                                             const short idcode,
                                             const char *idname,
                                             ReportList *reports)
{
  TempLibraryContext *temp_lib_ctx = MEM_new_zeroed<TempLibraryContext>(__func__);
  temp_lib_ctx->bmain_base = BKE_main_new();
  temp_lib_ctx->bf_reports.reports = reports;

  /* Copy the file path so any path remapping is performed properly. */
  STRNCPY(temp_lib_ctx->bmain_base->filepath, real_main->filepath);

  BlendHandle *blendhandle = BLO_blendhandle_from_file(blend_file_path, &temp_lib_ctx->bf_reports);

  LibraryLink_Params lib_link_params;
  BLO_library_link_params_init(&lib_link_params, temp_lib_ctx->bmain_base, 0, ID_TAG_TEMP_MAIN);

  Main *bmain_lib = BLO_library_link_begin(&blendhandle, blend_file_path, &lib_link_params);

  temp_lib_ctx->temp_id = BLO_library_link_named_part(
      bmain_lib, &blendhandle, idcode, idname, &lib_link_params);

  BLO_library_link_end(bmain_lib, &blendhandle, &lib_link_params, reports);
  BLO_blendhandle_close(blendhandle);

  temp_main_update_object_transforms(*temp_lib_ctx->bmain_base);

  return temp_lib_ctx;
}

void BLO_library_temp_free(TempLibraryContext *temp_lib_ctx)
{
  BKE_main_free(temp_lib_ctx->bmain_base);
  MEM_delete(temp_lib_ctx);
}

}  // namespace blender
