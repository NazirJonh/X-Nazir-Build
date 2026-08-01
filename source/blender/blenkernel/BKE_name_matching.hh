/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * #UserDef storage (add/remove/find) for image asset map-type / filter-tag name-matching
 * preferences. The matching algorithm itself lives in `BLI_name_matching.hh`.
 */

#pragma once

#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

namespace blender {

struct UserDef;
struct bUserNameMatchFilterTag;
struct bUserNameMatchMapType;
struct bUserNameMatchToken;

/**
 * Runtime filter state for template/image grid name-matching (not persisted in DNA as such --
 * each host bridges its own storage, e.g. #AssetShelfSettings's ListBaseT fields or the Generic
 * Grid API's comma-joined IDProperty string, into this shared type before resolving/matching).
 *
 * \a active_filter_tags is a forward-looking complement to \a active_map_type_ids: name-segment
 * matching (map types) assumes the asset's filename carries a recognizable affix (`_normal`,
 * `_BaseColor`, ...). Assets without such affixes -- e.g. textures a user drag-and-dropped and
 * assigned a map type to by hand -- have no name signal to match on, but can still be tagged with
 * asset metadata tags (#AssetMetaData::tags) naming the map type; \a active_filter_tags lets that
 * tag-based assignment participate in the same include filter as name matching.
 */
struct NameMatchFilterState {
  bool enabled = false;
  Set<std::string> active_map_type_ids;
  /** Free-form filter tag names (case-insensitive), matched via
   * #BLI_name_matching_filter_tag_matches against #AssetMetaData::tags first and, as a fallback,
   * as a name segment. Unlike map types these need no #UserDef lookup to resolve. */
  Set<std::string> active_filter_tags;
};

bool BKE_name_match_filter_is_active(const NameMatchFilterState &state);
void BKE_name_match_filter_set_enabled(NameMatchFilterState &state, bool enabled);
void BKE_name_match_filter_toggle_map_type(NameMatchFilterState &state, StringRef identifier);
bool BKE_name_match_filter_map_type_is_active(const NameMatchFilterState &state,
                                              StringRef identifier);
void BKE_name_match_filter_toggle_tag(NameMatchFilterState &state, StringRef tag);
bool BKE_name_match_filter_tag_is_active(const NameMatchFilterState &state, StringRef tag);
/** Clears both \a active_map_type_ids and \a active_filter_tags. */
void BKE_name_match_filter_clear_selection(NameMatchFilterState &state);

bool BKE_name_match_filter_asset_passes(const NameMatchFilterState &state,
                                    const UserDef &userdef,
                                    StringRef asset_name,
                                    Span<StringRef> metadata_tag_names);

/**
 * Map-type tokens (and filter tags) resolved from a #NameMatchFilterState against #UserDef once,
 * so a whole asset library can be tested against #BKE_name_match_resolved_asset_passes without
 * re-resolving active map-type ids to tokens (a #Set lookup + linear
 * #BKE_name_matching_map_type_find scan) for every single asset. References into both \a state
 * and \a userdef, so only valid as long as neither is mutated (tokens/map types added or removed,
 * \a state's sets changed) -- resolve short-lived, right before iterating a library, not stored
 * long-term.
 */
struct NameMatchResolvedFilter {
  bool active = false;
  Vector<Vector<StringRef>> token_lists;
  Vector<StringRef> filter_tags;
};

NameMatchResolvedFilter BKE_name_match_filter_resolve(const NameMatchFilterState &state,
                                                      const UserDef &userdef);

bool BKE_name_match_resolved_asset_passes(const NameMatchResolvedFilter &resolved,
                                         StringRef asset_name,
                                         Span<StringRef> metadata_tag_names);

/** Free #UserDef name-matching ListBases (including nested tokens). */
void BKE_name_matching_userdef_free(UserDef *userdef);

/**
 * If #UserDef.name_match_map_types is empty, seed the core map types and default tokens.
 * Filter tags are left empty.
 */
void BKE_name_matching_userdef_ensure_defaults(UserDef *userdef);

/**
 * Add a map type with unique \a identifier and \a name.
 * \a identifier must not contain `,` (reserved for future serialization).
 * \return nullptr if either is empty, already used, or \a identifier contains `,`.
 */
bUserNameMatchMapType *BKE_name_matching_map_type_add(UserDef *userdef,
                                                     const char *name,
                                                     const char *identifier,
                                                     short flag);

/**
 * Remove a map type. Built-in rows cannot be removed.
 * \return false if \a map_type is built-in or not found.
 */
bool BKE_name_matching_map_type_remove(UserDef *userdef, bUserNameMatchMapType *map_type);

bUserNameMatchMapType *BKE_name_matching_map_type_find(const UserDef *userdef,
                                                       StringRef identifier);

/**
 * Rename \a map_type's identifier to a unique value derived from \a identifier, uniquifying on
 * clash the same way #BKE_preferences_asset_library_name_set does for asset library names.
 * No-op if \a map_type is built-in (its identifier is not user-editable), \a identifier is
 * empty, or \a identifier contains `,`.
 */
void BKE_name_matching_map_type_identifier_set(UserDef *userdef,
                                              bUserNameMatchMapType *map_type,
                                              const char *identifier);

/**
 * Add a token to \a map_type. Empty values and case-insensitive duplicates are rejected.
 * \return nullptr on failure.
 */
bUserNameMatchToken *BKE_name_matching_token_add(bUserNameMatchMapType *map_type,
                                                 const char *value);

void BKE_name_matching_token_remove(bUserNameMatchMapType *map_type, bUserNameMatchToken *token);

/**
 * Rename \a token to \a value. Empty values and case-insensitive duplicates (against other tokens
 * of the same map type) are rejected, matching #BKE_name_matching_token_add's contract.
 * \return false if \a value is empty or already used by another token in \a map_type.
 */
bool BKE_name_matching_token_set_value(bUserNameMatchMapType *map_type,
                                       bUserNameMatchToken *token,
                                       const char *value);

/**
 * Add a filter tag with a unique name (case-insensitive).
 * \return nullptr if empty or duplicate.
 */
bUserNameMatchFilterTag *BKE_name_matching_filter_tag_add(UserDef *userdef, const char *name);

void BKE_name_matching_filter_tag_remove(UserDef *userdef, bUserNameMatchFilterTag *tag);

}  // namespace blender
