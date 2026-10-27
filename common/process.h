#pragma once

// starting other programs.
#include "types.h"

// starts argv[0] (searched for in PATH) with the rest of argv, in its own session, with
// default signal handling, and in directory (or ours, when it is nullptr or empty).
// the process id, or 0 when it could not start.
i32
ProcessSpawn(const char *const argv[], const char *directory);

// runs program in a foot terminal whose app id is "org.hinchley." followed by appId, unless
// the terminal started earlier for appId is still open, in which case it does nothing.
// program is a nullptr terminated argument list.
void
ProcessTui(const char *appId, const char *const program[]);
