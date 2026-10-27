#include "icon.h"

#include <dirent.h>
#include <librsvg/rsvg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "memory.h"
#include "strbuf.h"

// the image types searched for, in order.
static const char *const extensions[] = {".png", ".svg"};

static bool
IsFile(const char *path)
{
  struct stat status;
  return stat(path, &status) == 0 && S_ISREG(status.st_mode);
}

static bool
EndsWith(const char *text, const char *end)
{
  u64 length = strlen(text);
  u64 endLength = strlen(end);
  return length >= endLength && strcmp(text + length - endLength, end) == 0;
}

// a new size pixel square surface with source drawn into it, scaled to fit and centred.
static cairo_surface_t *
Fit(cairo_surface_t *source, f64 width, f64 height, u32 size)
{
  cairo_surface_t *icon =
    cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)size, (int)size);
  cairo_t *cairo = cairo_create(icon);
  f64 scale = fmin(size / width, size / height);
  cairo_translate(cairo, (size - width * scale) / 2, (size - height * scale) / 2);
  cairo_scale(cairo, scale, scale);
  cairo_set_source_surface(cairo, source, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cairo), CAIRO_FILTER_GOOD);
  cairo_paint(cairo);
  cairo_destroy(cairo);
  return icon;
}

// the image file at path, size pixels square, or nullptr.
static cairo_surface_t *
LoadFile(const char *path, u32 size)
{
  if (EndsWith(path, ".svg")) {
    RsvgHandle *handle = rsvg_handle_new_from_file(path, nullptr);
    if (!handle) return nullptr;

    cairo_surface_t *icon =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)size, (int)size);
    cairo_t *cairo = cairo_create(icon);
    RsvgRectangle viewport = {0, 0, size, size};
    rsvg_handle_render_document(handle, cairo, &viewport, nullptr);
    cairo_destroy(cairo);
    g_object_unref(handle);
    return icon;
  }

  cairo_surface_t *image = cairo_image_surface_create_from_png(path);
  if (cairo_surface_status(image) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(image);
    return nullptr;
  }
  cairo_surface_t *icon = Fit(
    image,
    cairo_image_surface_get_width(image),
    cairo_image_surface_get_height(image),
    size
  );
  cairo_surface_destroy(image);
  return icon;
}

// the folders icon themes live in, best first, as ':' separated text.
static void
ThemeBases(StrBuf *bases)
{
  const char *home = getenv("HOME");
  const char *dataHome = getenv("XDG_DATA_HOME");
  const char *dataDirs = getenv("XDG_DATA_DIRS");
  StrBufAppendFormat(bases, "%s/.icons", home ? home : "");
  if (dataHome && *dataHome) StrBufAppendFormat(bases, ":%s/icons", dataHome);
  else StrBufAppendFormat(bases, ":%s/.local/share/icons", home ? home : "");

  StrBuf dirs = {};
  StrBufAppendString(&dirs, dataDirs && *dataDirs ? dataDirs : "/usr/local/share:/usr/share");
  char *save = nullptr;
  for (char *dir = strtok_r(dirs.data, ":", &save); dir; dir = strtok_r(nullptr, ":", &save)) {
    StrBufAppendFormat(bases, ":%s/icons", dir);
  }
  StrBufFree(&dirs);
}

// the icon theme gtk is set to use, or "hicolor".
static void
ThemeName(StrBuf *name)
{
  const char *home = getenv("HOME");
  const char *configHome = getenv("XDG_CONFIG_HOME");
  char path[1024];
  if (configHome && *configHome) {
    snprintf(path, sizeof(path), "%s/gtk-3.0/settings.ini", configHome);
  } else snprintf(path, sizeof(path), "%s/.config/gtk-3.0/settings.ini", home ? home : "");

  StrBuf settings = {};
  StrBufReadFile(&settings, path);
  const char *key = strstr(StrBufText(&settings), "gtk-icon-theme-name");
  if (key) {
    const char *value = strchr(key, '=');
    if (value) {
      value++;
      while (*value == ' ' || *value == '"') value++;
      u64 length = strcspn(value, "\"\r\n ");
      StrBufAppend(name, value, length);
    }
  }
  StrBufFree(&settings);
  if (!name->length) StrBufAppendString(name, "hicolor");
}

// the index.theme of theme in any base folder, read into index.
// false when there is none.
static bool
ReadIndex(const char *bases, const char *theme, StrBuf *index)
{
  StrBuf copy = {};
  StrBufAppendString(&copy, bases);
  bool found = false;
  char *save = nullptr;
  for (char *base = strtok_r(copy.data, ":", &save); base && !found;
    base = strtok_r(nullptr, ":", &save)) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s/index.theme", base, theme);
    found = StrBufReadFile(index, path);
  }
  StrBufFree(&copy);
  return found;
}

// the value of key in the [group] section of an index.theme, copied into value.
static bool
IndexValue(const char *index, const char *group, const char *key, StrBuf *value)
{
  StrBufClear(value);
  char header[512];
  snprintf(header, sizeof(header), "[%s]\n", group);
  const char *section = strstr(index, header);
  if (!section) return false;

  u64 keyLength = strlen(key);
  for (const char *line = section + strlen(header); *line && *line != '[';) {
    const char *end = strchr(line, '\n');
    u64 length = end ? (u64)(end - line) : strlen(line);
    if (length > keyLength && strncmp(line, key, keyLength) == 0 && line[keyLength] == '=') {
      StrBufAppend(value, line + keyLength + 1, length - keyLength - 1);
      return true;
    }
    if (!end) break;
    line = end + 1;
  }
  return false;
}

// how badly a folder of the given size and type suits an icon of size pixels: 0 for a scalable
// folder or an exact size, otherwise the difference in pixels (bigger is better than smaller,
// as it scales down cleanly).
static u32
SizeDistance(u32 folderSize, bool scalable, u32 size)
{
  if (scalable || folderSize == size) return 0;
  if (folderSize > size) return folderSize - size;
  return 2 * (size - folderSize);
}

// searches theme (and the themes it inherits, then hicolor) for name, returning the best
// file's path in path.
// depth stops endless inheritance.
static bool
SearchTheme(
  const char *bases,
  const char *theme,
  const char *name,
  u32 size,
  StrBuf *path,
  u32 depth
)
{
  StrBuf index = {};
  StrBuf value = {};
  if (depth > 6 || !ReadIndex(bases, theme, &index)) {
    StrBufFree(&index);
    return false;
  }

  // every folder of the theme, in every base folder, with the icon in it.
  u32 bestDistance = UINT32_MAX;
  StrBuf directories = {};
  IndexValue(index.data, "Icon Theme", "Directories", &directories);
  StrBuf scaled = {};
  IndexValue(index.data, "Icon Theme", "ScaledDirectories", &scaled);
  if (scaled.length) StrBufAppendFormat(&directories, ",%s", scaled.data);
  if (!directories.data) StrBufAppend(&directories, "", 0);

  StrBuf baseCopy = {};
  char *saveDirectory = nullptr;
  for (char *directory = strtok_r(directories.data, ",", &saveDirectory); directory;
    directory = strtok_r(nullptr, ",", &saveDirectory)) {
    u32 folderSize = IndexValue(index.data, directory, "Size", &value) ? (u32)atoi(value.data)
                                                                       : 0;
    bool scalable = IndexValue(index.data, directory, "Type", &value) &&
      strcmp(value.data, "Scalable") == 0;
    u32 distance = SizeDistance(folderSize, scalable, size);
    if (distance >= bestDistance) continue;

    StrBufClear(&baseCopy);
    StrBufAppendString(&baseCopy, bases);
    char *saveBase = nullptr;
    for (char *base = strtok_r(baseCopy.data, ":", &saveBase); base;
      base = strtok_r(nullptr, ":", &saveBase)) {
      for (u32 extension = 0; extension < sizeof(extensions) / sizeof(extensions[0]);
        extension++) {
        char candidate[2048];
        snprintf(
          candidate,
          sizeof(candidate),
          "%s/%s/%s/%s%s",
          base,
          theme,
          directory,
          name,
          extensions[extension]
        );
        if (!IsFile(candidate)) continue;
        StrBufClear(path);
        StrBufAppendString(path, candidate);
        bestDistance = distance;
        break;
      }
      if (bestDistance == distance) break;
    }
  }
  StrBufFree(&baseCopy);
  StrBufFree(&directories);
  StrBufFree(&scaled);

  bool found = bestDistance != UINT32_MAX;
  if (!found && IndexValue(index.data, "Icon Theme", "Inherits", &value)) {
    char *saveParent = nullptr;
    for (char *parent = strtok_r(value.data, ",", &saveParent); parent && !found;
      parent = strtok_r(nullptr, ",", &saveParent)) {
      found = SearchTheme(bases, parent, name, size, path, depth + 1);
    }
  }
  StrBufFree(&index);
  StrBufFree(&value);
  return found;
}

// searches a folder an item gave (and the folders in it, a few levels down) for name.
static bool
SearchFolder(const char *folder, const char *name, StrBuf *path, u32 depth)
{
  for (u32 extension = 0; extension < sizeof(extensions) / sizeof(extensions[0]);
    extension++) {
    char candidate[2048];
    snprintf(candidate, sizeof(candidate), "%s/%s%s", folder, name, extensions[extension]);
    if (!IsFile(candidate)) continue;
    StrBufClear(path);
    StrBufAppendString(path, candidate);
    return true;
  }
  if (depth >= 4) return false;

  DIR *directory = opendir(folder);
  if (!directory) return false;
  bool found = false;
  struct dirent *item;
  while (!found && (item = readdir(directory))) {
    if (item->d_name[0] == '.') continue;
    char child[2048];
    snprintf(child, sizeof(child), "%s/%s", folder, item->d_name);
    struct stat status;
    if (stat(child, &status) == 0 && S_ISDIR(status.st_mode)) {
      found = SearchFolder(child, name, path, depth + 1);
    }
  }
  closedir(directory);
  return found;
}

cairo_surface_t *
IconLoad(const char *name, const char *themePath, u32 size)
{
  if (!name || !*name) return nullptr;
  if (name[0] == '/') return IsFile(name) ? LoadFile(name, size) : nullptr;

  StrBuf path = {};
  bool found = themePath && *themePath && SearchFolder(themePath, name, &path, 0);
  if (!found) {
    StrBuf bases = {};
    StrBuf theme = {};
    ThemeBases(&bases);
    ThemeName(&theme);
    found = SearchTheme(bases.data, theme.data, name, size, &path, 0) ||
      (strcmp(theme.data, "hicolor") != 0 &&
        SearchTheme(bases.data, "hicolor", name, size, &path, 0));
    StrBufFree(&bases);
    StrBufFree(&theme);
  }
  for (u32 extension = 0; !found && extension < sizeof(extensions) / sizeof(extensions[0]);
    extension++) {
    char candidate[1024];
    snprintf(
      candidate,
      sizeof(candidate),
      "/usr/share/pixmaps/%s%s",
      name,
      extensions[extension]
    );
    if (!IsFile(candidate)) continue;
    StrBufAppendString(&path, candidate);
    found = true;
  }

  cairo_surface_t *icon = found ? LoadFile(path.data, size) : nullptr;
  StrBufFree(&path);
  return icon;
}

cairo_surface_t *
IconFromPixels(const u8 *pixels, i32 width, i32 height, u32 size)
{
  // network byte order argb becomes cairo's premultiplied native argb.
  cairo_surface_t *image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  u8 *data = cairo_image_surface_get_data(image);
  i32 stride = cairo_image_surface_get_stride(image);
  for (i32 y = 0; y < height; y++) {
    u32 *row = (u32 *)(data + y * stride);
    for (i32 x = 0; x < width; x++) {
      const u8 *pixel = pixels + 4 * (y * width + x);
      u32 alpha = pixel[0];
      u32 red = pixel[1] * alpha / 255;
      u32 green = pixel[2] * alpha / 255;
      u32 blue = pixel[3] * alpha / 255;
      row[x] = alpha << 24 | red << 16 | green << 8 | blue;
    }
  }
  cairo_surface_mark_dirty(image);
  cairo_surface_t *icon = Fit(image, width, height, size);
  cairo_surface_destroy(image);
  return icon;
}
