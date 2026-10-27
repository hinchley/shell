#include "input.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

#include "memory.h"
#include "strbuf.h"

// the mime types of text, best first.
static const char *const textTypes[] =
  {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING"};
static constexpr u32 textTypeCount = sizeof(textTypes) / sizeof(textTypes[0]);

// what a clipboard offer can give, learnt from its offer events.
typedef struct Offer {
  struct wl_data_offer *offer;
  // the best text type offered, as an index into textTypes, or textTypeCount for none.
  u32 textType;
} Offer;

// a paste in progress: the clipboard's text arriving through a pipe.
typedef struct Paste {
  StrBuf text;
  void (*done)(void *context, const char *text);
  void *context;
} Paste;

typedef struct Input {
  struct wl_seat *seat;
  struct wl_pointer *pointer;
  struct wl_keyboard *keyboard;
  struct wp_cursor_shape_device_v1 *cursorShape;
  struct wl_data_device *dataDevice;
  u32 serial;

  // the surface under the pointer, and the serial of the pointer's entering it.
  Surface *pointerSurface;
  u32 enterSerial;
  // scrolling gathered over one pointer frame, in 120ths of a notch.
  i32 scroll120;
  // whether the frame had wheel (value120) events, which then win over the smooth values.
  bool scrollDiscrete;
  f64 scrollSmooth;

  struct xkb_context *xkb;
  struct xkb_keymap *keymap;
  struct xkb_state *state;
  Surface *keyboardSurface;
  // the surface the keyboard just left, told once the compositor's events have been handled,
  // unless the keyboard enters it again (moving between a surface and its subsurface).
  Surface *keyboardLeft;
  sd_event_source *leaveNotice;

  // key repeat: the held key, and the timer that repeats it.
  u32 repeatKey;
  sd_event_source *repeatTimer;
  i32 repeatRate;
  i32 repeatDelay;

  Offer *selection;
} Input;

static Input input = {.repeatRate = 25, .repeatDelay = 600};

u32
InputSerial(void)
{
  return input.serial;
}

void
InputForgetSurface(Surface *surface)
{
  if (input.pointerSurface == surface) input.pointerSurface = nullptr;
  if (input.keyboardSurface == surface) {
    input.keyboardSurface = nullptr;
    input.repeatKey = 0;
  }
  if (input.keyboardLeft == surface) input.keyboardLeft = nullptr;
}

void
InputUpdateCursor(Surface *surface)
{
  if (!input.cursorShape || input.pointerSurface != surface) return;

  wp_cursor_shape_device_v1_set_shape(input.cursorShape, input.enterSerial, surface->cursor);
}

static void
PointerEnter(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct wl_pointer *pointer,
  u32 serial,
  struct wl_surface *wlSurface,
  wl_fixed_t x,
  wl_fixed_t y
)
{
  Surface *surface = SurfaceFromWayland(wlSurface);
  input.pointerSurface = surface;
  input.enterSerial = serial;
  if (!surface) return;

  InputUpdateCursor(surface);
  const SurfaceHandlers *handlers = surface->handlers;
  f64 surfaceX = wl_fixed_to_double(x);
  f64 surfaceY = wl_fixed_to_double(y);
  if (handlers->PointerMotion) {
    handlers->PointerMotion(surface->context, surface, surfaceX, surfaceY);
  }
}

static void
PointerLeave(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct wl_pointer *pointer,
  [[maybe_unused]] u32 serial,
  [[maybe_unused]] struct wl_surface *wlSurface
)
{
  Surface *surface = input.pointerSurface;
  input.pointerSurface = nullptr;
  if (surface && surface->handlers->PointerLeave) {
    surface->handlers->PointerLeave(surface->context, surface);
  }
}

static void
PointerMotion(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct wl_pointer *pointer,
  [[maybe_unused]] u32 time,
  wl_fixed_t x,
  wl_fixed_t y
)
{
  Surface *surface = input.pointerSurface;
  if (!surface || !surface->handlers->PointerMotion) return;

  f64 surfaceX = wl_fixed_to_double(x);
  f64 surfaceY = wl_fixed_to_double(y);
  surface->handlers->PointerMotion(surface->context, surface, surfaceX, surfaceY);
}

static void
PointerButton(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct wl_pointer *pointer,
  u32 serial,
  u32 time,
  u32 button,
  u32 state
)
{
  input.serial = serial;
  Surface *surface = input.pointerSurface;
  if (!surface || !surface->handlers->PointerButton) return;

  bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
  surface->handlers->PointerButton(surface->context, surface, button, pressed, time);
}

static void
PointerAxis(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct wl_pointer *pointer,
  [[maybe_unused]] u32 time,
  u32 axis,
  wl_fixed_t value
)
{
  if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) input.scrollSmooth += wl_fixed_to_double(value);
}

static void
PointerAxisValue120(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct wl_pointer *pointer,
  u32 axis,
  i32 value120
)
{
  if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;

  input.scroll120 += value120;
  input.scrollDiscrete = true;
}

// delivers the frame's scrolling.
// smooth (touchpad) scrolling is converted the way qt does, 10 pixels to a notch.
static void
PointerFrame([[maybe_unused]] void *context, [[maybe_unused]] struct wl_pointer *pointer)
{
  i32 value120 = input.scrollDiscrete ? input.scroll120 : (i32)(input.scrollSmooth * 12);
  input.scroll120 = 0;
  input.scrollSmooth = 0;
  input.scrollDiscrete = false;

  Surface *surface = input.pointerSurface;
  if (!value120 || !surface || !surface->handlers->PointerScroll) return;

  surface->handlers->PointerScroll(surface->context, surface, value120);
}

static void
PointerAxisSource(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct wl_pointer *pointer,
  [[maybe_unused]] u32 source
)
{
}

static void
PointerAxisStop(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct wl_pointer *pointer,
  [[maybe_unused]] u32 time,
  [[maybe_unused]] u32 axis
)
{
}

static void
PointerAxisDiscrete(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct wl_pointer *pointer,
  [[maybe_unused]] u32 axis,
  [[maybe_unused]] i32 discrete
)
{
}

static void
PointerAxisRelativeDirection(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct wl_pointer *pointer,
  [[maybe_unused]] u32 axis,
  [[maybe_unused]] u32 direction
)
{
}

static const struct wl_pointer_listener pointerListener = {
  .enter = PointerEnter,
  .leave = PointerLeave,
  .motion = PointerMotion,
  .button = PointerButton,
  .axis = PointerAxis,
  .frame = PointerFrame,
  .axis_source = PointerAxisSource,
  .axis_stop = PointerAxisStop,
  .axis_discrete = PointerAxisDiscrete,
  .axis_value120 = PointerAxisValue120,
  .axis_relative_direction = PointerAxisRelativeDirection
};

static void
KeyboardKeymap(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct wl_keyboard *keyboard,
  u32 format,
  i32 file,
  u32 size
)
{
  if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
    close(file);
    return;
  }

  char *text = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, file, 0);
  close(file);
  if (text == MAP_FAILED) return;

  struct xkb_keymap *keymap = xkb_keymap_new_from_string(
    input.xkb,
    text,
    XKB_KEYMAP_FORMAT_TEXT_V1,
    XKB_KEYMAP_COMPILE_NO_FLAGS
  );
  munmap(text, size);
  if (!keymap) return;

  xkb_state_unref(input.state);
  xkb_keymap_unref(input.keymap);
  input.keymap = keymap;
  input.state = xkb_state_new(keymap);
}

static void
KeyboardEnter(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct wl_keyboard *keyboard,
  u32 serial,
  struct wl_surface *wlSurface,
  [[maybe_unused]] struct wl_array *keys
)
{
  input.serial = serial;
  // keys belong to the surface with the role (a subsurface is only part of it).
  Surface *surface = SurfaceFromWayland(wlSurface);
  while (surface && surface->parent) surface = surface->parent;
  input.keyboardSurface = surface;
  // hyprland moves the focus to a subsurface under the pointer, and back: that is no change.
  if (surface && surface == input.keyboardLeft) {
    input.keyboardLeft = nullptr;
    return;
  }
  if (surface && surface->handlers->KeyboardFocus) {
    surface->handlers->KeyboardFocus(surface->context, surface, true);
  }
}

// tells the surface the keyboard left that it has gone, if it did not come straight back.
static int
Left([[maybe_unused]] sd_event_source *source, [[maybe_unused]] void *context)
{
  Surface *surface = input.keyboardLeft;
  input.keyboardLeft = nullptr;
  if (surface && surface->handlers->KeyboardFocus) {
    surface->handlers->KeyboardFocus(surface->context, surface, false);
  }
  return 0;
}

static void
KeyboardLeave(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct wl_keyboard *keyboard,
  [[maybe_unused]] u32 serial,
  [[maybe_unused]] struct wl_surface *wlSurface
)
{
  Left(nullptr, nullptr);
  input.keyboardLeft = input.keyboardSurface;
  input.keyboardSurface = nullptr;
  input.repeatKey = 0;
  if (!input.keyboardLeft) return;

  if (!input.leaveNotice) {
    sd_event_add_defer(wayland.loop, &input.leaveNotice, Left, nullptr);
  }
  sd_event_source_set_enabled(input.leaveNotice, SD_EVENT_ONESHOT);
}

// the active modifiers, as InputModifier bits.
static u32
Modifiers(void)
{
  static const struct {
    const char *name;
    u32 bit;
  } names[] = {
    {XKB_MOD_NAME_SHIFT, InputModifierShift},
    {XKB_MOD_NAME_CTRL, InputModifierControl},
    {XKB_MOD_NAME_ALT, InputModifierAlt},
    {XKB_MOD_NAME_LOGO, InputModifierSuper}
  };

  u32 modifiers = 0;
  for (u32 index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
    enum xkb_state_component effective = XKB_STATE_MODS_EFFECTIVE;
    if (xkb_state_mod_name_is_active(input.state, names[index].name, effective) > 0) {
      modifiers |= names[index].bit;
    }
  }
  return modifiers;
}

// sends the key with evdev code key to the focused surface.
static void
DeliverKey(u32 key)
{
  Surface *surface = input.keyboardSurface;
  if (!surface || !surface->handlers->Key || !input.state) return;

  xkb_keycode_t code = key + 8;
  u32 keysym = xkb_state_key_get_one_sym(input.state, code);
  u32 codepoint = xkb_state_key_get_utf32(input.state, code);
  // control characters (from ctrl+letter, enter, escape) are not text.
  if (codepoint < 0x20 || codepoint == 0x7F) codepoint = 0;
  surface->handlers->Key(surface->context, surface, keysym, codepoint, Modifiers());
}

static int
Repeat(sd_event_source *source, [[maybe_unused]] u64 now, [[maybe_unused]] void *context)
{
  if (!input.repeatKey) return 0;

  DeliverKey(input.repeatKey);
  sd_event_source_set_time_relative(source, 1'000'000 / (u64)input.repeatRate);
  sd_event_source_set_enabled(source, SD_EVENT_ONESHOT);
  return 0;
}

static void
KeyboardKey(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct wl_keyboard *keyboard,
  u32 serial,
  [[maybe_unused]] u32 time,
  u32 key,
  u32 state
)
{
  if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
    if (key == input.repeatKey) input.repeatKey = 0;
    return;
  }

  input.serial = serial;
  DeliverKey(key);
  if (!input.keymap || !xkb_keymap_key_repeats(input.keymap, key + 8) ||
    input.repeatRate <= 0) {
    return;
  }

  input.repeatKey = key;
  u64 delay = (u64)input.repeatDelay * 1000;
  if (!input.repeatTimer) {
    sd_event_add_time_relative(
      wayland.loop,
      &input.repeatTimer,
      CLOCK_MONOTONIC,
      delay,
      1000,
      Repeat,
      nullptr
    );
  } else {
    sd_event_source_set_time_relative(input.repeatTimer, delay);
  }
  sd_event_source_set_enabled(input.repeatTimer, SD_EVENT_ONESHOT);
}

static void
KeyboardModifiers(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct wl_keyboard *keyboard,
  [[maybe_unused]] u32 serial,
  u32 depressed,
  u32 latched,
  u32 locked,
  u32 group
)
{
  if (input.state) xkb_state_update_mask(input.state, depressed, latched, locked, 0, 0, group);
}

static void
KeyboardRepeatInfo(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct wl_keyboard *keyboard,
  i32 rate,
  i32 delay
)
{
  input.repeatRate = rate;
  input.repeatDelay = delay;
}

static const struct wl_keyboard_listener keyboardListener = {
  .keymap = KeyboardKeymap,
  .enter = KeyboardEnter,
  .leave = KeyboardLeave,
  .key = KeyboardKey,
  .modifiers = KeyboardModifiers,
  .repeat_info = KeyboardRepeatInfo
};

static void
OfferOffer(void *context, [[maybe_unused]] struct wl_data_offer *wlOffer, const char *type)
{
  Offer *offer = context;
  for (u32 index = 0; index < offer->textType; index++) {
    if (strcmp(type, textTypes[index]) == 0) offer->textType = index;
  }
}

static void
OfferSourceActions(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct wl_data_offer *wlOffer,
  [[maybe_unused]] u32 actions
)
{
}

static void
OfferAction(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct wl_data_offer *wlOffer,
  [[maybe_unused]] u32 action
)
{
}

static const struct wl_data_offer_listener offerListener =
  {.offer = OfferOffer, .source_actions = OfferSourceActions, .action = OfferAction};

static void
FreeOffer(Offer *offer)
{
  if (!offer) return;

  wl_data_offer_destroy(offer->offer);
  free(offer);
}

static void
DataOffer(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct wl_data_device *device,
  struct wl_data_offer *wlOffer
)
{
  Offer *offer = MemoryAllocateZeroed(1, sizeof(Offer));
  offer->offer = wlOffer;
  offer->textType = textTypeCount;
  wl_data_offer_add_listener(wlOffer, &offerListener, offer);
}

static void
DataEnter(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct wl_data_device *device,
  [[maybe_unused]] u32 serial,
  [[maybe_unused]] struct wl_surface *surface,
  [[maybe_unused]] wl_fixed_t x,
  [[maybe_unused]] wl_fixed_t y,
  struct wl_data_offer *wlOffer
)
{
  // drag and drop is not used, so the dragged offer is dropped at once.
  if (wlOffer) FreeOffer(wl_data_offer_get_user_data(wlOffer));
}

static void
DataLeave([[maybe_unused]] void *context, [[maybe_unused]] struct wl_data_device *device)
{
}

static void
DataMotion(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct wl_data_device *device,
  [[maybe_unused]] u32 time,
  [[maybe_unused]] wl_fixed_t x,
  [[maybe_unused]] wl_fixed_t y
)
{
}

static void
DataDrop([[maybe_unused]] void *context, [[maybe_unused]] struct wl_data_device *device)
{
}

static void
DataSelection(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct wl_data_device *device,
  struct wl_data_offer *wlOffer
)
{
  FreeOffer(input.selection);
  input.selection = wlOffer ? wl_data_offer_get_user_data(wlOffer) : nullptr;
}

static const struct wl_data_device_listener dataDeviceListener = {
  .data_offer = DataOffer,
  .enter = DataEnter,
  .leave = DataLeave,
  .motion = DataMotion,
  .drop = DataDrop,
  .selection = DataSelection
};

static int
PasteReadable(sd_event_source *source, int file, [[maybe_unused]] u32 events, void *context)
{
  Paste *paste = context;
  char chunk[4096];
  ssize_t count = read(file, chunk, sizeof(chunk));
  if (count > 0) {
    StrBufAppend(&paste->text, chunk, (u64)count);
    return 0;
  }

  // the end of the text (or an error, which ends it just the same).
  paste->done(paste->context, StrBufText(&paste->text));
  StrBufFree(&paste->text);
  free(paste);
  close(file);
  sd_event_source_disable_unref(source);
  return 0;
}

void
InputPaste(void (*done)(void *context, const char *text), void *context)
{
  Offer *offer = input.selection;
  int pipes[2];
  if (!offer || offer->textType == textTypeCount || pipe2(pipes, O_CLOEXEC) != 0) {
    done(context, nullptr);
    return;
  }

  wl_data_offer_receive(offer->offer, textTypes[offer->textType], pipes[1]);
  close(pipes[1]);
  wl_display_flush(wayland.display);

  Paste *paste = MemoryAllocateZeroed(1, sizeof(Paste));
  paste->done = done;
  paste->context = context;
  sd_event_add_io(wayland.loop, nullptr, pipes[0], EPOLLIN, PasteReadable, paste);
}

static void
SeatCapabilities([[maybe_unused]] void *context, struct wl_seat *seat, u32 capabilities)
{
  bool hasPointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
  bool hasKeyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;

  if (hasPointer && !input.pointer) {
    input.pointer = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(input.pointer, &pointerListener, nullptr);
    if (wayland.cursorShapeManager) {
      input.cursorShape =
        wp_cursor_shape_manager_v1_get_pointer(wayland.cursorShapeManager, input.pointer);
    }
  } else if (!hasPointer && input.pointer) {
    if (input.cursorShape) wp_cursor_shape_device_v1_destroy(input.cursorShape);
    wl_pointer_release(input.pointer);
    input.cursorShape = nullptr;
    input.pointer = nullptr;
    input.pointerSurface = nullptr;
  }

  if (hasKeyboard && !input.keyboard) {
    input.keyboard = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(input.keyboard, &keyboardListener, nullptr);
  } else if (!hasKeyboard && input.keyboard) {
    wl_keyboard_release(input.keyboard);
    input.keyboard = nullptr;
    input.keyboardSurface = nullptr;
    input.repeatKey = 0;
  }
}

static void
SeatName(
  [[maybe_unused]] void *context,
  [[maybe_unused]] struct wl_seat *seat,
  [[maybe_unused]] const char *name
)
{
}

static const struct wl_seat_listener seatListener = {
  .capabilities = SeatCapabilities,
  .name = SeatName
};

void
InputAttach(struct wl_seat *seat)
{
  input.seat = seat;
  input.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  wl_seat_add_listener(seat, &seatListener, nullptr);

  if (wayland.dataDeviceManager) {
    input.dataDevice = wl_data_device_manager_get_data_device(wayland.dataDeviceManager, seat);
    wl_data_device_add_listener(input.dataDevice, &dataDeviceListener, nullptr);
  }
}
