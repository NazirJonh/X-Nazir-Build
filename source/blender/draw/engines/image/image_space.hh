/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw_engine
 */

#pragma once

#include "BLI_math_matrix_types.hh"

namespace blender {

struct ARegion;
struct ImBuf;
struct Image;
struct ImageUser;
struct Main;

namespace image_engine {

struct ShaderParameters;

/**
 * Space accessor.
 *
 * Image engine is used to draw the images inside multiple spaces \see SpaceLink.
 * The #AbstractSpaceAccessor is an interface to communicate with a space.
 */
class AbstractSpaceAccessor {
 public:
  virtual ~AbstractSpaceAccessor() = default;

  /**
   * Return the active image of the space.
   *
   * The returned image will be drawn in the space.
   *
   * The return value is optional.
   */
  virtual blender::Image *get_image(Main *bmain) = 0;

  /**
   * Return the #ImageUser of the space.
   *
   * The return value is optional.
   */
  virtual blender::ImageUser *get_image_user() = 0;

  /**
   * Acquire the image buffer of the image.
   *
   * \param image: Image to get the buffer from. Image is the same as returned from the #get_image
   * member.
   * \param lock: pointer to a lock object.
   * \return Image buffer of the given image.
   */
  virtual ImBuf *acquire_image_buffer(blender::Image *image, void **lock) = 0;

  /**
   * Release a previous locked image from #acquire_image_buffer.
   */
  virtual void release_buffer(blender::Image *image, ImBuf *image_buffer, void *lock) = 0;

  /**
   * Whether this space would display pixels of its own in place of the image's.
   *
   * Cheap by contract -- flags and pointers, never a node graph walk or an image lock -- because
   * the drawing modes ask it on more than one code path per frame. It says what the space is set
   * to, not that a buffer will really be produced: #acquire_display_override_buffer can still
   * return null, and the caller then falls back to the image's own pixels.
   *
   * The conservative answer is `true`: taking the override path for a space that turns out to have
   * none costs a full texture update, while missing one draws the wrong pixels.
   */
  virtual bool has_display_override() const
  {
    return false;
  }

  /**
   * A buffer to display in place of the image's own pixels, or null when there is none.
   *
   * The image is still what everything else is keyed on -- size, tiles, image user, partial
   * updates -- only its pixels come from elsewhere. Used by the Image Editor to show a material's
   * composited layer stack on the canvas of one of its layers.
   *
   * A space that returns a buffer here forces the screen-space drawing mode, since the image-space
   * one draws the image's own GPU texture and would never see these pixels.
   *
   * \param bmain: the #Main the override is resolved against. Passed in rather than looked up so
   *               that the engine resolves against the #Main it was initialized with.
   * \param r_revision: changes whenever the returned pixels do, so the drawing mode can tell
   *                    whether the texture it uploaded last frame is still current.
   * \param r_changed_region: which pixels changed since the previous revision, in the returned
   *                          buffer's own texel coordinates, so the drawing mode can refresh that
   *                          much of its texture rather than all of it. Empty means "no idea" as
   *                          well as "nothing moved" -- the two are told apart by \a r_revision --
   *                          so a caller that sees a new revision with an empty region must do a
   *                          full refresh.
   */
  virtual ImBuf *acquire_display_override_buffer(Main * /*bmain*/,
                                                 uint64_t * /*r_revision*/,
                                                 rcti * /*r_changed_region*/ = nullptr)
  {
    return nullptr;
  }

  /** Release a buffer from #acquire_display_override_buffer. */
  virtual void release_display_override_buffer(ImBuf * /*image_buffer*/) {}

  /**
   * The canvas extent the image occupies, in image pixels, or zero when the space cannot say.
   *
   * Asked of the space rather than measured from the displayed buffer, because a display override
   * is only pixels: it may be produced at a fraction of the canvas -- the Image Editor's Combined
   * preview is shaded at a power-of-two fraction chosen from the zoom -- and sizing the canvas
   * from it would shrink the drawn image as the zoom crossed an octave.
   *
   * Zero means "measure it from the buffer", which is right for every space that has no override.
   */
  virtual int2 get_canvas_size() const
  {
    return int2(0);
  }

  /**
   * Update the r_shader_parameters with space specific settings.
   *
   * Only update the #ShaderParameters.flags and #ShaderParameters.shuffle. Other parameters
   * are updated inside the image engine.
   */
  virtual void get_shader_parameters(ShaderParameters &r_shader_parameters,
                                     ImBuf *image_buffer) = 0;

  /** \brief Is (wrap) repeat option enabled in the space. */
  virtual bool use_tile_drawing() const = 0;

  /** \brief Draw image with display window offsets. */
  virtual bool use_display_window() const = 0;

  /** \brief Gets the zoom factor of the space. A factor of 2 is a zoom-in by two times. */
  virtual float get_zoom() const = 0;

  /** \brief Gets the aspect ratio of the image. The ratio is for the vertical axis. */
  virtual float get_aspect_ratio() const = 0;

  /** \brief Gets the pan offset of the space in image pixel space. */
  virtual float2 get_pan_offset() const = 0;

  /**
   * \brief Canvas rotation in radians applied to the displayed image.
   *
   * Returns 0 when the space does not support canvas rotation (only the Image Editor does).
   */
  virtual float get_canvas_rotation() const
  {
    return 0.0f;
  }

  /** \brief Pivot for the canvas rotation in image UV space (0..1, default center). */
  virtual float2 get_canvas_rotation_pivot() const
  {
    return float2(0.5f);
  }
};

}  // namespace image_engine

}  // namespace blender
