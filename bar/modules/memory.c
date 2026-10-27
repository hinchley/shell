// memory: how much memory is in use (all but what the kernel reports as available), from
// /proc/meminfo.
// a click opens btop.

#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "module.h"
#include "process.h"

static constexpr u64 interval = 2'000'000;

static i32 usage;

static void
Tick(void)
{
  FILE *file = fopen("/proc/meminfo", "re");
  if (!file) return;

  u64 total = 0;
  u64 available = 0;
  char line[256];
  while (fgets(line, sizeof(line), file)) {
    if (strncmp(line, "MemTotal:", 9) == 0) sscanf(line + 9, "%lu", &total);
    else if (strncmp(line, "MemAvailable:", 13) == 0) sscanf(line + 13, "%lu", &available);
  }
  fclose(file);
  if (!total) return;

  i32 used = (i32)round(100.0 * (f64)(total - available) / (f64)total);
  if (used == usage) return;
  usage = used;
  BarRedraw();
}

static void
Start(void)
{
  BarRepeat(interval, Tick);
}

static u32
Segments([[maybe_unused]] const BarOutput *output, Segment *segments)
{
  static char text[32];
  snprintf(text, sizeof(text), "\U000F035B %d%%", usage);
  segments[0] = SegmentLabel(text, themeCyan);
  return 1;
}

static void
Click(
  [[maybe_unused]] BarOutput *output,
  [[maybe_unused]] Box pill,
  [[maybe_unused]] Box segment,
  [[maybe_unused]] u32 action,
  u32 button
)
{
  static const char *const program[] = {"btop", nullptr};
  if (button == BTN_LEFT) ProcessTui("btop", program);
}

const BarModule memoryModule = {
  .name = "memory",
  .Start = Start,
  .Segments = Segments,
  .Click = Click,
  .padding = 30,
  .spacing = 4,
  .clickable = true
};
