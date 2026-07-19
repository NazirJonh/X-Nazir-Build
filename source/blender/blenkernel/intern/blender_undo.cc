/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Blend file undo (known as 'Global Undo').
 * DNA level diffing for undo.
 */

#ifndef _WIN32
#  include <unistd.h> /* for read close */
#else
#  include <io.h> /* for open close read */
#endif

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h> /* for open */

#include "DNA_userdef_types.h"

#include "BLI_path_utils.hh"
#include "BLI_string.h"

#include "BKE_appdir.hh"
#include "BKE_blender_undo.hh" /* own include */
#include "BKE_blendfile.hh"
#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_main.hh"
#include "BKE_sculpt_layers.hh"
#include "BKE_undo_system.hh"

#include "BLO_readfile.hh"
#include "BLO_undofile.hh"
#include "BLO_writefile.hh"

#include "DEG_depsgraph.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Global Undo
 * \{ */

#define UNDO_DISK 0

bool BKE_memfile_undo_decode(MemFileUndoData *mfu,
                             const eUndoStepDir undo_direction,
                             const bool use_old_bmain_data,
                             bContext *C)
{
  Main *bmain = CTX_data_main(C);
  char mainstr[sizeof(bmain->filepath)];
  int success = 0, fileflags;

  STRNCPY(mainstr, BKE_main_blendfile_path(bmain)); /* temporal store */

  /* A sculpt-layer weight-mask editing session cannot survive this read. The step being decoded was
   * encoded with every session suspended (see #BKE_memfile_undo_encode), so it holds the user's own
   * mask and the node's mask from before the session opened; the weights the session was authoring
   * are in neither, and the read is about to replace the storage they live in. Anything left of the
   * session afterwards would point at a buffer that is not the one it parked its mask out of, and
   * the next close would compress the user's restored mask onto the node and then overwrite that
   * mask with the parked copy — corrupting both.
   *
   * Given up here, before the read, while the storage the session borrowed is still the one it
   * borrowed. A #MaskEditSuspendGuard cannot serve: it would put the session back on the far side of
   * the read, which is the state this exists to prevent. */
  bke::sculpt_layers::mask_edit_abandon_all(*bmain);

  fileflags = G.fileflags;
  G.fileflags |= G_FILE_NO_UI;

  if (UNDO_DISK) {
    const BlendFileReadParams params{};
    BlendFileReadReport bf_reports{};
    BlendFileData *bfd = BKE_blendfile_read(mfu->filepath, &params, &bf_reports);
    if (bfd != nullptr) {
      BKE_blendfile_read_setup_undo(C, bfd, &params, &bf_reports);
      success = true;
    }
  }
  else {
    BlendFileReadParams params = {0};
    params.undo_direction = undo_direction;
    if (!use_old_bmain_data) {
      params.skip_flags |= BLO_READ_SKIP_UNDO_OLD_MAIN;
    }
    BlendFileReadReport blend_file_read_report{};
    BlendFileData *bfd = BKE_blendfile_read_from_memfile(bmain, &mfu->memfile, &params, nullptr);
    if (bfd != nullptr) {
      BKE_blendfile_read_setup_undo(C, bfd, &params, &blend_file_read_report);
      success = true;
    }
  }

  /* Restore, bmain has been re-allocated. */
  bmain = CTX_data_main(C);
  STRNCPY(bmain->filepath, mainstr);
  G.fileflags = fileflags;

  if (success) {
    /* important not to update time here, else non keyed transforms are lost */
    DEG_tag_on_visible_update(bmain, false);
  }

  return success;
}

MemFileUndoData *BKE_memfile_undo_encode(Main *bmain, MemFileUndoData *mfu_prev)
{
  MemFileUndoData *mfu = MEM_new_zeroed<MemFileUndoData>(__func__);

  /* A memfile undo step is a serialization of the whole #Main, so it is exposed to the same hazard
   * every other writer of the file is: while a sculpt-layer weight-mask editing session is open the
   * node's weights sit in the mesh's `.sculpt_mask` attribute (or in #SubdivCCG::masks), and a step
   * encoded there would record them as the user's own sculpt mask — to be handed back as such by
   * the undo that decodes it.
   *
   * Bracketed rather than made to close the session, exactly as auto-save is: a memfile step is
   * pushed by any operator that does not push one of its own, so the user runs into this while doing
   * something entirely unrelated to the mask they are painting, and closing their session as a side
   * effect would be both surprising and lossy. Inside the bracket the parked mask is back in place
   * and the encode sees precisely the state it would have seen with no session open.
   *
   * Placed on the primitive rather than on its caller (#memfile_undosys_step_encode), so that every
   * caller is covered by construction — including callers that do not exist yet — and so that both
   * the `UNDO_DISK` and the in-memory branch below are covered by one bracket. This mirrors where
   * the multires flush and #object_update_from_subsurf_ccg bracket themselves. */
  const bke::sculpt_layers::MaskEditSuspendGuard mask_edit_guard(*bmain);

  /* This flag used to be set because the undo step was written as #BLENDER_QUIT_FILE. It's not
   * clear whether there are still good reasons to keep it. Undo can also be thought of as a kind
   * of recovery, so better keep it for now. */
  const int fileflags = G.fileflags | G_FILE_RECOVER_WRITE;

  /* disk save version */
  if (UNDO_DISK) {
    static int counter = 0;
    char filepath[FILE_MAX];
    char numstr[32];

    /* Calculate current filepath. */
    counter++;
    counter = counter % U.undosteps;

    SNPRINTF(numstr, "%d.blend", counter);
    BLI_path_join(filepath, sizeof(filepath), BKE_tempdir_session(), numstr);

    const BlendFileWriteParams blend_file_write_params{};
    /* success = */ /* UNUSED */ BLO_write_file(
        bmain, filepath, fileflags, &blend_file_write_params, nullptr);

    STRNCPY(mfu->filepath, filepath);
  }
  else {
    MemFile *prevfile = (mfu_prev) ? &(mfu_prev->memfile) : nullptr;
    if (prevfile) {
      BLO_memfile_clear_future(prevfile);
    }
    /* success = */ /* UNUSED */ BLO_write_file_mem(bmain, prevfile, &mfu->memfile, fileflags);
    mfu->undo_size = mfu->memfile.size;
  }

  bmain->is_memfile_undo_written = true;

  return mfu;
}

void BKE_memfile_undo_free(MemFileUndoData *mfu)
{
  BLO_memfile_free(&mfu->memfile);
  MEM_delete(mfu);
}

/** \} */

}  // namespace blender
