#pragma once

// finding and loading tray icons: by name through the icon theme (as the freedesktop icon
// theme spec describes, if simplified), or from the pixmaps an item sends itself.
#include <cairo.h>

#include "types.h"

// the icon called name (or at the absolute path name), size pixels square, or nullptr when it
// cannot be found.
// themePath is a folder an item gives to search first (may be nullptr).
cairo_surface_t *
IconLoad(const char *name, const char *themePath, u32 size);

// an icon from argb32 pixels in network byte order (as d-bus items send them), scaled to size
// pixels square.
cairo_surface_t *
IconFromPixels(const u8 *pixels, i32 width, i32 height, u32 size);
