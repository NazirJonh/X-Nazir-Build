/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_name_matching.hh"

#include "BLI_listbase.h"
#include "BLI_name_matching.hh"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_string_utils.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_userdef_types.h"

#include "MEM_guardedalloc.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace blender {

static bool name_matching_listbase_heads_garbage(const ListBase &lb);
static bool name_matching_link_ptr_garbage(const void *ptr);

/**
 * Free a token chain the same defensive way the map-type chain is walked: stop (leak, don't crash)
 * at the first link that cannot be trusted. #ListBaseT::free_no_destruct would instead follow every
 * `next` blindly, which defeats the head check done by the callers.
 */
static void name_matching_token_list_free(ListBase &tokens)
{
  if (name_matching_listbase_heads_garbage(tokens) || !BLI_listbase_head_is_plausible(&tokens)) {
    BLI_listbase_clear(&tokens);
    return;
  }
  for (bUserNameMatchToken *token = static_cast<bUserNameMatchToken *>(tokens.first);
       token != nullptr;)
  {
    if (name_matching_link_ptr_garbage(token)) {
      break;
    }
    bUserNameMatchToken *next = token->next;
    MEM_delete(token);
    if (name_matching_link_ptr_garbage(next)) {
      break;
    }
    token = next;
  }
  BLI_listbase_clear(&tokens);
}

static void name_matching_map_type_free(bUserNameMatchMapType *map_type)
{
  name_matching_token_list_free(map_type->tokens);
  MEM_delete(map_type);
}

/**
 * True when a ListBase head must not be dereferenced (DNA 0xFF fill, partial nulls, etc.).
 * Does not walk the chain — only inspects `first`/`last` pointer values.
 */
static bool name_matching_listbase_heads_garbage(const ListBase &lb)
{
  if (lb.first == nullptr && lb.last == nullptr) {
    return false;
  }
  if (lb.first == nullptr || lb.last == nullptr) {
    return true;
  }
  const uintptr_t first = uintptr_t(lb.first);
  const uintptr_t last = uintptr_t(lb.last);
  constexpr uintptr_t min_addr = 4096;
  constexpr uintptr_t invalid_addr = uintptr_t(-1);
  return first < min_addr || last < min_addr || first == invalid_addr || last == invalid_addr;
}

static bool name_matching_link_ptr_garbage(const void *ptr)
{
  if (ptr == nullptr) {
    return false;
  }
  const uintptr_t addr = uintptr_t(ptr);
  return addr < 4096 || addr == uintptr_t(-1);
}

/**
 * Reset list heads that contain uninitialized DNA (e.g. 0xFF fill from an older userpref layout).
 * Without this, #ListBaseT::is_empty() is false for garbage heads and iteration crashes.
 */
static void name_matching_userdef_repair_lists(UserDef *userdef)
{
  if (name_matching_listbase_heads_garbage(userdef->name_match_map_types) ||
      !BLI_listbase_head_is_plausible(&userdef->name_match_map_types))
  {
    userdef->name_match_map_types.first = nullptr;
    userdef->name_match_map_types.last = nullptr;
  }
  if (name_matching_listbase_heads_garbage(userdef->name_match_filter_tags) ||
      !BLI_listbase_head_is_plausible(&userdef->name_match_filter_tags))
  {
    userdef->name_match_filter_tags.first = nullptr;
    userdef->name_match_filter_tags.last = nullptr;
  }
}

void BKE_name_matching_userdef_free(UserDef *userdef)
{
  name_matching_userdef_repair_lists(userdef);

  /* Walk defensively, the same way #BKE_name_matching_userdef_ensure_defaults does: a userpref
   * file saved by an older/WIP layout of these DNA fields can leave a poisoned `next` mid-chain
   * even though the list head itself just passed the repair check above. Stop and forget the
   * remainder (leak, don't crash) the moment a node looks like garbage, rather than blindly
   * dereferencing every `next` pointer down the chain. */
  for (bUserNameMatchMapType *map_type =
           static_cast<bUserNameMatchMapType *>(userdef->name_match_map_types.first);
       map_type != nullptr;)
  {
    if (name_matching_link_ptr_garbage(map_type)) {
      break;
    }
    bUserNameMatchMapType *next = map_type->next;
    if (name_matching_listbase_heads_garbage(map_type->tokens) ||
        !BLI_listbase_head_is_plausible(&map_type->tokens))
    {
      map_type->tokens.clear_no_delete();
    }
    name_matching_map_type_free(map_type);
    if (name_matching_link_ptr_garbage(next)) {
      break;
    }
    map_type = next;
  }
  userdef->name_match_map_types.clear_no_delete();

  if (name_matching_listbase_heads_garbage(userdef->name_match_filter_tags) ||
      !BLI_listbase_head_is_plausible(&userdef->name_match_filter_tags))
  {
    userdef->name_match_filter_tags.clear_no_delete();
    return;
  }
  for (bUserNameMatchFilterTag *tag =
           static_cast<bUserNameMatchFilterTag *>(userdef->name_match_filter_tags.first);
       tag != nullptr;)
  {
    if (name_matching_link_ptr_garbage(tag)) {
      break;
    }
    bUserNameMatchFilterTag *next = tag->next;
    MEM_delete(tag);
    if (name_matching_link_ptr_garbage(next)) {
      break;
    }
    tag = next;
  }
  userdef->name_match_filter_tags.clear_no_delete();
}

bUserNameMatchMapType *BKE_name_matching_map_type_find(const UserDef *userdef,
                                                      const StringRef identifier)
{
  if (identifier.is_empty()) {
    return nullptr;
  }
  for (bUserNameMatchMapType &map_type : userdef->name_match_map_types) {
    if (identifier == map_type.identifier) {
      return &map_type;
    }
  }
  return nullptr;
}

static bool name_matching_map_type_name_exists(const UserDef *userdef,
                                               const StringRef name,
                                               const bUserNameMatchMapType *ignore)
{
  for (const bUserNameMatchMapType &map_type : userdef->name_match_map_types) {
    if (&map_type == ignore) {
      continue;
    }
    if (BLI_strcasecmp(map_type.name, std::string(name).c_str()) == 0) {
      return true;
    }
  }
  return false;
}

bool BKE_name_match_filter_is_active(const NameMatchFilterState &state)
{
  return state.enabled &&
         (!state.active_map_type_ids.is_empty() || !state.active_filter_tags.is_empty());
}

void BKE_name_match_filter_set_enabled(NameMatchFilterState &state, const bool enabled)
{
  state.enabled = enabled;
}

void BKE_name_match_filter_toggle_map_type(NameMatchFilterState &state,
                                           const StringRef identifier)
{
  if (identifier.is_empty()) {
    return;
  }
  const std::string key = identifier;
  if (!state.active_map_type_ids.remove(key)) {
    state.active_map_type_ids.add(key);
  }
}

bool BKE_name_match_filter_map_type_is_active(const NameMatchFilterState &state,
                                              const StringRef identifier)
{
  return state.active_map_type_ids.contains_as(identifier);
}

void BKE_name_match_filter_toggle_tag(NameMatchFilterState &state, const StringRef tag)
{
  if (tag.is_empty()) {
    return;
  }
  const std::string key = tag;
  if (!state.active_filter_tags.remove(key)) {
    state.active_filter_tags.add(key);
  }
}

bool BKE_name_match_filter_tag_is_active(const NameMatchFilterState &state, const StringRef tag)
{
  return state.active_filter_tags.contains_as(tag);
}

void BKE_name_match_filter_clear_selection(NameMatchFilterState &state)
{
  state.active_map_type_ids.clear();
  state.active_filter_tags.clear();
}

NameMatchResolvedFilter BKE_name_match_filter_resolve(const NameMatchFilterState &state,
                                                      const UserDef &userdef)
{
  NameMatchResolvedFilter resolved;
  if (!state.enabled) {
    return resolved;
  }
  for (const std::string &id : state.active_map_type_ids) {
    const bUserNameMatchMapType *map_type = BKE_name_matching_map_type_find(&userdef, id);
    if (map_type == nullptr) {
      continue;
    }
    Vector<StringRef> tokens;
    for (const bUserNameMatchToken &token : map_type->tokens) {
      tokens.append(token.value);
    }
    if (!tokens.is_empty()) {
      resolved.token_lists.append(std::move(tokens));
    }
  }
  /* Filter tags need no #UserDef lookup: they're compared directly against asset metadata tags
   * (and, as a fallback, name-segment-matched), so the strings themselves are the resolved form. */
  for (const std::string &tag : state.active_filter_tags) {
    resolved.filter_tags.append(tag);
  }
  resolved.active = !resolved.token_lists.is_empty() || !resolved.filter_tags.is_empty();
  return resolved;
}

bool BKE_name_match_resolved_asset_passes(const NameMatchResolvedFilter &resolved,
                                         const StringRef asset_name,
                                         const Span<StringRef> metadata_tag_names)
{
  if (!resolved.active) {
    /* Disabled, or enabled with nothing resolvable → show all. */
    return true;
  }
  Vector<Span<StringRef>> token_spans;
  token_spans.reserve(resolved.token_lists.size());
  for (const Vector<StringRef> &tokens : resolved.token_lists) {
    token_spans.append(tokens.as_span());
  }
  return BLI_name_matching_asset_passes_include_filter(
      asset_name, metadata_tag_names, token_spans, resolved.filter_tags);
}

bool BKE_name_match_filter_asset_passes(const NameMatchFilterState &state,
                                        const UserDef &userdef,
                                        const StringRef asset_name,
                                        const Span<StringRef> metadata_tag_names)
{
  return BKE_name_match_resolved_asset_passes(
      BKE_name_match_filter_resolve(state, userdef), asset_name, metadata_tag_names);
}

bUserNameMatchMapType *BKE_name_matching_map_type_add(UserDef *userdef,
                                                     const char *name,
                                                     const char *identifier,
                                                     const short flag)
{
  if (name == nullptr || name[0] == '\0' || identifier == nullptr || identifier[0] == '\0') {
    return nullptr;
  }
  if (strchr(identifier, ',') != nullptr) {
    return nullptr;
  }
  if (BKE_name_matching_map_type_find(userdef, identifier) != nullptr) {
    return nullptr;
  }
  if (name_matching_map_type_name_exists(userdef, name, nullptr)) {
    return nullptr;
  }

  bUserNameMatchMapType *map_type = MEM_new<bUserNameMatchMapType>(__func__);
  STRNCPY_UTF8(map_type->identifier, identifier);
  STRNCPY_UTF8(map_type->name, name);
  map_type->flag = flag;
  BLI_addtail(&userdef->name_match_map_types, map_type);
  return map_type;
}

bool BKE_name_matching_map_type_remove(UserDef *userdef, bUserNameMatchMapType *map_type)
{
  if (map_type == nullptr) {
    return false;
  }
  if (map_type->flag & USER_NAME_MATCH_MAP_TYPE_BUILTIN) {
    return false;
  }
  if (BLI_findindex(&userdef->name_match_map_types, map_type) == -1) {
    return false;
  }
  BLI_remlink(&userdef->name_match_map_types, map_type);
  name_matching_map_type_free(map_type);
  return true;
}

void BKE_name_matching_map_type_identifier_set(UserDef *userdef,
                                              bUserNameMatchMapType *map_type,
                                              const char *identifier)
{
  if (userdef == nullptr || map_type == nullptr || identifier == nullptr ||
      identifier[0] == '\0')
  {
    return;
  }
  if (map_type->flag & USER_NAME_MATCH_MAP_TYPE_BUILTIN) {
    return;
  }
  if (strchr(identifier, ',') != nullptr) {
    return;
  }
  STRNCPY_UTF8(map_type->identifier, identifier);
  BLI_uniquename(&userdef->name_match_map_types,
                 map_type,
                 identifier,
                 '.',
                 offsetof(bUserNameMatchMapType, identifier),
                 sizeof(map_type->identifier));
}

bUserNameMatchToken *BKE_name_matching_token_add(bUserNameMatchMapType *map_type, const char *value)
{
  if (map_type == nullptr || value == nullptr || value[0] == '\0') {
    return nullptr;
  }
  for (const bUserNameMatchToken &token : map_type->tokens) {
    if (BLI_strcasecmp(token.value, value) == 0) {
      return nullptr;
    }
  }

  bUserNameMatchToken *token = MEM_new<bUserNameMatchToken>(__func__);
  STRNCPY_UTF8(token->value, value);
  BLI_addtail(&map_type->tokens, token);
  return token;
}

void BKE_name_matching_token_remove(bUserNameMatchMapType *map_type, bUserNameMatchToken *token)
{
  if (map_type == nullptr || token == nullptr) {
    return;
  }
  BLI_freelinkN(&map_type->tokens, token);
}

bool BKE_name_matching_token_set_value(bUserNameMatchMapType *map_type,
                                       bUserNameMatchToken *token,
                                       const char *value)
{
  if (map_type == nullptr || token == nullptr || value == nullptr || value[0] == '\0') {
    return false;
  }
  for (const bUserNameMatchToken &other : map_type->tokens) {
    if (&other != token && BLI_strcasecmp(other.value, value) == 0) {
      return false;
    }
  }
  STRNCPY_UTF8(token->value, value);
  return true;
}

bUserNameMatchFilterTag *BKE_name_matching_filter_tag_add(UserDef *userdef, const char *name)
{
  if (userdef == nullptr || name == nullptr || name[0] == '\0') {
    return nullptr;
  }
  for (const bUserNameMatchFilterTag &tag : userdef->name_match_filter_tags) {
    if (BLI_strcasecmp(tag.name, name) == 0) {
      return nullptr;
    }
  }

  bUserNameMatchFilterTag *tag = MEM_new<bUserNameMatchFilterTag>(__func__);
  STRNCPY_UTF8(tag->name, name);
  BLI_addtail(&userdef->name_match_filter_tags, tag);
  return tag;
}

void BKE_name_matching_filter_tag_remove(UserDef *userdef, bUserNameMatchFilterTag *tag)
{
  if (userdef == nullptr || tag == nullptr) {
    return;
  }
  BLI_freelinkN(&userdef->name_match_filter_tags, tag);
}

void BKE_name_matching_userdef_ensure_defaults(UserDef *userdef)
{
  name_matching_userdef_repair_lists(userdef);

  if (!userdef->name_match_map_types.is_empty()) {
    /* Sanitize nested token lists and abort if the chain itself is poisoned mid-list. */
    for (bUserNameMatchMapType *map_type =
             static_cast<bUserNameMatchMapType *>(userdef->name_match_map_types.first);
         map_type != nullptr;)
    {
      if (name_matching_link_ptr_garbage(map_type)) {
        userdef->name_match_map_types.first = nullptr;
        userdef->name_match_map_types.last = nullptr;
        break;
      }
      bUserNameMatchMapType *next = map_type->next;
      if (name_matching_listbase_heads_garbage(map_type->tokens) ||
          !BLI_listbase_head_is_plausible(&map_type->tokens))
      {
        map_type->tokens.first = nullptr;
        map_type->tokens.last = nullptr;
      }
      else {
        /* The head passed, but a poisoned `next` mid-chain would still be dereferenced by every
         * later range-for over the tokens (RNA, UI, filter resolution). Truncate here instead. */
        for (bUserNameMatchToken *token = static_cast<bUserNameMatchToken *>(map_type->tokens.first);
             token != nullptr;)
        {
          bUserNameMatchToken *token_next = token->next;
          if (name_matching_link_ptr_garbage(token_next)) {
            token->next = nullptr;
            map_type->tokens.last = token;
            break;
          }
          token = token_next;
        }
      }
      if (name_matching_link_ptr_garbage(next)) {
        map_type->next = nullptr;
        userdef->name_match_map_types.last = map_type;
        break;
      }
      map_type = next;
    }
    if (!userdef->name_match_map_types.is_empty()) {
      return;
    }
  }

  auto add_builtin = [&](const char *identifier,
                         const char *name,
                         const std::initializer_list<const char *> tokens) {
    bUserNameMatchMapType *map_type = BKE_name_matching_map_type_add(
        userdef, name, identifier, USER_NAME_MATCH_MAP_TYPE_BUILTIN);
    BLI_assert(map_type != nullptr);
    for (const char *token_value : tokens) {
      BKE_name_matching_token_add(map_type, token_value);
    }
  };

  add_builtin("BASE_COLOR",
              "Base Color",
              {"BaseColor", "albedo", "diff", "diffuse", "col", "color"});
  add_builtin("NORMAL", "Normal", {"N", "nor", "norm", "normal"});
  add_builtin("ROUGHNESS", "Roughness", {"R", "rough", "roughness"});
  add_builtin("METALLIC", "Metallic", {"M", "met", "metal", "metallic"});
  add_builtin(
      "HEIGHT", "Height / Displacement", {"H", "height", "disp", "displacement", "bump"});
  add_builtin("AO", "Ambient Occlusion", {"AO", "ao", "occlusion"});
  add_builtin("EMISSION", "Emission", {"E", "emit", "emission", "emissive"});
  add_builtin("ALPHA", "Opacity / Alpha", {"A", "alpha", "opacity", "transparency"});
  add_builtin("SPECULAR", "Specular", {"S", "spec", "specular"});
}

}  // namespace blender
