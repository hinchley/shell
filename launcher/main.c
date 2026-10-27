// launcher: a searchable list of applications that runs the one chosen.
//
// the applications are the desktop entries in $XDG_DATA_HOME/applications and each
// $XDG_DATA_DIRS/applications, less those hidden from menus and those a blacklist matches.
// a whitelist in the config replaces the desktop entries altogether.
//
// the config, ~/.config/launcher/config.ini (or $XDG_CONFIG_HOME/launcher/config.ini), has two
// optional sections.
// lines starting with # or ; are comments.
//
//   [blacklist]
//   # one entry per line: a desktop entry is hidden when its name, id, command or comment
//   # contains one, compared as lowercase words.
//   Avahi
//
//   [whitelist]
//   # name = command: when there is any, only these are listed, run with sh -c.
//   Firefox = firefox --new-window
//
// without a config the blacklist below is used.
// running the launcher while it is open closes it, so one keybind both opens and closes it,
// and it closes hyprshorts, so only one of the two is ever open.
//
// usage: launcher [--list [query]]
//   --list  prints the applications matching query, best first, with the command each would
//           run, instead of showing them.

#include <dirent.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "instance.h"
#include "match.h"
#include "memory.h"
#include "picker.h"
#include "process.h"
#include "strbuf.h"
#include "wayland.h"

// the blacklist used when the config has none.
static const char *const defaultBlacklist[] = {
  "Avahi",
  "btop++",
  "Foot Client",
  "Foot Server",
  "Hardware Locality",
  "Kvantum",
  "Limine",
  "Manage Printing",
  "QT",
  "Qt V4L2",
  "uuctl",
  "UXterm",
  "Vim",
  "wiremix",
  "XTerm"
};

// one application.
typedef struct App {
  // the desktop file id ("org.gnome.Nautilus"), or nullptr for a whitelist entry.
  char *id;
  char *name;
  // the desktop entry's Exec line as written, or a whitelist entry's shell command.
  char *command;
  char *comment;
  char *keywords;
  char *categories;
  // the folder to run in, or nullptr.
  char *directory;
  // the desktop file's path, for the %k field code.
  char *path;
  char *icon;
  bool terminal;

  char *nameNormal;
  char *commandNormal;
  char *commentNormal;
  char *keywordsNormal;
  char *categoriesNormal;
} App;

typedef struct Apps {
  App *items;
  u64 count;
  u64 capacity;
} Apps;

// a list of strings.
typedef struct Strings {
  char **items;
  u64 count;
  u64 capacity;
} Strings;

typedef struct Config {
  Strings blacklist;
  // name and command pairs, one after the other.
  Strings whitelist;
  bool hasBlacklist;
} Config;

static void
StringsAdd(Strings *strings, char *string)
{
  if (strings->count == strings->capacity) {
    strings->capacity = strings->capacity ? strings->capacity * 2 : 16;
    strings->items = MemoryResize(strings->items, strings->capacity, sizeof(char *));
  }
  strings->items[strings->count++] = string;
}

static bool
StringsHas(const Strings *strings, const char *string)
{
  for (u64 index = 0; index < strings->count; index++) {
    if (strcmp(strings->items[index], string) == 0) return true;
  }
  return false;
}

// the start of text without leading spaces, cut (in place) before its trailing ones.
static char *
Trim(char *text)
{
  while (*text == ' ' || *text == '\t') text++;
  u64 length = strlen(text);
  while (length &&
    (text[length - 1] == ' ' || text[length - 1] == '\t' || text[length - 1] == '\r')) {
    length--;
  }
  text[length] = '\0';
  return text;
}

static void
ReadConfig(Config *config)
{
  const char *configHome = getenv("XDG_CONFIG_HOME");
  const char *home = getenv("HOME");
  char path[1024];
  if (configHome && *configHome) {
    snprintf(path, sizeof(path), "%s/launcher/config.ini", configHome);
  } else {
    snprintf(path, sizeof(path), "%s/.config/launcher/config.ini", home ? home : "");
  }

  StrBuf text = {};
  if (StrBufReadFile(&text, path)) {
    enum {
      SectionNone,
      SectionBlacklist,
      SectionWhitelist
    } section = SectionNone;
    char *save = nullptr;
    for (char *line = strtok_r(text.data, "\n", &save); line;
      line = strtok_r(nullptr, "\n", &save)) {
      line = Trim(line);
      if (!*line || *line == '#' || *line == ';') continue;

      if (strcmp(line, "[blacklist]") == 0) {
        section = SectionBlacklist;
        config->hasBlacklist = true;
      } else if (strcmp(line, "[whitelist]") == 0) {
        section = SectionWhitelist;
      } else if (*line == '[') {
        section = SectionNone;
        Warn("%s: unknown section %s", path, line);
      } else if (section == SectionBlacklist) {
        StringsAdd(&config->blacklist, MatchNormalized(line));
      } else if (section == SectionWhitelist) {
        char *equals = strchr(line, '=');
        if (!equals) {
          Warn("%s: whitelist entry without '=': %s", path, line);
          continue;
        }
        *equals = '\0';
        char *name = Trim(line);
        char *command = Trim(equals + 1);
        if (!*name || !*command) continue;
        StringsAdd(&config->whitelist, MemoryCopyString(name));
        StringsAdd(&config->whitelist, MemoryCopyString(command));
      }
    }
  }
  StrBufFree(&text);

  if (config->hasBlacklist) return;
  for (u64 index = 0; index < sizeof(defaultBlacklist) / sizeof(defaultBlacklist[0]);
    index++) {
    StringsAdd(&config->blacklist, MatchNormalized(defaultBlacklist[index]));
  }
}

// the locale's names for localised keys, best first: "en_AU", "en".
typedef struct Locale {
  char full[32];
  char language[16];
} Locale;

static Locale
CurrentLocale(void)
{
  Locale locale = {};
  const char *name = setlocale(LC_MESSAGES, nullptr);
  if (!name || strcmp(name, "C") == 0 || strcmp(name, "POSIX") == 0) return locale;

  // "en_AU.UTF-8@euro" keeps "en_AU", and "en".
  snprintf(locale.full, sizeof(locale.full), "%.*s", (int)strcspn(name, ".@"), name);
  snprintf(locale.language, sizeof(locale.language), "%.*s", (int)strcspn(name, "_.@"), name);
  return locale;
}

// a desktop entry's [Desktop Entry] keys, while reading it.
// each value keeps how well its locale matched, so a better one replaces it.
typedef struct Entry {
  char *values[16];
  u32 ranks[16];
} Entry;

// the keys read, in the order of Entry's values.
enum EntryKey {
  EntryKeyType,
  EntryKeyName,
  EntryKeyExec,
  EntryKeyComment,
  EntryKeyKeywords,
  EntryKeyCategories,
  EntryKeyPath,
  EntryKeyIcon,
  EntryKeyTerminal,
  EntryKeyNoDisplay,
  EntryKeyHidden,
  EntryKeyCount
};

static const char *const entryKeys[EntryKeyCount] = {
  "Type",
  "Name",
  "Exec",
  "Comment",
  "Keywords",
  "Categories",
  "Path",
  "Icon",
  "Terminal",
  "NoDisplay",
  "Hidden"
};

// unescapes a desktop entry value in place (\s, \n, \t, \r and \\), and for a list (a value
// split at ';') turns the separators into spaces, as words to match.
static char *
Unescape(char *value, bool list)
{
  char *out = value;
  for (char *at = value; *at; at++) {
    if (*at == '\\' && at[1]) {
      at++;
      switch (*at) {
        case 's':
          *out++ = ' ';
          break;
        case 'n':
          *out++ = '\n';
          break;
        case 't':
          *out++ = '\t';
          break;
        case 'r':
          *out++ = '\r';
          break;
        default:
          *out++ = *at;
          break;
      }
      continue;
    }
    *out++ = list && *at == ';' ? ' ' : *at;
  }
  *out = '\0';
  return Trim(value);
}

// reads the desktop file at path into entry.
// false when it cannot be read.
static bool
ReadEntry(const char *path, const Locale *locale, Entry *entry)
{
  StrBuf text = {};
  if (!StrBufReadFile(&text, path)) return false;

  *entry = (Entry){};
  bool inGroup = false;
  char *save = nullptr;
  for (char *line = strtok_r(text.data, "\n", &save); line;
    line = strtok_r(nullptr, "\n", &save)) {
    if (*line == '[') {
      // only the main group matters; actions and the rest come after it.
      if (inGroup) break;
      inGroup = strncmp(line, "[Desktop Entry]", 15) == 0;
      continue;
    }
    if (!inGroup || *line == '#') continue;

    char *equals = strchr(line, '=');
    if (!equals) continue;
    *equals = '\0';
    char *key = Trim(line);
    char *value = equals + 1;

    // "Name[en_AU]" ranks 3, "Name[en]" 2 and "Name" 1; other locales are skipped.
    u32 rank = 1;
    char *bracket = strchr(key, '[');
    if (bracket) {
      char *tag = bracket + 1;
      tag[strcspn(tag, "]")] = '\0';
      *bracket = '\0';
      if (*locale->full && strcmp(tag, locale->full) == 0) rank = 3;
      else if (*locale->language && strcmp(tag, locale->language) == 0) rank = 2;
      else continue;
    }

    for (u32 index = 0; index < EntryKeyCount; index++) {
      if (strcmp(key, entryKeys[index]) != 0 || rank <= entry->ranks[index]) continue;

      bool list = index == EntryKeyKeywords || index == EntryKeyCategories;
      free(entry->values[index]);
      entry->values[index] = MemoryCopyString(Unescape(value, list));
      entry->ranks[index] = rank;
    }
  }
  StrBufFree(&text);
  return true;
}

static void
FreeEntry(Entry *entry)
{
  for (u32 index = 0; index < EntryKeyCount; index++) free(entry->values[index]);
}

// the entry's value for key, or "".
static const char *
EntryValue(const Entry *entry, enum EntryKey key)
{
  return entry->values[key] ? entry->values[key] : "";
}

static bool
EntryTrue(const Entry *entry, enum EntryKey key)
{
  return strcmp(EntryValue(entry, key), "true") == 0;
}

// fills in an app's normalised fields.
static void
Normalise(App *app)
{
  app->nameNormal = MatchNormalized(app->name);
  app->commandNormal = MatchNormalized(app->command);
  app->commentNormal = MatchNormalized(app->comment);
  app->keywordsNormal = MatchNormalized(app->keywords);
  app->categoriesNormal = MatchNormalized(app->categories);
}

static App *
AddApp(Apps *apps)
{
  if (apps->count == apps->capacity) {
    apps->capacity = apps->capacity ? apps->capacity * 2 : 64;
    apps->items = MemoryResize(apps->items, apps->capacity, sizeof(App));
  }
  App *app = &apps->items[apps->count++];
  *app = (App){};
  return app;
}

// true when a blacklist entry appears in the app's name, id, command or comment.
static bool
Blacklisted(const Config *config, const App *app)
{
  StrBuf haystack = {};
  StrBufAppendFormat(&haystack, "%s %s %s %s", app->name, app->id, app->command, app->comment);
  char *normal = MatchNormalized(StrBufText(&haystack));
  StrBufFree(&haystack);

  bool found = false;
  for (u64 index = 0; index < config->blacklist.count && !found; index++) {
    const char *needle = config->blacklist.items[index];
    found = *needle && strstr(normal, needle);
  }
  free(normal);
  return found;
}

// reads the desktop files under folder (an applications folder, or one inside it), whose ids
// start with prefix.
// ids already seen (from a folder earlier in the search path) are skipped, even when the entry
// was hidden there.
static void
ScanFolder(
  Apps *apps,
  Strings *seen,
  const Config *config,
  const Locale *locale,
  const char *folder,
  const char *prefix
)
{
  DIR *directory = opendir(folder);
  if (!directory) return;

  struct dirent *item;
  while ((item = readdir(directory))) {
    const char *name = item->d_name;
    if (name[0] == '.') continue;

    char path[2048];
    snprintf(path, sizeof(path), "%s/%s", folder, name);
    struct stat status;
    if (stat(path, &status) != 0) continue;

    // a file in a subfolder has an id with the subfolder's name and a dash before it.
    if (S_ISDIR(status.st_mode)) {
      char subPrefix[512];
      snprintf(subPrefix, sizeof(subPrefix), "%s%s-", prefix, name);
      ScanFolder(apps, seen, config, locale, path, subPrefix);
      continue;
    }

    u64 length = strlen(name);
    if (length <= 8 || strcmp(name + length - 8, ".desktop") != 0) continue;

    char id[512];
    snprintf(id, sizeof(id), "%s%.*s", prefix, (int)(length - 8), name);
    if (StringsHas(seen, id)) continue;
    StringsAdd(seen, MemoryCopyString(id));

    Entry entry;
    if (!ReadEntry(path, locale, &entry)) continue;

    bool listed = strcmp(EntryValue(&entry, EntryKeyType), "Application") == 0 &&
      *EntryValue(&entry, EntryKeyName) && !EntryTrue(&entry, EntryKeyNoDisplay) &&
      !EntryTrue(&entry, EntryKeyHidden);
    if (listed) {
      App *app = AddApp(apps);
      app->id = MemoryCopyString(id);
      app->name = MemoryCopyString(EntryValue(&entry, EntryKeyName));
      app->command = MemoryCopyString(EntryValue(&entry, EntryKeyExec));
      app->comment = MemoryCopyString(EntryValue(&entry, EntryKeyComment));
      app->keywords = MemoryCopyString(EntryValue(&entry, EntryKeyKeywords));
      app->categories = MemoryCopyString(EntryValue(&entry, EntryKeyCategories));
      app->directory = MemoryCopyString(EntryValue(&entry, EntryKeyPath));
      app->icon = MemoryCopyString(EntryValue(&entry, EntryKeyIcon));
      app->path = MemoryCopyString(path);
      app->terminal = EntryTrue(&entry, EntryKeyTerminal);
      if (Blacklisted(config, app)) apps->count--;
      else Normalise(app);
    }
    FreeEntry(&entry);
  }
  closedir(directory);
}

// the applications to list: the whitelist when there is one, else the desktop entries.
static void
LoadApps(Apps *apps, const Config *config)
{
  if (config->whitelist.count) {
    for (u64 index = 0; index + 1 < config->whitelist.count; index += 2) {
      App *app = AddApp(apps);
      app->name = config->whitelist.items[index];
      app->command = config->whitelist.items[index + 1];
      app->comment = app->keywords = app->categories = "";
      Normalise(app);
    }
    return;
  }

  // $XDG_DATA_HOME first, then each of $XDG_DATA_DIRS, so earlier folders win.
  const char *dataHome = getenv("XDG_DATA_HOME");
  const char *dataDirs = getenv("XDG_DATA_DIRS");
  const char *home = getenv("HOME");
  StrBuf folders = {};
  if (dataHome && *dataHome) StrBufAppendString(&folders, dataHome);
  else StrBufAppendFormat(&folders, "%s/.local/share", home ? home : "");
  StrBufAppendByte(&folders, ':');
  StrBufAppendString(
    &folders,
    dataDirs && *dataDirs ? dataDirs : "/usr/local/share:/usr/share"
  );

  Locale locale = CurrentLocale();
  Strings seen = {};
  char *save = nullptr;
  for (char *folder = strtok_r(folders.data, ":", &save); folder;
    folder = strtok_r(nullptr, ":", &save)) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/applications", folder);
    ScanFolder(apps, &seen, config, &locale, path, "");
  }
  StrBufFree(&folders);
}

static i64
Score(void *context, u64 index, const MatchQuery *query)
{
  const App *app = &((const Apps *)context)->items[index];
  i64 score = 0;
  for (u64 token = 0; token < query->count; token++) {
    const char *text = query->tokens[token];
    i64 nameScore = MatchTextScore(app->nameNormal, text);
    if (nameScore > 0) score += nameScore;
    else if (strstr(app->commandNormal, text)) score += 35;
    else if (strstr(app->keywordsNormal, text)) score += 25;
    else if (strstr(app->categoriesNormal, text)) score += 15;
    else if (strstr(app->commentNormal, text)) score += 8;
    else return -1;
  }
  return score;
}

static int
Compare(void *context, u64 left, u64 right)
{
  const Apps *apps = context;
  return strcoll(apps->items[left].name, apps->items[right].name);
}

static u64
Filter(void *context, const char *text, u64 *out)
{
  Apps *apps = context;
  MatchQuery query = MatchQueryParse(text);
  u64 count = MatchRank(apps->count, &query, Score, Compare, apps, out);
  MatchQueryFree(&query);
  return count;
}

// splits a desktop entry's Exec line into arguments, as the desktop entry spec has it: double
// quotes group, a backslash escapes within them, and field codes are expanded or dropped (a
// lone %f, %F, %u or %U argument is dropped altogether, as nothing is opened).
static Strings
ExecArguments(const App *app)
{
  Strings arguments = {};
  StrBuf argument = {};
  bool started = false;
  bool quoted = false;

  for (const char *at = app->command;; at++) {
    char byte = *at;
    if (!byte || (!quoted && (byte == ' ' || byte == '\t'))) {
      if (started) {
        StringsAdd(&arguments, MemoryCopyString(StrBufText(&argument)));
        StrBufClear(&argument);
      }
      started = false;
      if (!byte) break;
      continue;
    }

    if (byte == '"') {
      quoted = !quoted;
      started = true;
    } else if (quoted && byte == '\\' && at[1]) {
      StrBufAppendByte(&argument, *++at);
    } else if (byte == '%' && at[1]) {
      char code = *++at;
      if (code == '%') {
        StrBufAppendByte(&argument, '%');
        started = true;
      } else if (code == 'c') {
        StrBufAppendString(&argument, app->name);
        started = true;
      } else if (code == 'k' && app->path) {
        StrBufAppendString(&argument, app->path);
        started = true;
      } else if (code == 'i' && app->icon && *app->icon) {
        StringsAdd(&arguments, MemoryCopyString("--icon"));
        StrBufAppendString(&argument, app->icon);
        started = true;
      }
    } else {
      StrBufAppendByte(&argument, byte);
      started = true;
    }
  }
  StrBufFree(&argument);
  StringsAdd(&arguments, nullptr);
  arguments.count--;
  return arguments;
}

static void
Launch(const App *app)
{
  // a whitelist entry is a shell command.
  if (!app->id) {
    const char *argv[] = {"sh", "-c", app->command, nullptr};
    ProcessSpawn(argv, nullptr);
    return;
  }

  Strings arguments = ExecArguments(app);
  if (!arguments.count) return;

  // a terminal application runs in foot.
  if (app->terminal) {
    const char **argv = MemoryResize(nullptr, arguments.count + 3, sizeof(char *));
    argv[0] = "foot";
    argv[1] = "-e";
    for (u64 index = 0; index <= arguments.count; index++) {
      argv[index + 2] = arguments.items[index];
    }
    ProcessSpawn(argv, app->directory);
    free(argv);
    return;
  }
  ProcessSpawn((const char *const *)arguments.items, app->directory);
}

static void
Activated(void *context, u64 index)
{
  Launch(&((const Apps *)context)->items[index]);
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

  bool list = argc > 1 && strcmp(argv[1], "--list") == 0;
  const char *query = list && argc > 2 ? argv[2] : "";

  if (!list) {
    // a second run closes the first.
    pid_t holder;
    if (!InstanceLock("launcher", &holder)) {
      if (holder > 0) kill(holder, SIGTERM);
      return 0;
    }
    // one overlay at a time: an open hyprshorts closes.
    InstanceSignal("hyprshorts", SIGTERM);
  }

  Config config = {};
  ReadConfig(&config);
  Apps apps = {};
  LoadApps(&apps, &config);

  PickerRow *rows = MemoryAllocateZeroed(apps.count, sizeof(PickerRow));
  for (u64 index = 0; index < apps.count; index++) {
    rows[index].primary = apps.items[index].name;
  }

  if (list) {
    u64 *order = MemoryResize(nullptr, apps.count, sizeof(u64));
    u64 count = Filter(&apps, query, order);
    for (u64 index = 0; index < count; index++) {
      const App *app = &apps.items[order[index]];
      printf("%s\t%s\t", app->id ? app->id : "", app->name);
      if (!app->id) {
        printf("%s\n", app->command);
        continue;
      }
      // the arguments it would run with.
      Strings arguments = ExecArguments(app);
      for (u64 argument = 0; argument < arguments.count; argument++) {
        printf("%s%s", argument ? " " : "", arguments.items[argument]);
      }
      printf("\n");
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
    .placeholder = "Run",
    .panelWidth = 560,
    .panelHeight = 420,
    .Filter = Filter,
    .Activated = Activated,
    .context = &apps
  };
  PickerOpen(&options, rows, apps.count);
  return WaylandRun();
}
