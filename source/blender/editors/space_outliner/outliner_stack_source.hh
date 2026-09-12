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
 * the Outliner plumbing -- and it is why the rows below name no domain data-block and no node
 * identity: a row is a name, a value and the content it expands into, whatever produced it.
 *
 * How the optional vocabulary describes another source, so the model stays honest to more than the
 * one implementation that exists: shape keys are rows with no preview slots and no sections at all
 * (#keeps_row_icon never comes up); a grease pencil layer group is a row whose own mask slot
 * declares #StackRowPreview::keeps_row_icon, exactly the paint source's folder does; a modifier
 * stack needs none of the slots -- a row is its name, its value and its mute. None of those call
 * for a change in the generic code that reads these structures; if adding the next source does,
 * the model is not general enough yet.
 */

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "DNA_object_enums.h"
#include "DNA_space_enums.h"

#include "RNA_types.hh"

#include "WM_types.hh"

struct bContext;
struct ID;
struct Main;
struct Object;
struct Scene;
struct SpaceOutliner;
struct ViewLayer;
struct wmEvent;
struct wmNotifier;
struct wmRegionMessageSubscribeParams;

namespace blender {
/* Session "before" pixels for a fill-color picker, owned by the picker's operator (see
 * #ED_image_paint_tile_map_new): the image-undo entry committed at close restores the texture,
 * which the memfile step alone cannot. Forward-declared to keep this seam free of the paint
 * module's headers; only passed through as an opaque session handle, never dereferenced here. */
struct PaintTileMap;
}  // namespace blender

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
  /**
   * The data-block is kept but switched off: it is still listed, but never made a target when the
   * row is activated.
   */
  bool inactive = false;
};

/**
 * One clickable preview slot of a row.
 *
 * A row may show multiple previews -- the paint source shows a layer's channel map and its mask
 * side-by-side. Each preview is independently clickable, has its own tooltip, and switches the
 * row's expanded content.
 */
struct StackRowPreview {
  /**
   * The data-block this preview shows, by session UID and type.
   *
   * Zero UID means no preview; the draw shows the row's plain icon instead. The source provides
   * only the UID and type -- resolving it to the actual ID and fetching the preview icon is the
   * draw's business, not the source's, because the source has no context and the draw owns the
   * preview cache.
   */
  uint32_t id_uid = 0;
  short id_type = 0;
  /** Non-zero icon drawn instead of the data-block's image preview. */
  int icon = 0;
  /** Label shown in tooltip, or empty to use the data-block's name. */
  std::string label;

  /**
   * The #StackContentSection this slot makes active on click, by its identifier, or empty when
   * clicking the slot switches nothing.
   *
   * This is the only name a slot carries, and it names what the slot *reveals* rather than what
   * the slot is: a slot the row cannot expand into has nothing to be named by, and generic code
   * has no other question to ask of a slot's identity.
   */
  std::string section_id;

  /**
   * The row's own icon (#StackRow::icon) is drawn ahead of this slot, not replaced by it.
   *
   * This is how a folder with a mask stays a folder: the folder icon first, the mask thumbnail
   * behind it. The source declares it -- generic code does not know what a mask is and has no
   * name of the slot's to infer it from.
   */
  bool keeps_row_icon = false;

  /**
   * The data slot has nothing to show yet: the thumbnail is still empty although the data-block
   * exists.
   *
   * Generic code draws the empty-texture placeholder in this slot's place instead of a preview.
   * Only the source can know this -- for a paint layer it is a generated transparent image that
   * no brush has touched yet; for another source it will be something else entirely.
   */
  bool is_blank = false;

  /**
   * The slot shows a flat colour rather than a data-block preview or an icon.
   *
   * A row that *is* a colour -- a fill layer, say -- has no thumbnail to fetch and no icon that
   * says more than the colour itself does. The colour travels here because it is the row's own
   * fact, and generic code draws exactly what the slot declares.
   */
  bool is_color_swatch = false;
  /** The colour #is_color_swatch draws; meaningless otherwise. */
  float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
};

/**
 * One section of a row's expandable content.
 *
 * A row's sub-rows may be grouped into multiple named sections that the user switches between --
 * a paint layer shows either its channel textures or its mask content, not both at once. Each
 * section has a stable identifier, a human-readable name, and the list of sub-rows it displays.
 * A row without sections shows nothing below it when expanded.
 */
struct StackContentSection {
  /**
   * Source-defined stable identifier for this section, used to remember which section is
   * currently active when the tree rebuilds.
   *
   * Must be unique among sections of the same row. Paint layers use "CHANNELS" and "MASK".
   */
  std::string identifier;
  /** Human-readable section name shown in UI, or empty if the source has no name for it. */
  std::string name;
  /** The sub-rows that belong to this section, shown when it is active. */
  Vector<StackSubRow> sub_rows;
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
  /** The row can hold other rows: there is a meaning to dropping onto it, and to nesting under it. */
  bool can_hold_children = false;
  /** #can_hold_children and currently holds at least one. */
  bool has_children = false;
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
   * pointed at a copy would lose every keystroke to the next rebuild. Ask #can_rename rather than
   * testing this for null: that null *is* the contract, but it reads as an accident unless it is
   * named.
   */
  char *name_buffer = nullptr;

  /** Whether the row can be renamed at all: only a row with storage for its own name can. */
  bool can_rename() const
  {
    return name_buffer != nullptr;
  }
  int icon = 0;

  /**
   * Color tag for folders, same as collections use. -1 = no color, 0-7 = color index.
   * Only meaningful when #can_hold_children is true. The mode guarantees 8 colors (matching
   * Grease Pencil layer groups), but the contract lives here, not in the source.
   */
  int8_t color_tag = -1;

  /**
   * Preview slots this row shows, left to right.
   *
   * When empty, the row shows no preview at all: the draw answers with the row's plain icon.
   * Each slot is independently clickable and may open a different content section -- a paint
   * layer's channels slot opens its channel textures, its mask slot opens the mask content.
   */
  Vector<StackRowPreview> preview_slots;

  /** Property drawn in the value column, typically an opacity or an influence. */
  std::optional<PointerRNA> value_ptr;
  const char *value_prop = nullptr;
  /** Property drawn in the mode column, typically a blend mode. */
  std::optional<PointerRNA> mode_ptr;
  const char *mode_prop = nullptr;

  /**
   * Content sections this row can expand into.
   *
   * Each section groups a subset of the row's content, and only the active section's sub-rows
   * are shown at once. The user switches sections by clicking the preview slots whose
   * #StackRowPreview::section_id names them. When empty, the row has nothing to show below it.
   */
  Vector<StackContentSection> content_sections;
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

constexpr int STACK_ROW_ORDINAL_MAX = (32767 / STACK_ROW_SUB_ROW_STRIDE) - 1;

/**
 * One kind of row a stack can grow, as the source declares it.
 *
 * What a stack can add is the domain's vocabulary -- the seam names no kinds of its own, because
 * "an empty layer" and "one that starts out covering what is below" is what a paint stack means
 * by them, and another source means something else entirely. The identifier is the stable name
 * scripts see; the position in the list the source hands out is what #StackEditor::row_add reads
 * back.
 */
struct StackAddKindInfo {
  /** Stable name for scripts, e.g. "EMPTY". */
  std::string identifier;
  /** The name the user reads. */
  std::string name;
  /** What the kind means, for the UI's tooltips. */
  std::string description;
  /** Icon drawn next to the name. */
  int icon = 0;
  /** A kind whose creation takes a colour: the Add UI asks for one before calling #row_add. */
  bool takes_color = false;
  /**
   * The ID type of the existing data-block a row of this kind is made from, or 0 for a kind made
   * from nothing.
   *
   * The Add names that data-block by name and resolves it before calling #row_add, so the call
   * stays repeatable and scriptable. Choosing *which* data-block is a UI's job -- a browser, not
   * the plain Add button, which has nothing to hand over.
   */
  short source_id_type = 0;
};

/**
 * What an Add is given besides its kind and its anchor row. Each field is meaningful only to a
 * kind that declared the matching need in #StackAddKindInfo; every other kind ignores it.
 */
struct StackAddArgs {
  /** RGBA, for a kind with #StackAddKindInfo::takes_color; null otherwise. */
  const float *color = nullptr;
  /** The data-block a kind with #StackAddKindInfo::source_id_type is made from; null otherwise. */
  ID *source = nullptr;
};

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

/**
 * What a source may read while the tree is being built.
 *
 * Deliberately not a #bContext: building a tree is a pure function of the file and the space,
 * and a source that could reach the context would be able to read the active area, the running
 * operator or the mouse position -- none of which a row may depend on if the tree is to come out
 * the same however the rebuild was triggered.
 */
struct StackReadContext {
  Main *bmain = nullptr;
  Scene *scene = nullptr;
  ViewLayer *view_layer = nullptr;
};

/** What the space is currently pointed at. Sources resolve their own owner from it. */
struct StackFocus {
  /**
   * The object whose stack is shown, by session UID: a UID survives undo, a file load and a
   * remap, where a pointer names whatever happens to be allocated there now. 0 = "the active
   * object", resolved at use.
   */
  uint32_t object_uid = 0;
  /** Source-defined sub-selection within the object, such as a material slot. -1 means active. */
  int sub_index = -1;
};

/**
 * One variant of choosing within an object: the material slot for paint layers, and whatever
 * plays the same part for another source.
 */
struct StackSubSelection {
  std::string name;
  /** The data-block whose preview shows next to the name, or null for a plain item. */
  ID *preview_id = nullptr;
};

/**
 * The object a focus names, or null: the one it pins, found by session UID, or the active object
 * of the read context when it pins none.
 *
 * A miss -- the pinned object is gone -- is not an error here; the space-level resolver answers
 * it by dropping the focus (#outliner_stack_focus_object_resolve).
 */
Object *outliner_stack_focus_object_get(const StackReadContext &ctx, const StackFocus &focus);

/**
 * What is being dropped, in terms the seam can name.
 *
 * Owns its data: a drag outlives more than one rebuild, and the row or data-block it started from
 * may be long gone by the time it lands.
 */
struct StackDropPayload {
  /** A local data-block: its session UID and expected type. No pointer is kept here. */
  uint32_t id_uid = 0;
  short id_type = 0;
  /** The stack row being dragged, when that is what this is. */
  StackItemIdentity source_item;
};

/** Where a drop is aimed. */
struct StackDropTarget {
  StackItemIdentity anchor;
  /** Role of the sub-row aimed at, when it is one; -1 means the row itself. */
  int sub_role = -1;
  /**
   * Beside the anchor, or -- for #StackMovePlace::Into -- onto it.
   *
   * A dragged row means by Into what the enum says: inside a row that holds others. A dropped
   * data-block has no such reading and means "onto this row" instead, which is the source's to
   * interpret: a paint stack takes an image dropped onto a layer as that layer's map, and one
   * dropped beside it as a new layer there. A source with only one reading of a drop can ignore
   * this.
   */
  StackMovePlace place = StackMovePlace::Above;
};

/**
 * What a source does with something dropped onto its stack.
 *
 * A source with nothing to accept leaves #StackSource::drop_handler at its default of null rather
 * than implementing this with every method refusing.
 */
class StackDropHandler {
 public:
  virtual ~StackDropHandler() = default;

  /**
   * Whether \a payload may be dropped at \a target right now.
   *
   * \param r_disabled_hint: when refusing, a reason to show the user; left untouched to decline
   * silently (a plain "no" cursor, no tooltip).
   */
  virtual bool can_accept(const StackReadContext &ctx,
                          const ID &owner,
                          const StackDropPayload &payload,
                          const StackDropTarget &target,
                          const char **r_disabled_hint) const = 0;
  virtual bool execute(bContext &C,
                       const StackFocus &focus,
                       ID &owner,
                       const StackDropPayload &payload,
                       const StackDropTarget &target,
                       const wmEvent *event,
                       int *r_affected_ordinal) const = 0;
};

/** Widths, in UI units, of the columns a stack row reserves at its right edge. */
struct StackColumnLayout {
  /** Zero (or negative) hides the column. Fractional values are allowed -- a width of 2.5
   * reserves two and a half UI units. */
  float value_width = 0;
  /** Zero (or negative) hides the column. Fractional values are allowed. */
  float mode_width = 0;
  /**
   * Columns for the row's own toggles: the visibility toggle. A fixed count rather than one
   * derived per row, because the columns have to line up down the whole list.
   */
  int icon_columns = 1;
};

/**
 * The grouping half of a #StackEditor's vocabulary: folders, merging, masks.
 *
 * Split out from #StackEditor because it is a genuinely optional second vocabulary, not a few
 * methods a source may or may not refine: a Grease Pencil stack has all of it, a paint stack has
 * all of it, and the next source may well have none of it -- which should read as "this editor
 * has no grouping half" rather than five more stub overrides to trip over. Queried as
 * #StackEditor::grouping; every caller already has to ask whether the verb exists.
 */
class StackGroupingEditor {
 public:
  virtual ~StackGroupingEditor() = default;

  /**
   * Give the row at \a ordinal a mask filled with \a initial_color, or take its mask away.
   *
   * The fill is a color rather than a black-or-white flag: "a mask" is a notion another source may
   * share, while what this one fills it with is the caller's choice, not part of the verb.
   */
  virtual bool row_mask_set(bContext & /*C*/,
                            const StackFocus & /*focus*/,
                            ID & /*owner*/,
                            int /*ordinal*/,
                            bool /*add*/,
                            const float /*initial_color*/[4]) const
  {
    return false;
  }

  /**
   * Merge the row at \a ordinal into the one below it -- below in the compositing sense, the row
   * at \a ordinal - 1 -- so that the pair reads as one row from then on.
   *
   * A merge that keeps every node a user can still reach is a collapse into a folder: the two
   * rows composite exactly as they did, inside a group whose blend is the lower row's, and an
   * ungroup puts them back. Sources that cannot merge leave the default.
   *
   * \return the merged row's ordinal, or -1 when nothing merged.
   */
  virtual int row_merge_down(bContext & /*C*/,
                             const StackFocus & /*focus*/,
                             ID & /*owner*/,
                             int /*ordinal*/) const
  {
    return -1;
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
   * Unwrap the group row at \a ordinal back into the rows it holds.
   *
   * \return how many rows came back, or -1 when the row is not a group this source can unwrap.
   */
  virtual int row_ungroup(bContext & /*C*/,
                          const StackFocus & /*focus*/,
                          ID & /*owner*/,
                          int /*ordinal*/) const
  {
    return -1;
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
   * Set the color tag of the group row at \a ordinal.
   *
   * Color tags are a visual aid for organizing folders, the same as collections use. The mode
   * guarantees 8 colors (0-7), matching Grease Pencil layer groups. -1 clears the color.
   * Sources that do not support color tags or cannot store them leave the default.
   *
   * \return true when the color was set, false when the source does not support it.
   */
  virtual bool row_color_tag_set(bContext & /*C*/,
                                 const StackFocus & /*focus*/,
                                 ID & /*owner*/,
                                 int /*ordinal*/,
                                 int /*color_tag*/) const
  {
    return false;
  }

  /**
   * Re-fill the maps of the fill row at \a ordinal with \a color, and record the colour on it.
   *
   * When \a session_tiles is given, the source first captures the layer's pristine pixels into
   * it (first touch wins, later captures keep the originals) before writing a byte, so the
   * caller can later commit exactly one image-undo entry or roll back. Null means a one-shot
   * call with no session around it.
   *
   * The fill is a colour rather than a repaint: what the row *stands for* changes with it, which
   * is why the source records it and not just the pixels. Sources with no fill rows leave the
   * default.
   *
   * \return true when the colour was applied, false when the row is not a fill this source can
   * re-fill.
   */
  virtual bool row_fill_color_set(bContext & /*C*/,
                                  const StackFocus & /*focus*/,
                                  ID & /*owner*/,
                                  int /*ordinal*/,
                                  const float /*color*/[4],
                                  PaintTileMap * /*session_tiles*/) const
  {
    return false;
  }

  /**
   * Live preview for a fill-color picker: show \a color on the fill row at \a ordinal without
   * recording it and without pushing an undo step.
   *
   * Session handling is the same as #row_fill_color_set: pristine pixels are captured into \a
   * session_tiles on first touch when given, so a later commit or rollback sees the true
   * pre-picker canvas rather than an intermediate tick.
   *
   * Unlike #row_fill_color_set, a refusal here must stay silent: this runs per picker tick, and
   * every tick reporting would spam the status bar. Sources with no fill rows leave the
   * default.
   *
   * \return true when the preview was shown, false when the row is not a fill this source can
   * preview.
   */
  virtual bool row_fill_color_preview(bContext & /*C*/,
                                      const StackFocus & /*focus*/,
                                      ID & /*owner*/,
                                      int /*ordinal*/,
                                      const float /*color*/[4],
                                      PaintTileMap * /*session_tiles*/) const
  {
    return false;
  }
};

/**
 * The mutations a #StackSource's rows support.
 *
 * Split out from #StackSource because reading a stack and writing to one are different
 * questions: every source answers the first, not every source need answer the second, and an
 * interface sized for both was a source with nothing to edit away from sixteen stub overrides of
 * methods it could never sensibly implement. A source with something to write returns one of
 * these from #StackSource::editor; one with nothing to write leaves it null.
 */
class StackEditor {
 public:
  virtual ~StackEditor() = default;

  /**
   * Whether the rows of \a owner can change places at all.
   *
   * Separate from #StackSource::is_editable because order is not always editable even when values
   * are: a paint stack whose chain is shared with another node can have its opacity changed and
   * not its order.
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
   * The kinds of rows this editor can create, in the order #row_add reads them back: the \a
   * index-th entry of this list is what #row_add's \a kind names.
   *
   * Empty by default: a source with nothing to declare gets no Add anywhere, instead of an Add
   * that always refuses -- see #can_edit for how much the rest of the editing vocabulary asks.
   */
  virtual void add_kinds(Vector<StackAddKindInfo> & /*r_kinds*/) const {}

  /**
   * Create a row of \a kind -- a place in this editor's own #add_kinds list -- placed relative to
   * the row \a ordinal names: directly above it, or, when that row is a folder, inside it. -1
   * names no row and puts the new one on top of the stack.
   *
   * \a ordinal is the row the Add was invoked on, not a position to land at: a source resolves
   * "above" or "into" itself, since only it knows whether a row is a folder and where its
   * contents live.
   *
   * A source that has its own "+" elsewhere in the UI leaves this at its default rather than
   * duplicating it: shape keys are added from the Object Data properties, and a second button that
   * does the same thing in a different place is a second thing to keep in step.
   *
   * \return the ordinal of the new row, or -1 when nothing was created.
   */
  virtual int row_add(bContext & /*C*/,
                      const StackFocus & /*focus*/,
                      ID & /*owner*/,
                      int /*kind*/,
                      int /*ordinal*/) const
  {
    return -1;
  }

  /**
   * #row_add with what the kind asked for in #StackAddKindInfo -- a colour, a source data-block.
   *
   * The default ignores \a args, which is right for a source whose kinds ask for nothing.
   */
  virtual int row_add(bContext &C,
                      const StackFocus &focus,
                      ID &owner,
                      int kind,
                      int ordinal,
                      const StackAddArgs & /*args*/) const
  {
    return this->row_add(C, focus, owner, kind, ordinal);
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
   * Copy the row at \a ordinal, putting the copy directly above it.
   *
   * The Outliner selects and addresses the copy by that promise; a source that leaves its copy
   * anywhere else is wrong, and no caller here papers over the wrong position.
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

  /** Take the row at \a ordinal out of the stack. */
  virtual bool row_remove(bContext & /*C*/,
                          const StackFocus & /*focus*/,
                          ID & /*owner*/,
                          int /*ordinal*/) const
  {
    return false;
  }

  /**
   * Move the row at \a from_ordinal next to the row at \a anchor_ordinal.
   *
   * What a drop expresses: a place beside a row rather than a position in the list. A source that
   * nests its rows needs this, because "into that group, at the top" is not a position any row is
   * numbered by. Sources that only ever show a flat list can leave it to #row_reorder.
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
   * The grouping half of this editor -- folders, merging, masks -- or null when this source's rows
   * have no grouping vocabulary at all. The verbs live in #StackGroupingEditor; a caller asks for
   * it the same way it asks for this editor, and treats a null the same as a verb that refuses.
   */
  virtual const StackGroupingEditor *grouping() const
  {
    return nullptr;
  }
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

  /**
   * Whether \a object contributes a stack of this kind, for the object overview.
   */
  virtual bool object_has_stack(const StackReadContext &ctx, Object &object) const = 0;

  /**
   * The data-block whose preview the object row in the overview shows, or null.
   *
   * The overview lists the objects that have a stack of this source, and an object row reads as
   * its stack: for paint layers that is the active material. A source without such a
   * representative leaves the default, and the row keeps the plain object icon.
   */
  virtual ID *object_preview_id(const StackReadContext & /*ctx*/, Object & /*object*/) const
  {
    return nullptr;
  }

  /** The data-block whose stack \a focus resolves to, or null when there is nothing to show. */
  virtual ID *owner_get(const StackReadContext &ctx, const StackFocus &focus) const = 0;

  /**
   * The sub-selections #StackFocus::sub_index can take on \a focus's object, in index order.
   *
   * What a sub-index *means* is the source's own business -- a material slot for paint layers,
   * nothing at all for shape keys -- so this is how the UI offers the choice without knowing.
   * Leaving it empty means the object has one stack and no choice to make.
   */
  virtual void sub_selections_get(const StackReadContext & /*ctx*/,
                                  const StackFocus & /*focus*/,
                                  Vector<StackSubSelection> & /*r_items*/) const
  {
  }

  /**
   * The side effects of choosing a sub-selection, in the data themselves.
   *
   * Changing the material slot has to move #Object::actcol along with it, or the brush stays on
   * the material it was on. A source without such a notion -- shape keys, whose choice lives in
   * #Object.shapenr and moves with the row, not with the sub-index -- keeps the no-op default.
   */
  virtual void sub_selection_apply(bContext & /*C*/,
                                   const StackFocus & /*focus*/,
                                   Object & /*object*/) const
  {
  }

  /**
   * A value that changes whenever #rows_build would produce something different.
   *
   * Must be cheap: it is computed on every tree rebuild, and the whole point of it is to avoid
   * rebuilding a model that did not change.
   *
   * The contract has a second half, and a source that ignores it builds rows that go stale
   * silently: #notifier_invalidates must answer true for every notifier category that can reach
   * an input of this hash. The space routes an answered notifier to a full invalidation and
   * redraw; an unanswered one leaves the cached rows standing until some unrelated event triggers
   * a rebuild -- and if the hash covers the changed input, that rebuild still catches it, but only
   * by luck of the user touching something else. Say in #notifier_invalidates's own comment which
   * notifiers carry which inputs, so the next source does not have to rediscover the wiring.
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

  /** Double-click on a sub-row. Sources with no sub-rows need not implement this. */
  virtual bool sub_row_activate(bContext & /*C*/,
                                const StackFocus & /*focus*/,
                                ID & /*owner*/,
                                const StackRow & /*row*/,
                                const StackSubRow & /*sub_row*/) const
  {
    return false;
  }

  /**
   * A single click landed on a preview slot naming a content section, right after #row_activate
   * already ran for the row it belongs to. Sources with nothing extra to do on a preview click
   * need not implement this.
   */
  virtual bool preview_activate(bContext & /*C*/,
                                ID & /*owner*/,
                                const StackRow & /*row*/,
                                StringRef /*section_id*/) const
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
   * The object mode that focusing this stack and asking to work on it right away should switch
   * to, or nothing for a source with no such notion -- editing a shape key is not a mode the way
   * painting a layer is.
   */
  virtual std::optional<eObjectMode> focus_object_mode() const
  {
    return std::nullopt;
  }

  /**
   * Subscribe to whatever this source's rows are read from but the space's own focus properties
   * do not cover, so that an edit made elsewhere -- a node editor, an image editor -- still
   * redraws the stack. Sources whose rows come entirely from \a owner need not implement this.
   */
  virtual void message_subscribe(const wmRegionMessageSubscribeParams & /*params*/) const {}

  /**
   * Whether \a notifier is reason enough to invalidate the cached rows.
   *
   * #NC_WM | #ND_UNDO invalidates regardless of what this returns; a source answers for the
   * data its rows are read from beyond that -- a node edit, an image added or removed.
   *
   * This is the second half of #state_hash's contract: every category that can reach a hash
   * input has to answer true here. Answering narrowly is fine -- a paint source refuses a stroke's
   * #NC_IMAGE | #NA_PAINTING, since pixels are not rows -- but answering false to a category that
   * edits what the rows are read from strands that input: the hash only gets asked again when
   * something else happens to trigger a rebuild. Name the inputs each answer covers, so the
   * pairing stays auditable.
   */
  virtual bool notifier_invalidates(const wmNotifier & /*notifier*/) const
  {
    return false;
  }

  /** What this source does with something dropped onto its stack, or null to accept nothing. */
  virtual const StackDropHandler *drop_handler() const
  {
    return nullptr;
  }

  /** How this source's rows may be mutated, or null when they may not be at all. */
  virtual const StackEditor *editor() const
  {
    return nullptr;
  }

  /**
   * Side effects of opening \a owner's stack for editing for the first time after a focus change.
   *
   * Some sources keep part of their model in the data itself -- identities handed out on first
   * read, a shape a legacy file needs converting into -- and those have to be written before the
   * rows can be read. This is that write, done once per focus change as its own explicit, undoable
   * step, rather than as a hidden effect of reading the rows on every redraw. A source whose stack
   * is fully readable as-is leaves the no-op default.
   *
   * The source judges editability itself: a stack that may not be written has no business being
   * fixed up here either.
   *
   * \return true when \a owner's data actually changed, so the caller knows to push an undo step.
   */
  virtual bool focus_will_open(bContext & /*C*/, ID & /*owner*/) const
  {
    return false;
  }

  /**
   * Whether \a owner has an editor at all, and data that editor may currently write to.
   *
   * The one question every one of the removed `can_add` / `can_set_enabled` / `can_mask` /
   * `can_remove` predicates reduced to: none of them ever asked anything #is_editable did not
   * already answer, once "is this implemented at all" became #editor's business instead of a
   * predicate's.
   */
  bool can_edit(const ID &owner) const
  {
    return this->editor() != nullptr && this->is_editable(owner);
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
