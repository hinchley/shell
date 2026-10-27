#pragma once

// a full screen overlay with a search field over a filtered list, as the launcher and
// hyprshorts show.
// typing filters the list, the arrow keys (or ctrl+p and ctrl+n) and page keys move the
// selection, enter or a double click activates it, and escape, ctrl+c or a click outside the
// panel dismisses the overlay.
#include "types.h"

// one entry of the list.
typedef struct PickerRow {
  const char *primary;
  // the second column, or nullptr in a one column picker.
  const char *secondary;
  // draws the second column in the muted colour.
  bool muted;
} PickerRow;

typedef struct PickerOptions {
  // shown in the empty search field.
  const char *placeholder;
  // the panel's size in logical pixels.
  f64 panelWidth;
  f64 panelHeight;
  // the width of the first column in a two column picker, or 0 for one column.
  f64 primaryWidth;

  // fills out (which has room for every row) with the indexes of the rows that match query, in
  // the order to list them, and returns how many there are.
  u64 (*Filter)(void *context, const char *query, u64 *out);
  // the row at index was chosen.
  // the picker has closed by then.
  // nullptr makes the list one to look through only: choosing a row does nothing.
  void (*Activated)(void *context, u64 index);
  void *context;
} PickerOptions;

// shows the picker over rows, which must outlive it.
// WaylandRun then runs until it is dismissed or a row is activated.
void
PickerOpen(const PickerOptions *options, const PickerRow *rows, u64 rowCount);
