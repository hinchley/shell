#include "instance.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <unistd.h>

// the pid file's path for name.
static void
PidPath(const char *name, char *path, u64 size)
{
  const char *runtime = getenv("XDG_RUNTIME_DIR");
  snprintf(path, size, "%s/%s.pid", runtime && *runtime ? runtime : "/tmp", name);
}

// the process id written in the file, or 0.
static pid_t
ReadPid(int file)
{
  char text[32] = {};
  ssize_t count = pread(file, text, sizeof(text) - 1, 0);
  if (count <= 0) return 0;
  return (pid_t)strtol(text, nullptr, 10);
}

bool
InstanceLock(const char *name, pid_t *holder)
{
  char path[512];
  PidPath(name, path, sizeof(path));
  *holder = 0;

  // the file stays open (and so locked) for the life of the process.
  int file = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (file < 0) return true;

  if (flock(file, LOCK_EX | LOCK_NB) != 0) {
    *holder = ReadPid(file);
    close(file);
    return false;
  }

  char text[32];
  int length = snprintf(text, sizeof(text), "%d\n", getpid());
  if (ftruncate(file, 0) == 0) {
    [[maybe_unused]] ssize_t written = pwrite(file, text, (u64)length, 0);
  }
  return true;
}

bool
InstanceSignal(const char *name, int signal)
{
  char path[512];
  PidPath(name, path, sizeof(path));
  int file = open(path, O_RDONLY | O_CLOEXEC);
  if (file < 0) return false;

  // a lock that can be taken means nobody holds it, so nothing is running.
  bool running = flock(file, LOCK_SH | LOCK_NB) != 0;
  pid_t process = running ? ReadPid(file) : 0;
  close(file);
  return process > 0 && kill(process, signal) == 0;
}
