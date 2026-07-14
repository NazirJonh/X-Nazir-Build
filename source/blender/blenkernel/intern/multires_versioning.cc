/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "CLG_log.h"

#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"

#include "BKE_subdiv.hh"
#include "BKE_subdiv_eval.hh"

#include "multires_reshape.hh"
#include "opensubdiv_converter_capi.hh"
#include "subdiv_converter.hh"

static CLG_LogRef LOG = {"bke.multires"};

namespace blender {

#ifdef WITH_OPENSUBDIV

static float simple_to_catmull_clark_get_edge_sharpness(const OpenSubdiv_Converter * /*converter*/,
                                                        int /*manifold_edge_index*/)
{
  return 10.0f;
}

static bool simple_to_catmull_clark_is_infinite_sharp_vertex(
    const OpenSubdiv_Converter * /*converter*/, int /*manifold_vertex_index*/)
{
  return true;
}

static bke::subdiv::Subdiv *subdiv_for_simple_to_catmull_clark(Object *object,
                                                               MultiresModifierData *mmd)
{
  using namespace blender::bke;
  subdiv::Settings subdiv_settings;
  BKE_multires_subdiv_settings_init(&subdiv_settings, mmd);

  const Mesh *base_mesh = id_cast<const Mesh *>(object->data);

  OpenSubdiv_Converter converter;
  subdiv::converter_init_for_mesh(&converter, &subdiv_settings, base_mesh);
  converter.getEdgeSharpness = simple_to_catmull_clark_get_edge_sharpness;
  converter.isInfiniteSharpVertex = simple_to_catmull_clark_is_infinite_sharp_vertex;

  subdiv::Subdiv *subdiv = subdiv::new_from_converter(&subdiv_settings, &converter);
  subdiv::converter_free(&converter);

  if (!subdiv::eval_begin_from_mesh(subdiv, base_mesh, subdiv::SUBDIV_EVALUATOR_TYPE_CPU)) {
    subdiv::free(subdiv);
    return nullptr;
  }

  return subdiv;
}

#endif

void multires_do_versions_tangent_space_conversion(Object *object, MultiresModifierData *mmd)
{
#ifdef WITH_OPENSUBDIV
  const Mesh *base_mesh = id_cast<const Mesh *>(object->data);
  if (base_mesh->corners_num == 0) {
    return;
  }

  /* Decode the displacement grids into object space using the legacy tangent matrices. */
  {
    bke::subdiv::Settings subdiv_settings;
    BKE_multires_subdiv_settings_init(&subdiv_settings, mmd);
    bke::subdiv::Subdiv *subdiv = bke::subdiv::new_from_mesh(&subdiv_settings, base_mesh);
    if (subdiv == nullptr) {
      CLOG_ERROR(&LOG,
                 "Multires tangent-space versioning of mesh '%s' skipped: subdivision could not "
                 "be created; grids stay in the legacy encoding and will be decoded incorrectly.",
                 base_mesh->id.name + 2);
      return;
    }
    if (!bke::subdiv::eval_begin_from_mesh(
            subdiv, base_mesh, bke::subdiv::SUBDIV_EVALUATOR_TYPE_CPU))
    {
      bke::subdiv::free(subdiv);
      CLOG_ERROR(&LOG,
                 "Multires tangent-space versioning of mesh '%s' skipped: subdivision evaluation "
                 "failed; grids stay in the legacy encoding and will be decoded incorrectly.",
                 base_mesh->id.name + 2);
      return;
    }
    MultiresReshapeContext reshape_context;
    if (!multires_reshape_context_create_from_subdiv(
            &reshape_context, object, mmd, subdiv, mmd->totlvl))
    {
      bke::subdiv::free(subdiv);
      CLOG_ERROR(&LOG,
                 "Multires tangent-space versioning of mesh '%s' skipped: reshape context "
                 "creation failed; grids stay in the legacy encoding.",
                 base_mesh->id.name + 2);
      return;
    }

    multires_reshape_store_original_grids(&reshape_context);
    multires_reshape_assign_final_coords_from_mdisps_for_versioning(&reshape_context);
    multires_reshape_context_free(&reshape_context);

    bke::subdiv::free(subdiv);
  }

  /* Re-encode the object space coordinates with the well-conditioned tangent matrices. */
  {
    MultiresReshapeContext reshape_context;
    if (!multires_reshape_context_create_from_modifier(&reshape_context, object, mmd, mmd->totlvl))
    {
      CLOG_ERROR(&LOG,
                 "Multires tangent-space versioning of mesh '%s' partially applied: re-encode "
                 "context creation failed; grids may be left in object space.",
                 base_mesh->id.name + 2);
      return;
    }
    multires_reshape_object_grids_to_tangent_displacement(&reshape_context);
    multires_reshape_context_free(&reshape_context);
  }
#else
  CLOG_WARN(&LOG,
            "Multires tangent-space versioning skipped: this build has no OpenSubdiv; multires "
            "displacement from older files may be decoded incorrectly until re-saved by a build "
            "with OpenSubdiv.");
  UNUSED_VARS(object, mmd);
#endif
}

void multires_do_versions_simple_to_catmull_clark(Object *object, MultiresModifierData *mmd)
{
#ifdef WITH_OPENSUBDIV
  const Mesh *base_mesh = id_cast<const Mesh *>(object->data);
  if (base_mesh->corners_num == 0) {
    return;
  }

  /* Store the grids displacement in object space against the simple limit surface. */
  {
    bke::subdiv::Subdiv *subdiv = subdiv_for_simple_to_catmull_clark(object, mmd);
    MultiresReshapeContext reshape_context;
    if (!multires_reshape_context_create_from_subdiv(
            &reshape_context, object, mmd, subdiv, mmd->totlvl))
    {
      bke::subdiv::free(subdiv);
      return;
    }

    multires_reshape_store_original_grids(&reshape_context);
    multires_reshape_assign_final_coords_from_mdisps(&reshape_context);
    multires_reshape_context_free(&reshape_context);

    bke::subdiv::free(subdiv);
  }

  /* Calculate the new tangent displacement against the new Catmull-Clark limit surface. */
  {
    MultiresReshapeContext reshape_context;
    if (!multires_reshape_context_create_from_modifier(&reshape_context, object, mmd, mmd->totlvl))
    {
      return;
    }
    multires_reshape_object_grids_to_tangent_displacement(&reshape_context);
    multires_reshape_context_free(&reshape_context);
  }
#else
  UNUSED_VARS(object, mmd);
#endif
}

}  // namespace blender
