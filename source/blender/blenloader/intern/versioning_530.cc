/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup blenloader
 */

#define DNA_DEPRECATED_ALLOW

#include "MEM_guardedalloc.h"

#include "DNA_ID.h"
#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BLI_listbase_iterator.hh"
#include "BLI_set.hh"
#include "BLI_sys_types.h"

#include "BKE_main.hh"
#include "BKE_multires.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"

#include "readfile.hh"

#include "versioning_common.hh"

#include "CLG_log.h"

namespace blender {

static CLG_LogRef LOG = {"blend.doversion"};

void do_versions_after_linking_530(FileData * /*fd*/, Main *bmain)
{
  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 503, 3)) {
    /* Re-encode multires displacement grids into the well-conditioned tangent space (see
     * #BKE_multires_construct_tangent_matrix). The conversion rewrites the mesh's shared
     * #CD_MDISPS in place, so each mesh must be converted exactly once: a mesh used by several
     * objects (linked duplicates) or an object carrying two Multires modifiers would otherwise be
     * decoded a second time with the legacy frames and permanently corrupt the displacement. */
    Set<const void *> converted_meshes;
    for (Object &ob : bmain->objects) {
      for (ModifierData &md : ob.modifiers) {
        if (md.type != eModifierType_Multires) {
          continue;
        }
        if (ob.data == nullptr || !converted_meshes.add(ob.data)) {
          continue;
        }
        MultiresModifierData *mmd = reinterpret_cast<MultiresModifierData *>(&md);
        multires_do_versions_tangent_space_conversion(&ob, mmd);
      }
    }
  }

  /**
   * Always bump subversion in BKE_blender_version.h when adding versioning
   * code here, and wrap it inside a MAIN_VERSION_FILE_ATLEAST check.
   *
   * \note Keep this message at the bottom of the function.
   */
}

void blo_do_versions_530(FileData * /*fd*/, Library * /*lib*/, Main *bmain)
{
  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 503, 4)) {
    /* Sculpt layer system V2: grid-domain sculpt layers switched from object-space displacement
     * deltas to tangent-space displacement stored in the MDisps grid layout. Old data cannot be
     * converted reliably (it was tied to runtime CCG snapshots), so wipe it. Vertex-domain layers
     * are unaffected. */
    bool warned = false;
    for (Mesh &mesh : bmain->meshes) {
      for (SculptLayer &layer : mesh.sculpt_layers) {
        if (layer.domain != SCULPT_LAYER_DOMAIN_GRID || layer.data == nullptr) {
          continue;
        }
        MEM_delete_void(layer.data);
        layer.data = nullptr;
        layer.totelem = 0;
        layer.level = 0;
        if (!warned) {
          CLOG_WARN(&LOG,
                    "Grid sculpt layer data from an older file version was cleared: the storage "
                    "format changed to tangent space.");
          warned = true;
        }
      }
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 503, 1)) {
    for (Scene &scene : bmain->scenes) {
      VPaint *wpaint = scene.toolsettings->wpaint;
      if (wpaint) {
        const StringRefNull old_asset_id =
            wpaint->paint.brush_asset_reference->relative_asset_identifier;
        if (wpaint->paint.brush == nullptr && old_asset_id.endswith("Paint")) {
          /* The "Paint" brush asset was renamed to "Add Weight", find it via the default instead
           * of hardcoding the new name. */
          if (std::optional<AssetWeakReference> paint_brush_asset_reference =
                  BKE_paint_brush_type_default_reference(PaintMode::Weight,
                                                         WPAINT_BRUSH_TYPE_DRAW))
          {
            BKE_paint_brush_set(bmain, &wpaint->paint, *paint_brush_asset_reference);
          }
        }
      }
    }
  }
  /**
   * Always bump subversion in BKE_blender_version.h when adding versioning
   * code here, and wrap it inside a MAIN_VERSION_FILE_ATLEAST check.
   *
   * \note Keep this message at the bottom of the function.
   */
}

}  // namespace blender
