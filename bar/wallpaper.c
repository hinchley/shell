#include "wallpaper.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "draw.h"
#include "memory.h"
#include "theme.h"

// the image is decoded only while a wallpaper is drawn (at start, or when an output's size or
// scale changes) and freed straight after: decoded, a large wallpaper is far bigger than the
// screen buffer it ends up in.
static void
Draw([[maybe_unused]] void *context, Surface *surface, cairo_t *cairo)
{
  DrawSetColour(cairo, themeBg);
  cairo_paint(cairo);

  const char *home = getenv("HOME");
  char path[1024];
  snprintf(path, sizeof(path), "%s/.config/backgrounds/background.png", home ? home : "");
  cairo_surface_t *image = cairo_image_surface_create_from_png(path);
  if (cairo_surface_status(image) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(image);
    return;
  }

  // cover the surface, keeping the image's shape and centring what overflows.
  f64 imageWidth = cairo_image_surface_get_width(image);
  f64 imageHeight = cairo_image_surface_get_height(image);
  f64 scale = fmax(surface->width / imageWidth, surface->height / imageHeight);
  cairo_translate(
    cairo,
    (surface->width - imageWidth * scale) / 2,
    (surface->height - imageHeight * scale) / 2
  );
  cairo_scale(cairo, scale, scale);
  cairo_set_source_surface(cairo, image, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cairo), CAIRO_FILTER_GOOD);
  cairo_paint(cairo);
  cairo_surface_destroy(image);
  MemoryTrim();
}

static const SurfaceHandlers handlers = {.Draw = Draw};

Surface *
WallpaperCreate(Output *output)
{
  LayerOptions options = {
    .output = output,
    .layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
    .anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
      ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
    .exclusiveZone = -1,
    .keyboard = ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE,
    .nameSpace = "wallpaper"
  };
  Surface *surface = SurfaceCreateLayer(&options, &handlers, nullptr);
  surface->releaseBuffers = true;
  return surface;
}

void
WallpaperDestroy(Surface *surface)
{
  SurfaceDestroy(surface);
}
