#include "process.h"

#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <systemd/sd-event.h>
#include <unistd.h>

#include "memory.h"
#include "wayland.h"

extern char **environ;

// a terminal started by ProcessTui that is still open.
typedef struct Tui {
  char appId[32];
  i32 process;
} Tui;

static constexpr u32 tuiCapacity = 16;
static Tui tuis[tuiCapacity];

i32
ProcessSpawn(const char *const argv[], const char *directory)
{
  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);

  // children start with nothing blocked and every signal at its default.
  sigset_t none;
  sigset_t all;
  sigemptyset(&none);
  sigfillset(&all);
  posix_spawnattr_setsigmask(&attributes, &none);
  posix_spawnattr_setsigdefault(&attributes, &all);
  posix_spawnattr_setflags(
    &attributes,
    POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF
  );

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  if (directory && *directory) posix_spawn_file_actions_addchdir_np(&actions, directory);

  pid_t process = 0;
  int error =
    posix_spawnp(&process, argv[0], &actions, &attributes, (char *const *)argv, environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
  if (error != 0) {
    Warn("cannot run %s: %s", argv[0], strerror(error));
    return 0;
  }
  return process;
}

// forgets a terminal once it closes.
static int
TuiExited(sd_event_source *source, [[maybe_unused]] const siginfo_t *info, void *context)
{
  Tui *tui = context;
  tui->process = 0;
  sd_event_source_disable_unref(source);
  return 0;
}

void
ProcessTui(const char *appId, const char *const program[])
{
  Tui *slot = nullptr;
  for (u32 index = 0; index < tuiCapacity; index++) {
    Tui *tui = &tuis[index];
    if (tui->process && strcmp(tui->appId, appId) == 0) return;
    if (!tui->process && !slot) slot = tui;
  }
  if (!slot) return;

  char appIdOption[64];
  snprintf(appIdOption, sizeof(appIdOption), "--app-id=org.hinchley.%s", appId);
  const char *argv[16] = {"foot", appIdOption, "-e"};
  u32 count = 3;
  for (u32 index = 0; program[index] && count < 15; index++) argv[count++] = program[index];
  argv[count] = nullptr;

  // sd-event watches children through SIGCHLD, which must be blocked for it.
  sigset_t childSignal;
  sigemptyset(&childSignal);
  sigaddset(&childSignal, SIGCHLD);
  sigprocmask(SIG_BLOCK, &childSignal, nullptr);

  i32 process = ProcessSpawn(argv, nullptr);
  if (!process) return;

  snprintf(slot->appId, sizeof(slot->appId), "%s", appId);
  slot->process = process;
  sd_event_add_child(wayland.loop, nullptr, process, WEXITED, TuiExited, slot);
}
