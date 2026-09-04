/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * The seam between the Stack Layers display mode and whatever data it is showing.
 *
 * A "stack" here is the shape a lot of Blender data already has and no editor presents as such: an
 * ordered list of layers, bottom to top, each with a name, an on/off state, a value that modulates
 * it, and sometimes a mode that says how it combines with what is below. A material's paint layers
 * are one; shape keys are another; modifiers, mask layers and grease pencil layers are the same
 * shape again.
 *
 * The display mode owns the tree, the persistence, the columns and the operators. A #StackSource
 * owns nothing but the answer to "what are the rows, and what happens when one is clicked". That
 * split is what lets a second kind of stack arrive as one new file rather than as a second copy of
 * the Outliner plumbing -- and it is why the rows below carry no #Image, no #Material and no node
 * identity: a row is a name, a value and a list of sub-rows, whatever produced it.
 */

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "DNA_space_enums.h"

#include "RNA_types.hh"

struct bContext;
struct ID;
struct Main;
struct Object;
struct Scene;
struct SpaceOutliner;
struct ViewLayer;

namespace blender::ed::outliner {

/**
 * A data-block a row is made of: a channel's map for a paint layer, and whatever plays the same
 * part for another source. Sources with nothing to show below a row simply leave this empty.
 */
struct StackSubRow {
  /** Source-defined role, and the sub-row's share of the tree-store key. Must stay below
   * #STACK_ROW_SUB_ROW_STRIDE. */
  int role = 0;
  std::string name;
  /** The data-block the sub-row stands for, or null. Only used for display and activation. */
  ID *id = nullptr;
  int icon = 0;
};

/** One row of a stack, bottom to top. */
struct StackRow {
  /**
   * Position in the rebuilt model, and the row's identity for the tree store.
   *
   * Deterministic rather than persistent: it survives a rebuild of an unchanged stack, which is
   * what collapsed and selected state needs, and is recomputed when the stack itself changes --
   * which is exactly when it stops being enough for that: an edit that shifts rows past this one
   * gives it a new ordinal, and a *different* row already sitting at that number the last time the
   * tree was built leaves its own collapsed or selected state behind for this one to inherit. See
   * #stable_id for what survives that.
   */
  int16_t ordinal = 0;
  int depth = 0;
  /** Ordinal of the enclosing group row, or -1 at the top level. */
  int16_t parent_ordinal = -1;
  bool is_group = false;
  /**
   * The row's identity across an edit that renumbers it, nil when the source has none to give.
   *
   * #ordinal is only a position, recomputed by every rebuild; this is what lets a caller ask "is
   * this the same row I had open a moment ago" when an edit moved it instead, and carry its
   * collapsed or selected state across rather than have it settle on whichever row now happens to
   * share its old ordinal.
   */
  bUUID stable_id = {};
  /**
   * The row is the stack's own base rather than a layer laid over something.
   *
   * Such a row has no blend mode, opacity or mute of its own, so the controls that would edit those
   * are left off it. A stack of the current shape has none: its lowest row blends over transparency
   * like every other, which is what lets a layer be put below it.
   */
  bool is_bare_base = false;
  bool enabled = true;
  /**
   * False when the source recognized something it cannot represent. Such a row is still listed --
   * a layer the user cannot see is worse than one it cannot edit -- but carries no controls.
   */
  bool supported = true;
  const char *unsupported_reason = nullptr;

  std::string name;
  /**
   * Where a rename types, or null when the row cannot be renamed.
   *
   * #name is what the row reads as and is rebuilt with the rows; this is the source's own storage
   * for it, at least #MAX_NAME bytes, which outlives a rebuild. The Outliner's in-row rename field
   * writes here directly, the way it writes into a data-block's name everywhere else -- a field
   * pointed at a copy would lose every keystroke to the next rebuild.
   */
  char *name_buffer = nullptr;
  int icon = 0;

  /** Property drawn in the value column, typically an opacity or an influence. */
  std::optional<PointerRNA> value_ptr;
  const char *value_prop = nullptr;
  /** Property drawn in the mode column, typically a blend mode. */
  std::optional<PointerRNA> mode_ptr;
  const char *mode_prop = nullptr;

  Vector<StackSubRow> sub_rows;
};

/**
 * Sub-rows share the layer's #TreeStoreElem.nr, which is a short.
 *
 * The key of a sub-row is `ordinal * STACK_ROW_SUB_ROW_STRIDE + role`, so a fixed stride bounds
 * both how many roles a source may use and how deep a stack the Outliner can keep state for. A
 * collision here does not misdraw anything, it swaps which row remembers being open, which is the
 * kind of bug that is never reported and never reproduced.
 */
constexpr int STACK_ROW_SUB_ROW_STRIDE = 16;

/**
 * Columns reserved at the right of a stack row for its own toggles: the visibility toggle.
 *
 * A fixed count rather than one derived per row, because the columns have to line up down the
 * whole list.
 */
constexpr int STACK_ROW_ICON_COLUMNS = 1;
constexpr int STACK_ROW_ORDINAL_MAX = (32767 / STACK_ROW_SUB_ROW_STRIDE) - 1;

/**
 * What a source may read while the tree is being built.
 *
 * Deliberately not a #bContext: building a tree is a pure function of the file and the space,
 * and a source that could reach the context would be able to read the active area, the running
 * operator or the mouse position -- none of which a row may depend on if the tree is to come out
 * the same however the rebuild was triggered.
 */
/**
 * What kind of row #StackSource::row_add creates.
 *
 * Deliberately coarse: the seam names the two things any stack of layers has a notion of -- an
 * empty layer and one that starts out covering what is below it -- and leaves the details (which
 * channels, what size, which color) to the source.
 */
/** Where a moved row lands relative to the row it was aimed at. */
enum class StackMovePlace : int8_t {
  /** Directly above the anchor, among its siblings. */
  Above = 0,
  /** Directly below the anchor, among its siblings. */
  Below,
  /**
   * Inside the anchor, which has to be a row that holds others: on top of what it holds.
   *
   * Named separately from "above its topmost row" because a row that holds nothing yet has no row
   * to name, and an empty group is exactly what a folder is when it is made to be filled.
   */
  Into,
};

enum class StackAddKind : int8_t {
  /** The plain one. Every source that supports adding at all supports this. */
  Empty = 0,
  /** A layer that is opaque from the start. Sources without such a notion may refuse it. */
  Fill = 1,
};

struct StackReadContext {
  Main *bmain = nullptr;
  Scene *scene = nullptr;
  ViewLayer *view_layer = nullptr;
};

/** What the space is currently pointed at. Sources resolve their own owner from it. */
struct StackFocus {
  Object *object = nullptr;
  /** Source-defined sub-selection within the object, such as a material slot. -1 means active. */
  int sub_index = -1;
};

/** Widths, in UI units, of the two right-hand columns. Zero hides the column. */
struct StackColumnLayout {
  int value_width = 0;
  int mode_width = 0;
};

/**
 * One kind of stack the display mode can show.
 *
 * Implementations are stateless with respect to the tree: they are asked to describe the rows and
 * to act on one, never to remember which is selected. Whatever runtime state activation needs --
 * the paint source has a paint-target owner, for instance -- belongs to the implementation and is
 * dropped through #undo_reset.
 */
class StackSource {
 public:
  virtual ~StackSource() = default;

  virtual eSpaceOutliner_StackSource type() const = 0;
  virtual StringRefNull ui_name() const = 0;

  /** Whether \a object contributes a stack of this kind, for the object overview. */
  virtual bool object_has_stack(const StackReadContext &ctx, Object &object) const = 0;

  /** The data-block whose stack \a focus resolves to, or null when there is nothing to show. */
  virtual ID *owner_get(const StackReadContext &ctx, const StackFocus &focus) const = 0;

  /**
   * The sub-selections #StackFocus::sub_index can take on \a focus's object, in index order.
   *
   * What a sub-index *means* is the source's own business -- a material slot for paint layers,
   * nothing at all for shape keys -- so this is how the UI offers the choice without knowing.
   * Leaving it empty means the object has one stack and no choice to make.
   */
  virtual void sub_selection_names(const StackReadContext & /*ctx*/,
                                   const StackFocus & /*focus*/,
                                   Vector<std::string> & /*r_names*/) const
  {
  }

  /**
   * A value that changes whenever #rows_build would produce something different.
   *
   * Must be cheap: it is computed on every tree rebuild, and the whole point of it is to avoid
   * rebuilding a model that did not change.
   */
  virtual uint64_t state_hash(const StackReadContext &ctx, const ID &owner) const = 0;

  virtual bool rows_build(const StackReadContext &ctx,
                          const StackFocus &focus,
                          ID &owner,
                          Vector<StackRow> &r_rows) const = 0;

  /** Whether the stack may be edited at all: not linked, not an override. */
  virtual bool is_editable(const ID &owner) const = 0;

  virtual StackColumnLayout column_layout() const = 0;

  /** Make the row the thing the rest of Blender acts on. */
  virtual bool row_activate(bContext &C,
                            const StackFocus &focus,
                            ID &owner,
                            int ordinal,
                            const StackRow &row) const = 0;
  virtual bool row_is_active(const StackReadContext &ctx,
                             const StackFocus &focus,
                             const ID &owner,
                             const StackRow &row) const = 0;

  /**
   * Whether the rows of \a owner can change places at all.
   *
   * Separate from #is_editable because order is not always editable even when values are: a paint
   * stack whose chain is shared with another node can have its opacity changed and not its order.
   */
  virtual bool can_reorder(const ID & /*owner*/) const
  {
    return false;
  }

  /** Move the row at \a from_ordinal so that it ends up at \a to_ordinal. */
  virtual bool row_reorder(bContext & /*C*/,
                           const StackFocus & /*focus*/,
                           ID & /*owner*/,
                           int /*from_ordinal*/,
                           int /*to_ordinal*/) const
  {
    return false;
  }

  /**
   * Whether a row can be created at all.
   *
   * A source that has its own "+" elsewhere in the UI leaves this alone rather than duplicating
   * it: shape keys are added from the Object Data properties, and a second button that does the
   * same thing in a different place is a second thing to keep in step.
   */
  virtual bool can_add(const ID & /*owner*/) const
  {
    return false;
  }

  /**
   * Create a row of \a kind so that it ends up at \a ordinal; -1 puts it on top.
   *
   * \return the ordinal of the new row, or -1 when nothing was created.
   */
  virtual int row_add(bContext & /*C*/,
                      const StackFocus & /*focus*/,
                      ID & /*owner*/,
                      StackAddKind /*kind*/,
                      int /*ordinal*/) const
  {
    return -1;
  }

  /**
   * Whether #row_set_enabled does anything.
   *
   * Not the same question as #is_editable for every source: turning a paint layer off mutates the
   * node graph, so it needs the same permissions as a reorder, while a source whose "enabled" is
   * a plain flag could allow it more freely.
   */
  virtual bool can_set_enabled(const ID & /*owner*/) const
  {
    return false;
  }

  /** Turn the row at \a ordinal on or off. */
  virtual bool row_set_enabled(bContext & /*C*/,
                               const StackFocus & /*focus*/,
                               ID & /*owner*/,
                               int /*ordinal*/,
                               bool /*enable*/) const
  {
    return false;
  }

  /**
   * Copy the row at \a ordinal, putting the copy above it.
   *
   * \return the ordinal of the copy, or -1 when nothing was created.
   */
  virtual int row_duplicate(bContext & /*C*/,
                            const StackFocus & /*focus*/,
                            ID & /*owner*/,
                            int /*ordinal*/) const
  {
    return -1;
  }

  /** Rename the row at \a ordinal. Sources whose rows are named by something else refuse. */
  virtual bool row_rename(bContext & /*C*/,
                          const StackFocus & /*focus*/,
                          ID & /*owner*/,
                          int /*ordinal*/,
                          StringRefNull /*name*/) const
  {
    return false;
  }

  /**
   * Whether the rows of \a owner can carry a mask -- something that modulates one row and is
   * itself editable, such as a paint layer's mask image or a shape key's vertex group.
   */
  virtual bool can_mask(const ID & /*owner*/) const
  {
    return false;
  }

  /** Give the row at \a ordinal a mask, or take its mask away. */
  virtual bool row_mask_set(bContext & /*C*/,
                            const StackFocus & /*focus*/,
                            ID & /*owner*/,
                            int /*ordinal*/,
                            bool /*add*/) const
  {
    return false;
  }

  /**
   * Wrap the rows \a from_ordinal .. \a to_ordinal into one group row.
   *
   * \return the ordinal of the group row, or -1 when nothing was made.
   */
  virtual int rows_group(bContext & /*C*/,
                         const StackFocus & /*focus*/,
                         ID & /*owner*/,
                         int /*from_ordinal*/,
                         int /*to_ordinal*/) const
  {
    return -1;
  }

  /**
   * Unwrap the group row at  ordinal back into the rows it holds.
   *
   * eturn how many rows came back, or -1 when the row is not a group this source can unwrap.
   */
  virtual int row_ungroup(bContext & /*C*/,
                          const StackFocus & /*focus*/,
                          ID & /*owner*/,
                          int /*ordinal*/) const
  {
    return -1;
  }

  virtual bool can_remove(const ID & /*owner*/) const
  {
    return false;
  }

  virtual bool row_remove(bContext & /*C*/,
                          const StackFocus & /*focus*/,
                          ID & /*owner*/,
                          int /*ordinal*/) const
  {
    return false;
  }

  /** Double-click on a sub-row. Sources with no sub-rows need not implement this. */
  virtual bool sub_row_activate(bContext & /*C*/,
                                const StackFocus & /*focus*/,
                                ID & /*owner*/,
                                const StackRow & /*row*/,
                                const StackSubRow & /*sub_row*/) const
  {
    return false;
  }

  /** Undo the effect of #row_activate on the rest of the file. */
  virtual bool target_clear(bContext & /*C*/) const
  {
    return false;
  }

  /**
   * Drop runtime state an undo step invalidated.
   *
   * Session UIDs survive undo but pointers do not, so anything a source remembers by address is
   * stale here even though it still looks valid.
   */
  virtual void undo_reset() const {}

  /**
   * Move the row at \a from_ordinal next to the row at \a anchor_ordinal.
   *
   * What a drop expresses: a place beside a row rather than a position in the list. A source that
   * nests its rows needs this, because "into that group, at the top" is not a position any row is
   * numbered by. Sources that only ever show a flat list can leave it to #row_reorder.
   *
   * \note Kept last on purpose. Every method here is virtual, so a new one added in the middle
   * renumbers the ones after it, and a caller left over from a partial build then jumps into the
   * wrong one.
   *
   * \param r_ordinal: when given, receives the ordinal the moved row has afterwards -- a move
   * renumbers rows past it, so a caller that wants to keep it selected needs this.
   */
  virtual bool row_move(bContext & /*C*/,
                        const StackFocus & /*focus*/,
                        ID & /*owner*/,
                        int /*from_ordinal*/,
                        int /*anchor_ordinal*/,
                        StackMovePlace /*place*/,
                        int * /*r_ordinal*/ = nullptr) const
  {
    return false;
  }

  /**
   * Add an empty group above the row at \a ordinal, moving nothing into it.
   *
   * Distinct from #rows_group, which makes a folder out of rows that already exist. Both are worth
   * having: one collects what is there, the other makes somewhere to put what comes next.
   *
   * \return the new group's ordinal, or -1 when nothing was created.
   */
  virtual int group_add(bContext & /*C*/,
                        const StackFocus & /*focus*/,
                        ID & /*owner*/,
                        int /*ordinal*/) const
  {
    return -1;
  }

  /**
   * Bring \a owner's graph into the shape the rest of this interface expects, before its rows are
   * read for the first time after a focus change.
   *
   * A stack built by hand, by an older tool version, or read back from a file that predates a
   * contract revision can still have a bare bottom row -- one with no Mix node, and so no Factor
   * for #StackRow::value_ptr to point at. Rows still read correctly either way, but a bare bottom
   * shows no value. Rather than rewriting the graph as a side effect of drawing it, the read side
   * calls this once per focus change, so the conversion is its own explicit, undoable step tied to
   * opening the stack, not a hidden effect of looking at it. Sources with no such legacy shape can
   * leave this as the no-op default.
   *
   * \return true when the graph actually changed, so the caller knows to push an undo step.
   */
  virtual bool normalize_for_read(bContext & /*C*/, ID & /*owner*/) const
  {
    return false;
  }
};

/* The built-in sources. Defined in their own files, listed by `outliner_stack_source.cc`. */
std::unique_ptr<StackSource> stack_source_paint_material_create();
std::unique_ptr<StackSource> stack_source_shape_keys_create();

/** Every source built into this Blender, in the order they are listed to the user. */
Span<const StackSource *> stack_sources_get();
/** The source \a type selects, never null: an unknown type falls back to the first source. */
const StackSource *stack_source_get(eSpaceOutliner_StackSource type);
/** The source a space is currently set to. */
const StackSource *stack_source_for_space(const SpaceOutliner &space_outliner);

}  // namespace blender::ed::outliner
