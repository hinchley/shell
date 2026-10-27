#include "dbus.h"

#include <stdio.h>
#include <string.h>

bool
DbusReadProperties(sd_bus_message *message, DbusVisit visit, void *context)
{
  if (sd_bus_message_enter_container(message, 'a', "{sv}") <= 0) return false;

  while (sd_bus_message_enter_container(message, 'e', "sv") > 0) {
    const char *name;
    if (sd_bus_message_read_basic(message, 's', &name) < 0) break;
    if (!visit(context, name, message)) sd_bus_message_skip(message, "v");
    sd_bus_message_exit_container(message);
  }
  sd_bus_message_exit_container(message);
  return true;
}

// enters the variant at the message's position when its type is one of types (a list of one
// character signatures), returning the type, or skips it and returns 0.
static char
EnterVariant(sd_bus_message *message, const char *types)
{
  char type;
  const char *contents;
  if (sd_bus_message_peek_type(message, &type, &contents) <= 0 || type != 'v') return 0;
  if (strlen(contents) != 1 || !strchr(types, contents[0])) {
    sd_bus_message_skip(message, "v");
    return 0;
  }
  if (sd_bus_message_enter_container(message, 'v', contents) <= 0) return 0;
  return contents[0];
}

bool
DbusVariantBool(sd_bus_message *message, bool *value)
{
  if (!EnterVariant(message, "b")) return false;

  int flag = 0;
  sd_bus_message_read_basic(message, 'b', &flag);
  *value = flag;
  sd_bus_message_exit_container(message);
  return true;
}

bool
DbusVariantU32(sd_bus_message *message, u32 *value)
{
  if (!EnterVariant(message, "u")) return false;

  sd_bus_message_read_basic(message, 'u', value);
  sd_bus_message_exit_container(message);
  return true;
}

bool
DbusVariantI64(sd_bus_message *message, i64 *value)
{
  char type = EnterVariant(message, "xtiuqny");
  if (!type) return false;

  // each integer type, read at its own width (by its d-bus type character).
  union {
    i64 signed64;
    u64 unsigned64;
    i32 signed32;
    u32 unsigned32;
    u16 unsigned16;
    i16 signed16;
    u8 byte;
  } number = {};
  sd_bus_message_read_basic(message, type, &number);
  switch (type) {
    case 'x':
      *value = number.signed64;
      break;
    case 't':
      *value = (i64)number.unsigned64;
      break;
    case 'i':
      *value = number.signed32;
      break;
    case 'u':
      *value = number.unsigned32;
      break;
    case 'q':
      *value = number.unsigned16;
      break;
    case 'n':
      *value = number.signed16;
      break;
    default:
      *value = number.byte;
      break;
  }
  sd_bus_message_exit_container(message);
  return true;
}

bool
DbusVariantDouble(sd_bus_message *message, f64 *value)
{
  if (!EnterVariant(message, "d")) return false;

  sd_bus_message_read_basic(message, 'd', value);
  sd_bus_message_exit_container(message);
  return true;
}

bool
DbusVariantString(sd_bus_message *message, StrBuf *out)
{
  StrBufClear(out);
  char type = EnterVariant(message, "so");
  if (!type) return false;

  const char *text = "";
  sd_bus_message_read_basic(message, type, &text);
  StrBufAppendString(out, text);
  sd_bus_message_exit_container(message);
  return true;
}

bool
DbusVariantStrings(sd_bus_message *message, StrBuf *out, const char *separator)
{
  StrBufClear(out);
  char type;
  const char *contents;
  if (sd_bus_message_peek_type(message, &type, &contents) <= 0 || type != 'v') return false;
  if (strcmp(contents, "as") != 0) {
    sd_bus_message_skip(message, "v");
    return false;
  }

  sd_bus_message_enter_container(message, 'v', "as");
  sd_bus_message_enter_container(message, 'a', "s");
  const char *text;
  while (sd_bus_message_read_basic(message, 's', &text) > 0) {
    if (out->length) StrBufAppendString(out, separator);
    StrBufAppendString(out, text);
  }
  sd_bus_message_exit_container(message);
  sd_bus_message_exit_container(message);
  return true;
}

bool
DbusVariantProperties(sd_bus_message *message, DbusVisit visit, void *context)
{
  char type;
  const char *contents;
  if (sd_bus_message_peek_type(message, &type, &contents) <= 0 || type != 'v') return false;
  if (strcmp(contents, "a{sv}") != 0) {
    sd_bus_message_skip(message, "v");
    return false;
  }

  sd_bus_message_enter_container(message, 'v', "a{sv}");
  DbusReadProperties(message, visit, context);
  sd_bus_message_exit_container(message);
  return true;
}

sd_bus_message *
DbusMethod(
  sd_bus *bus,
  const char *destination,
  const char *path,
  const char *interface,
  const char *member
)
{
  sd_bus_message *call;
  if (!bus ||
    sd_bus_message_new_method_call(bus, &call, destination, path, interface, member) < 0) {
    return nullptr;
  }
  sd_bus_message_set_auto_start(call, false);
  return call;
}

void
DbusSend(sd_bus *bus, sd_bus_message *call, sd_bus_message_handler_t callback, void *context)
{
  if (!call) return;

  sd_bus_call_async(bus, nullptr, call, callback, context, 0);
  sd_bus_message_unref(call);
}

void
DbusWatchName(sd_bus *bus, const char *name, sd_bus_message_handler_t handler)
{
  char rule[512];
  snprintf(
    rule,
    sizeof(rule),
    "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
    "member='NameOwnerChanged',arg0='%s'",
    name
  );
  sd_bus_add_match(bus, nullptr, rule, handler, nullptr);
}

void
DbusWatchSender(sd_bus *bus, const char *sender, sd_bus_message_handler_t handler)
{
  char rule[512];
  snprintf(rule, sizeof(rule), "type='signal',sender='%s'", sender);
  sd_bus_add_match(bus, nullptr, rule, handler, nullptr);
}
