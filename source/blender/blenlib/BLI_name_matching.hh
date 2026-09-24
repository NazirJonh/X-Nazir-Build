/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * Name-matching helpers for image asset map-type / filter-tag include filters.
 */

#pragma once

#include <string>

#include "BLI_span.hh"
#include "BLI_string_ref.hh"

namespace blender {

/**
 * Strip trailing Blender duplicate suffix (`.NNN`) then a known image extension.
 * Matching against the result uses case-insensitive compares.
 */
std::string BLI_name_matching_normalize_asset_name(StringRef name);

/**
 * True if \a token matches \a normalized_name as a delimited segment
 * (prefix / postfix / mid-name). Empty token never matches.
 */
bool BLI_name_matching_token_matches(StringRef normalized_name, StringRef token);

/**
 * \a normalized_name with every delimited occurrence of any of \a tokens removed (together with
 * one neighboring delimiter), and leading/trailing delimiters trimmed. For example
 * `brick_wall_basecolor` with the token `basecolor` gives `brick_wall`. Uses the same segment
 * rules as #BLI_name_matching_token_matches.
 */
std::string BLI_name_matching_strip_tokens(StringRef normalized_name, Span<StringRef> tokens);

/** True if any token in \a tokens segment-matches \a normalized_name. */
bool BLI_name_matching_map_type_matches_name(StringRef normalized_name, Span<StringRef> tokens);

/**
 * True if \a filter_tag equals any metadata tag (case-insensitive) or
 * segment-matches \a normalized_name.
 */
bool BLI_name_matching_filter_tag_matches(StringRef normalized_name,
                                          Span<StringRef> metadata_tag_names,
                                          StringRef filter_tag);

/**
 * Include-OR filter for shelf visibility.
 * Empty \a active_map_type_token_lists and empty \a active_filter_tags → true (filter off).
 */
bool BLI_name_matching_asset_passes_include_filter(
    StringRef asset_name,
    Span<StringRef> metadata_tag_names,
    Span<Span<StringRef>> active_map_type_token_lists,
    Span<StringRef> active_filter_tags);

}  // namespace blender
