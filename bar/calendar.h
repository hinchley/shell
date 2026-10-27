#pragma once

// the month calendar that drops down from the clock.
#include "draw.h"
#include "module.h"

// opens the calendar below anchor (the clock's pill) on output's bar, or closes it when it is
// open.
void
CalendarToggle(BarOutput *output, Box anchor);

// closes the calendar if it is open on output (whose bar is going away).
void
CalendarCloseOn(BarOutput *output);

// redraws the open calendar, so today moves on at midnight.
void
CalendarRefresh(void);
