#pragma once

// the seat: pointer, keyboard (with key repeat), cursor shapes and clipboard paste.
// events go to the handlers of the Surface they happen on.
#include <wayland-client.h>

#include "types.h"
#include "wayland.h"

// the modifier bits given with each key.
enum InputModifier {
  InputModifierShift = 1 << 0,
  InputModifierControl = 1 << 1,
  InputModifierAlt = 1 << 2,
  InputModifierSuper = 1 << 3
};

// starts listening to seat's devices.
void
InputAttach(struct wl_seat *seat);

// the serial of the latest button or key press, which a popup grab needs.
u32
InputSerial(void);

// stops sending events to surface, which is being destroyed.
void
InputForgetSurface(Surface *surface);

// shows surface's cursor, if the pointer is over it.
void
InputUpdateCursor(Surface *surface);

// asks for the clipboard's text.
// done is called later with it, or with nullptr when the clipboard holds no text.
void
InputPaste(void (*done)(void *context, const char *text), void *context);
