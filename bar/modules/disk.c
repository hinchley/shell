// disk: how full the root filesystem is, rounded up as df rounds it.
// a click opens dua.

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <sys/statvfs.h>

#include "module.h"
#include "process.h"

static constexpr u64 interval = 30'000'000;

static i32 usage;

static void
Tick(void)
{
  struct statvfs status;
  if (statvfs("/", &status) != 0) return;

  // the share of the space usable without root's reserve, as df's Use% has it.
  u64 used = status.f_blocks - status.f_bfree;
  u64 usable = used + status.f_bavail;
  if (!usable) return;

  i32 percent = (i32)((used * 100 + usable - 1) / usable);
  if (percent == usage) return;
  usage = percent;
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
  snprintf(text, sizeof(text), "\U000F02CA %d%%", usage);
  segments[0] = SegmentLabel(text, themeBlue);
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
  static const char *const program[] = {"dua", "i", "/", nullptr};
  if (button == BTN_LEFT) ProcessTui("dua", program);
}

const BarModule diskModule = {
  .name = "disk",
  .Start = Start,
  .Segments = Segments,
  .Click = Click,
  .padding = 30,
  .spacing = 4,
  .clickable = true
};
