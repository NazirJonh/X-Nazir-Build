/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#define WB_RESOLVE_GROUP_SIZE 8

/**
 * Poly Paint: neutral value of the per-vertex Specular channel, i.e. what every object that is not
 * painting it writes.
 *
 * #get_world_lighting maps Specular to dielectric F0 as `0.08 * specular`, the same way Principled
 * BSDF's "IOR Level" does. 0.625f is the value for which that yields exactly the fixed 0.05f
 * reflectance Workbench used before per-vertex Specular existed, so an unpainted scene shades
 * bit-for-bit as it did before. Do not "round" this to 0.5f: that is the Principled default, not
 * Workbench's, and would darken the specular of every existing scene.
 */
#define WORKBENCH_DEFAULT_SPECULAR 0.625f

/**
 * Poly Paint: bits of the `vertex_material_props` push constant, i.e. `1 << eMaterialPaintChannel`
 * for the channels the prepass has a per-vertex input for.
 *
 * The DNA enum is not visible from shader code, so these are spelled out here and checked against
 * it by a static_assert in workbench_engine.cc - adding a channel before these in the enum without
 * updating them would silently read the wrong bit.
 */
#define WB_VERTEX_PROP_METALLIC (1 << 1)
#define WB_VERTEX_PROP_ROUGHNESS (1 << 2)
#define WB_VERTEX_PROP_SPECULAR (1 << 3)

/* Resources bind slots. */

/* Textures. */
/* Slot 0-1 are reserved by curves and pointcloud attributes. */
#define WB_MATCAP_SLOT 2
#define WB_TEXTURE_SLOT 3
#define WB_TILE_ARRAY_SLOT 4
#define WB_TILE_DATA_SLOT 5
#define WB_CURVES_UV_SLOT 6
#define WB_CURVES_COLOR_SLOT 7

/* UBOs (Storage buffers in Workbench Next). */
#define WB_MATERIAL_SLOT 0
#define WB_WORLD_SLOT 1
