// connectivity: the bluetooth and network pills sharing one pill, to save room on a narrow
// bar.
// each half is drawn by its own module and still takes its own clicks: bluetooth opens
// bluetui, and network opens impala.
//
// build.sh compiles bluetooth.c and network.c along with this one.

#include "module.h"

extern const BarModule bluetoothModule;
extern const BarModule networkModule;

// the modules sharing the pill, left to right; a segment's action is its module's index + 1.
static const BarModule *const parts[] = {&bluetoothModule, &networkModule};
static constexpr u32 partCount = sizeof(parts) / sizeof(parts[0]);

// the height of each part's cell: the pill's own (main.c's pillHeight), so a click anywhere
// above or below an icon still reaches it.
static constexpr f64 cellHeight = 28;

static void
Start(void)
{
  for (u32 index = 0; index < partCount; index++) parts[index]->Start();
}

static u32
Segments(const BarOutput *output, Segment *segments)
{
  u32 count = 0;
  for (u32 index = 0; index < partCount; index++) {
    Segment *part = segments + count;
    u32 added = parts[index]->Segments(output, part);
    for (u32 segment = 0; segment < added; segment++) {
      part[segment].action = index + 1;
      part[segment].pointer = true;
      part[segment].height = cellHeight;
    }
    count += added;
  }
  return count;
}

static void
Click(BarOutput *output, Box pill, Box segment, u32 action, u32 button)
{
  if (action < 1 || action > partCount) return;
  const BarModule *part = parts[action - 1];
  // each part was a clickable pill on its own, so it is given action 0, as the bar would.
  if (part->Click) part->Click(output, pill, segment, 0, button);
}

static void
Hide(BarOutput *output)
{
  for (u32 index = 0; index < partCount; index++) {
    if (parts[index]->Hide) parts[index]->Hide(output);
  }
}

const BarModule connectivityModule = {
  .name = "connectivity",
  .Start = Start,
  .Segments = Segments,
  .Click = Click,
  .Hide = Hide,
  .padding = 30,
  .spacing = 12
};
