/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_node_types.h"

#include "BLI_hash.hh"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_task.hh"

#include "BKE_attribute.hh"
#include "BKE_geometry_fields.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_3d_view_selection_cc {

NODE_STORAGE_FUNCS(NodeGeometry3DViewSelection)

/**
 * Field input that converts stored element IDs to a boolean selection field.
 */
class Selection3DViewFieldInput final : public bke::GeometryFieldInput {
 private:
  Span<int> selected_ids_;
  AttrDomain domain_;

 public:
  Selection3DViewFieldInput(const NodeGeometry3DViewSelection &storage)
      : bke::GeometryFieldInput(CPPType::get<bool>(), "3D View Selection")
  {
    if (storage.selected_ids != nullptr && storage.selected_ids_num > 0) {
      selected_ids_ = Span<int>(storage.selected_ids, storage.selected_ids_num);
    }
    domain_ = AttrDomain(storage.domain);
    category_ = Category::Generated;
  }

  GVArray get_varray_for_context(const bke::GeometryFieldContext &context,
                                  const IndexMask & /*mask*/) const override
  {
    const AttrDomain domain = context.domain();
    const AttributeAccessor attributes = *context.attributes();
    const int64_t size = attributes.domain_size(domain);

    /* Return all false if domain doesn't match */
    if (domain != domain_) {
      return VArray<bool>::from_single(false, size);
    }

    /* If no selection stored, return all false */
    if (selected_ids_.is_empty()) {
      return VArray<bool>::from_single(false, size);
    }

    /* Get ID attribute */
    const bke::AttributeReader<int> ids = attributes.lookup<int>("id", domain);
    if (!ids || ids.varray.is_empty()) {
      /* No ID attribute - return all false */
      return VArray<bool>::from_single(false, size);
    }

    /* Build selection from IDs */
    const Set<int> selected_set(selected_ids_);
    Array<bool> selection(size);

    const VArray<int> &ids_varray = ids.varray;
    threading::parallel_for(IndexRange(size), 4096, [&](const IndexRange range) {
      for (const int i : range) {
        selection[i] = selected_set.contains(ids_varray[i]);
      }
    });

    return VArray<bool>::from_container(std::move(selection));
  }

  uint64_t hash() const override
  {
    return get_default_hash(selected_ids_.size(), domain_);
  }

  bool is_equal_to(const fn::FieldNode &other) const override
  {
    if (const auto *other_input = dynamic_cast<const Selection3DViewFieldInput *>(&other))
    {
      return selected_ids_ == other_input->selected_ids_ && domain_ == other_input->domain_;
    }
    return false;
  }
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_default_layout();

  b.add_input<decl::Geometry>("Geometry")
      .description("Geometry to select elements from");
  b.add_output<decl::Geometry>("Geometry")
      .propagate_all()
      .align_with_previous();
  b.add_output<decl::Bool>("Selection")
      .field_source()
      .description("Selection as a boolean field");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);

  layout.prop(ptr, "attribute_name", UI_ITEM_NONE, IFACE_("Attribute"), ICON_NONE);
  layout.prop(ptr, "domain", UI_ITEM_NONE, IFACE_("Domain"), ICON_NONE);
  layout.prop(ptr, "mode", UI_ITEM_NONE, IFACE_("Mode"), ICON_NONE);

  layout.separator();

  /* Action buttons */
  ui::Layout &row = layout.row(true);
  row.op("NODE_OT_gn_selection_enter", IFACE_("Enter Selection Mode"), ICON_NONE);
  row.op("NODE_OT_gn_selection_clear", IFACE_("Clear"), ICON_X);

  layout.separator();

  /* Status display */
  const bNode &node = *static_cast<const bNode *>(ptr->data);
  const NodeGeometry3DViewSelection &storage = node_storage(node);

  char status[128];
  if (storage.selected_ids_num > 0) {
    SNPRINTF(status, "%d elements selected", storage.selected_ids_num);
  }
  else {
    STRNCPY(status, "No selection");
  }
  layout.label(status, ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometry3DViewSelection *data = MEM_new<NodeGeometry3DViewSelection>(__func__);

  STRNCPY(data->attribute_name, "selection_3d");
  data->domain = int8_t(AttrDomain::Face);
  data->mode = 0;       /* Input mode */
  data->storage_mode = 0; /* ID-based */
  data->version = 1;

  node->storage = data;
}

static void node_free_storage(bNode *node)
{
  if (node->storage == nullptr) {
    return;
  }

  NodeGeometry3DViewSelection *storage =
      static_cast<NodeGeometry3DViewSelection *>(node->storage);

  if (storage->selected_ids != nullptr) {
    MEM_delete_void(static_cast<void *>(storage->selected_ids));
  }
  if (storage->selected_indices != nullptr) {
    MEM_delete_void(static_cast<void *>(storage->selected_indices));
  }

  MEM_delete(storage);
  node->storage = nullptr;
}

static void node_copy_storage(bNodeTree * /*tree*/, bNode *dst_node, const bNode *src_node)
{
  if (src_node->storage == nullptr) {
    dst_node->storage = nullptr;
    return;
  }

  const NodeGeometry3DViewSelection &src =
      *static_cast<const NodeGeometry3DViewSelection *>(src_node->storage);

  NodeGeometry3DViewSelection *dst = MEM_new<NodeGeometry3DViewSelection>(__func__,
                                                                           dna::shallow_copy(src));

  /* Deep copy selected_ids array */
  dst->selected_ids = nullptr;
  dst->selected_ids_num = 0;
  if (src.selected_ids != nullptr && src.selected_ids_num > 0) {
    dst->selected_ids = MEM_new_array<int>(src.selected_ids_num, __func__);
    memcpy(dst->selected_ids, src.selected_ids, src.selected_ids_num * sizeof(int));
    dst->selected_ids_num = src.selected_ids_num;
  }

  /* Deep copy selected_indices array */
  dst->selected_indices = nullptr;
  dst->selected_indices_num = 0;
  if (src.selected_indices != nullptr && src.selected_indices_num > 0) {
    dst->selected_indices = MEM_new_array<int>(src.selected_indices_num, __func__);
    memcpy(dst->selected_indices, src.selected_indices, src.selected_indices_num * sizeof(int));
    dst->selected_indices_num = src.selected_indices_num;
  }

  dst_node->storage = dst;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry = params.extract_input<GeometrySet>("Geometry");
  const NodeGeometry3DViewSelection &storage = node_storage(params.node());

  /* Create selection field */
  Field<bool> selection_field{std::make_shared<Selection3DViewFieldInput>(storage)};

  /* Validate attribute name */
  const std::string attr_name = storage.attribute_name;
  if (attr_name.empty()) {
    params.error_message_add(NodeWarningType::Error,
                              TIP_("Attribute name cannot be empty"));
    params.set_output("Geometry", std::move(geometry));
    params.set_output("Selection", fn::make_constant_field<bool>(false));
    return;
  }

  /* Note: ID attribute check is done by the Selection3DViewFieldInput at evaluation time */

  params.set_output("Geometry", std::move(geometry));
  params.set_output("Selection", std::move(selection_field));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNode3DViewSelection", GEO_NODE_3D_VIEW_SELECTION);

  ntype.ui_name = "3D View Selection";
  ntype.ui_description =
      "Interactively select elements from generated geometry in the 3D Viewport";
  ntype.enum_name_legacy = "3D_VIEW_SELECTION";
  ntype.nclass = NODE_CLASS_INPUT;

  bke::node_type_storage(ntype,
                         "NodeGeometry3DViewSelection",
                         node_free_storage,
                         node_copy_storage);

  bke::node_type_size(ntype, 180, 140, 300);
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.geometry_node_execute = node_geo_exec;

  bke::node_register_type(ntype);
}

NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_3d_view_selection_cc
