/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * The list of stack kinds the Outliner knows about.
 *
 * The built-ins are C++ in this module and ordered by hand; runtime-registered sources (a
 * Python-defined #StackSource subclass, today) are appended after them through
 * #stack_source_register and named by idname rather than by the DNA enum, whose values have to
 * stay file-stable.
 */

#include <algorithm>

#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "BKE_idprop.hh"

#include "DNA_space_types.h"

#include "ED_outliner_stack.hh"

#include "UI_resources.hh"

#include "outliner_stack_source.hh"
#include "outliner_stack_source_python.hh"

namespace blender::ed::outliner {

namespace {

/* The built-ins: stateless descriptions created once. */
Span<const StackSource *> stack_sources_builtin_ensure()
{
  /* Static storage: the sources are stateless descriptions, and nothing outlives them that could
   * still ask for one during static destruction at exit. */
  static Vector<std::unique_ptr<StackSource>> owned = []() {
    Vector<std::unique_ptr<StackSource>> sources;
    sources.append(stack_source_paint_layers_create());
    sources.append(stack_source_shape_keys_create());
    return sources;
  }();
  static Vector<const StackSource *> pointers = [&]() {
    Vector<const StackSource *> result;
    for (const std::unique_ptr<StackSource> &source : owned) {
      result.append(source.get());
    }
    return result;
  }();
  return pointers;
}

/** One runtime-registered source, with the session value scripts and the space's source property
 * address it by. The value is assigned once at registration and rides with the source: a position
 * in this vector would move with every register and unregister around it, and a value a script
 * saved, or an item listed under, would then silently name a different source. */
struct RegisteredStackSource {
  std::unique_ptr<StackSource> source;
  int session_value = 0;
};

/* The runtime-registered sources, in registration order. Sources arrive with the scripts that
 * register them and leave with those scripts' unregister, so this list lives as long as the
 * session does. */
Vector<RegisteredStackSource> &stack_sources_runtime()
{
  static Vector<RegisteredStackSource> runtime_sources;
  return runtime_sources;
}

/* The session value the next registered source takes: monotonic and past #SO_STACK_SRC_CUSTOM,
 * where the built-ins' DNA enum values end, so it can never collide with a built-in's value nor
 * repeat within the session. */
int &stack_source_session_counter()
{
  static int counter = int(SO_STACK_SRC_CUSTOM);
  return counter;
}

/* Sources parked by an unregistration that arrived from inside a bridge callback into one of
 * them (a source dropping itself while its own rows_build runs): freeing one now would leave the
 * callback executing against freed memory, so it waits until the outermost callback ends
 * (#stack_bridge_callback_end). */
Vector<std::unique_ptr<StackSource>> &stack_sources_retired()
{
  static Vector<std::unique_ptr<StackSource>> retired;
  return retired;
}

/* One counter for every bridge callback into a registered Python object, source or decorator: a
 * guard always enters both registries, so two counters would only ever hold the same number (see
 * the header declaration for what the shared one buys). */
int &stack_bridge_callback_depth()
{
  static int depth = 0;
  return depth;
}

}  // namespace

void stack_bridge_callback_begin()
{
  stack_bridge_callback_depth()++;
}

void stack_bridge_callback_end()
{
  if (--stack_bridge_callback_depth() == 0) {
    /* The outermost callback ended: nothing is running against either registry anymore, so the
     * parked sources are freed and the retired decorators leave their registry for good. */
    stack_sources_retired().clear_and_shrink();
    stack_decorators_retire_sweep();
  }
}

bool stack_bridge_callback_active()
{
  return stack_bridge_callback_depth() > 0;
}

namespace {

/* Bumped on every registration and unregistration, so the combined listing below can tell a
 * cached copy from a changed registry without rebuilding it on every ask. */
uint64_t &stack_sources_registry_version()
{
  static uint64_t version = 0;
  return version;
}

/* Combined listing of every known source: the built-ins, then the registered ones in
 * registration order. */
Span<const StackSource *> stack_sources_all_ensure()
{
  static Vector<const StackSource *> cached;
  static uint64_t cached_version = uint64_t(-1);
  if (cached_version != stack_sources_registry_version()) {
    cached.clear();
    cached.extend(stack_sources_builtin_ensure());
    for (const RegisteredStackSource &registered : stack_sources_runtime()) {
      cached.append(registered.source.get());
    }
    cached_version = stack_sources_registry_version();
  }
  return cached;
}

}  // namespace

void stack_source_register(std::unique_ptr<StackSource> source)
{
  BLI_assert(source != nullptr);
  /* A re-registration of the same idname replaces the previous one, the way a re-registered
   * Python class does: the scripts own exactly one source per idname. */
  stack_source_unregister(source->idname());
  RegisteredStackSource registered;
  registered.source = std::move(source);
  registered.session_value = ++stack_source_session_counter();
  stack_sources_runtime().append(std::move(registered));
  stack_sources_registry_version()++;
}

bool stack_source_unregister(StringRefNull idname)
{
  Vector<RegisteredStackSource> &runtime_sources = stack_sources_runtime();
  for (const int64_t index : runtime_sources.index_range()) {
    if (runtime_sources[index].source->idname() == idname) {
      if (stack_bridge_callback_active()) {
        /* A bridge callback into a source is running; park instead of free (see
         * #stack_sources_retired). */
        stack_sources_retired().append(std::move(runtime_sources[index].source));
      }
      runtime_sources.remove(index);
      stack_sources_registry_version()++;
      return true;
    }
  }
  return false;
}

const StackSource *stack_source_custom_find(StringRefNull idname)
{
  if (idname.is_empty()) {
    return nullptr;
  }
  for (const RegisteredStackSource &registered : stack_sources_runtime()) {
    if (registered.source->idname() == idname) {
      return registered.source.get();
    }
  }
  return nullptr;
}

Span<const StackSource *> stack_sources_get()
{
  return stack_sources_all_ensure();
}

void outliner_stack_sources_list(Vector<StackSourceListItem> &r_items)
{
  r_items.clear();
  /* The built-ins first, then the registered ones in registration order: the same order
   * #stack_sources_get lists them in. The listing is built from the two containers directly
   * rather than through that combined cache, because only the entries here carry the session
   * value a registered source is addressed by. */
  const Span<const StackSource *> builtins = stack_sources_builtin_ensure();
  r_items.reserve(builtins.size() + stack_sources_runtime().size());
  for (const StackSource *source : builtins) {
    StackSourceListItem item;
    item.idname = source->idname();
    item.ui_name = source->ui_name();
    item.type = source->type();
    item.is_custom = (item.type == SO_STACK_SRC_CUSTOM);
    r_items.append(std::move(item));
  }
  for (const RegisteredStackSource &registered : stack_sources_runtime()) {
    StackSourceListItem item;
    item.idname = registered.source->idname();
    item.ui_name = registered.source->ui_name();
    item.type = registered.source->type();
    item.is_custom = (item.type == SO_STACK_SRC_CUSTOM);
    item.session_value = registered.session_value;
    r_items.append(std::move(item));
  }
}

int StackSource::focus_button_icon() const
{
  return ICON_BRUSH_DATA;
}

int StackSource::empty_preview_icon() const
{
  return ICON_TEXTURE_DATA;
}

StackRowPreview::StackRowPreview(const StackRowPreview &other)
    : id_uid(other.id_uid),
      id_type(other.id_type),
      icon(other.icon),
      glyph(other.glyph),
      label(other.label),
      tooltip(other.tooltip),
      operator_idname(other.operator_idname),
      enabled(other.enabled),
      properties(other.properties != nullptr ?
                     OwnedIDProperty(IDP_CopyProperty(other.properties.get())) :
                     nullptr),
      section_id(other.section_id),
      keeps_row_icon(other.keeps_row_icon),
      is_blank(other.is_blank),
      is_color_swatch(other.is_color_swatch),
      render_kind(other.render_kind),
      render_hash(other.render_hash),
      sphere_metallic(other.sphere_metallic),
      sphere_roughness(other.sphere_roughness)
{
  std::copy(other.color, other.color + 4, color);
  std::copy(other.sphere_map_uid, other.sphere_map_uid + 3, sphere_map_uid);
}

const StackSource *stack_source_get(const eSpaceOutliner_StackSource type)
{
  if (type == SO_STACK_SRC_CUSTOM) {
    /* A caller that only has the enum cannot name a custom source; the space-level resolver
     * below is the one that reads the idname. Fall through to the built-ins the same way an
     * unknown value does. */
    return stack_sources_builtin_ensure().first();
  }
  const Span<const StackSource *> sources = stack_sources_all_ensure();
  for (const StackSource *source : sources) {
    if (source->type() == type) {
      return source;
    }
  }
  /* A file can name a source this build does not have, the same way it can name a display mode it
   * does not have. Falling back beats returning null to a caller that has a tree to draw. */
  return stack_sources_builtin_ensure().first();
}

const StackSource *stack_source_for_space(const SpaceOutliner &space_outliner)
{
  if (space_outliner.stack_source == SO_STACK_SRC_CUSTOM) {
    const StackSource *custom = stack_source_custom_find(space_outliner.stack_source_idname);
    if (custom != nullptr) {
      return custom;
    }
    /* The script that registered the source is gone (an add-on was disabled, a startup file was
     * written before it existed). The same fallback an unknown built-in gets. */
  }
  return stack_source_get(space_outliner.stack_source);
}

}  // namespace blender::ed::outliner
