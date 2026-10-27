#pragma once

// a small reader for the json hyprland replies with.
// it walks the text in place without building a tree: a value is a pointer to its first
// character, and the functions below find members, step through arrays and read scalars.
#include "strbuf.h"
#include "types.h"

// steps through an array's elements.
typedef struct JsonArray {
  const char *at;
  bool started;
} JsonArray;

// an iterator over the array value.
JsonArray
JsonArrayBegin(const char *value);

// the array's next element, or nullptr after the last (or when the value is not an array).
const char *
JsonArrayNext(JsonArray *array);

// the value of key in the object value, or nullptr when it has none.
const char *
JsonMember(const char *object, const char *key);

// the number value as an integer, or fallback when it is not a number.
i64
JsonInteger(const char *value, i64 fallback);

// the string value, unescaped, replacing out's contents.
// false (leaving out empty) when the value is missing or not a string.
bool
JsonString(const char *value, StrBuf *out);
