/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * The list of stack kinds the Outliner knows about.
 *
 * Built-in and ordered by hand rather than registered dynamically: the sources are C++ in this
 * module, there is no run-time registration to serve, and a fixed list keeps the DNA enum and the
 * RNA items honest. A Python-side registry, if it is ever wanted, plugs in here without any of the
 * code above #StackSource having to learn about it.
 */

#include "BLI_vector.hh"

#include "DNA_space_types.h"

#include "outliner_stack_source.hh"

namespace blender::ed::outliner {

namespace {

Span<const StackSource *> stack_sources_ensure()
{
  /* Owned for the lifetime of the process: sources are stateless descriptions, and freeing them at
   * exit would only add an order-of-destruction problem to save nothing. */
  static Vector<std::unique_ptr<StackSource>> owned = []() {
    Vector<std::unique_ptr<StackSource>> sources;
    sources.append(stack_source_paint_material_create());
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

}  // namespace

Span<const StackSource *> stack_sources_get()
{
  return stack_sources_ensure();
}

const StackSource *stack_source_get(const eSpaceOutliner_StackSource type)
{
  const Span<const StackSource *> sources = stack_sources_ensure();
  for (const StackSource *source : sources) {
    if (source->type() == type) {
      return source;
    }
  }
  /* A file can name a source this build does not have, the same way it can name a display mode it
   * does not have. Falling back beats returning null to a caller that has a tree to draw. */
  return sources.first();
}

const StackSource *stack_source_for_space(const SpaceOutliner &space_outliner)
{
  return stack_source_get(space_outliner.stack_source);
}

}  // namespace blender::ed::outliner
