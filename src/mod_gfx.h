#pragma once
#ifndef CATA_SRC_MOD_GFX_H
#define CATA_SRC_MOD_GFX_H

#include <string>
#include <vector>

#include "point.h"

/**
 * Draws a picture (the bytes of a jpg / png file) on top of the text cells of the screen.
 * Used by the pictures in mod windows (album covers).  Call it right after the window was
 * refreshed (wnoutrefresh), so the picture ends up above the window and below windows that are
 * drawn later.
 *
 * @param key       name of the picture; the decoded picture is remembered under this name
 * @param data      file bytes, only used the first time a key is seen.  Empty = grey placeholder.
 * @param cell_pos  top left cell of the area (screen cells)
 * @param cell_size size of the area in cells; the picture is shrunk to fit and centered.
 * @return true if a real picture was drawn.  Always false in the curses (non graphical) build.
 */
#if defined(TILES)
bool mod_gfx_draw_image( const std::string &key, const std::vector<unsigned char> &data,
                         const point &cell_pos, const point &cell_size );
/** Forget all remembered pictures. */
void mod_gfx_forget_images();
#else
inline bool mod_gfx_draw_image( const std::string &, const std::vector<unsigned char> &,
                                const point &, const point & )
{
    return false;
}
inline void mod_gfx_forget_images() { }
#endif

#endif // CATA_SRC_MOD_GFX_H
