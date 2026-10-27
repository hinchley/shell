#pragma once

// one running instance per program, found through a locked pid file in $XDG_RUNTIME_DIR.
#include <sys/types.h>

#include "types.h"

// takes the instance lock for name, and keeps it until the program ends.
// true when it was taken; false when another process holds it, with *holder set to that
// process's id (or 0 when it is unknown).
bool
InstanceLock(const char *name, pid_t *holder);

// sends signal to the running instance of name.
// false when none is running.
bool
InstanceSignal(const char *name, int signal);
