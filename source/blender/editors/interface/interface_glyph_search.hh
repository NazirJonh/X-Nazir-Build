/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editorui
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Glyph data structure for C++ search.
 * This mirrors the Python glyph dictionary structure.
 */
typedef struct GlyphData {
  const char *name;
  const char *unicode;
  const char *codepoint;
  const char *category;
  int popularity;
  void *user_data;
} GlyphData;

#ifdef __cplusplus
namespace blender::ui::glyph_search {

/**
 * Search glyphs using BLI_string_search.
 *
 * \param glyphs: Array of glyph data to search through
 * \param glyphs_count: Number of glyphs in the array
 * \param query: Search query string
 * \param category: Optional category filter (empty string for no filter)
 * \param max_results: Maximum number of results to return
 * \param r_results: Output array of result indices (caller must free)
 * \return: Number of results found
 */
int search_glyphs(const GlyphData *glyphs,
                  int glyphs_count,
                  const char *query,
                  const char *category,
                  int max_results,
                  int **r_results);

/**
 * Free results array allocated by search_glyphs.
 */
void free_search_results(int *results);

/**
 * Calculate fuzzy match score between query and text.
 * Returns -1 if no match, otherwise returns error count.
 */
int fuzzy_match_score(const char *query, const char *text);

/**
 * Calculate Damerau-Levenshtein distance between two strings.
 */
int levenshtein_distance(const char *a, const char *b);

}  // namespace blender::ui::glyph_search
#endif

#ifdef __cplusplus
}
#endif
