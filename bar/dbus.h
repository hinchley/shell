#pragma once

// helpers for the d-bus modules: reading property dictionaries and the variants in them, and
// watching services come and go.
#include <systemd/sd-bus.h>

#include "strbuf.h"
#include "types.h"

// visits one property: message is at its value (a variant), which the visitor reads with a
// DbusVariant function and returns true, or leaves unread and returns false (it is then
// skipped).
typedef bool (*DbusVisit)(void *context, const char *name, sd_bus_message *message);

// reads the a{sv} property dictionary at the message's position, visiting each property.
// false when the message holds no such dictionary there.
bool
DbusReadProperties(sd_bus_message *message, DbusVisit visit, void *context);

// each reads the variant at the message's position when it holds the type the function reads,
// and skips it otherwise (returning false).
bool
DbusVariantBool(sd_bus_message *message, bool *value);

bool
DbusVariantU32(sd_bus_message *message, u32 *value);

bool
DbusVariantI64(sd_bus_message *message, i64 *value);

bool
DbusVariantDouble(sd_bus_message *message, f64 *value);

// a string or object path, replacing out's contents.
bool
DbusVariantString(sd_bus_message *message, StrBuf *out);

// an array of strings, joined with separator, replacing out's contents.
bool
DbusVariantStrings(sd_bus_message *message, StrBuf *out, const char *separator);

// a nested property dictionary.
bool
DbusVariantProperties(sd_bus_message *message, DbusVisit visit, void *context);

// a new method call that will not start the service if it is not running (the bar only shows
// what is there), or nullptr.
// append its arguments, then send it with DbusSend.
sd_bus_message *
DbusMethod(
  sd_bus *bus,
  const char *destination,
  const char *path,
  const char *interface,
  const char *member
);

// sends a call made by DbusMethod (and releases it); callback, if any, gets the reply.
void
DbusSend(sd_bus *bus, sd_bus_message *call, sd_bus_message_handler_t callback, void *context);

// calls handler whenever name gains or loses an owner (the service starts or stops).
void
DbusWatchName(sd_bus *bus, const char *name, sd_bus_message_handler_t handler);

// calls handler on every signal sent by sender.
void
DbusWatchSender(sd_bus *bus, const char *sender, sd_bus_message_handler_t handler);
