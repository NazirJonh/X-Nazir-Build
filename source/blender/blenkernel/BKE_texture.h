/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/** \file
 * \ingroup bke
 */

namespace blender {

struct Brush;
struct ColorBand;
struct FreestyleLineStyle;
struct Image;
struct ImagePool;
struct LibraryForeachIDData;
struct MTex;
struct Main;
struct ParticleSettings;
struct Tex;
struct TexMapping;
struct TexResult;

enum eTex_Type : short;

/** #ColorBand.data length. */
#define MAXCOLORBAND 32

/**
 * Utility for all IDs using those texture slots.
 */
void BKE_texture_mtex_foreach_id(struct LibraryForeachIDData *data, struct MTex *mtex);

void BKE_texture_default(struct Tex *tex);
struct Tex *BKE_texture_add(struct Main *bmain, const char *name);
void BKE_texture_type_set(struct Tex *tex, eTex_Type type);

/**
 * Return a #TEX_IMAGE texture wrapping \a image, ready to be owned by a single assignment slot
 * (a brush texture slot, a Curve Patch texture slot, ...).
 *
 * Contract: if \a current is an image texture the slot may safely take over -- its only real user
 * (#ID_REAL_USERS <= 1), no fake user, not an asset, editable in place -- its image is retargeted
 * and \a current is returned with its user count unchanged. Otherwise a new texture is created,
 * returned holding exactly one user (the assigning slot's), with one user added on \a image. The
 * caller must not take an extra reference when storing the result directly into a DNA slot;
 * assigning it through a reference-counted RNA pointer property adds the setter's own user, so a
 * newly created texture must have that creation user dropped afterwards (#id_us_min).
 */
struct Tex *BKE_texture_image_wrap_for_slot(struct Main *bmain,
                                            struct Tex *current,
                                            struct Image *image);

void BKE_texture_mtex_default(struct MTex *mtex);
struct MTex *BKE_texture_mtex_add();
/**
 * Slot -1 for first free ID.
 */
struct MTex *BKE_texture_mtex_add_id(struct ID *id, int slot);
/* UNUSED */
// void autotexname(struct Tex *tex);

struct Tex *give_current_linestyle_texture(struct FreestyleLineStyle *linestyle);
struct Tex *give_current_brush_texture(struct Brush *br);
struct Tex *give_current_particle_texture(struct ParticleSettings *part);

bool give_active_mtex(struct ID *id, struct MTex ***mtex_ar, short *act);
void set_active_mtex(struct ID *id, short act);

void set_current_brush_texture(struct Brush *br, struct Tex *tex);
void set_current_linestyle_texture(struct FreestyleLineStyle *linestyle, struct Tex *tex);
void set_current_particle_texture(struct ParticleSettings *part, struct Tex *tex);

struct TexMapping *BKE_texture_mapping_add(int type);
void BKE_texture_mapping_default(struct TexMapping *texmap, int type);
void BKE_texture_mapping_init(struct TexMapping *texmap);

struct ColorMapping *BKE_texture_colormapping_add();
void BKE_texture_colormapping_default(struct ColorMapping *colormap);

bool BKE_texture_dependsOnTime(const struct Tex *texture);
/**
 * \returns true if this texture can use its #Texture.ima (even if its NULL).
 */
bool BKE_texture_is_image_user(const struct Tex *tex);

void BKE_texture_get_value_ex(struct Tex *texture,
                              const float *tex_co,
                              struct TexResult *texres,
                              struct ImagePool *pool,
                              bool use_color_management);

void BKE_texture_get_value(struct Tex *texture,
                           const float *tex_co,
                           struct TexResult *texres,
                           bool use_color_management);

/**
 * Make sure all images used by texture are loaded into pool.
 */
void BKE_texture_fetch_images_for_pool(struct Tex *texture, struct ImagePool *pool);

}  // namespace blender
