#include "wayland.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <unistd.h>

#include "input.h"
#include "memory.h"

// one shared memory buffer with a cairo surface over its pixels.
typedef struct Buffer {
  struct wl_buffer *buffer;
  cairo_surface_t *cairo;
  void *pixels;
  u64 size;
  u32 width;
  u32 height;
  // the compositor has not released it yet.
  bool busy;
  // the surface it is drawn for, or nullptr for one that is shared.
  Surface *owner;
} Buffer;

Wayland wayland;

// the one transparent pixel that blank surfaces show, stretched.
static Buffer *blankPixel;

static void
FreeBuffer(Buffer *buffer)
{
  if (!buffer) return;

  cairo_surface_destroy(buffer->cairo);
  wl_buffer_destroy(buffer->buffer);
  munmap(buffer->pixels, buffer->size);
  free(buffer);
}

static void
Draw(Surface *surface);

// a released buffer may be what a waiting draw needs.
static void
BufferRelease(void *context, [[maybe_unused]] struct wl_buffer *wlBuffer)
{
  Buffer *buffer = context;
  buffer->busy = false;

  Surface *surface = buffer->owner;
  if (!surface) return;
  if (surface->releaseBuffers) {
    for (u32 index = 0; index < 2; index++) {
      if (surface->buffers[index] == buffer) surface->buffers[index] = nullptr;
    }
    FreeBuffer(buffer);
  }
  if (surface->dirty && !surface->frame && !surface->drawing) Draw(surface);
}

static const struct wl_buffer_listener bufferListener = {.release = BufferRelease};

// a new width by height buffer, in the argb format cairo and every compositor share.
static Buffer *
CreateBuffer(Surface *owner, u32 width, u32 height)
{
  u32 stride = (u32)cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, (int)width);
  u64 size = (u64)stride * height;

  int file = memfd_create("buffer", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (file < 0) Fatal("memfd_create: %s", strerror(errno));
  if (ftruncate(file, (off_t)size) != 0) Fatal("ftruncate: %s", strerror(errno));

  void *pixels = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
  if (pixels == MAP_FAILED) Fatal("mmap: %s", strerror(errno));

  struct wl_shm_pool *pool = wl_shm_create_pool(wayland.shm, file, (i32)size);
  Buffer *buffer = MemoryAllocateZeroed(1, sizeof(Buffer));
  buffer->buffer = wl_shm_pool_create_buffer(
    pool,
    0,
    (i32)width,
    (i32)height,
    (i32)stride,
    WL_SHM_FORMAT_ARGB8888
  );
  wl_shm_pool_destroy(pool);
  close(file);

  buffer->cairo = cairo_image_surface_create_for_data(
    pixels,
    CAIRO_FORMAT_ARGB32,
    (int)width,
    (int)height,
    (int)stride
  );
  buffer->pixels = pixels;
  buffer->size = size;
  buffer->width = width;
  buffer->height = height;
  buffer->owner = owner;
  wl_buffer_add_listener(buffer->buffer, &bufferListener, buffer);
  return buffer;
}

// a buffer of the given size the compositor is not reading, made or remade as needed.
static Buffer *
FreeBufferFor(Surface *surface, u32 width, u32 height)
{
  for (u32 index = 0; index < 2; index++) {
    Buffer *buffer = surface->buffers[index];
    if (buffer && buffer->busy) continue;

    if (buffer && (buffer->width != width || buffer->height != height)) {
      FreeBuffer(buffer);
      buffer = nullptr;
    }
    if (!buffer) surface->buffers[index] = buffer = CreateBuffer(surface, width, height);
    return buffer;
  }
  return nullptr;
}

static void
FrameDone(void *context, struct wl_callback *callback, [[maybe_unused]] u32 time);

static const struct wl_callback_listener frameListener = {.done = FrameDone};

// commits a blank surface: the transparent pixel, stretched over it by its viewport.
static void
DrawBlank(Surface *surface)
{
  // the pixel is shared, belongs to no surface, and starts (and stays) transparent.
  if (!blankPixel) blankPixel = CreateBuffer(nullptr, 1, 1);
  surface->dirty = false;
  wp_viewport_set_destination(surface->viewport, (i32)surface->width, (i32)surface->height);
  wl_surface_attach(surface->surface, blankPixel->buffer, 0, 0);
  wl_surface_damage_buffer(surface->surface, 0, 0, 1, 1);
  wl_surface_commit(surface->surface);
}

// draws the surface into a free buffer and commits it.
static void
Draw(Surface *surface)
{
  if (!surface->configured || !surface->width || !surface->height) return;
  if (surface->blank && surface->viewport) {
    DrawBlank(surface);
    return;
  }

  // buffer pixels for the logical size, rounded as the fractional scale protocol asks.
  u32 width = (surface->width * surface->scale120 + 60) / 120;
  u32 height = (surface->height * surface->scale120 + 60) / 120;
  Buffer *buffer = FreeBufferFor(surface, width, height);
  // both buffers are still held: draw once one is released, at the next frame.
  if (!buffer) return;

  surface->dirty = false;
  surface->drawing = true;
  cairo_t *cairo = cairo_create(buffer->cairo);
  cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
  cairo_paint(cairo);
  cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
  cairo_scale(cairo, (f64)width / surface->width, (f64)height / surface->height);
  if (surface->handlers->Draw) surface->handlers->Draw(surface->context, surface, cairo);
  cairo_destroy(cairo);
  cairo_surface_flush(buffer->cairo);
  surface->drawing = false;

  if (surface->viewport) {
    wp_viewport_set_destination(surface->viewport, (i32)surface->width, (i32)surface->height);
  } else {
    wl_surface_set_buffer_scale(surface->surface, (i32)(surface->scale120 / 120));
  }
  wl_surface_attach(surface->surface, buffer->buffer, 0, 0);
  wl_surface_damage_buffer(surface->surface, 0, 0, (i32)width, (i32)height);
  surface->frame = wl_surface_frame(surface->surface);
  wl_callback_add_listener(surface->frame, &frameListener, surface);
  wl_surface_commit(surface->surface);
  // hyprland only repaints soon for a commit on the surface with the role, so a subsurface's
  // new frame could wait for something else (the pointer moving) to show.
  if (surface->parent) wl_surface_commit(surface->parent->surface);
  buffer->busy = true;
}

static void
FrameDone(void *context, struct wl_callback *callback, [[maybe_unused]] u32 time)
{
  Surface *surface = context;
  wl_callback_destroy(callback);
  surface->frame = nullptr;
  if (surface->dirty) Draw(surface);
}

void
SurfaceRedraw(Surface *surface)
{
  surface->dirty = true;
  if (!surface->frame && !surface->drawing) Draw(surface);
}

// draws surface, and the subsurfaces on it, at scale (in 120ths).
// the compositor tells only a surface with a role its scale, so its subsurfaces take it too.
static void
SetScale(Surface *surface, u32 scale)
{
  if (surface->scale120 == scale) return;

  surface->scale120 = scale;
  SurfaceRedraw(surface);
  for (Surface *child = surface->children; child; child = child->sibling) {
    SetScale(child, scale);
  }
}

static void
PreferredScale(
  void *context,
  [[maybe_unused]] struct wp_fractional_scale_v1 *fractionalScale,
  u32 scale
)
{
  SetScale(context, scale);
}

static const struct wp_fractional_scale_v1_listener fractionalScaleListener = {
  .preferred_scale = PreferredScale
};

// the compositor's integer scale, used only when it lacks fractional scaling.
static void
PreferredBufferScale(void *context, [[maybe_unused]] struct wl_surface *wlSurface, i32 factor)
{
  Surface *surface = context;
  if (surface->fractionalScale || surface->parent) return;

  SetScale(surface, (u32)factor * 120);
}

static void
Enter(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct wl_surface *wlSurface,
  [[maybe_unused]] struct wl_output *output
)
{
}

static void
Leave(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct wl_surface *wlSurface,
  [[maybe_unused]] struct wl_output *output
)
{
}

static void
PreferredBufferTransform(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct wl_surface *wlSurface,
  [[maybe_unused]] u32 transform
)
{
}

static const struct wl_surface_listener surfaceListener = {
  .enter = Enter,
  .leave = Leave,
  .preferred_buffer_scale = PreferredBufferScale,
  .preferred_buffer_transform = PreferredBufferTransform
};

// a surface without a role yet, set up for scaling.
// a subsurface (with a parent) is drawn at its parent's scale.
static Surface *
CreateSurface(const SurfaceHandlers *handlers, void *context, Surface *parent)
{
  Surface *surface = MemoryAllocateZeroed(1, sizeof(Surface));
  surface->handlers = handlers;
  surface->context = context;
  surface->parent = parent;
  surface->scale120 = parent ? parent->scale120 : 120;
  surface->cursor = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
  surface->surface = wl_compositor_create_surface(wayland.compositor);
  wl_surface_add_listener(surface->surface, &surfaceListener, surface);

  if (wayland.viewporter && wayland.fractionalScaleManager) {
    surface->viewport = wp_viewporter_get_viewport(wayland.viewporter, surface->surface);
  }
  if (surface->viewport && !parent) {
    surface->fractionalScale = wp_fractional_scale_manager_v1_get_fractional_scale(
      wayland.fractionalScaleManager,
      surface->surface
    );
    wp_fractional_scale_v1_add_listener(
      surface->fractionalScale,
      &fractionalScaleListener,
      surface
    );
  }
  return surface;
}

static void
LayerConfigure(
  void *context,
  struct zwlr_layer_surface_v1 *layer,
  u32 serial,
  u32 width,
  u32 height
)
{
  Surface *surface = context;
  zwlr_layer_surface_v1_ack_configure(layer, serial);

  bool resized = surface->width != width || surface->height != height;
  surface->width = width;
  surface->height = height;
  surface->configured = true;
  if (resized && surface->handlers->Resized) {
    surface->handlers->Resized(surface->context, surface);
  }
  SurfaceRedraw(surface);
}

static void
LayerClosed(void *context, [[maybe_unused]] struct zwlr_layer_surface_v1 *layer)
{
  Surface *surface = context;
  if (surface->handlers->Closed) surface->handlers->Closed(surface->context, surface);
}

static const struct zwlr_layer_surface_v1_listener layerListener = {
  .configure = LayerConfigure,
  .closed = LayerClosed
};

Surface *
SurfaceCreateLayer(const LayerOptions *options, const SurfaceHandlers *handlers, void *context)
{
  Surface *surface = CreateSurface(handlers, context, nullptr);
  surface->output = options->output;
  surface->layer = zwlr_layer_shell_v1_get_layer_surface(
    wayland.layerShell,
    surface->surface,
    options->output ? options->output->output : nullptr,
    options->layer,
    options->nameSpace
  );
  zwlr_layer_surface_v1_add_listener(surface->layer, &layerListener, surface);
  zwlr_layer_surface_v1_set_anchor(surface->layer, options->anchor);
  zwlr_layer_surface_v1_set_size(surface->layer, options->width, options->height);
  zwlr_layer_surface_v1_set_exclusive_zone(surface->layer, options->exclusiveZone);
  zwlr_layer_surface_v1_set_keyboard_interactivity(surface->layer, options->keyboard);
  // the first commit, without a buffer, asks the compositor for a configure.
  wl_surface_commit(surface->surface);
  return surface;
}

static void
XdgConfigure(void *context, struct xdg_surface *xdgSurface, u32 serial)
{
  Surface *surface = context;
  xdg_surface_ack_configure(xdgSurface, serial);
  surface->configured = true;
  SurfaceRedraw(surface);
}

static const struct xdg_surface_listener xdgSurfaceListener = {.configure = XdgConfigure};

static void
PopupConfigure(
  void *context,
  [[maybe_unused]] struct xdg_popup *popup,
  [[maybe_unused]] i32 x,
  [[maybe_unused]] i32 y,
  i32 width,
  i32 height
)
{
  Surface *surface = context;
  if (width <= 0 || height <= 0) return;
  if (surface->width == (u32)width && surface->height == (u32)height) return;

  surface->width = (u32)width;
  surface->height = (u32)height;
  if (surface->handlers->Resized) surface->handlers->Resized(surface->context, surface);
}

static void
PopupDone(void *context, [[maybe_unused]] struct xdg_popup *popup)
{
  Surface *surface = context;
  if (surface->handlers->Closed) surface->handlers->Closed(surface->context, surface);
}

static void
PopupRepositioned(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct xdg_popup *popup,
  [[maybe_unused]] u32 token
)
{
}

static const struct xdg_popup_listener popupListener =
  {.configure = PopupConfigure, .popup_done = PopupDone, .repositioned = PopupRepositioned};

Surface *
SurfaceCreatePopup(const PopupOptions *options, const SurfaceHandlers *handlers, void *context)
{
  Surface *parent = options->parent;
  Surface *surface = CreateSurface(handlers, context, nullptr);
  surface->output = parent->output;
  surface->width = options->width;
  surface->height = options->height;
  surface->xdgSurface = xdg_wm_base_get_xdg_surface(wayland.wmBase, surface->surface);
  xdg_surface_add_listener(surface->xdgSurface, &xdgSurfaceListener, surface);

  // the popup hangs centred below the anchor rectangle (or to its right), moved by the offset,
  // and slides (or flips) to stay on screen.
  struct xdg_positioner *positioner = xdg_wm_base_create_positioner(wayland.wmBase);
  xdg_positioner_set_size(positioner, (i32)options->width, (i32)options->height);
  xdg_positioner_set_anchor_rect(
    positioner,
    options->anchorX,
    options->anchorY,
    options->anchorWidth > 0 ? options->anchorWidth : 1,
    options->anchorHeight > 0 ? options->anchorHeight : 1
  );
  if (options->placement == PopupPlacementBeside) {
    xdg_positioner_set_anchor(positioner, XDG_POSITIONER_ANCHOR_TOP_RIGHT);
    xdg_positioner_set_gravity(positioner, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
  } else {
    xdg_positioner_set_anchor(positioner, XDG_POSITIONER_ANCHOR_BOTTOM);
    xdg_positioner_set_gravity(positioner, XDG_POSITIONER_GRAVITY_BOTTOM);
  }
  xdg_positioner_set_offset(positioner, options->offsetX, options->offsetY);
  xdg_positioner_set_constraint_adjustment(
    positioner,
    XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
      XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
      XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
      XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y
  );

  if (parent->layer) {
    surface->popup = xdg_surface_get_popup(surface->xdgSurface, nullptr, positioner);
    zwlr_layer_surface_v1_get_popup(parent->layer, surface->popup);
  } else {
    surface->popup =
      xdg_surface_get_popup(surface->xdgSurface, parent->xdgSurface, positioner);
  }
  xdg_positioner_destroy(positioner);
  xdg_popup_add_listener(surface->popup, &popupListener, surface);
  if (options->grab) xdg_popup_grab(surface->popup, wayland.seat, InputSerial());
  wl_surface_commit(surface->surface);
  return surface;
}

Surface *
SurfaceCreateSubsurface(
  Surface *parent,
  i32 x,
  i32 y,
  u32 width,
  u32 height,
  const SurfaceHandlers *handlers,
  void *context
)
{
  Surface *surface = CreateSurface(handlers, context, parent);
  surface->sibling = parent->children;
  parent->children = surface;
  surface->output = parent->output;
  surface->width = width;
  surface->height = height;
  surface->subsurface =
    wl_subcompositor_get_subsurface(wayland.subcompositor, surface->surface, parent->surface);
  wl_subsurface_set_position(surface->subsurface, x, y);
  // it commits on its own, without waiting for the parent's commits.
  wl_subsurface_set_desync(surface->subsurface);
  // a subsurface has no configure: it can be drawn straight away.
  surface->configured = true;
  SurfaceRedraw(surface);
  return surface;
}

void
SurfaceMove(Surface *surface, i32 x, i32 y)
{
  wl_subsurface_set_position(surface->subsurface, x, y);
  // the position applies with the parent's next commit.
  wl_surface_commit(surface->parent->surface);
}

void
SurfaceDestroy(Surface *surface)
{
  if (!surface) return;

  InputForgetSurface(surface);
  if (surface->parent) {
    Surface **link = &surface->parent->children;
    while (*link != surface) link = &(*link)->sibling;
    *link = surface->sibling;
  }
  if (surface->frame) wl_callback_destroy(surface->frame);
  if (surface->fractionalScale) wp_fractional_scale_v1_destroy(surface->fractionalScale);
  if (surface->viewport) wp_viewport_destroy(surface->viewport);
  if (surface->popup) xdg_popup_destroy(surface->popup);
  if (surface->subsurface) wl_subsurface_destroy(surface->subsurface);
  if (surface->xdgSurface) xdg_surface_destroy(surface->xdgSurface);
  if (surface->layer) zwlr_layer_surface_v1_destroy(surface->layer);
  wl_surface_destroy(surface->surface);
  for (u32 index = 0; index < 2; index++) FreeBuffer(surface->buffers[index]);
  free(surface);
}

void
SurfaceSetCursor(Surface *surface, u32 shape)
{
  if (surface->cursor == shape) return;

  surface->cursor = shape;
  InputUpdateCursor(surface);
}

Surface *
SurfaceFromWayland(struct wl_surface *surface)
{
  if (!surface) return nullptr;
  // every surface we create has our listener, so its user data is the Surface.
  if (wl_proxy_get_listener((struct wl_proxy *)surface) != (const void *)&surfaceListener) {
    return nullptr;
  }
  return wl_surface_get_user_data(surface);
}

static void
OutputGeometry(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct wl_output *wlOutput,
  [[maybe_unused]] i32 x,
  [[maybe_unused]] i32 y,
  [[maybe_unused]] i32 physicalWidth,
  [[maybe_unused]] i32 physicalHeight,
  [[maybe_unused]] i32 subpixel,
  [[maybe_unused]] const char *make,
  [[maybe_unused]] const char *model,
  [[maybe_unused]] i32 transform
)
{
}

static void
OutputMode(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct wl_output *wlOutput,
  [[maybe_unused]] u32 flags,
  [[maybe_unused]] i32 width,
  [[maybe_unused]] i32 height,
  [[maybe_unused]] i32 refresh
)
{
}

static void
OutputDone(void *context, [[maybe_unused]] struct wl_output *wlOutput)
{
  Output *output = context;
  if (output->ready) return;

  output->ready = true;
  if (wayland.outputHandlers.Added) {
    wayland.outputHandlers.Added(wayland.outputContext, output);
  }
}

static void
OutputScale(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct wl_output *wlOutput,
  [[maybe_unused]] i32 factor
)
{
}

static void
OutputName(void *context, [[maybe_unused]] struct wl_output *wlOutput, const char *name)
{
  Output *output = context;
  free(output->connector);
  output->connector = MemoryCopyString(name);
}

static void
OutputDescription(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct wl_output *wlOutput,
  [[maybe_unused]] const char *description
)
{
}

static const struct wl_output_listener outputListener = {
  .geometry = OutputGeometry,
  .mode = OutputMode,
  .done = OutputDone,
  .scale = OutputScale,
  .name = OutputName,
  .description = OutputDescription
};

static void
AddOutput(u32 name, u32 version)
{
  Output *output = MemoryAllocateZeroed(1, sizeof(Output));
  output->name = name;
  output->output =
    wl_registry_bind(wayland.registry, name, &wl_output_interface, version < 4 ? version : 4);
  wl_output_add_listener(output->output, &outputListener, output);

  if (wayland.outputCount == wayland.outputCapacity) {
    wayland.outputCapacity = wayland.outputCapacity ? wayland.outputCapacity * 2 : 4;
    wayland.outputs = MemoryResize(wayland.outputs, wayland.outputCapacity, sizeof(Output *));
  }
  wayland.outputs[wayland.outputCount++] = output;
}

static void
RegistryGlobal(
  [[maybe_unused]] void *context,
  struct wl_registry *registry,
  u32 name,
  const char *interface,
  u32 version
)
{
  if (strcmp(interface, wl_compositor_interface.name) == 0) {
    // version 6 has the surface's preferred buffer scale.
    wayland.compositor =
      wl_registry_bind(registry, name, &wl_compositor_interface, version < 6 ? version : 6);
  } else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
    wayland.subcompositor = wl_registry_bind(registry, name, &wl_subcompositor_interface, 1);
  } else if (strcmp(interface, wl_shm_interface.name) == 0) {
    wayland.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
  } else if (strcmp(interface, wl_seat_interface.name) == 0 && !wayland.seat) {
    // version 8 has high resolution (value120) scrolling.
    wayland.seat =
      wl_registry_bind(registry, name, &wl_seat_interface, version < 8 ? version : 8);
  } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
    wayland.dataDeviceManager =
      wl_registry_bind(registry, name, &wl_data_device_manager_interface, 3);
  } else if (strcmp(interface, wl_output_interface.name) == 0) {
    AddOutput(name, version);
  } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
    wayland.layerShell = wl_registry_bind(
      registry,
      name,
      &zwlr_layer_shell_v1_interface,
      version < 4 ? version : 4
    );
  } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
    wayland.wmBase = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
  } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
    wayland.viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
  } else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
    wayland.fractionalScaleManager =
      wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1);
  } else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
    wayland.cursorShapeManager =
      wl_registry_bind(registry, name, &wp_cursor_shape_manager_v1_interface, 1);
  }
}

static void
RegistryGlobalRemove(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct wl_registry *registry,
  u32 name
)
{
  for (u64 index = 0; index < wayland.outputCount; index++) {
    Output *output = wayland.outputs[index];
    if (output->name != name) continue;

    if (output->ready && wayland.outputHandlers.Removed) {
      wayland.outputHandlers.Removed(wayland.outputContext, output);
    }
    wl_output_release(output->output);
    free(output->connector);
    free(output);
    wayland.outputs[index] = wayland.outputs[--wayland.outputCount];
    return;
  }
}

static const struct wl_registry_listener registryListener = {
  .global = RegistryGlobal,
  .global_remove = RegistryGlobalRemove
};

static void
WmBasePing([[maybe_unused]] void *context, struct xdg_wm_base *wmBase, u32 serial)
{
  xdg_wm_base_pong(wmBase, serial);
}

static const struct xdg_wm_base_listener wmBaseListener = {.ping = WmBasePing};

// reads and dispatches whatever the compositor sent.
static int
DisplayReadable(
  [[maybe_unused]] sd_event_source *source,
  [[maybe_unused]] int file,
  u32 events,
  [[maybe_unused]] void *context
)
{
  if (events & (EPOLLHUP | EPOLLERR) || wl_display_dispatch(wayland.display) < 0) {
    Warn("lost the connection to the compositor");
    WaylandQuit(1);
  }
  return 0;
}

void
WaylandConnect(sd_event *loop, const OutputHandlers *handlers, void *context)
{
  wayland.loop = loop;
  if (handlers) wayland.outputHandlers = *handlers;
  wayland.outputContext = context;

  wayland.display = wl_display_connect(nullptr);
  if (!wayland.display) Fatal("cannot connect to the wayland compositor");

  wayland.registry = wl_display_get_registry(wayland.display);
  wl_registry_add_listener(wayland.registry, &registryListener, nullptr);
  // the first roundtrip binds the globals, the second gets the outputs' and seat's details.
  wl_display_roundtrip(wayland.display);
  if (!wayland.compositor || !wayland.shm) {
    Fatal("the compositor lacks wl_compositor or wl_shm");
  }
  if (!wayland.layerShell) Fatal("the compositor lacks wlr-layer-shell");
  if (wayland.wmBase) xdg_wm_base_add_listener(wayland.wmBase, &wmBaseListener, nullptr);
  // after the first roundtrip, so the cursor shape and data device managers are bound too.
  if (wayland.seat) InputAttach(wayland.seat);
  wl_display_roundtrip(wayland.display);

  sd_event_add_io(
    loop,
    nullptr,
    wl_display_get_fd(wayland.display),
    EPOLLIN,
    DisplayReadable,
    nullptr
  );
}

int
WaylandRun(void)
{
  wayland.running = true;
  while (wayland.running) {
    // handle anything already queued, and send our requests, before sleeping.
    wl_display_dispatch_pending(wayland.display);
    if (wl_display_flush(wayland.display) < 0 && errno != EAGAIN) {
      Warn("lost the connection to the compositor");
      return 1;
    }
    if (sd_event_run(wayland.loop, UINT64_MAX) < 0) break;
  }
  wl_display_flush(wayland.display);
  return wayland.exitCode;
}

void
WaylandQuit(int exitCode)
{
  wayland.exitCode = exitCode;
  wayland.running = false;
}
