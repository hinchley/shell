// temp: the cpu package temperature, from the first hwmon chip of a known cpu driver
// (coretemp, k10temp or zenpower).
// a click opens btop.

#include <dirent.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>

#include "module.h"
#include "process.h"

static constexpr u64 interval = 2'000'000;

// the chips whose sensors measure the cpu, and the labels of their package-wide sensor.
static const char *const drivers[] = {"coretemp", "k10temp", "zenpower"};
static const char *const packageLabels[] = {"Package id 0", "Tctl", "Tdie"};

typedef struct Temp {
  // the sensor's input file, once found.
  char path[512];
  // degrees celsius, or -1 when there is no sensor.
  i32 degrees;
} Temp;

static Temp temp = {.degrees = -1};

// the first line of the file at path, without its newline.
// false when it cannot be read.
static bool
ReadLine(const char *path, char *line, u64 size)
{
  FILE *file = fopen(path, "re");
  if (!file) return false;

  bool read = fgets(line, (int)size, file) != nullptr;
  fclose(file);
  if (read) line[strcspn(line, "\n")] = '\0';
  return read;
}

static bool
KnownDriver(const char *name)
{
  for (u32 index = 0; index < sizeof(drivers) / sizeof(drivers[0]); index++) {
    if (strcmp(name, drivers[index]) == 0) return true;
  }
  return false;
}

static bool
PackageLabel(const char *label)
{
  for (u32 index = 0; index < sizeof(packageLabels) / sizeof(packageLabels[0]); index++) {
    if (strcmp(label, packageLabels[index]) == 0) return true;
  }
  return false;
}

// finds the sensor: a known chip's package sensor, or else that chip's first sensor.
static bool
FindSensor(void)
{
  DIR *hwmon = opendir("/sys/class/hwmon");
  if (!hwmon) return false;

  bool found = false;
  struct dirent *chip;
  while (!found && (chip = readdir(hwmon))) {
    if (chip->d_name[0] == '.') continue;

    char path[512];
    char name[64];
    snprintf(path, sizeof(path), "/sys/class/hwmon/%s/name", chip->d_name);
    if (!ReadLine(path, name, sizeof(name)) || !KnownDriver(name)) continue;

    for (u32 sensor = 1; sensor < 64 && !found; sensor++) {
      char label[64];
      snprintf(path, sizeof(path), "/sys/class/hwmon/%s/temp%u_label", chip->d_name, sensor);
      if (!ReadLine(path, label, sizeof(label)) || !PackageLabel(label)) continue;
      snprintf(
        temp.path,
        sizeof(temp.path),
        "/sys/class/hwmon/%s/temp%u_input",
        chip->d_name,
        sensor
      );
      found = true;
    }
    if (found) break;

    snprintf(path, sizeof(path), "/sys/class/hwmon/%s/temp1_input", chip->d_name);
    char value[32];
    if (ReadLine(path, value, sizeof(value))) {
      snprintf(temp.path, sizeof(temp.path), "%s", path);
      found = true;
    }
  }
  closedir(hwmon);
  return found;
}

static void
Tick(void)
{
  char value[32];
  i32 degrees = -1;
  if ((*temp.path || FindSensor()) && ReadLine(temp.path, value, sizeof(value))) {
    i64 millidegrees;
    if (sscanf(value, "%ld", &millidegrees) == 1) degrees = (i32)(millidegrees / 1000);
  } else {
    // the chip may come back under another hwmon number.
    temp.path[0] = '\0';
  }

  if (degrees == temp.degrees) return;
  temp.degrees = degrees;
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
  if (temp.degrees < 0) snprintf(text, sizeof(text), " --");
  else snprintf(text, sizeof(text), " %d°C", temp.degrees);
  segments[0] = SegmentLabel(text, themeRed);
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

const BarModule tempModule = {
  .name = "temp",
  .Start = Start,
  .Segments = Segments,
  .Click = Click,
  .padding = 30,
  .spacing = 4,
  .clickable = true
};
