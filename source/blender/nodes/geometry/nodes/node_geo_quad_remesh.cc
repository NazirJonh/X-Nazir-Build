/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_mesh.hh"
#include "BKE_mesh_remesh_voxel.hh"

#include "GEO_foreach_geometry.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_quad_remesh_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Mesh"_ustr).supported_type(GeometryComponent::Type::Mesh);
  b.add_input<decl::Int>("Target Faces"_ustr)
      .default_value(4000)
      .min(1)
      .description("Approximate number of quads in the output");
  b.add_input<decl::Vector>("Guide Direction"_ustr)
      .subtype(PROP_DIRECTION)
      .default_value({0.0f, 0.0f, 1.0f})
      .evaluated_geometry_field()
      .description("Per-point direction that biases the edge flow of the result");
  b.add_input<decl::Float>("Guide Weight"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .evaluated_geometry_field()
      .description("How strongly the guide constrains each point (0 ignores it)");
  b.add_input<decl::Float>("Guide Scale"_ustr)
      .default_value(1.0f)
      .min(0.1f)
      .max(10.0f)
      .evaluated_geometry_field()
      .description(
          "Relative quad size per point: values below 1 give smaller quads (more detail), "
          "values above 1 larger quads");
  b.add_input<decl::Int>("Relax Iterations"_ustr)
      .default_value(2)
      .min(0)
      .max(20)
      .description(
          "Number of smoothing steps applied to the result while keeping it on the input "
          "surface, for more uniform quad shapes");
  b.add_input<decl::Bool>("Preserve Sharp"_ustr)
      .default_value(false)
      .description("Try to align the output topology with sharp edges");
  b.add_input<decl::Bool>("Preserve Boundary"_ustr)
      .default_value(false)
      .description("Try to preserve the mesh boundary");
  b.add_input<decl::Int>("Seed"_ustr).description(
      "Seed for the solver; different seeds give different quad layouts");
  b.add_output<decl::Geometry>("Mesh"_ustr);
}

/* QuadriFlow calls the progress callback unconditionally, so a no-op is required
 * even when the node does not report progress. */
static void quad_remesh_progress_noop(void * /*user_data*/, float /*progress*/, int * /*cancel*/) {}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Mesh"_ustr);
  const int target_faces = params.extract_input<int>("Target Faces"_ustr);
  const int seed = params.extract_input<int>("Seed"_ustr);
  const bool preserve_sharp = params.extract_input<bool>("Preserve Sharp"_ustr);
  const bool preserve_boundary = params.extract_input<bool>("Preserve Boundary"_ustr);
  Field<float3> guide_dir_field = params.extract_input<Field<float3>>("Guide Direction"_ustr);
  Field<float> guide_weight_field = params.extract_input<Field<float>>("Guide Weight"_ustr);
  Field<float> guide_scale_field = params.extract_input<Field<float>>("Guide Scale"_ustr);
  const int relax_iterations = params.extract_input<int>("Relax Iterations"_ustr);

  geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry) {
    const Mesh *mesh = geometry.get_mesh();
    if (mesh == nullptr) {
      return;
    }
    if (mesh->verts_num == 0 || mesh->faces_num == 0) {
      geometry.replace_mesh(nullptr);
      return;
    }

    /* Evaluate the procedural guide field on the input vertices, then flatten it
     * into the contiguous buffers the QuadriFlow wrapper expects. */
    const bke::MeshFieldContext field_context{*mesh, bke::AttrDomain::Point};
    fn::FieldEvaluator evaluator{field_context, mesh->verts_num};
    evaluator.add(guide_dir_field);
    evaluator.add(guide_weight_field);
    evaluator.add(guide_scale_field);
    evaluator.evaluate();
    const VArray<float3> guide_dir = evaluator.get_evaluated<float3>(0);
    const VArray<float> guide_weight = evaluator.get_evaluated<float>(1);
    const VArray<float> guide_scale = evaluator.get_evaluated<float>(2);

    Array<float3> guide_dir_buf(mesh->verts_num);
    guide_dir.materialize(guide_dir_buf);
    Array<float> guide_weight_buf(mesh->verts_num);
    guide_weight.materialize(guide_weight_buf);

    /* A constant neutral scale is the common case; skip the override so the
     * solver keeps its uniform density path. */
    Array<float> guide_scale_buf;
    const float *guide_scale_ptr = nullptr;
    const std::optional<float> single_scale = guide_scale.get_if_single();
    if (!(single_scale.has_value() && *single_scale == 1.0f)) {
      guide_scale_buf.reinitialize(mesh->verts_num);
      guide_scale.materialize(guide_scale_buf);
      guide_scale_ptr = guide_scale_buf.data();
    }

    Mesh *output = BKE_mesh_remesh_quadriflow(
        mesh,
        target_faces,
        seed,
        preserve_sharp,
        preserve_boundary,
        /*adaptive_scale*/ false,
        quad_remesh_progress_noop,
        nullptr,
        reinterpret_cast<const float *>(guide_dir_buf.data()),
        guide_weight_buf.data(),
        nullptr,
        guide_scale_ptr);

    if (output == nullptr) {
      geometry.replace_mesh(nullptr);
      return;
    }
    if (relax_iterations > 0) {
      bke::mesh_relax_reproject(
          *output, *mesh, relax_iterations, 0.5f, /*sharp_angle=30deg*/ 0.523599f);
    }
    geometry.replace_mesh(output);
  });

  params.set_output("Mesh"_ustr, std::move(geometry_set));
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeQuadRemesh"_ustr, GEO_NODE_QUAD_REMESH);
  ntype.ui_name = "Quad Remesh";
  ntype.ui_description =
      "Generate a quad mesh whose edge flow follows a guide direction field, using QuadriFlow";
  ntype.enum_name_legacy = "QUAD_REMESH";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_quad_remesh_cc
