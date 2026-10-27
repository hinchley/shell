#pragma once

// the wallpaper: ~/.config/backgrounds/background.png on a background layer surface behind
// everything on an output, scaled to cover it (cropping the overflow) over the theme's
// background colour.
#include "wayland.h"

// a wallpaper surface on output.
Surface *
WallpaperCreate(Output *output);

// nullptr is ignored.
void
WallpaperDestroy(Surface *surface);
