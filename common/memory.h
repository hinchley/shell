#pragma once

// checked allocation and fatal errors.
// every allocating function either succeeds or ends the program through Fatal.
// release memory with free.
#include "types.h"

// prints the message (prefixed with the program name) to stderr and exits with status 1.
[[noreturn, gnu::format(printf, 1, 2)]] void
Fatal(const char *format, ...);

// prints a warning (prefixed with the program name) to stderr.
[[gnu::format(printf, 1, 2)]] void
Warn(const char *format, ...);

// zero filled allocation of count elements.
void *
MemoryAllocateZeroed(u64 count, u64 size);

// resizes to hold count elements of size bytes.
void *
MemoryResize(void *pointer, u64 count, u64 size);

// a heap copy of a nul terminated string.
char *
MemoryCopyString(const char *string);

// a heap copy of length bytes of string, nul terminated.
char *
MemoryCopyStringLength(const char *string, u64 length);

// hands memory freed to the heap back to the system.
void
MemoryTrim(void);
