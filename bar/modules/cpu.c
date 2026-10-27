// cpu: how busy the processors were over the last two seconds, from /proc/stat.
// a click opens btop.

#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>

#include "module.h"
#include "process.h"

static constexpr u64 interval = 2'000'000;

typedef struct Cpu {
  // the previous reading's idle and total time, in clock ticks.
  u64 idle;
  u64 total;
  i32 usage;
} Cpu;

static Cpu cpu;

static void
Tick(void)
{
  FILE *file = fopen("/proc/stat", "re");
  if (!file) return;

  // cpu  user nice system idle iowait irq softirq steal ...
  u64 times[8] = {};
  int read = fscanf(
    file,
    "cpu %lu %lu %lu %lu %lu %lu %lu %lu",
    &times[0],
    &times[1],
    &times[2],
    &times[3],
    &times[4],
    &times[5],
    &times[6],
    &times[7]
  );
  fclose(file);
  if (read < 4) return;

  u64 idle = times[3] + times[4];
  u64 total = 0;
  for (u32 index = 0; index < 8; index++) total += times[index];

  if (cpu.total && total > cpu.total) {
    f64 elapsed = (f64)(total - cpu.total);
    f64 waited = (f64)(idle - cpu.idle);
    i32 usage = (i32)round(100 * (elapsed - waited) / elapsed);
    if (usage != cpu.usage) {
      cpu.usage = usage;
      BarRedraw();
    }
  }
  cpu.idle = idle;
  cpu.total = total;
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
  snprintf(text, sizeof(text), "\U000F0EE0 %d%%", cpu.usage);
  segments[0] = SegmentLabel(text, themeYellow);
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

const BarModule cpuModule = {
  .name = "cpu",
  .Start = Start,
  .Segments = Segments,
  .Click = Click,
  .padding = 30,
  .spacing = 4,
  .clickable = true
};
