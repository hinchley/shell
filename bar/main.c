// bar: a status bar along the top of each output, with a row of pills on the left (by default
// the workspaces, the playing track and the focused window), the clock in the centre and
// system status on the right.
// it also draws the wallpaper behind everything.
//
// each pill is a module (see module.h and modules/), built in or left out by build.sh.
// the bar lays out and draws the pills, and sends clicks and scrolling to the modules.
//
// usage: bar [toggle | calendar]
//   (none)    runs the bar.
//   toggle    hides or shows the running bar.
//   calendar  opens or closes the running bar's calendar.

#include <linux/input-event-codes.h>
#include <locale.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "calendar.h"
#include "draw.h"
#include "hyprland.h"
#include "instance.h"
#include "json.h"
#include "memory.h"
#include "module.h"
#include "strbuf.h"
#include "wallpaper.h"
#include "wayland.h"

// the layout, in logical pixels, as the quickshell bar had it.
static constexpr f64 barHeight = 42;
static constexpr f64 pillHeight = 28;
static constexpr f64 pillRadius = 12;
static constexpr f64 leftMargin = 10;
static constexpr f64 rightMargin = 6;
static constexpr f64 pillGap = 5;
// outputs narrower than this get the compact bar.
static constexpr u32 compactWidth = 1920;

// something drawn that takes clicks: a clickable pill, or a segment with an action.
typedef struct Hit {
  const BarModule *module;
  Box pill;
  Box box;
  u32 action;
  bool pointer;
} Hit;

// a pill measured and ready to draw.
typedef struct Pill {
  const BarModule *module;
  Segment segments[segmentCapacity];
  Text texts[segmentCapacity];
  // each segment's width.
  f64 widths[segmentCapacity];
  u32 count;
  f64 width;
} Pill;

typedef struct Bar {
  BarOutput **outputs;
  u64 outputCount;
  u64 outputCapacity;
  bool hidden;
  sd_bus *userBus;
  sd_bus *systemBus;
  // the hit a button went down on, matched against the hit under the pointer at release.
  Hit pressed;
  u32 pressedButton;
  bool pressing;
} Bar;

static Bar bar;

// the text style of a segment.
static TextStyle
SegmentStyle(const Segment *segment)
{
  return (TextStyle){
    .size = themeFontSize + segment->sizeDelta,
    .bold = segment->bold,
    .tabularDigits = segment->tabularDigits
  };
}

// asks module for its pill on output and measures it, leaving out its optional segments when
// brief.
// false when the pill is hidden.
static bool
MeasurePill(Pill *pill, const BarModule *module, const BarOutput *output, bool brief)
{
  pill->module = module;
  pill->count = module->Segments(output, pill->segments);
  if (brief) {
    u32 kept = 0;
    for (u32 index = 0; index < pill->count; index++) {
      if (!pill->segments[index].optional) pill->segments[kept++] = pill->segments[index];
    }
    pill->count = kept;
  }
  if (!pill->count) return false;

  f64 content = 0;
  i32 elided = -1;
  for (u32 index = 0; index < pill->count; index++) {
    const Segment *segment = &pill->segments[index];
    pill->texts[index] = (Text){};
    f64 width = segment->width;
    if (segment->icon) {
      width = segment->iconSize;
    } else {
      TextStyle style = SegmentStyle(segment);
      pill->texts[index] = TextLayout(segment->text ? segment->text : "", &style, 0, 0);
      if (width == 0) width = segment->leftPadding + ceil(pill->texts[index].width);
    }
    if (segment->elide) elided = (i32)index;
    pill->widths[index] = width;
    content += width;
  }
  content += module->spacing * (pill->count - 1);
  pill->width = content + module->padding;

  // too wide: the elidable segment gives up the excess, ending in an ellipsis.
  if (module->maxWidth > 0 && pill->width > module->maxWidth && elided >= 0) {
    u32 index = (u32)elided;
    const Segment *segment = &pill->segments[index];
    f64 excess = pill->width - module->maxWidth;
    f64 room = fmax(0, pill->widths[index] - excess - segment->leftPadding);
    TextStyle style = SegmentStyle(segment);
    TextFree(&pill->texts[index]);
    pill->texts[index] = TextLayout(segment->text ? segment->text : "", &style, room, 0);
    pill->widths[index] -= excess;
    pill->width = module->maxWidth;
  }
  return true;
}

static void
FreePill(Pill *pill)
{
  for (u32 index = 0; index < pill->count; index++) TextFree(&pill->texts[index]);
}

static void
AddHit(BarOutput *output, Hit hit)
{
  if (output->hitCount == output->hitCapacity) {
    output->hitCapacity = output->hitCapacity ? output->hitCapacity * 2 : 32;
    output->hits = MemoryResize(output->hits, output->hitCapacity, sizeof(Hit));
  }
  output->hits[output->hitCount++] = hit;
}

// draws a measured pill with its left edge at x, and records what in it takes clicks.
static void
DrawPill(cairo_t *cairo, BarOutput *output, const Pill *pill, f64 x)
{
  const BarModule *module = pill->module;
  f64 top = (barHeight - pillHeight) / 2;
  Box box = {.x = x, .y = top, .width = pill->width, .height = pillHeight};
  DrawRoundedRect(
    cairo,
    x,
    top,
    pill->width,
    pillHeight,
    pillRadius,
    themePill,
    themeBorderFaint
  );

  f64 cursor = x + module->padding / 2;
  for (u32 index = 0; index < pill->count; index++) {
    const Segment *segment = &pill->segments[index];
    const Text *text = &pill->texts[index];
    f64 width = pill->widths[index];
    Box area;

    if (segment->icon) {
      f64 size = segment->iconSize;
      area = (Box){cursor, round((barHeight - size) / 2), size, size};
      cairo_save(cairo);
      cairo_translate(cairo, area.x, area.y);
      f64 scale = size / cairo_image_surface_get_width(segment->icon);
      cairo_scale(cairo, scale, scale);
      cairo_set_source_surface(cairo, segment->icon, 0, 0);
      cairo_pattern_set_filter(cairo_get_source(cairo), CAIRO_FILTER_GOOD);
      cairo_paint(cairo);
      cairo_restore(cairo);
    } else if (segment->height > 0) {
      // a cell with the text centred in it (on whole pixels, rounding down as qt does).
      area = (Box){cursor, floor((barHeight - segment->height) / 2), width, segment->height};
      f64 textX = area.x + floor((area.width - ceil(text->width)) / 2);
      f64 textY = area.y + floor((area.height - ceil(text->height)) / 2);
      TextDraw(cairo, text, textX, textY, segment->colour);
      if (segment->underline) {
        DrawRoundedRect(
          cairo,
          area.x + (area.width - 12) / 2,
          area.y + area.height - 2,
          12,
          2,
          1,
          segment->colour,
          themeNone
        );
      }
    } else {
      f64 textY = round((barHeight - ceil(text->height)) / 2);
      area = (Box){cursor, textY, width, ceil(text->height)};
      TextDraw(cairo, text, cursor + segment->leftPadding, textY, segment->colour);
    }

    if (segment->action) {
      AddHit(
        output,
        (Hit){
          .module = module,
          .pill = box,
          .box = area,
          .action = segment->action,
          .pointer = segment->pointer
        }
      );
    }
    cursor += width + module->spacing;
  }

  if (module->clickable) {
    AddHit(output, (Hit){.module = module, .pill = box, .box = box, .pointer = true});
  }
}

// a section of the bar, measured.
typedef struct Section {
  const BarModule *const *modules;
  // room for every module, of which count are shown.
  Pill *pills;
  u32 count;
  // the pills' total width, gaps included.
  f64 width;
} Section;

// the number of modules in a section.
static u32
SectionSize(const BarModule *const modules[])
{
  u32 count = 0;
  while (modules[count]) count++;
  return count;
}

// measures each module of section into its pills (see MeasurePill for brief).
static void
MeasureSection(Section *section, const BarOutput *output, bool brief)
{
  section->count = 0;
  section->width = 0;
  for (u32 index = 0; section->modules[index]; index++) {
    Pill *pill = &section->pills[section->count];
    if (!MeasurePill(pill, section->modules[index], output, brief)) continue;
    section->width += (section->count ? pillGap : 0) + pill->width;
    section->count++;
  }
}

static void
FreeSection(Section *section)
{
  for (u32 index = 0; index < section->count; index++) FreePill(&section->pills[index]);
  section->count = 0;
}

// the left edge of section (0 left, 1 center, 2 right) on a bar width wide.
static f64
SectionX(const Section *sections, u32 section, f64 width)
{
  return section == 0 ? leftMargin
    : section == 1    ? floor((width - sections[1].width) / 2)
                      : width - rightMargin - sections[2].width;
}

// whether the side section (0 or 2) runs into its neighbour: the center, or the other side
// when the center is empty.
static bool
SectionCrowded(const Section *sections, u32 section, f64 width)
{
  if (!sections[section].count) return false;
  u32 other = sections[1].count ? 1 : 2 - section;
  if (!sections[other].count) return false;
  u32 left = section < other ? section : other;
  u32 right = section < other ? other : section;
  return SectionX(sections, left, width) + sections[left].width + pillGap >
    SectionX(sections, right, width);
}

static void
DrawBar(void *context, Surface *surface, cairo_t *cairo)
{
  BarOutput *output = context;
  output->hitCount = 0;
  f64 width = surface->width;

  Section sections[3] = {{.modules = barLeft}, {.modules = barCenter}, {.modules = barRight}};
  for (u32 section = 0; section < 3; section++) {
    u32 size = SectionSize(sections[section].modules);
    sections[section].pills = MemoryResize(nullptr, size ? size : 1, sizeof(Pill));
    MeasureSection(&sections[section], output, false);
  }

  // a side that would run into its neighbour (the clock) leaves out its optional segments.
  for (u32 section = 0; section < 3; section += 2) {
    if (!SectionCrowded(sections, section, width)) continue;
    FreeSection(&sections[section]);
    MeasureSection(&sections[section], output, true);
  }

  for (u32 section = 0; section < 3; section++) {
    f64 x = SectionX(sections, section, width);
    for (u32 index = 0; index < sections[section].count; index++) {
      DrawPill(cairo, output, &sections[section].pills[index], x);
      x += sections[section].pills[index].width + pillGap;
    }
    FreeSection(&sections[section]);
    free(sections[section].pills);
  }
}

// the hit under x, y on output, or nullptr.
// segments are recorded before their pill, so the first hit found is the innermost.
static const Hit *
HitAt(const BarOutput *output, f64 x, f64 y)
{
  for (u32 index = 0; index < output->hitCount; index++) {
    if (BoxContains(output->hits[index].box, x, y)) return &output->hits[index];
  }
  return nullptr;
}

static void
Resized(void *context, Surface *surface)
{
  BarOutput *output = context;
  output->compact = surface->width < compactWidth;
}

static void
PointerMotion(void *context, Surface *surface, f64 x, f64 y)
{
  BarOutput *output = context;
  output->pointerX = x;
  output->pointerY = y;
  const Hit *hit = HitAt(output, x, y);
  bool pointer = hit && hit->pointer;
  SurfaceSetCursor(
    surface,
    pointer ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT
  );
}

static void
PointerLeave([[maybe_unused]] void *context, [[maybe_unused]] Surface *surface)
{
  bar.pressing = false;
}

// a click is a press and release on the same thing, as a qt MouseArea has it.
static void
PointerButton(
  void *context,
  [[maybe_unused]] Surface *surface,
  u32 button,
  bool pressed,
  [[maybe_unused]] u32 time
)
{
  BarOutput *output = context;
  const Hit *hit = HitAt(output, output->pointerX, output->pointerY);
  if (pressed) {
    bar.pressing = hit != nullptr;
    if (hit) bar.pressed = *hit;
    bar.pressedButton = button;
    return;
  }

  bool clicked = bar.pressing && hit && button == bar.pressedButton &&
    hit->module == bar.pressed.module && hit->action == bar.pressed.action;
  bar.pressing = false;
  if (clicked && hit->module->Click) {
    Hit target = *hit;
    target.module->Click(output, target.pill, target.box, target.action, button);
  }
}

static void
PointerScroll(void *context, [[maybe_unused]] Surface *surface, i32 value120)
{
  BarOutput *output = context;
  const Hit *hit = HitAt(output, output->pointerX, output->pointerY);
  if (hit && hit->action && hit->module->Scroll) {
    hit->module->Scroll(output, hit->action, value120);
  }
}

static void
SurfaceClosed(void *context, [[maybe_unused]] Surface *surface)
{
  BarOutput *output = context;
  SurfaceDestroy(output->surface);
  output->surface = nullptr;
}

static const SurfaceHandlers barHandlers = {
  .Draw = DrawBar,
  .Resized = Resized,
  .Closed = SurfaceClosed,
  .PointerMotion = PointerMotion,
  .PointerLeave = PointerLeave,
  .PointerButton = PointerButton,
  .PointerScroll = PointerScroll
};

static void
ShowBar(BarOutput *output)
{
  if (output->surface) return;

  LayerOptions options = {
    .output = output->output,
    .layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP,
    .anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
      ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
    .height = (u32)barHeight,
    .exclusiveZone = (i32)barHeight,
    .keyboard = ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE,
    .nameSpace = "bar"
  };
  output->surface = SurfaceCreateLayer(&options, &barHandlers, output);
}

static void
HideBar(BarOutput *output)
{
  CalendarCloseOn(output);
  const BarModule *const *sections[] = {barLeft, barCenter, barRight};
  for (u32 section = 0; section < 3; section++) {
    for (u32 index = 0; sections[section][index]; index++) {
      const BarModule *module = sections[section][index];
      if (module->Hide) module->Hide(output);
    }
  }
  SurfaceDestroy(output->surface);
  output->surface = nullptr;
  output->hitCount = 0;
}

static void
OutputAdded([[maybe_unused]] void *context, Output *wlOutput)
{
  BarOutput *output = MemoryAllocateZeroed(1, sizeof(BarOutput));
  output->output = wlOutput;
  wlOutput->data = output;

  if (bar.outputCount == bar.outputCapacity) {
    bar.outputCapacity = bar.outputCapacity ? bar.outputCapacity * 2 : 4;
    bar.outputs = MemoryResize(bar.outputs, bar.outputCapacity, sizeof(BarOutput *));
  }
  bar.outputs[bar.outputCount++] = output;

  // the bar first, so it is drawn before the (slower) wallpaper.
  if (!bar.hidden) ShowBar(output);
  if (barWallpaper) output->wallpaper = WallpaperCreate(wlOutput);
}

static void
OutputRemoved([[maybe_unused]] void *context, Output *wlOutput)
{
  BarOutput *output = wlOutput->data;
  HideBar(output);
  WallpaperDestroy(output->wallpaper);
  free(output->hits);

  for (u64 index = 0; index < bar.outputCount; index++) {
    if (bar.outputs[index] != output) continue;
    bar.outputs[index] = bar.outputs[--bar.outputCount];
    break;
  }
  free(output);
}

void
BarRedraw(void)
{
  for (u64 index = 0; index < bar.outputCount; index++) {
    if (bar.outputs[index]->surface) SurfaceRedraw(bar.outputs[index]->surface);
  }
}

sd_bus *
BarUserBus(void)
{
  if (bar.userBus) return bar.userBus;
  if (sd_bus_open_user(&bar.userBus) < 0) {
    bar.userBus = nullptr;
    return nullptr;
  }
  sd_bus_attach_event(bar.userBus, wayland.loop, SD_EVENT_PRIORITY_NORMAL);
  return bar.userBus;
}

sd_bus *
BarSystemBus(void)
{
  if (bar.systemBus) return bar.systemBus;
  if (sd_bus_open_system(&bar.systemBus) < 0) {
    bar.systemBus = nullptr;
    return nullptr;
  }
  sd_bus_attach_event(bar.systemBus, wayland.loop, SD_EVENT_PRIORITY_NORMAL);
  return bar.systemBus;
}

// a repeating timer's interval and what it calls.
typedef struct Repeat {
  u64 interval;
  void (*tick)(void);
} Repeat;

static int
RepeatTick(sd_event_source *source, [[maybe_unused]] u64 now, void *context)
{
  Repeat *repeat = context;
  sd_event_source_set_time_relative(source, repeat->interval);
  repeat->tick();
  return 0;
}

void
BarRepeat(u64 interval, void (*tick)(void))
{
  Repeat *repeat = MemoryAllocateZeroed(1, sizeof(Repeat));
  repeat->interval = interval;
  repeat->tick = tick;

  sd_event_source *source;
  sd_event_add_time_relative(
    wayland.loop,
    &source,
    CLOCK_MONOTONIC,
    interval,
    0,
    RepeatTick,
    repeat
  );
  sd_event_source_set_enabled(source, SD_EVENT_ON);
  tick();
}

// a callback waiting to be called by BarSoon.
typedef struct Soon {
  void (*callback)(void);
  sd_event_source *source;
} Soon;

static constexpr u32 soonCapacity = 16;
// how long BarSoon waits, for the rest of a burst to arrive.
static constexpr u64 soonDelay = 50'000;
static Soon soons[soonCapacity];

static int
SoonTick([[maybe_unused]] sd_event_source *source, [[maybe_unused]] u64 now, void *context)
{
  Soon *soon = context;
  soon->callback();
  return 0;
}

void
BarSoon(void (*callback)(void))
{
  for (u32 index = 0; index < soonCapacity; index++) {
    Soon *soon = &soons[index];
    if (soon->callback && soon->callback != callback) continue;

    if (!soon->callback) {
      soon->callback = callback;
      sd_event_add_time_relative(
        wayland.loop,
        &soon->source,
        CLOCK_MONOTONIC,
        soonDelay,
        0,
        SoonTick,
        soon
      );
      return;
    }

    // already waiting: leave it be, unless it has already been called.
    int enabled;
    sd_event_source_get_enabled(soon->source, &enabled);
    if (enabled != SD_EVENT_OFF) return;
    sd_event_source_set_time_relative(soon->source, soonDelay);
    sd_event_source_set_enabled(soon->source, SD_EVENT_ONESHOT);
    return;
  }
  callback();
}

bool
BarPillBox(const BarOutput *output, const BarModule *module, Box *box)
{
  for (u32 index = 0; index < output->hitCount; index++) {
    if (output->hits[index].module != module) continue;
    *box = output->hits[index].pill;
    return true;
  }
  return false;
}

BarOutput *
BarFocusedOutput(void)
{
  if (!bar.outputCount) return nullptr;

  char *reply = HyprlandRequest("j/monitors");
  StrBuf name = {};
  JsonArray monitors = JsonArrayBegin(reply);
  for (const char *monitor; (monitor = JsonArrayNext(&monitors));) {
    const char *focused = JsonMember(monitor, "focused");
    if (!focused || strncmp(focused, "true", 4) != 0) continue;
    JsonString(JsonMember(monitor, "name"), &name);
    break;
  }
  free(reply);

  BarOutput *focused = bar.outputs[0];
  for (u64 index = 0; index < bar.outputCount && name.length; index++) {
    const char *connector = bar.outputs[index]->output->connector;
    if (connector && strcmp(connector, name.data) == 0) focused = bar.outputs[index];
  }
  StrBufFree(&name);
  return focused;
}

// opens or closes the calendar below the clock on the focused output, as `bar calendar` asks.
static void
ToggleCalendar(void)
{
  BarOutput *output = BarFocusedOutput();
  if (!output || !output->surface) return;

  // the clock's pill, or the middle of the bar when there is no clock.
  for (u32 index = 0; index < output->hitCount; index++) {
    const Hit *hit = &output->hits[index];
    if (strcmp(hit->module->name, "clock") != 0) continue;
    CalendarToggle(output, hit->pill);
    return;
  }
  f64 middle = output->surface->width / 2.0;
  f64 top = (barHeight - pillHeight) / 2;
  CalendarToggle(output, (Box){middle - 1, top, 2, pillHeight});
}

static int
Signalled(
  [[maybe_unused]] sd_event_source *source,
  const struct signalfd_siginfo *info,
  [[maybe_unused]] void *context
)
{
  switch (info->ssi_signo) {
    case SIGUSR1:
      bar.hidden = !bar.hidden;
      for (u64 index = 0; index < bar.outputCount; index++) {
        if (bar.hidden) HideBar(bar.outputs[index]);
        else ShowBar(bar.outputs[index]);
      }
      break;
    case SIGUSR2:
      if (!bar.hidden) ToggleCalendar();
      break;
    default:
      WaylandQuit(0);
      break;
  }
  return 0;
}

// the signals that drive the running bar: toggle, calendar, and the two that end it.
static const int signals[] = {SIGUSR1, SIGUSR2, SIGTERM, SIGINT};

int
main(int argc, char **argv)
{
  setlocale(LC_ALL, "");

  if (argc > 1) {
    int signal = strcmp(argv[1], "toggle") == 0 ? SIGUSR1
      : strcmp(argv[1], "calendar") == 0        ? SIGUSR2
                                                : 0;
    if (!signal) {
      fprintf(stderr, "usage: bar [toggle | calendar]\n");
      return 2;
    }
    if (!InstanceSignal("bar", signal)) Fatal("the bar is not running");
    return 0;
  }

  pid_t holder;
  if (!InstanceLock("bar", &holder)) Fatal("the bar is already running (process %d)", holder);

  sd_event *loop;
  sd_event_default(&loop);
  sigset_t blocked;
  sigemptyset(&blocked);
  // SIGCHLD too, for the terminals the pills open (see process.c).
  sigaddset(&blocked, SIGCHLD);
  for (u32 index = 0; index < sizeof(signals) / sizeof(signals[0]); index++) {
    sigaddset(&blocked, signals[index]);
  }
  sigprocmask(SIG_BLOCK, &blocked, nullptr);
  for (u32 index = 0; index < sizeof(signals) / sizeof(signals[0]); index++) {
    sd_event_add_signal(loop, nullptr, signals[index], Signalled, nullptr);
  }

  // the modules start before the first draw, so it already shows their state.
  wayland.loop = loop;
  const BarModule *const *sections[] = {barLeft, barCenter, barRight};
  for (u32 section = 0; section < 3; section++) {
    for (u32 index = 0; sections[section][index]; index++) {
      if (sections[section][index]->Start) sections[section][index]->Start();
    }
  }

  OutputHandlers handlers = {.Added = OutputAdded, .Removed = OutputRemoved};
  WaylandConnect(loop, &handlers, nullptr);
  return WaylandRun();
}
