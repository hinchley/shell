// workspaces: buttons for workspaces 1 to 5.
// the focused one is underlined, and those that exist are lit.
// a click switches to it.

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hyprland.h"
#include "json.h"
#include "module.h"

static constexpr i64 workspaceCount = 5;

// the hyprland events after which the workspaces are read again.
static const char *const events[] = {
  "connected",
  "workspace",
  "workspacev2",
  "createworkspace",
  "createworkspacev2",
  "destroyworkspace",
  "destroyworkspacev2",
  "moveworkspace",
  "moveworkspacev2",
  "focusedmon",
  "focusedmonv2",
  "monitoradded",
  "monitorremoved"
};

typedef struct Workspaces {
  // whether workspace n (from 1) exists.
  bool exists[workspaceCount + 1];
  i64 active;
} Workspaces;

static Workspaces workspaces;

static void
Refresh(void)
{
  Workspaces read = {};
  char *reply = HyprlandRequest("j/workspaces");
  JsonArray list = JsonArrayBegin(reply);
  for (const char *workspace; (workspace = JsonArrayNext(&list));) {
    i64 id = JsonInteger(JsonMember(workspace, "id"), 0);
    if (id >= 1 && id <= workspaceCount) read.exists[id] = true;
  }
  free(reply);

  reply = HyprlandRequest("j/activeworkspace");
  read.active = JsonInteger(JsonMember(reply, "id"), 0);
  free(reply);

  if (memcmp(&read, &workspaces, sizeof(read)) == 0) return;
  workspaces = read;
  BarRedraw();
}

static void
Event(const char *name, [[maybe_unused]] const char *data)
{
  for (u32 index = 0; index < sizeof(events) / sizeof(events[0]); index++) {
    if (strcmp(name, events[index]) != 0) continue;
    BarSoon(Refresh);
    return;
  }
}

static void
Start(void)
{
  HyprlandListen(Event);
  Refresh();
}

static u32
Segments([[maybe_unused]] const BarOutput *output, Segment *segments)
{
  static const char *const numbers[] = {"", "1", "2", "3", "4", "5"};
  for (i64 id = 1; id <= workspaceCount; id++) {
    bool active = id == workspaces.active;
    segments[id - 1] = (Segment){
      .text = numbers[id],
      .colour = active || workspaces.exists[id] ? themeCyan : themeMuted,
      .bold = true,
      .width = 20,
      .height = 20,
      .underline = active,
      .action = (u32)id
    };
  }
  return workspaceCount;
}

static void
Click(
  [[maybe_unused]] BarOutput *output,
  [[maybe_unused]] Box pill,
  [[maybe_unused]] Box segment,
  u32 action,
  u32 button
)
{
  if (button != BTN_LEFT) return;

  char argument[64];
  snprintf(argument, sizeof(argument), "hl.dsp.focus({ workspace = %u })", action);
  HyprlandDispatch(argument);
}

const BarModule workspacesModule = {
  .name = "workspaces",
  .Start = Start,
  .Segments = Segments,
  .Click = Click,
  .padding = 20,
  .spacing = 4
};
