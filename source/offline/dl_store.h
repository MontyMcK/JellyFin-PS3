#pragma once
#include <stdint.h>
#include "dl_model.h"

// -------------------------------------------------------------------------
//  Offline downloads -- on-disk layout
// -------------------------------------------------------------------------
//  One root, one directory per item, fixed file names inside it:
//
//    <root>/
//      layout.txt                "jfdl-layout 1"   (format version marker)
//      items/
//        <item id>/
//          state.txt             DlRecord  -- queue/transfer bookkeeping
//          meta.txt              DlMeta    -- what to show, how to play it
//          media.ts.part         media while it is still arriving
//          media.ts              media once complete and verified
//          poster.jpg            artwork (optional)
//          backdrop.jpg          artwork (optional)
//
//  Why per item rather than media/ meta/ art/ state/ side by side: an item is
//  the unit everything happens to -- enqueue, cancel, delete, restore -- and
//  with its files together, deleting one is "remove these known names, then
//  the directory", with nothing to leave orphaned in a sibling tree.  The
//  store only ever deletes the names listed above; it never recursively
//  deletes whatever it finds, so a mistyped root cannot eat the HDD.
//
//  The media file is .ts because the existing player consumes MPEG-TS (see
//  video/ts_demux.cpp); every download is requested in that form.
//
//  Where <root> is on the console is decided in dl_ps3.cpp.
// -------------------------------------------------------------------------

#define DL_FILE_STATE    "state.txt"
#define DL_FILE_META     "meta.txt"
#define DL_FILE_MEDIA    "media.ts"
#define DL_FILE_PART     "media.ts.part"
#define DL_FILE_POSTER   "poster.jpg"
#define DL_FILE_BACKDROP "backdrop.jpg"

#define DL_PATH_MAX 256

// Creates <root>, <root>/items and the layout marker.  False if the root
// cannot be created or written -- the caller should try another root.
bool dl_store_init(const char *root);
const char *dl_store_root(void);

// "<root>/items/<id>" and "<root>/items/<id>/<leaf>".  False (and "")
// for an invalid id or a path that would not fit.
bool dl_store_item_dir(char *out, int cap, const char *id);
bool dl_store_item_file(char *out, int cap, const char *id, const char *leaf);

// Records are written atomically: to <file>.tmp, then swapped in.  A crash
// at any point leaves either the old record or the new one readable (load
// falls back to .tmp when the main file is missing or damaged).
bool dl_store_save_record(const DlRecord *r);
bool dl_store_load_record(const char *id, DlRecord *out);
bool dl_store_save_meta(const DlMeta *m);
bool dl_store_load_meta(const char *id, DlMeta *out);

bool dl_store_make_item_dir(const char *id);
// Deletes the item's known files, then its directory.  True when the
// directory is gone afterwards.
bool dl_store_remove_item(const char *id);
// A small binary file (artwork) written the same atomic way as records:
// <leaf>.tmp, then swapped in.  A reader never sees half an image.
bool dl_store_save_blob(const char *id, const char *leaf,
                        const uint8_t *data, int len);

// Deletes only the partial media (cancel).
bool dl_store_remove_partial(const char *id);

// Sub-directories of <root>/items whose names are valid ids.  Returns the
// count written to ids[] (at most max).
int dl_store_list_ids(char (*ids)[DL_ID_MAX], int max);
