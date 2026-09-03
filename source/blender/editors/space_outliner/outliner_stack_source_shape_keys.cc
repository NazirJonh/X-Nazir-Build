/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * Shape keys as a stack.
 *
 * The second source, and the reason the first one is behind an interface at all: shape keys share
 * nothing with paint layers except their shape. They are a real list rather than a node graph,
 * their value is a property on the data-block rather than a socket, they have no sub-rows and no
 * blend mode, and activating one writes #Object.shapenr instead of scene paint settings.
 *
 * Everything above #StackSource -- the tree, the ordinals, the persistence, the columns, the
 * operators -- is unchanged by any of that, which is the property the seam exists to have.
 */

#include "DNA_key_types.h"
#include "DNA_object_types.h"

#include "BKE_context.hh"
#include "BKE_key.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"

#include "BLI_hash.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_string.h"

#include "DEG_depsgraph.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "outliner_stack_source.hh"

namespace blender::ed::outliner {

namespace {

Key *shape_keys_get(const StackFocus &focus)
{
  return focus.object != nullptr ? BKE_key_from_object(focus.object) : nullptr;
}

class ShapeKeyStackSource final : public StackSource {
 public:
  eSpaceOutliner_StackSource type() const override
  {
    return SO_STACK_SRC_SHAPE_KEYS;
  }

  StringRefNull ui_name() const override
  {
    return "Shape Keys";
  }

  bool object_has_stack(const StackReadContext & /*ctx*/, Object &object) const override
  {
    const Key *key = BKE_key_from_object(&object);
    return key != nullptr && !key->block.is_empty();
  }

  ID *owner_get(const StackReadContext & /*ctx*/, const StackFocus &focus) const override
  {
    Key *key = shape_keys_get(focus);
    return key != nullptr && !key->block.is_empty() ? &key->id : nullptr;
  }

  uint64_t state_hash(const StackReadContext & /*ctx*/, const ID &owner) const override
  {
    const Key &key = reinterpret_cast<const Key &>(owner);
    uint64_t hash = 0;
    for (const KeyBlock &block : key.block) {
      /* Order and names, not values: a value change redraws, it does not rebuild the tree. */
      hash = hash * 31u + get_default_hash(StringRef(block.name));
    }
    return hash;
  }

  bool rows_build(const StackReadContext & /*ctx*/,
                  const StackFocus & /*focus*/,
                  ID &owner,
                  Vector<StackRow> &r_rows) const override
  {
    Key &key = reinterpret_cast<Key &>(owner);
    int16_t ordinal = 0;
    for (KeyBlock &block : key.block) {
      if (ordinal > STACK_ROW_ORDINAL_MAX) {
        break;
      }
      StackRow row;
      row.ordinal = ordinal;
      row.name = block.name;
      row.enabled = (block.flag & KEYBLOCK_MUTE) == 0;
      row.icon = ICON_SHAPEKEY_DATA;
      /* The Basis has nothing to interpolate towards, so it has no value to show. */
      if (&block != key.block.first) {
        row.value_ptr = RNA_pointer_create_discrete(&key.id, RNA_ShapeKey, &block);
        row.value_prop = "value";
      }
      r_rows.append(std::move(row));
      ordinal++;
    }
    return !r_rows.is_empty();
  }

  bool is_editable(const ID &owner) const override
  {
    return ID_IS_EDITABLE(&owner) && !ID_IS_OVERRIDE_LIBRARY(&owner);
  }

  StackColumnLayout column_layout() const override
  {
    /* No blend mode: a shape key mixes into the mesh one way only. */
    return {4, 0};
  }

  bool row_activate(bContext &C,
                    const StackFocus &focus,
                    ID & /*owner*/,
                    const int ordinal,
                    const StackRow & /*row*/) const override
  {
    if (focus.object == nullptr) {
      return false;
    }
    focus.object->shapenr = short(ordinal + 1);
    DEG_id_tag_update(&focus.object->id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(&C, NC_OBJECT | ND_DATA, &focus.object->id);
    return true;
  }

  bool row_is_active(const StackReadContext & /*ctx*/,
                     const StackFocus &focus,
                     const ID & /*owner*/,
                     const StackRow &row) const override
  {
    return focus.object != nullptr && focus.object->shapenr - 1 == row.ordinal;
  }

  bool can_reorder(const ID &owner) const override
  {
    return this->is_editable(owner);
  }

  bool row_reorder(bContext &C,
                   const StackFocus &focus,
                   ID & /*owner*/,
                   const int from_ordinal,
                   const int to_ordinal) const override
  {
    /* Shape keys reorder by moving one list entry, and #BKE_keyblock_move already fixes up the
     * relative-key references that a move would otherwise break. Nothing about the Outliner side
     * of a reorder differs from the paint stack's; that is the point of the seam. */
    if (focus.object == nullptr) {
      return false;
    }
    if (!BKE_keyblock_move(focus.object, from_ordinal, to_ordinal)) {
      return false;
    }
    DEG_id_tag_update(&focus.object->id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(&C, NC_OBJECT | ND_DATA, &focus.object->id);
    return true;
  }
};

}  // namespace

std::unique_ptr<StackSource> stack_source_shape_keys_create()
{
  return std::make_unique<ShapeKeyStackSource>();
}

}  // namespace blender::ed::outliner
