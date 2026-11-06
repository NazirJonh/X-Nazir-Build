/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * Implementation of the Python-defined stack source: the wiring between the #StackSource
 * vocabulary and the registered #bpy.types.OutlinerStackSource subclass's RNA functions, and the
 * row builder plumbing.
 */

#include "outliner_stack_source_python.hh"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_map.hh"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_threads.h"
#include "BLI_uuid.h"

#include "CLG_log.h"

#include "BKE_idprop.hh"
#include "BKE_main.hh"

#include "DNA_ID.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_uuid_types.h"

#include "ED_outliner_stack.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../outliner_intern.hh"

namespace blender::ed::outliner {

static CLG_LogRef LOG = {"ed.outliner.stack_source_python"};

namespace {

/* Marks the span of a bridge callback into a registered source. An unregistration that arrives
 * from inside such a callback (a source dropping itself while its own rows_build runs) parks the
 * object in its registry's retired list instead of freeing it, so no callback ever executes
 * against freed memory; the parking spots are swept when the outermost callback ends. */
class BridgeUseGuard {
 public:
  BridgeUseGuard()
  {
    stack_source_callback_begin();
  }
  ~BridgeUseGuard()
  {
    stack_source_callback_end();
  }
};

/**
 * One call into the registered subclass, scoped: the parameter list is created against the
 * source's instance and the function, filled with #set, run with #call, and read back through the
 * typed returns. The list is freed on scope exit, so no early return can leak it. Holds the
 * instance so the same pointer the list was built against is the one dispatched on.
 *
 * A method's own guard (`funcs_.name != nullptr`) runs before constructing this, so #func is
 * never null here.
 */
class PythonCall {
 public:
  PythonCall(PointerRNA instance, FunctionRNA *func, StructCallbackFunc dispatch)
      : func_(func), dispatch_(dispatch), instance_(instance)
  {
    RNA_parameter_list_create(&list_, &instance_, func_);
  }
  PythonCall(const PythonCall &) = delete;
  PythonCall &operator=(const PythonCall &) = delete;
  ~PythonCall()
  {
    RNA_parameter_list_free(&list_);
  }

  /** Name one input parameter. #value is a pointer to the argument the RNA reads: the address of
   * a scalar or pointer for those, and for a string too -- the address of its `const char *`,
   * unless the parameter is a thick-wrapped fixed array. */
  void set(const char *name, const void *value)
  {
    RNA_parameter_set_lookup(&list_, name, value);
  }

  /** Run the method. A null #C is fine: the dispatcher falls back to the global context, so a
   * method always reads a valid one. Returns nonzero when the call itself failed (an exception the
   * dispatcher has already reported), which the callers that cache on the answer treat as "keep the
   * previous answer" rather than a fresh value. */
  int call(bContext *C)
  {
    BridgeUseGuard guard;
    return dispatch_(C, &instance_, func_, &list_);
  }

  /* Read one output, or #fallback when the subclass left the return out. */
  bool ret_bool(const char *name, bool fallback)
  {
    void *ret = read(name);
    return (ret != nullptr) ? *static_cast<bool *>(ret) : fallback;
  }
  int ret_int(const char *name, int fallback)
  {
    void *ret = read(name);
    return (ret != nullptr) ? *static_cast<int *>(ret) : fallback;
  }
  float ret_float(const char *name, float fallback)
  {
    void *ret = read(name);
    return (ret != nullptr) ? *static_cast<float *>(ret) : fallback;
  }
  /** A pointer return that is not flagged to carry a whole #PointerRNA is stored as the bare data
   * pointer, so the ID comes back as an `ID *`. */
  void *ret_ptr_raw(const char *name)
  {
    void *ret = nullptr;
    RNA_parameter_get_lookup(&list_, name, &ret);
    return ret;
  }

 private:
  void *read(const char *name)
  {
    void *ret = nullptr;
    RNA_parameter_get_lookup(&list_, name, &ret);
    return ret;
  }

  FunctionRNA *func_;
  StructCallbackFunc dispatch_;
  PointerRNA instance_;
  ParameterList list_;
};

/** The owner pointer an owner-taking function hands the Python side. */
PointerRNA python_owner_ptr(ID &owner)
{
  return RNA_id_pointer_create(&owner);
}

/* One value mixed into an #object_has_stack cache key. The shift-and-add fold spreads low-order
 * changes (a revision moving by one) across the whole word, which is what keeps two states of
 * one object from landing on the same key. */
uint64_t object_stack_key_mix(uint64_t hash, const uint64_t value)
{
  hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
  return hash;
}

/**
 * The part of an #object_has_stack cache key a source defines for itself, through the optional
 * #has_stack_key method: what its #has_stack answer depends on beyond the object's identity. A
 * data-tree-backed source answers the tree's structural revision, so any change that can flip the
 * answer bumps the key; a source that never changes its mind answers a constant. No method, or a
 * failed one, contributes 0 -- the identity part still distinguishes the asks, and the
 * notifier-driven invalidations (#notifier_invalidates, #undo_reset) drop stale answers.
 */

/* Call \a func on every registered Python source. Only custom sources are #StackSourcePython: the
 * built-ins are other classes, and casting one to the subclass to reach its members would read
 * memory that is not theirs. */
template<typename Func> static void for_each_python_source(Func &&func)
{
  for (const StackSource *source : stack_sources_get()) {
    if (source->type() == SO_STACK_SRC_CUSTOM) {
      func(static_cast<const StackSourcePython &>(*source));
    }
  }
}

}  // namespace

float StackSourcePython::object_stack_key_source_part(const Object &object) const
{
  if (funcs_.has_stack_key == nullptr) {
    return 0.0f;
  }
  PythonCall pc(instance_ptr(), funcs_.has_stack_key, py_call_);
  bContext *C = nullptr;
  pc.set("context", &C);
  PointerRNA object_ptr = RNA_id_pointer_create(const_cast<ID *>(&object.id));
  pc.set("object", &object_ptr);
  if (pc.call(nullptr) != 0) {
    /* The dispatcher has reported the failure; a stable 0 keeps the cache from churning. */
    return 0.0f;
  }
  const float value = pc.ret_float("key", 0.0f);
  if (!(value >= 0.0f) || value >= 16777216.0f /* 2^24, an RNA float's exact integer range */) {
    return 0.0f;
  }
  return value;
}

/* -------------------------------------------------------------------- */
/** \name StackSourcePython
 * \{ */

StackSourcePython::StackSourcePython(const StackSourceInfo &info) : info(info) {}

StackSourcePython::~StackSourcePython() {}

void StackSourcePython::python_bind(void *py_data,
                                    StructCallbackFunc py_call,
                                    StructFreeFunc py_free,
                                    StructRNA *py_srna,
                                    const StackSourceFunctionSet &funcs)
{
  py_data_ = py_data;
  py_call_ = py_call;
  py_free_ = py_free;
  py_srna_ = py_srna;
  funcs_ = funcs;
}

PointerRNA StackSourcePython::instance_ptr() const
{
  return RNA_pointer_create_discrete(nullptr,
                                     py_srna_,
                                     const_cast<StackSourceInfo *>(&info));
}

eSpaceOutliner_StackSource StackSourcePython::type() const
{
  return SO_STACK_SRC_CUSTOM;
}

StringRefNull StackSourcePython::idname() const
{
  return info.idname;
}

StringRefNull StackSourcePython::ui_name() const
{
  return info.ui_name;
}

bool StackSourcePython::object_has_stack(const StackReadContext &ctx, Object &object) const
{
  if (funcs_.has_stack == nullptr) {
    return false;
  }

  /* The cache belongs to one file. This source outlives a load (it lives with the registered
   * Python class), and the answers it holds name objects and meshes of the Main they were asked
   * under, so the first ask under a different Main drops them all. Null contexts answer nothing
   * here: every caller that reaches the cache carries a bmain. */
  if (ctx.bmain != nullptr) {
    if (object_has_stack_cache_bmain_ != nullptr &&
        ctx.bmain != object_has_stack_cache_bmain_)
    {
      object_has_stack_cache_clear();
    }
    object_has_stack_cache_bmain_ = ctx.bmain;
  }

  const std::optional<uint64_t> key = object_stack_key(ctx, object);
  if (!key.has_value()) {
    return object_has_stack_python(object);
  }
  const ObjectHasStackCacheEntry *entry = object_has_stack_cache_.lookup_ptr(&object);
  if (entry != nullptr && entry->key == *key) {
    return entry->has_stack;
  }
  /* Bound the growth: entries of objects that went away stay behind until a full drop (see the
   * cache's own comment), so a cache that holds more entries than the file has objects is mostly
   * those. The object count is only paid when the cache has outgrown the bound the last drop
   * computed; a cache with one entry per object is never dropped. */
  if (object_has_stack_cache_.size() > object_has_stack_cache_size_limit_) {
    object_has_stack_cache_.clear();
    object_has_stack_cache_size_limit_ = (ctx.bmain != nullptr) ?
                                            BLI_listbase_count(&ctx.bmain->objects) :
                                            0;
  }
  const bool has_stack = object_has_stack_python(object);
  object_has_stack_cache_.add_overwrite(&object, {*key, has_stack});
  return has_stack;
}

std::optional<uint64_t> StackSourcePython::object_stack_key(
    const StackReadContext & /*ctx*/, const Object &object) const
{
  /* The identity of the ask, C++-resolved: an object by pointer-carrying session UID (the
   * pointer is the map's own key, the UID is what survives its address being reused), its data
   * by pointer and session UID (a different mesh, or a new mesh at the old mesh's address, must
   * not read as the same one), and the object type. */
  uint64_t key = 0;
  key = object_stack_key_mix(key, uint64_t(object.id.session_uid));
  key = object_stack_key_mix(key, uint64_t(uintptr_t(object.data)));
  key = object_stack_key_mix(key, uint64_t(object.type));
  if (object.data != nullptr) {
    key = object_stack_key_mix(key,
                               uint64_t(static_cast<const ID *>(object.data)->session_uid));
  }
  /* What the answer actually reads beyond the identity: the source's own #has_stack_key, mixed
   * in as its integer-valued float (see #object_stack_key_source_part). */
  key = object_stack_key_mix(key,
                             uint64_t(object_stack_key_source_part(object)));
  return key;
}

bool StackSourcePython::object_has_stack_python(Object &object) const
{
  PythonCall pc(instance_ptr(), funcs_.has_stack, py_call_);
  /* Building a tree is a pure function of the file and the space (#StackReadContext), so these
   * answers run with no bContext: the Python side receives None, and a method that needs the
   * active object reads bpy.context itself. */
  bContext *C = nullptr;
  pc.set("context", &C);
  PointerRNA object_ptr = RNA_id_pointer_create(&object.id);
  pc.set("object", &object_ptr);
  pc.call(nullptr);
  return pc.ret_bool("has_stack", false);
}

void StackSourcePython::object_has_stack_cache_clear() const
{
  object_has_stack_cache_.clear();
}

ID *StackSourcePython::owner_get(const StackReadContext & /*ctx*/, const StackFocus &focus) const
{
  if (funcs_.owner_get == nullptr) {
    return nullptr;
  }
  PythonCall pc(instance_ptr(), funcs_.owner_get, py_call_);
  bContext *C = nullptr;
  pc.set("context", &C);
  pc.set("object_uid", &focus.object_uid);
  pc.set("sub_index", &focus.sub_index);
  pc.call(nullptr);
  void *ret = pc.ret_ptr_raw("owner");
  if (ret == nullptr) {
    return nullptr;
  }
  /* A pointer return that is not flagged to carry a whole #PointerRNA is stored as the bare data
   * pointer (see the `PARM_RNAPTR` branches of `pyrna_py_to_prop`): for an ID that is the ID. */
  return *static_cast<ID **>(ret);
}

uint64_t StackSourcePython::state_hash(const StackReadContext & /*ctx*/, const ID &owner) const
{
  if (funcs_.state_hash == nullptr) {
    /* No hash to ask for: report a different value on every ask, so the rows are re-read rather
     * than going silently stale. */
    return ++fallback_state_hash_;
  }
  BLI_assert(BLI_thread_is_main());
  PythonCall pc(instance_ptr(), funcs_.state_hash, py_call_);
  bContext *C = nullptr;
  pc.set("context", &C);
  PointerRNA owner_ptr = python_owner_ptr(const_cast<ID &>(owner));
  pc.set("owner", &owner_ptr);
  pc.call(nullptr);
  /* The Python side returns an integer-valued float: the RNA float is a 32-bit C float, exact for
   * integers up to 2^24, which is all the mixing a rebuild guard needs. A negative, non-finite or
   * out-of-range answer reads as "no hash". A failed call repeats the last good one: minting a new
   * value here would make every redraw look like a change and rebuild (and traceback) in a loop.
   * The dispatcher has already reported the failure to the Info log. */
  const float hash = pc.ret_float("hash", -1.0f);
  if (!(hash >= 0.0f) || hash >= 16777216.0f /* 2^24 */) {
    if (!last_state_hash_failed_) {
      if (!state_hash_error_logged_) {
        CLOG_STR_WARN(&LOG, "State hash out of range or failed; keeping the previous one");
        state_hash_error_logged_ = true;
      }
      last_state_hash_failed_ = true;
    }
    return last_state_hash_;
  }
  last_state_hash_ = uint64_t(hash);
  last_state_hash_failed_ = false;
  state_hash_error_logged_ = false;
  return last_state_hash_;
}

bool StackSourcePython::rows_build(const StackReadContext & /*ctx*/,
                                   const StackFocus & /*focus*/,
                                   ID &owner,
                                   Vector<StackRow> &r_rows) const
{
  if (funcs_.rows_build == nullptr) {
    return false;
  }
  BLI_assert(BLI_thread_is_main());

  StackRowBuilderData builder_data{};
  builder_data.rows_vector = &r_rows;
  builder_data.current_row = nullptr;
  builder_data.source = const_cast<StackSourcePython *>(this);

  PythonCall pc(instance_ptr(), funcs_.rows_build, py_call_);
  bContext *C = nullptr;
  pc.set("context", &C);
  PointerRNA owner_ptr = python_owner_ptr(owner);
  pc.set("owner", &owner_ptr);
  /* The address of the struct, as the other typed-pointer parameters take it (see #decorate_row). */
  StackRowBuilderData *builder_data_ptr = &builder_data;
  pc.set("rows", &builder_data_ptr);
  const int err = pc.call(nullptr);
  if (err != 0) {
    /* The dispatcher has reported the failure; the rows stay whatever the call described before
     * erroring, and the caller keeps the previous state hash, so this does not rebuild (and
     * traceback) on every redraw. */
    if (!rows_build_error_logged_) {
      CLOG_STR_WARN(&LOG, "Rows build failed; keeping the rows the call described");
      rows_build_error_logged_ = true;
    }
  }
  else {
    rows_build_error_logged_ = false;
  }

  /* Rows list top first, so a folder's row comes after the rows it holds and names a parent that
   * the build could not point at yet. The whole vector is in now: resolve every forward reference,
   * clear one that names no row, the row itself or a row that cannot hold children, and mark the
   * parents that do hold one. A native source never reaches this: its builder resolves parents as
   * it appends. */
  for (const int index : r_rows.index_range()) {
    StackRow &row = r_rows[index];
    if (row.parent_ordinal < 0) {
      continue;
    }
    const int parent_ordinal = row.parent_ordinal;
    if (parent_ordinal >= r_rows.size() || parent_ordinal == index ||
        !r_rows[parent_ordinal].can_hold_children)
    {
      row.parent_ordinal = -1;
      continue;
    }
    r_rows[parent_ordinal].has_children = true;
  }
  return true;
}

bool StackSourcePython::is_editable(const ID &owner) const
{
  return !ID_IS_LINKED(&owner) && !ID_IS_OVERRIDE_LIBRARY(&owner);
}

StackColumnLayout StackSourcePython::column_layout() const
{
  StackColumnLayout layout;
  /* The widths come from Python class attributes and reach here without having passed a range
   * check the C++ half controls (a registration does not run one on every later read, and a NaN
   * survives every comparison): a non-finite or oversized one would corrupt the layout every row
   * draws with, so the value is not trusted as-is. Zero stays the "column hidden" answer. */
  const auto clamp_width = [](const float width) {
    return std::isfinite(width) ? std::clamp(width, 0.0f, STACK_SOURCE_COLUMN_WIDTH_MAX) : 0.0f;
  };
  layout.value_width = clamp_width(info.value_width);
  layout.mode_width = clamp_width(info.mode_width);
  layout.action_width = clamp_width(info.action_width);
  layout.icon_columns = std::clamp(info.icon_columns, 0, STACK_SOURCE_ICON_COLUMNS_MAX);
  return layout;
}

bool StackSourcePython::supports_row_lock() const
{
  /* A static class attribute, not a method: the registration copied it into #info. */
  return info.supports_row_lock != 0;
}

const char *StackSourcePython::string_intern(StringRefNull text) const
{
  const std::unique_ptr<std::string> *owned = interned_strings_.lookup_ptr_as(text);
  if (owned != nullptr) {
    return owned->get()->c_str();
  }
  interned_strings_.add_as(std::string(text), std::make_unique<std::string>(text));
  return interned_strings_.lookup_as(text).get()->c_str();
}

bool StackSourcePython::row_activate(
    bContext &C, const StackFocus & /*focus*/, ID &owner, int ordinal, const StackRow & /*row*/)
    const
{
  if (funcs_.row_activate == nullptr) {
    return false;
  }
  PythonCall pc(instance_ptr(), funcs_.row_activate, py_call_);
  bContext *C_ptr = &C;
  pc.set("context", &C_ptr);
  PointerRNA owner_ptr = python_owner_ptr(owner);
  pc.set("owner", &owner_ptr);
  pc.set("ordinal", &ordinal);
  pc.call(&C);
  return pc.ret_bool("activated", false);
}

bool StackSourcePython::row_is_active(const StackReadContext & /*ctx*/,
                                      const StackFocus & /*focus*/,
                                      const ID &owner,
                                      const StackRow &row) const
{
  if (funcs_.row_is_active == nullptr) {
    return false;
  }
  PythonCall pc(instance_ptr(), funcs_.row_is_active, py_call_);
  bContext *C = nullptr;
  pc.set("context", &C);
  PointerRNA owner_ptr = python_owner_ptr(const_cast<ID &>(owner));
  int ordinal = row.ordinal;
  pc.set("owner", &owner_ptr);
  pc.set("ordinal", &ordinal);
  pc.call(nullptr);
  return pc.ret_bool("active", false);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Registration from the RNA side
 * \{ */

StackSourcePython *outliner_stack_source_python_new(const StackSourceInfo &info)
{
  return new StackSourcePython(info);
}

StructRNA *outliner_stack_source_python_srna(StringRefNull idname)
{
  const StackSource *source = stack_source_custom_find(idname);
  if (source == nullptr) {
    return nullptr;
  }
  return static_cast<const StackSourcePython &>(*source).py_srna_get();
}

StructRNA *outliner_stack_source_python_refine(const StackSourceInfo *info)
{
  if (info == nullptr) {
    return nullptr;
  }
  StructRNA *srna = nullptr;
  /* Only custom sources are #StackSourcePython; the built-ins must not be cast to it (see
   * #for_each_python_source). */
  for_each_python_source([&](const StackSourcePython &python_source) {
    if (&python_source.info == info) {
      srna = python_source.py_srna_get();
    }
  });
  return srna;
}

void outliner_stack_source_python_register(Main &bmain,
                                           StackSourcePython *source_owned,
                                           void *py_class,
                                           StructCallbackFunc py_call,
                                           StructFreeFunc py_free,
                                           StructRNA *py_srna,
                                           const StackSourceFunctionSet &funcs)
{
  if (source_owned == nullptr) {
    return;
  }
  std::unique_ptr<StackSourcePython> source(source_owned);
  /* The source is stored in the srna's blender-type slot as its base #StackSource -- the same type
   * the unregister path reads the slot back through (see
   * #outliner_stack_source_python_unregister): a void* round-trip only round-trips to the type it
   * was stored as, so neither side names the subclass, whose base list may grow. */
  RNA_struct_blender_type_set(py_srna, static_cast<StackSource *>(source.get()));
  source->python_bind(py_class, py_call, py_free, py_srna, funcs);
  stack_source_register(std::move(source));
  outliner_stack_rows_invalidate_all(bmain);
}

bool outliner_stack_source_python_unregister(Main &bmain, StructRNA *srna)
{
  /* The registration stored the source as its base #StackSource, exactly the type read back here:
   * a void* round-trip through #RNA_struct_blender_type_get only round-trips to the type it was
   * stored as, so neither side names the subclass (a second base in the subclass would not change
   * the address the base pointer carries). */
  const StackSource *source = static_cast<const StackSource *>(
      RNA_struct_blender_type_get(srna));
  if (source == nullptr || source->type() != SO_STACK_SRC_CUSTOM) {
    return false;
  }
  /* The pieces the module still needs once the registry has dropped the object: read before
   * #stack_source_unregister destroys it -- the unique_ptr there owns the source, and touching
   * it afterwards would be use-after-free. */
  const StackSourcePython &python_source = static_cast<const StackSourcePython &>(*source);
  void *py_data = python_source.py_data_get();
  StructFreeFunc py_free = python_source.py_free_get();

  /* The module drops the source first: its callbacks must never run against a freed srna. */
  if (!stack_source_unregister(source->idname())) {
    return false;
  }
  outliner_stack_rows_invalidate_all(bmain);

  /* The Python class the registration held goes with it; the srna's py_type is released the
   * same door the panel type's unregister uses. */
  ExtensionRNA rna_ext{};
  rna_ext.data = py_data;
  rna_ext.free = py_free;
  RNA_struct_free_extension(srna, &rna_ext);
  RNA_struct_free(&RNA_blender_rna_get(), srna);

  WM_main_add_notifier(NC_WINDOW, nullptr);
  return true;
}

void outliner_stack_rows_invalidate_all(Main &bmain)
{
  /* Every Python source's #object_has_stack cache dies with the rows: this is the one place the
   * registration path already funnels a whole-registry change through, and any future caller that
   * invalidates all rows invalidates the answers that listed them too. */
  for_each_python_source(
      [](const StackSourcePython &python_source) { python_source.object_has_stack_cache_clear(); });
  for (bScreen *screen = static_cast<bScreen *>(bmain.screens.first); screen != nullptr;
       screen = static_cast<bScreen *>(screen->id.next))
  {
    for (ScrArea &area : screen->areabase) {
      for (SpaceLink &space_link : area.spacedata) {
        if (space_link.spacetype != SPACE_OUTLINER) {
          continue;
        }
        SpaceOutliner &space_outliner = reinterpret_cast<SpaceOutliner &>(space_link);
        if (space_outliner.outlinevis != SO_STACK_LAYERS || space_outliner.runtime == nullptr) {
          continue;
        }
        outliner_stack_rows_invalidate(space_outliner);
      }
    }
  }
}

/* The srna of the #OutlinerStackRowBuilder RNA type, set once by the first registration: the
 * builder pointers handed to Python have to be typed by it. */
static StructRNA *stack_row_builder_srna_ = nullptr;

void outliner_stack_row_builder_srna_set(StructRNA *srna)
{
  stack_row_builder_srna_ = srna;
}

StructRNA *outliner_stack_row_builder_srna()
{
  return stack_row_builder_srna_;
}

/** \} */

}  // namespace blender::ed::outliner
