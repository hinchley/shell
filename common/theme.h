#pragma once

// the colour scheme and font shared by the bar, the launcher and hyprshorts.
// the values are those of the quickshell config this replaces (a tokyo night palette).
#include "types.h"

// a straight (not premultiplied) rgba colour, each channel 0 to 1.
typedef struct Colour {
  f64 red;
  f64 green;
  f64 blue;
  f64 alpha;
} Colour;

// the desktop behind the wallpaper, and dark text on a cyan fill.
static constexpr Colour themeBg = {0x1A / 255.0, 0x1B / 255.0, 0x26 / 255.0, 1};
// the fill of every pill, popup and overlay panel.
static constexpr Colour themePill = {0x24 / 255.0, 0x25 / 255.0, 0x3A / 255.0, 1};
static constexpr Colour themeFg = {0xA9 / 255.0, 0xB1 / 255.0, 0xD6 / 255.0, 1};
static constexpr Colour themeMuted = {0x44 / 255.0, 0x4B / 255.0, 0x6A / 255.0, 1};
static constexpr Colour themeCyan = {0x0D / 255.0, 0xB9 / 255.0, 0xD7 / 255.0, 1};
static constexpr Colour themePurple = {0xAD / 255.0, 0x8E / 255.0, 0xE6 / 255.0, 1};
static constexpr Colour themeRed = {0xF7 / 255.0, 0x76 / 255.0, 0x8E / 255.0, 1};
static constexpr Colour themeYellow = {0xE0 / 255.0, 0xAF / 255.0, 0x68 / 255.0, 1};
static constexpr Colour themeBlue = {0x7A / 255.0, 0xA2 / 255.0, 0xF7 / 255.0, 1};
static constexpr Colour themeGreen = {0x9E / 255.0, 0xCE / 255.0, 0x6A / 255.0, 1};

// translucent white, for borders and fills layered over themePill.
static constexpr Colour themeBorderFaint = {1, 1, 1, 0.04};
static constexpr Colour themeBorderMed = {1, 1, 1, 0.06};
static constexpr Colour themeBorderSoft = {1, 1, 1, 0.05};
static constexpr Colour themeBorderStrong = {1, 1, 1, 0.10};
static constexpr Colour themeFillFaint = {1, 1, 1, 0.025};
static constexpr Colour themeFillSubtle = {1, 1, 1, 0.03};
static constexpr Colour themeFillInput = {1, 1, 1, 0.035};
static constexpr Colour themeFillHover = {1, 1, 1, 0.08};

// nothing at all, for a border that is not drawn.
static constexpr Colour themeNone = {0, 0, 0, 0};

static constexpr char themeFontFamily[] = "IoskeleyMonoTerm Nerd Font Mono";
// the base text size in logical pixels; other sizes are given relative to it.
static constexpr f64 themeFontSize = 13;

// colour with its alpha multiplied by opacity.
static inline Colour
ThemeFade(Colour colour, f64 opacity)
{
  colour.alpha *= opacity;
  return colour;
}
