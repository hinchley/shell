#pragma once

// the interface between the bar and its modules, one module to a pill.
//
// a module watches some source (a file, a d-bus service, hyprland) and, when it changes, calls
// BarRedraw.
// the bar then asks every module for its pill's segments (runs of text, or an icon) on each
// output, lays the pills out, draws them and routes clicks and scrolling back to the module
// that drew what was hit.
//
// which modules are built, and in what order, is chosen by bar/build.sh, which generates the
// barLeft, barCenter and barRight tables below.
#include <cairo.h>
#include <systemd/sd-bus.h>

#include "draw.h"
#include "theme.h"
#include "types.h"
#include "wayland.h"

// a bar on one output.
typedef struct BarOutput {
  Output *output;
  // the bar's surface, or nullptr while the bar is hidden.
  Surface *surface;
  Surface *wallpaper;
  // a narrow output (under 1920 logical pixels): some pills show less.
  bool compact;

  // what the last draw put where, for hit testing (see main.c).
  struct Hit *hits;
  u32 hitCount;
  u32 hitCapacity;
  f64 pointerX;
  f64 pointerY;
} BarOutput;

// one run of text or one icon in a pill.
typedef struct Segment {
  // the text, which only needs to stay valid until the next call to the module's Segments.
  const char *text;
  // an icon drawn instead of text, iconSize pixels square.
  cairo_surface_t *icon;
  f64 iconSize;
  Colour colour;
  // added to the theme's font size.
  f64 sizeDelta;
  bool bold;
  bool tabularDigits;
  // a fixed width and height with the text centred in it (a workspace's cell), or 0 for the
  // text's own size.
  f64 width;
  f64 height;
  // room left empty before the text.
  f64 leftPadding;
  // draws a short bar along the bottom of the cell (the active workspace).
  bool underline;
  // lets the text be cut with an ellipsis when the pill would be wider than its maxWidth.
  bool elide;
  // left out when its section would otherwise run into the next one (the clock).
  bool optional;
  // what a click on the segment does, given to the module's Click; 0 makes it inert.
  u32 action;
  // shows the pointing hand cursor over the segment.
  bool pointer;
} Segment;

// a bold label in colour, the content of most pills.
static inline Segment
SegmentLabel(const char *text, Colour colour)
{
  return (Segment){.text = text, .colour = colour, .bold = true};
}

// the most segments one pill can have.
static constexpr u32 segmentCapacity = 32;

typedef struct BarModule {
  const char *name;
  // starts watching the module's sources; called once, before the first draw.
  void (*Start)(void);
  // fills segments (room for segmentCapacity) with the pill's content on output, and returns
  // how many there are; 0 hides the pill.
  u32 (*Segments)(const BarOutput *output, Segment *segments);
  // a click on the segment with action (or, for a clickable pill, anywhere on it, with action
  // 0).
  // button is a linux input code (BTN_LEFT, BTN_RIGHT).
  // pill is the pill's box on output's surface, and segment the box of the segment clicked.
  void (*Click)(BarOutput *output, Box pill, Box segment, u32 action, u32 button);
  // scrolling over a segment with an action, in 120ths of a notch, positive downwards.
  void (*Scroll)(BarOutput *output, u32 action, i32 value120);
  // output's bar is going away: anything opened from it (a popup) must close first.
  void (*Hide)(BarOutput *output);

  // the room either side of the content, together, in logical pixels.
  f64 padding;
  // the gap between segments.
  f64 spacing;
  // the widest the pill may be, or 0 for no limit.
  f64 maxWidth;
  // the whole pill takes clicks (given to Click with action 0), with the pointing hand.
  bool clickable;
} BarModule;

// the pills of each section, in order from left to right, ended by nullptr.
extern const BarModule *const barLeft[];
extern const BarModule *const barCenter[];
extern const BarModule *const barRight[];
// whether the bar also draws the wallpaper.
extern const bool barWallpaper;

// redraws the bar on every output, at the next frame.
void
BarRedraw(void);

// the user and system d-bus connections, attached to the event loop and opened on first use.
// nullptr when the bus cannot be reached.
sd_bus *
BarUserBus(void);

sd_bus *
BarSystemBus(void);

// calls tick now, then every interval microseconds.
void
BarRepeat(u64 interval, void (*tick)(void));

// calls callback once, shortly: calls made before then are folded into the one.
// for refreshing after a burst of signals.
void
BarSoon(void (*callback)(void));

// the box of module's pill on output as last drawn.
// false when it is not shown there.
bool
BarPillBox(const BarOutput *output, const BarModule *module, Box *box);

// the output hyprland has focused, or the first, or nullptr when there are none.
BarOutput *
BarFocusedOutput(void);
