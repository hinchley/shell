#pragma once

// a tray item's menu, read over d-bus (com.canonical.dbusmenu) and shown as popups from the
// bar, with submenus opening beside it.
#include "draw.h"
#include "module.h"

// opens the menu at path of the d-bus service below anchor on output's bar, closing any menu
// already open.
void
MenuOpen(BarOutput *output, Box anchor, const char *service, const char *path);

// closes the open menu, if any.
void
MenuClose(void);

// closes the menu if it is open on output (whose bar is going away).
void
MenuCloseOn(BarOutput *output);
