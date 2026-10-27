#pragma once

// drawing helpers over cairo: rounded rectangles drawn as qt draws them, and text laid out
// with pango in the theme's font.
#include <cairo.h>
#include <pango/pangocairo.h>

#include "theme.h"
#include "types.h"

// a rectangle in logical pixels.
typedef struct Box {
  f64 x;
  f64 y;
  f64 width;
  f64 height;
} Box;

// true when x, y lies inside box.
static inline bool
BoxContains(Box box, f64 x, f64 y)
{
  return x >= box.x && x < box.x + box.width && y >= box.y && y < box.y + box.height;
}

// how a piece of text is set.
typedef struct TextStyle {
  // the size in logical pixels.
  f64 size;
  bool bold;
  // digits all the same width (opentype tnum), so a clock does not jitter.
  bool tabularDigits;
} TextStyle;

// laid out text, ready to measure and draw.
typedef struct Text {
  PangoLayout *layout;
  // the logical size, in logical pixels.
  f64 width;
  f64 height;
} Text;

// sets fill to colour.
void
DrawSetColour(cairo_t *cairo, Colour colour);

// a filled rectangle with rounded corners and a one pixel border just inside its edge, drawn
// as a qt quick Rectangle draws it.
// a border with no alpha is not drawn.
void
DrawRoundedRect(
  cairo_t *cairo,
  f64 x,
  f64 y,
  f64 width,
  f64 height,
  f64 radius,
  Colour fill,
  Colour border
);

// lays out text in style.
// a maxWidth above 0 limits the width: the text is cut with an ellipsis at the end, or, when
// maxLines is above 0, wrapped onto at most that many lines with the last one cut.
Text
TextLayout(const char *text, const TextStyle *style, f64 maxWidth, u32 maxLines);

// draws text with its top left at x, y.
void
TextDraw(cairo_t *cairo, const Text *text, f64 x, f64 y, Colour colour);

// the byte offset in the text nearest to x (logical pixels from its left edge).
u64
TextOffsetAt(const Text *text, f64 x);

// the x of the caret before byte offset, from the text's left edge.
f64
TextCaretAt(const Text *text, u64 offset);

void
TextFree(Text *text);
