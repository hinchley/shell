#include "draw.h"

#include <math.h>

// the one pango context every layout is made in.
// its metrics are not hinted, so text measures the same at every output scale.
static PangoContext *context;

static PangoContext *
Context(void)
{
  if (context) return context;

  context = pango_font_map_create_context(pango_cairo_font_map_get_default());
  cairo_font_options_t *options = cairo_font_options_create();
  cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_OFF);
  // glyphs are drawn as qt draws them here: outlines unhinted, smoothed per rgb subpixel.
  cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_NONE);
  cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_SUBPIXEL);
  cairo_font_options_set_subpixel_order(options, CAIRO_SUBPIXEL_ORDER_RGB);
  pango_cairo_context_set_font_options(context, options);
  cairo_font_options_destroy(options);
  pango_context_set_round_glyph_positions(context, false);
  return context;
}

void
DrawSetColour(cairo_t *cairo, Colour colour)
{
  cairo_set_source_rgba(cairo, colour.red, colour.green, colour.blue, colour.alpha);
}

// adds a rounded rectangle path.
static void
RoundedPath(cairo_t *cairo, f64 x, f64 y, f64 width, f64 height, f64 radius)
{
  radius = fmin(radius, fmin(width, height) / 2);
  cairo_new_sub_path(cairo);
  cairo_arc(cairo, x + width - radius, y + radius, radius, -M_PI / 2, 0);
  cairo_arc(cairo, x + width - radius, y + height - radius, radius, 0, M_PI / 2);
  cairo_arc(cairo, x + radius, y + height - radius, radius, M_PI / 2, M_PI);
  cairo_arc(cairo, x + radius, y + radius, radius, M_PI, 3 * M_PI / 2);
  cairo_close_path(cairo);
}

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
)
{
  if (border.alpha == 0) {
    RoundedPath(cairo, x, y, width, height, radius);
    DrawSetColour(cairo, fill);
    cairo_fill(cairo);
    return;
  }

  // as in qt, the fill stops inside the border, and the border is a ring over whatever is
  // behind the rectangle (so a translucent border tints the background, not the fill).
  RoundedPath(cairo, x + 1, y + 1, width - 2, height - 2, fmax(radius - 1, 0));
  DrawSetColour(cairo, fill);
  cairo_fill(cairo);

  cairo_save(cairo);
  cairo_set_fill_rule(cairo, CAIRO_FILL_RULE_EVEN_ODD);
  RoundedPath(cairo, x, y, width, height, radius);
  RoundedPath(cairo, x + 1, y + 1, width - 2, height - 2, fmax(radius - 1, 0));
  DrawSetColour(cairo, border);
  cairo_fill(cairo);
  cairo_restore(cairo);
}

Text
TextLayout(const char *text, const TextStyle *style, f64 maxWidth, u32 maxLines)
{
  PangoLayout *layout = pango_layout_new(Context());

  PangoFontDescription *font = pango_font_description_new();
  pango_font_description_set_family_static(font, themeFontFamily);
  pango_font_description_set_absolute_size(font, style->size * PANGO_SCALE);
  pango_font_description_set_weight(
    font,
    style->bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL
  );
  pango_layout_set_font_description(layout, font);
  pango_font_description_free(font);

  if (style->tabularDigits) {
    PangoAttrList *attributes = pango_attr_list_new();
    pango_attr_list_insert(attributes, pango_attr_font_features_new("tnum=1"));
    pango_layout_set_attributes(layout, attributes);
    pango_attr_list_unref(attributes);
  }

  if (maxWidth > 0) {
    pango_layout_set_width(layout, (int)(maxWidth * PANGO_SCALE));
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    if (maxLines) {
      pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
      // a negative height is a limit in lines.
      pango_layout_set_height(layout, -(int)maxLines);
    }
  }
  pango_layout_set_text(layout, text, -1);

  PangoRectangle logical;
  pango_layout_get_extents(layout, nullptr, &logical);
  return (Text){
    .layout = layout,
    .width = (f64)logical.width / PANGO_SCALE,
    .height = (f64)logical.height / PANGO_SCALE
  };
}

void
TextDraw(cairo_t *cairo, const Text *text, f64 x, f64 y, Colour colour)
{
  DrawSetColour(cairo, colour);
  cairo_move_to(cairo, x, y);
  pango_cairo_show_layout(cairo, text->layout);
}

u64
TextOffsetAt(const Text *text, f64 x)
{
  int index;
  int trailing;
  pango_layout_xy_to_index(text->layout, (int)(x * PANGO_SCALE), 0, &index, &trailing);

  // trailing counts characters past index, so step over that many utf-8 sequences.
  const char *bytes = pango_layout_get_text(text->layout);
  u64 offset = (u64)index;
  for (; trailing > 0 && bytes[offset]; trailing--) {
    offset++;
    while ((bytes[offset] & 0xC0) == 0x80) offset++;
  }
  return offset;
}

f64
TextCaretAt(const Text *text, u64 offset)
{
  PangoRectangle strong;
  pango_layout_get_cursor_pos(text->layout, (int)offset, &strong, nullptr);
  return (f64)strong.x / PANGO_SCALE;
}

void
TextFree(Text *text)
{
  if (text->layout) g_object_unref(text->layout);
  *text = (Text){};
}
