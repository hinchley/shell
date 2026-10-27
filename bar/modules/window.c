// window: the focused window's title, or "Desktop" when nothing is focused.

#include <stdlib.h>
#include <string.h>

#include "hyprland.h"
#include "json.h"
#include "module.h"
#include "strbuf.h"

static StrBuf title;
// the focused window's address, as the events write it (hex, without "0x"), or empty.
static StrBuf address;

// shows text (length bytes) as the title, or "Desktop" when it is empty.
static void
SetTitle(const char *text, u64 length)
{
  if (!length) {
    text = "Desktop";
    length = strlen(text);
  }
  if (title.length == length && memcmp(title.data, text, length) == 0) return;

  StrBufClear(&title);
  StrBufAppend(&title, text, length);
  BarRedraw();
}

// reads the focused window from hyprland, for the start and after a reconnection.
static void
Refresh(void)
{
  char *reply = HyprlandRequest("j/activewindow");
  StrBuf read = {};
  JsonString(JsonMember(reply, "address"), &read);
  const char *hex = StrBufText(&read);
  if (strncmp(hex, "0x", 2) == 0) hex += 2;
  StrBufClear(&address);
  StrBufAppendString(&address, hex);

  JsonString(JsonMember(reply, "title"), &read);
  free(reply);
  SetTitle(StrBufText(&read), read.length);
  StrBufFree(&read);
}

// the events carry the title, so it is shown straight away, without asking hyprland:
//   activewindow>>class,title      (">>," when nothing is focused)
//   activewindowv2>>address
//   windowtitlev2>>address,title
static void
Event(const char *name, const char *data)
{
  if (strcmp(name, "connected") == 0) {
    Refresh();
  } else if (strcmp(name, "activewindow") == 0) {
    const char *comma = strchr(data, ',');
    const char *text = comma ? comma + 1 : "";
    SetTitle(text, strlen(text));
  } else if (strcmp(name, "activewindowv2") == 0) {
    StrBufClear(&address);
    StrBufAppendString(&address, data);
  } else if (strcmp(name, "windowtitlev2") == 0) {
    const char *comma = strchr(data, ',');
    if (!comma || !address.length) return;
    u64 length = (u64)(comma - data);
    if (length != address.length || memcmp(data, address.data, length) != 0) return;
    SetTitle(comma + 1, strlen(comma + 1));
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
  segments[0] = SegmentLabel(StrBufText(&title), themeYellow);
  segments[0].elide = true;
  return 1;
}

const BarModule windowModule = {
  .name = "window",
  .Start = Start,
  .Segments = Segments,
  .padding = 30,
  .spacing = 4,
  .maxWidth = 320
};
