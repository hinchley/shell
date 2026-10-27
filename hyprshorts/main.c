// hyprshorts: a searchable list of every keybind in the hyprland lua config.
//
// the config is run with an embedded lua against a stub `hl` table that records each hl.bind()
// call and quietly accepts everything else, so binds made in loops or by helper functions are
// listed exactly as hyprland would make them.
// each bind is shown as its keys (made readable: "code:10" becomes "1", "XF86AudioMute"
// becomes "Audio Mute") beside its description.
//
// running hyprshorts while it is open closes it, so one keybind both opens and closes it, and
// it closes the launcher, so only one of the two is ever open.
//
// usage: hyprshorts [--list [query]] [config]
//   --list  prints the binds matching query, best first, instead of showing them.
//   config  the hyprland config to read (default ~/.config/hypr/hyprland.lua).

#include <ctype.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include "instance.h"
#include "match.h"
#include "memory.h"
#include "picker.h"
#include "strbuf.h"
#include "wayland.h"

// one keybind.
typedef struct Bind {
  // the keys made readable, such as "SUPER + SHIFT + 1".
  char *keys;
  // the description, or "(no description)" when the config gave none.
  char *description;
  bool undescribed;

  // the keys and description normalised for matching.
  char *keysNormal;
  char *descriptionNormal;
  // the description lowercased, with each run of digits padded to six, so that sorting it puts
  // "workspace 2" before "workspace 10".
  char *sortKey;
} Bind;

typedef struct Binds {
  Bind *items;
  u64 count;
  u64 capacity;
} Binds;

// the evdev codes of the number row in hyprland.lua's us layout, from code:10 ("1") on.
static const char *const numberRow[] =
  {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "="};

static const struct {
  const char *key;
  const char *name;
} mouseButtons[] =
  {{"mouse:272", "Mouse Left"}, {"mouse:273", "Mouse Right"}, {"mouse:274", "Mouse Middle"}};

// appends one key of a combination, made readable.
static void
FormatKey(StrBuf *out, const char *key, u64 length)
{
  if (length > 5 && strncmp(key, "code:", 5) == 0) {
    char *end;
    long code = strtol(key + 5, &end, 10);
    if (end == key + length && code >= 10 && code <= 21) {
      StrBufAppendString(out, numberRow[code - 10]);
      return;
    }
  }

  for (u64 index = 0; index < sizeof(mouseButtons) / sizeof(mouseButtons[0]); index++) {
    const char *button = mouseButtons[index].key;
    if (strlen(button) == length && strncmp(key, button, length) == 0) {
      StrBufAppendString(out, mouseButtons[index].name);
      return;
    }
  }

  // "XF86MonBrightnessUp" becomes "Mon Brightness Up": a space goes before each capital that
  // follows a lowercase letter or a digit.
  if (length > 4 && strncmp(key, "XF86", 4) == 0) {
    for (u64 index = 4; index < length; index++) {
      char previous = key[index - 1];
      bool splits = index > 4 && isupper((unsigned char)key[index]) &&
        (islower((unsigned char)previous) || isdigit((unsigned char)previous));
      if (splits) StrBufAppendByte(out, ' ');
      StrBufAppendByte(out, key[index]);
    }
    return;
  }

  StrBufAppend(out, key, length);
}

// keys as hyprland writes them ("SUPER + SHIFT + code:10"), made readable.
static char *
FormatKeys(const char *keys)
{
  StrBuf out = {};
  const char *at = keys;
  while (true) {
    const char *plus = strchr(at, '+');
    const char *end = plus ? plus : at + strlen(at);

    // each part is trimmed of spaces.
    const char *start = at;
    while (start < end && isspace((unsigned char)*start)) start++;
    const char *stop = end;
    while (stop > start && isspace((unsigned char)stop[-1])) stop--;

    if (out.length) StrBufAppendString(&out, " + ");
    FormatKey(&out, start, (u64)(stop - start));
    if (!plus) break;
    at = plus + 1;
  }
  if (!out.data) StrBufAppend(&out, "", 0);
  return out.data;
}

// the description lowercased with digit runs padded to six places.
static char *
SortKey(const char *description)
{
  StrBuf out = {};
  for (const char *at = description; *at;) {
    if (!isdigit((unsigned char)*at)) {
      StrBufAppendByte(&out, (char)tolower((unsigned char)*at));
      at++;
      continue;
    }

    u64 length = strspn(at, "0123456789");
    for (u64 pad = length; pad < 6; pad++) StrBufAppendByte(&out, '0');
    StrBufAppend(&out, at, length);
    at += length;
  }
  if (!out.data) StrBufAppend(&out, "", 0);
  return out.data;
}

// records a bind from its keys as written and its description (with whitespace runs already
// collapsed).
static void
AddBind(Binds *binds, const char *keys, const char *description)
{
  if (binds->count == binds->capacity) {
    binds->capacity = binds->capacity ? binds->capacity * 2 : 64;
    binds->items = MemoryResize(binds->items, binds->capacity, sizeof(Bind));
  }

  // the description is trimmed, as the quickshell parser did.
  while (isspace((unsigned char)*description)) description++;
  u64 length = strlen(description);
  while (length && isspace((unsigned char)description[length - 1])) length--;
  char *trimmed = MemoryCopyStringLength(description, length);

  Bind *bind = &binds->items[binds->count++];
  bind->keys = FormatKeys(keys);
  bind->undescribed = !length;
  bind->description = length ? trimmed : MemoryCopyString("(no description)");
  bind->keysNormal = MatchNormalized(bind->keys);
  bind->descriptionNormal = MatchNormalized(trimmed);
  bind->sortKey = SortKey(trimmed);
  if (!length) free(trimmed);
}

// hl.bind(keys, action, options): records keys and options.description.
static int
LuaBind(lua_State *lua)
{
  Binds *binds = lua_touserdata(lua, lua_upvalueindex(1));
  const char *keys = lua_tostring(lua, 1);
  if (!keys) return 0;

  // tostring(options.description), or "" when there are no options or no description.
  StrBuf description = {};
  if (lua_type(lua, 3) == LUA_TTABLE && lua_getfield(lua, 3, "description") != LUA_TNIL) {
    const char *text = luaL_tolstring(lua, -1, nullptr);
    // every run of whitespace becomes one space, as gsub("%s+", " ") did.
    bool space = false;
    for (const char *at = text; *at; at++) {
      if (isspace((unsigned char)*at)) {
        if (!space) StrBufAppendByte(&description, ' ');
        space = true;
        continue;
      }
      space = false;
      StrBufAppendByte(&description, *at);
    }
  }
  AddBind(binds, keys, StrBufText(&description));
  StrBufFree(&description);
  return 0;
}

// the stub's every field and call: the stub itself (its first upvalue).
static int
LuaStub(lua_State *lua)
{
  lua_pushvalue(lua, lua_upvalueindex(1));
  return 1;
}

// pushes the stub: a table whose every field and call gives the stub again, standing in for
// every hl.* value other than hl.bind.
static void
PushStub(lua_State *lua)
{
  lua_newtable(lua);
  lua_newtable(lua);
  lua_pushvalue(lua, -2);
  lua_pushcclosure(lua, LuaStub, 1);
  lua_setfield(lua, -2, "__index");
  lua_pushvalue(lua, -2);
  lua_pushcclosure(lua, LuaStub, 1);
  lua_setfield(lua, -2, "__call");
  lua_setmetatable(lua, -2);
}

// runs the config at path and collects its binds.
// errors are reported, keeping whatever binds were made before them.
static void
LoadBinds(Binds *binds, const char *path)
{
  lua_State *lua = luaL_newstate();
  luaL_openlibs(lua);

  if (luaL_loadfilex(lua, path, "t") != LUA_OK) {
    Warn("%s", lua_tostring(lua, -1));
    lua_close(lua);
    return;
  }

  // the chunk's environment: hl, falling back to the globals for everything else.
  lua_newtable(lua);
  lua_newtable(lua);
  lua_pushlightuserdata(lua, binds);
  lua_pushcclosure(lua, LuaBind, 1);
  lua_setfield(lua, -2, "bind");
  lua_newtable(lua);
  PushStub(lua);
  lua_pushcclosure(lua, LuaStub, 1);
  lua_setfield(lua, -2, "__index");
  lua_setmetatable(lua, -2);
  lua_setfield(lua, -2, "hl");
  lua_newtable(lua);
  lua_pushglobaltable(lua);
  lua_setfield(lua, -2, "__index");
  lua_setmetatable(lua, -2);
  // the environment is a chunk's first upvalue, _ENV.
  lua_setupvalue(lua, -2, 1);

  if (lua_pcall(lua, 0, 0, 0) != LUA_OK) Warn("%s", luaL_tolstring(lua, -1, nullptr));
  lua_close(lua);
}

static i64
Score(void *context, u64 index, const MatchQuery *query)
{
  const Bind *bind = &((const Binds *)context)->items[index];
  i64 score = 0;
  for (u64 token = 0; token < query->count; token++) {
    const char *text = query->tokens[token];
    i64 keyScore = MatchWholeWord(bind->keysNormal, text) ? 100
      : strstr(bind->keysNormal, text)                    ? 30
                                                          : 0;
    i64 descriptionScore = MatchTextScore(bind->descriptionNormal, text);
    i64 best = keyScore > descriptionScore ? keyScore : descriptionScore;
    if (!best) return -1;
    score += best;
  }
  return score;
}

// orders binds by description (naturally, undescribed last), then keys.
static int
Compare(void *context, u64 left, u64 right)
{
  const Bind *a = &((const Binds *)context)->items[left];
  const Bind *b = &((const Binds *)context)->items[right];
  if (a->undescribed != b->undescribed) return a->undescribed ? 1 : -1;

  int order = strcmp(a->sortKey, b->sortKey);
  return order ? order : strcoll(a->keys, b->keys);
}

static u64
Filter(void *context, const char *text, u64 *out)
{
  Binds *binds = context;
  MatchQuery query = MatchQueryParse(text);
  u64 count = MatchRank(binds->count, &query, Score, Compare, binds, out);
  MatchQueryFree(&query);
  return count;
}

static int
Terminated(
  [[maybe_unused]] sd_event_source *source,
  [[maybe_unused]] const struct signalfd_siginfo *info,
  [[maybe_unused]] void *context
)
{
  WaylandQuit(0);
  return 0;
}

int
main(int argc, char **argv)
{
  setlocale(LC_ALL, "");

  bool list = false;
  const char *query = "";
  const char *path = nullptr;
  for (int index = 1; index < argc; index++) {
    if (strcmp(argv[index], "--list") == 0) {
      list = true;
      if (index + 1 < argc && argv[index + 1][0] != '/') query = argv[++index];
    } else {
      path = argv[index];
    }
  }

  char defaultPath[512];
  if (!path) {
    const char *home = getenv("HOME");
    snprintf(
      defaultPath,
      sizeof(defaultPath),
      "%s/.config/hypr/hyprland.lua",
      home ? home : ""
    );
    path = defaultPath;
  }

  if (!list) {
    // a second run closes the first.
    pid_t holder;
    if (!InstanceLock("hyprshorts", &holder)) {
      if (holder > 0) kill(holder, SIGTERM);
      return 0;
    }
    // one overlay at a time: an open launcher closes.
    InstanceSignal("launcher", SIGTERM);
  }

  Binds binds = {};
  LoadBinds(&binds, path);
  PickerRow *rows = MemoryAllocateZeroed(binds.count, sizeof(PickerRow));
  for (u64 index = 0; index < binds.count; index++) {
    rows[index] = (PickerRow){
      .primary = binds.items[index].keys,
      .secondary = binds.items[index].description,
      .muted = binds.items[index].undescribed
    };
  }

  if (list) {
    u64 *order = MemoryResize(nullptr, binds.count, sizeof(u64));
    u64 count = Filter(&binds, query, order);
    for (u64 index = 0; index < count; index++) {
      printf("%s\t%s\n", rows[order[index]].primary, rows[order[index]].secondary);
    }
    return 0;
  }

  sd_event *loop;
  sd_event_default(&loop);
  sigset_t terminate;
  sigemptyset(&terminate);
  sigaddset(&terminate, SIGTERM);
  sigaddset(&terminate, SIGINT);
  sigprocmask(SIG_BLOCK, &terminate, nullptr);
  sd_event_add_signal(loop, nullptr, SIGTERM, Terminated, nullptr);
  sd_event_add_signal(loop, nullptr, SIGINT, Terminated, nullptr);

  WaylandConnect(loop, nullptr, nullptr);
  PickerOptions options = {
    .placeholder = "Search keyboard shortcuts",
    .panelWidth = 900,
    .panelHeight = 520,
    .primaryWidth = 260,
    .Filter = Filter,
    .context = &binds
  };
  PickerOpen(&options, rows, binds.count);
  return WaylandRun();
}
