// clock: the date and time, to the minute.
// a click opens the calendar.

#include <linux/input-event-codes.h>
#include <time.h>

#include "calendar.h"
#include "module.h"

typedef struct Clock {
  char text[64];
  // fires at the start of each minute.
  sd_event_source *timer;
} Clock;

static Clock clockPill;

// microseconds on the wall clock.
static u64
Now(void)
{
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  return (u64)now.tv_sec * 1'000'000 + (u64)now.tv_nsec / 1000;
}

static void
Update(void)
{
  time_t now = time(nullptr);
  struct tm local;
  localtime_r(&now, &local);
  strftime(clockPill.text, sizeof(clockPill.text), "%a %b %d %H:%M", &local);
  BarRedraw();
  CalendarRefresh();
}

static int
Tick(sd_event_source *source, [[maybe_unused]] u64 now, [[maybe_unused]] void *context)
{
  Update();
  // the next minute's start, plus a moment so the time has surely turned.
  u64 minute = 60'000'000;
  sd_event_source_set_time(source, (Now() / minute + 1) * minute + 1000);
  sd_event_source_set_enabled(source, SD_EVENT_ONESHOT);
  return 0;
}

static void
Start(void)
{
  sd_event_add_time(wayland.loop, &clockPill.timer, CLOCK_REALTIME, 0, 0, Tick, nullptr);
  Tick(clockPill.timer, 0, nullptr);
}

static u32
Segments([[maybe_unused]] const BarOutput *output, Segment *segments)
{
  segments[0] = SegmentLabel(clockPill.text, themeCyan);
  segments[0].tabularDigits = true;
  return 1;
}

static void
Click(
  BarOutput *output,
  Box pill,
  [[maybe_unused]] Box segment,
  [[maybe_unused]] u32 action,
  u32 button
)
{
  if (button == BTN_LEFT) CalendarToggle(output, pill);
}

const BarModule clockModule = {
  .name = "clock",
  .Start = Start,
  .Segments = Segments,
  .Click = Click,
  .padding = 30,
  .spacing = 4,
  .clickable = true
};
