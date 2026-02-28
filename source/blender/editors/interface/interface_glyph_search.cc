/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * \file
 * \ingroup editorui
 *
 * Glyph search integration using BLI_string_search.
 */

#include "BLI_string_search.hh"
#include "BLI_vector.hh"
#include "interface_glyph_search.hh"

namespace blender::ui::glyph_search {

using blender::string_search::StringSearch;

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
                  int **r_results)
{
  if (!glyphs || glyphs_count <= 0 || !query || !r_results) {
    return 0;
  }

  /* Create search object */
  StringSearch<int> search(nullptr, blender::string_search::MainWordsHeuristic::FirstGroup);

  /* Add glyphs to search, filtering by category if specified */
  bool has_category_filter = category && category[0] != '\0';

  for (int i = 0; i < glyphs_count; i++) {
    const GlyphData &glyph = glyphs[i];

    /* Skip if category filter is active and doesn't match */
    if (has_category_filter && glyph.category) {
      if (strcmp(glyph.category, category) != 0) {
        continue;
      }
    }

    /* Add glyph to search with popularity as weight */
    search.add(glyph.name, new int(i), glyph.popularity);
  }

  /* Perform search */
  blender::Vector<int *> search_results = search.query(query);

  /* Convert results to output array */
  int result_count = std::min(int(search_results.size()), max_results);
  *r_results = static_cast<int *>(malloc(result_count * sizeof(int)));

  for (int i = 0; i < result_count; i++) {
    (*r_results)[i] = *search_results[i];
    delete search_results[i];
  }

  return result_count;
}

/**
 * Free results array allocated by search_glyphs.
 */
void free_search_results(int *results)
{
  if (results) {
    free(results);
  }
}

/**
 * Calculate fuzzy match score between query and text.
 * Returns -1 if no match, otherwise returns error count.
 */
int fuzzy_match_score(const char *query, const char *text)
{
  if (!query || !text) {
    return -1;
  }

  return blender::string_search::get_fuzzy_match_errors(query, text);
}

/**
 * Calculate Damerau-Levenshtein distance between two strings.
 */
int levenshtein_distance(const char *a, const char *b)
{
  if (!a || !b) {
    return 0;
  }

  return blender::string_search::damerau_levenshtein_distance(a, b);
}

}  // namespace blender::ui::glyph_search
