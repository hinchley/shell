#pragma once

// the connection to the compositor: its globals, the outputs, and surfaces drawn with cairo
// into shared memory buffers.
// everything runs on one sd-event loop, which the apps also use for their own timers, signals,
// d-bus connections and child processes.
#include <cairo.h>
#include <systemd/sd-event.h>
#include <wayland-client.h>

#include "cursor-shape-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "types.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

typedef struct Surface Surface;

// a monitor.
typedef struct Output {
  struct wl_output *output;
  // the registry name, which identifies the output for the life of the connection.
  u32 name;
  // the connector, such as "DP-1", once the compositor has sent it.
  char *connector;
  // set once the output's first batch of properties has arrived.
  bool ready;
  // whatever the app keeps for this output.
  void *data;
} Output;

// what an app is told about outputs coming and going.
// both are optional.
typedef struct OutputHandlers {
  void (*Added)(void *context, Output *output);
  // called before the output is freed.
  void (*Removed)(void *context, Output *output);
} OutputHandlers;

// the compositor's globals, bound once at startup.
typedef struct Wayland {
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_compositor *compositor;
  struct wl_shm *shm;
  struct wl_subcompositor *subcompositor;
  struct wl_seat *seat;
  struct wl_data_device_manager *dataDeviceManager;
  struct zwlr_layer_shell_v1 *layerShell;
  struct xdg_wm_base *wmBase;
  struct wp_viewporter *viewporter;
  struct wp_fractional_scale_manager_v1 *fractionalScaleManager;
  struct wp_cursor_shape_manager_v1 *cursorShapeManager;
  sd_event *loop;

  Output **outputs;
  u64 outputCount;
  u64 outputCapacity;
  OutputHandlers outputHandlers;
  void *outputContext;

  bool running;
  int exitCode;
} Wayland;

// the one connection each app has.
extern Wayland wayland;

// what a surface's owner is told.
// every handler is optional, and is given the context the surface was created with.
typedef struct SurfaceHandlers {
  // draws the whole surface.
  // cairo is already scaled, so coordinates are logical pixels from the top left.
  void (*Draw)(void *context, Surface *surface, cairo_t *cairo);
  // the compositor gave the surface a new logical size (in width and height).
  void (*Resized)(void *context, Surface *surface);
  // the compositor closed the surface (a layer surface's output went away, or a popup was
  // dismissed by a click elsewhere).
  // the owner should destroy it.
  void (*Closed)(void *context, Surface *surface);

  void (*PointerMotion)(void *context, Surface *surface, f64 x, f64 y);
  void (*PointerLeave)(void *context, Surface *surface);
  // button is a linux input code (BTN_LEFT and so on), time is in milliseconds.
  void (*PointerButton)(void *context, Surface *surface, u32 button, bool pressed, u32 time);
  // one frame's vertical scrolling in 120ths of a wheel notch, positive downwards.
  void (*PointerScroll)(void *context, Surface *surface, i32 value120);

  void (*KeyboardFocus)(void *context, Surface *surface, bool focused);
  // a key press (or repeat).
  // codepoint is the character it types, or 0.
  // modifiers is a set of InputModifier bits.
  void (*Key)(void *context, Surface *surface, u32 keysym, u32 codepoint, u32 modifiers);
} SurfaceHandlers;

// a drawable surface with the role of a layer surface or a popup.
struct Surface {
  struct wl_surface *surface;
  struct zwlr_layer_surface_v1 *layer;
  struct xdg_surface *xdgSurface;
  struct xdg_popup *popup;
  struct wl_subsurface *subsurface;
  // the surface a subsurface is placed on.
  Surface *parent;
  // the subsurfaces placed on this surface, linked through their sibling.
  Surface *children;
  Surface *sibling;
  struct wp_viewport *viewport;
  struct wp_fractional_scale_v1 *fractionalScale;
  struct wl_callback *frame;
  Output *output;

  // the logical size.
  u32 width;
  u32 height;
  // the scale from logical to buffer pixels, in 120ths (120 is 1x).
  u32 scale120;
  bool configured;
  // a draw is wanted at the next frame.
  bool dirty;
  // the Draw handler is running, so a redraw it asks for waits for the next frame.
  bool drawing;
  // the cursor shown while the pointer is over the surface (a cursor shape constant).
  u32 cursor;
  // frees each buffer as soon as the compositor has taken its contents, for a large surface
  // that rarely changes (the wallpaper), rather than keeping it to draw into again.
  bool releaseBuffers;
  // shows nothing: a single transparent pixel stretched over the surface, for a large surface
  // that only takes input (and holds subsurfaces).
  // its Draw handler is not called.
  bool blank;

  // two buffers, so one can be drawn while the compositor still reads the other.
  struct Buffer *buffers[2];

  const SurfaceHandlers *handlers;
  void *context;
};

// where and how a layer surface is placed.
typedef struct LayerOptions {
  // nullptr lets the compositor choose (the focused output).
  Output *output;
  enum zwlr_layer_shell_v1_layer layer;
  // a set of ZWLR_LAYER_SURFACE_V1_ANCHOR bits.
  u32 anchor;
  // 0 stretches the surface between opposite anchored edges.
  u32 width;
  u32 height;
  // how much of the output the surface reserves, or -1 to ignore other surfaces' zones.
  i32 exclusiveZone;
  enum zwlr_layer_surface_v1_keyboard_interactivity keyboard;
  const char *nameSpace;
} LayerOptions;

// which side of its anchor rectangle a popup opens on.
enum PopupPlacement {
  // centred below it, as a menu from a bar.
  PopupPlacementBelow,
  // to its right, top edges level, as a submenu.
  PopupPlacementBeside
};

// where a popup is placed, relative to a rectangle of its parent (logical pixels).
typedef struct PopupOptions {
  Surface *parent;
  i32 anchorX;
  i32 anchorY;
  i32 anchorWidth;
  i32 anchorHeight;
  enum PopupPlacement placement;
  // how far the popup is moved from its place by the anchor rectangle.
  i32 offsetX;
  i32 offsetY;
  u32 width;
  u32 height;
  // takes keyboard focus, and closes on a click outside.
  bool grab;
} PopupOptions;

// connects, binds the globals and learns the outputs.
// ends the program when the compositor lacks something needed.
void
WaylandConnect(sd_event *loop, const OutputHandlers *handlers, void *context);

// runs the event loop until WaylandQuit, then returns the exit code given to it.
int
WaylandRun(void);

void
WaylandQuit(int exitCode);

Surface *
SurfaceCreateLayer(
  const LayerOptions *options,
  const SurfaceHandlers *handlers,
  void *context
);

Surface *
SurfaceCreatePopup(
  const PopupOptions *options,
  const SurfaceHandlers *handlers,
  void *context
);

// a subsurface of parent, width by height, with its top left at x, y on parent.
// it shows once parent is shown, and stays above it.
Surface *
SurfaceCreateSubsurface(
  Surface *parent,
  i32 x,
  i32 y,
  u32 width,
  u32 height,
  const SurfaceHandlers *handlers,
  void *context
);

// moves a subsurface's top left to x, y on its parent.
void
SurfaceMove(Surface *surface, i32 x, i32 y);

// destroys the surface.
// nullptr is ignored.
void
SurfaceDestroy(Surface *surface);

// asks for the surface to be drawn again: now, or at the next frame when one is pending.
// a Draw handler can call it to animate, drawing again at the next frame.
void
SurfaceRedraw(Surface *surface);

// sets the cursor shown over the surface (WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT and so on).
void
SurfaceSetCursor(Surface *surface, u32 shape);

// the surface that owns a wl_surface, or nullptr when it is not one of ours.
Surface *
SurfaceFromWayland(struct wl_surface *surface);
