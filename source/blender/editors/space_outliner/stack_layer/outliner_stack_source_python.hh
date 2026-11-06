/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * The Outliner-facing half of a Python-defined stack source: a #StackSource whose every answer
 * comes from a registered #bpy.types.OutlinerStackSource subclass, and the row decorator twin of
 * it.
 *
 * This module never touches Python itself: the registered subclass's methods are invoked through
 * the RNA extension the registration installed (the same dispatch a Python panel's #draw uses),
 * which takes the GIL for the call. What lives here is the vocabulary wiring -- which RNA function
 * answers which #StackSource question, the row builder the #rows_build callback writes into, and
 * the storage the rows need to outlive a rebuild (rename buffers, interned property names).
 */

#pragma once

#include <array>
#include <memory>

#include "BLI_map.hh"
#include "BLI_set.hh"
#include "BLI_string_ref.hh"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "DNA_space_types.h"

#include "RNA_types.hh"

#include "ED_outliner_stack.hh"

#include "outliner_stack_source.hh"

struct bContext;
struct Main;
struct wmNotifier;

namespace blender::ed::outliner {

/**
 * A stack source whose answers come from a registered #bpy.types.OutlinerStackSource subclass.
 *
 * The static description lives in #info (what the class attributes said at registration); the
 * dynamic answers are RNA calls into the subclass. A source whose subclass left a method out
 * refuses that verb, the same way a C++ source that left an editor method at its default does.
 */
class StackSourcePython final : public StackSource {
 public:
  explicit StackSourcePython(const StackSourceInfo &info);
  ~StackSourcePython() override;

  /** Wire the registered subclass in: the extension the registration created (its class object,
   * its dispatch, the free function that releases it), the subclass srna, and which of the
   * optional functions the subclass actually answers. */
  void python_bind(void *py_data,
                   StructCallbackFunc py_call,
                   StructFreeFunc py_free,
                   StructRNA *py_srna,
                   const StackSourceFunctionSet &funcs);

  /** What the RNA side needs back from a registered source: its srna (the refinement reads it),
   * and the class object with the free function that releases it (the srna's extension is released
   * through them at unregister, after the registry has already dropped this object). */
  StructRNA *py_srna_get() const
  {
    return py_srna_;
  }
  void *py_data_get() const
  {
    return py_data_;
  }
  StructFreeFunc py_free_get() const
  {
    return py_free_;
  }

  /* ---- StackSource ---- */
  eSpaceOutliner_StackSource type() const override;
  StringRefNull idname() const override;
  StringRefNull ui_name() const override;
  bool object_has_stack(const StackReadContext &ctx, Object &object) const override;
  std::optional<uint64_t> object_stack_key(const StackReadContext &ctx,
                                           const Object &object) const override;
  ID *owner_get(const StackReadContext &ctx, const StackFocus &focus) const override;
  uint64_t state_hash(const StackReadContext &ctx, const ID &owner) const override;
  bool rows_build(const StackReadContext &ctx,
                  const StackFocus &focus,
                  ID &owner,
                  Vector<StackRow> &r_rows) const override;
  bool is_editable(const ID &owner) const override;
  StackColumnLayout column_layout() const override;
  bool supports_row_lock() const override;
  bool row_activate(
      bContext &C, const StackFocus &focus, ID &owner, int ordinal, const StackRow &row) const
      override;
  bool row_is_active(const StackReadContext &ctx,
                     const StackFocus &focus,
                     const ID &owner,
                     const StackRow &row) const override;

  /* ---- Python-facing helpers the row builder uses ---- */

  /** The storage the row's rename field types into, keyed by the row's stable key. The buffer is
   * at least #MAX_NAME bytes, is filled with \a name, and outlives the rebuild that asked for it;
   * a key no build mentions again has its buffer freed. */
  char *name_buffer_ensure(StringRefNull key, StringRefNull name) const;
  /** A string owned as long as the source is: property names and reasons a row carries. */
  const char *string_intern(StringRefNull text) const;
  /** The row identity a stable key stands for: a deterministic #bUUID, the same on every rebuild
   * and in every session as long as the key is the same. */
  static bUUID stable_id_from_key(StringRefNull key);

  /**
   * Drop every cached #object_has_stack answer.
   *
   * The cache keys answers on the object, its data and the source's own #has_stack_key
   * (#object_stack_key), so a key change self-invalidates one entry. A full drop is for the
   * events that can invalidate every entry at once: a whole-rows invalidation, a new #Main (a
   * file load, which the #object_has_stack call itself detects).
   */
  void object_has_stack_cache_clear() const;

  /** The static description the subclass registered with. */
  const StackSourceInfo info;

  /** The part of the #object_has_stack cache key the subclass defines through its optional
   * #has_stack_key, as a float; 0 when the source names no key or the read failed. */
  float object_stack_key_source_part(const Object &object) const;

 private:
  /** The pointer the subclass methods run on: this source's description, typed by the subclass
   * srna, so the Python `self` reads the registered properties of its own class. */
  PointerRNA instance_ptr() const;

  /** The registered subclass's #has_stack, one #PythonCall: what #object_has_stack asks when its
   * cache has no answer. */
  bool object_has_stack_python(Object &object) const;

  /** One cached #object_has_stack answer, good while the key it was keyed on still holds. */
  struct ObjectHasStackCacheEntry {
    uint64_t key = 0;
    bool has_stack = false;
  };

  /**
   * The cached answers, keyed by the object each was asked about.
   *
   * The object pointer keys the lookup, but the *entry* is keyed on
   * #StackSource::object_stack_key's mix of the object's session UID, its data pointer and that
   * data's session UID, the object type, and the source's own #has_stack_key float: a deleted
   * object's address reused by a new one answers a different key and re-asks, so no stale entry
   * can ever be handed out. Entries for objects that went away stay behind inert until a full
   * drop clears them; the cache is small (one key and one bool per object) and the drops are
   * frequent enough on a working session.
   *
   * Main thread only, like every other ask into the registered subclass: the three
   * #object_has_stack call sites are the draw pass, the tree build and the focus follow, and all
   * three run on the main thread. No lock on purpose, exactly as for #name_buffers_.
   */
  mutable Map<const Object *, ObjectHasStackCacheEntry> object_has_stack_cache_;
  /**
   * The #Main the cache was built against. A file load replaces every object and mesh behind the
   * stored answers while this source survives it (it lives with the registered Python class), so
   * the first ask under a different #Main drops the cache before it looks anything up.
   */
  mutable const Main *object_has_stack_cache_bmain_ = nullptr;
  /**
   * The entry count the cache is dropped above: entries of objects that went away stay behind
   * until a full drop (see #object_has_stack_cache_), so a cache that holds more entries than the
   * file has objects is mostly those. Recomputed from the file each time the bound is exceeded.
   */
  mutable int object_has_stack_cache_size_limit_ = 0;

  /** The registered subclass: its class object (not owned), its RNA dispatch, the free function
   * that releases the class, its srna. */
  void *py_data_ = nullptr;
  StructCallbackFunc py_call_ = nullptr;
  StructFreeFunc py_free_ = nullptr;
  StructRNA *py_srna_ = nullptr;
  /** The base-type RNA functions the registration resolved; an entry is null when the subclass did
   * not answer that verb, which is how each method knows to refuse it instead of erroring. */
  StackSourceFunctionSet funcs_;

  /* Rows are rebuilt under a const #rows_build; the storage the rows point into is the source's
   * own and only grows and shrinks with what the current build used. Keyed on the row's stable
   * key; the buffer is held by pointer because the map moves its entries as it grows, and the
   * `char *` a row's rename field types into has to stay put. */
  mutable Map<std::string, std::unique_ptr<std::array<char, MAX_NAME>>> name_buffers_;
  mutable Set<std::string> name_buffer_keys_used_;
  mutable Map<std::string, std::unique_ptr<std::string>> interned_strings_;
  /** A state hash stand-in for a source whose subclass answers no #state_hash: it changes on every
   * ask, so the rows are re-read every tree rebuild instead of going silently stale. */
  mutable uint64_t fallback_state_hash_ = 1;
  /**
   * The last successfully read #state_hash, and whether the read before the next one failed: a
   * failed read repeats it rather than minting a new value, so a Python error in #state_hash does
   * not make every rebuild look like a change (an error would rebuild and traceback on every
   * redraw). Cleared on the next successful read.
   */
  mutable uint64_t last_state_hash_ = 0;
  mutable bool last_state_hash_failed_ = false;
  /** One-shot log flags: a Python error in #state_hash or #rows_build is reported once per source
   * until one of the two succeeds again. */
  mutable bool state_hash_error_logged_ = false;
  mutable bool rows_build_error_logged_ = false;

  void name_buffers_prune() const;
};

/**
 * A row decorator: a registered #bpy.types.OutlinerStackRowDecorator subclass whose
 * #decorate_row adds buttons (and nothing else) to the rows of the source its `source` names.
 */
class StackRowDecoratorPython {
 public:
  explicit StackRowDecoratorPython(const StackDecoratorInfo &info) : info(info) {}

  /** Wire the registered subclass in: the extension the registration created (its class object,
   * its dispatch, the free function that releases it), the subclass srna, and the #decorate_row
   * function of the base type. */
  void python_bind(void *py_data,
                   StructCallbackFunc py_call,
                   StructFreeFunc py_free,
                   StructRNA *py_srna,
                   FunctionRNA *func)
  {
    py_data_ = py_data;
    py_call_ = py_call;
    py_free_ = py_free;
    py_srna_ = py_srna;
    decorate_row_ = func;
    registered_ = true;
  }

  /** Whether this decorator answers \a source_idname's rows: its own `source` names them, or
   * names every source. */
  bool matches_source(StringRefNull source_idname) const;

  /** Run the decorator on \a row. Errors the Python side raises are reported there and leave the
   * row as it was. */
  void decorate(bContext *C, ID &owner, StackRow &row) const;

  const StackDecoratorInfo info;
  /** Whether the RNA side finished binding it: a decorator is only listed once it is. */
  bool registered_ = false;
  /**
   * Whether an unregistration from inside a decorate callback named it: the object stays in the
   * registry until the callback's apply sweep (removing it mid-iteration would invalidate the
   * loop), but stops matching sources and stops decorating.
   */
  bool retired_ = false;

  void retire()
  {
    retired_ = true;
  }

  /** The pieces the RNA-side unregister and the refinement need. */
  void *py_data_get() const
  {
    return py_data_;
  }
  StructFreeFunc py_free_get() const
  {
    return py_free_;
  }
  StructRNA *py_srna_get() const
  {
    return py_srna_;
  }

 private:
  PointerRNA instance_ptr() const;

  void *py_data_ = nullptr;
  StructCallbackFunc py_call_ = nullptr;
  StructFreeFunc py_free_ = nullptr;
  StructRNA *py_srna_ = nullptr;
  FunctionRNA *decorate_row_ = nullptr;
};

/* The decorator registry. Decorators are listed to the rows while they are being built, so a
 * registration or unregistration also bumps the version #outliner_stack_decorators_version
 * answers -- the rebuild cache mixes it into its state. */
void stack_decorator_register(std::unique_ptr<StackRowDecoratorPython> decorator);
bool stack_decorator_unregister(StringRefNull idname);
/** Takes every decorator an unregistration inside a bridge callback retired out of the registry;
 * called by #stack_bridge_callback_end when the outermost callback has ended. */
void stack_decorators_retire_sweep();
/** Run every matching decorator over \a rows.
 *
 * \a C may be null: the rows are built from #outliner_stack_rows_ensure, which is reached without
 * a #bContext from RNA reads and notifier flushes as well, so the contract is that a decorator
 * gets whatever context the session's global one answers -- the same fallback the RNA dispatcher
 * itself makes for a null context. A decorator that needs the concrete Outliner reads it from the
 * context window; it has no other handle onto the caller. */
void stack_decorators_apply(bContext *C,
                            const StackSource &source,
                            ID &owner,
                            MutableSpan<StackRow> rows);
/** A value that changes whenever the set of registered decorators does. */
uint64_t stack_decorators_version();

}  // namespace blender::ed::outliner
