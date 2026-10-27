// mpris: the track a media player is playing (or has paused), with previous, play/pause and
// next buttons.
// hidden when no player has a track.
//
// players are the d-bus services named org.mpris.MediaPlayer2.*. the one shown is the first
// playing one, else the first paused one, ignoring players that know neither the track's title
// nor its artist.

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dbus.h"
#include "memory.h"
#include "module.h"
#include "strbuf.h"

static constexpr char prefix[] = "org.mpris.MediaPlayer2.";
static constexpr char playerPath[] = "/org/mpris/MediaPlayer2";
static constexpr char playerInterface[] = "org.mpris.MediaPlayer2.Player";
// songs longer than this many characters are cut to two fewer, plus an ellipsis.
static constexpr u64 songLimit = 40;

enum MprisAction {
  MprisActionPrevious = 1,
  MprisActionPlayPause,
  MprisActionNext
};

typedef struct Player {
  // the service's well-known name, and its current unique name (which signals come from).
  char *name;
  StrBuf owner;
  StrBuf title;
  StrBuf artist;
  StrBuf status;
  bool canControl;
  bool canGoPrevious;
  bool canGoNext;
} Player;

typedef struct Mpris {
  // the players, in the order they appeared.
  Player **players;
  u64 count;
  u64 capacity;
} Mpris;

static Mpris mpris;

static Player *
FindByName(const char *name)
{
  for (u64 index = 0; index < mpris.count; index++) {
    if (strcmp(mpris.players[index]->name, name) == 0) return mpris.players[index];
  }
  return nullptr;
}

static Player *
FindByOwner(const char *owner)
{
  for (u64 index = 0; index < mpris.count; index++) {
    if (strcmp(StrBufText(&mpris.players[index]->owner), owner) == 0) {
      return mpris.players[index];
    }
  }
  return nullptr;
}

// the player to show, or nullptr.
static Player *
Chosen(void)
{
  Player *paused = nullptr;
  for (u64 index = 0; index < mpris.count; index++) {
    Player *player = mpris.players[index];
    if (!player->title.length && !player->artist.length) continue;

    const char *status = StrBufText(&player->status);
    if (strcmp(status, "Playing") == 0) return player;
    if (!paused && strcmp(status, "Paused") == 0) paused = player;
  }
  return paused;
}

static bool
VisitMetadata(void *context, const char *name, sd_bus_message *message)
{
  Player *player = context;
  if (strcmp(name, "xesam:title") == 0) return DbusVariantString(message, &player->title);
  if (strcmp(name, "xesam:artist") == 0) {
    return DbusVariantStrings(message, &player->artist, ", ");
  }
  return false;
}

static bool
VisitPlayer(void *context, const char *name, sd_bus_message *message)
{
  Player *player = context;
  if (strcmp(name, "PlaybackStatus") == 0) return DbusVariantString(message, &player->status);
  if (strcmp(name, "CanControl") == 0) return DbusVariantBool(message, &player->canControl);
  if (strcmp(name, "CanGoPrevious") == 0) {
    return DbusVariantBool(message, &player->canGoPrevious);
  }
  if (strcmp(name, "CanGoNext") == 0) return DbusVariantBool(message, &player->canGoNext);
  if (strcmp(name, "Metadata") == 0) {
    // new metadata replaces the old, fields it lacks included.
    StrBufClear(&player->title);
    StrBufClear(&player->artist);
    return DbusVariantProperties(message, VisitMetadata, player);
  }
  return false;
}

static int
GotAll(sd_bus_message *reply, void *context, [[maybe_unused]] sd_bus_error *error)
{
  char *name = context;
  Player *player = FindByName(name);
  free(name);
  if (!player || sd_bus_message_is_method_error(reply, nullptr)) return 0;

  DbusReadProperties(reply, VisitPlayer, player);
  BarRedraw();
  return 0;
}

static void
Fetch(Player *player)
{
  sd_bus_call_method_async(
    BarUserBus(),
    nullptr,
    player->name,
    playerPath,
    "org.freedesktop.DBus.Properties",
    "GetAll",
    GotAll,
    MemoryCopyString(player->name),
    "s",
    playerInterface
  );
}

static int
GotOwner(sd_bus_message *reply, void *context, [[maybe_unused]] sd_bus_error *error)
{
  char *name = context;
  Player *player = FindByName(name);
  free(name);
  const char *owner;
  if (!player || sd_bus_message_read_basic(reply, 's', &owner) <= 0) return 0;

  StrBufClear(&player->owner);
  StrBufAppendString(&player->owner, owner);
  return 0;
}

static void
AddPlayer(const char *name, const char *owner)
{
  if (FindByName(name)) return;

  Player *player = MemoryAllocateZeroed(1, sizeof(Player));
  player->name = MemoryCopyString(name);
  if (mpris.count == mpris.capacity) {
    mpris.capacity = mpris.capacity ? mpris.capacity * 2 : 8;
    mpris.players = MemoryResize(mpris.players, mpris.capacity, sizeof(Player *));
  }
  mpris.players[mpris.count++] = player;

  if (owner) {
    StrBufAppendString(&player->owner, owner);
  } else {
    sd_bus_call_method_async(
      BarUserBus(),
      nullptr,
      "org.freedesktop.DBus",
      "/org/freedesktop/DBus",
      "org.freedesktop.DBus",
      "GetNameOwner",
      GotOwner,
      MemoryCopyString(name),
      "s",
      name
    );
  }
  Fetch(player);
}

static void
RemovePlayer(const char *name)
{
  for (u64 index = 0; index < mpris.count; index++) {
    Player *player = mpris.players[index];
    if (strcmp(player->name, name) != 0) continue;

    free(player->name);
    StrBufFree(&player->owner);
    StrBufFree(&player->title);
    StrBufFree(&player->artist);
    StrBufFree(&player->status);
    free(player);
    memmove(
      &mpris.players[index],
      &mpris.players[index + 1],
      (mpris.count - index - 1) * sizeof(Player *)
    );
    mpris.count--;
    BarRedraw();
    return;
  }
}

static int
GotNames(
  sd_bus_message *reply,
  [[maybe_unused]] void *context,
  [[maybe_unused]] sd_bus_error *error
)
{
  if (sd_bus_message_enter_container(reply, 'a', "s") <= 0) return 0;

  const char *name;
  while (sd_bus_message_read_basic(reply, 's', &name) > 0) {
    if (strncmp(name, prefix, sizeof(prefix) - 1) == 0) AddPlayer(name, nullptr);
  }
  sd_bus_message_exit_container(reply);
  return 0;
}

// a player appeared, went away, or changed hands.
static int
OwnerChanged(
  sd_bus_message *message,
  [[maybe_unused]] void *context,
  [[maybe_unused]] sd_bus_error *error
)
{
  const char *name;
  const char *before;
  const char *after;
  if (sd_bus_message_read(message, "sss", &name, &before, &after) < 0) return 0;
  if (strncmp(name, prefix, sizeof(prefix) - 1) != 0) return 0;

  if (!*after) {
    RemovePlayer(name);
    return 0;
  }
  Player *player = FindByName(name);
  if (!player) {
    AddPlayer(name, after);
    return 0;
  }
  StrBufClear(&player->owner);
  StrBufAppendString(&player->owner, after);
  Fetch(player);
  return 0;
}

static int
PropertiesChanged(
  sd_bus_message *message,
  [[maybe_unused]] void *context,
  [[maybe_unused]] sd_bus_error *error
)
{
  Player *player = FindByOwner(sd_bus_message_get_sender(message));
  const char *interface;
  if (!player || sd_bus_message_read_basic(message, 's', &interface) < 0) return 0;
  if (strcmp(interface, playerInterface) != 0) return 0;

  DbusReadProperties(message, VisitPlayer, player);

  // properties that were only said to have changed are read again.
  bool invalidated = false;
  if (sd_bus_message_enter_container(message, 'a', "s") > 0) {
    const char *name;
    while (sd_bus_message_read_basic(message, 's', &name) > 0) invalidated = true;
    sd_bus_message_exit_container(message);
  }
  if (invalidated) Fetch(player);
  BarRedraw();
  return 0;
}

static void
Start(void)
{
  sd_bus *bus = BarUserBus();
  if (!bus) return;

  sd_bus_add_match(
    bus,
    nullptr,
    "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
    "member='NameOwnerChanged',arg0namespace='org.mpris.MediaPlayer2'",
    OwnerChanged,
    nullptr
  );
  sd_bus_match_signal(
    bus,
    nullptr,
    nullptr,
    playerPath,
    "org.freedesktop.DBus.Properties",
    "PropertiesChanged",
    PropertiesChanged,
    nullptr
  );
  sd_bus_call_method_async(
    bus,
    nullptr,
    "org.freedesktop.DBus",
    "/org/freedesktop/DBus",
    "org.freedesktop.DBus",
    "ListNames",
    GotNames,
    nullptr,
    ""
  );
}

// the number of characters in text (utf-8).
static u64
CharacterCount(const char *text)
{
  u64 count = 0;
  for (const char *at = text; *at; at++) count += (*at & 0xC0) != 0x80;
  return count;
}

// appends the first limit characters of text (utf-8) to out.
static void
AppendCharacters(StrBuf *out, const char *text, u64 limit)
{
  u64 characters = 0;
  for (const char *at = text; *at; at++) {
    bool starts = (*at & 0xC0) != 0x80;
    if (starts && characters++ == limit) return;
    StrBufAppendByte(out, *at);
  }
}

static u32
Segments([[maybe_unused]] const BarOutput *output, Segment *segments)
{
  Player *player = Chosen();
  if (!player) return 0;

  // "artist – title", or whichever of them there is.
  StrBuf song = {};
  if (player->artist.length && player->title.length) {
    StrBufAppendFormat(&song, "%s – %s", player->artist.data, player->title.data);
  } else {
    StrBufAppendString(&song, player->title.length ? player->title.data : player->artist.data);
  }

  static StrBuf shown;
  StrBufClear(&shown);
  if (CharacterCount(song.data) > songLimit) {
    AppendCharacters(&shown, song.data, songLimit - 2);
    StrBufAppendString(&shown, "…");
  } else {
    StrBufAppendString(&shown, song.data);
  }
  StrBufFree(&song);

  bool playing = strcmp(StrBufText(&player->status), "Playing") == 0;
  segments[0] = (Segment){
    .text = "\U000F04AE",
    .colour = player->canGoPrevious ? themePurple : themeMuted,
    .sizeDelta = 1,
    .action = MprisActionPrevious,
    .pointer = true
  };
  segments[1] = (Segment){
    .text = playing ? "\U000F03E4" : "\U000F040A",
    .colour = themePurple,
    .sizeDelta = 1,
    .action = MprisActionPlayPause,
    .pointer = true
  };
  segments[2] = (Segment){
    .text = "\U000F04AD",
    .colour = player->canGoNext ? themePurple : themeMuted,
    .sizeDelta = 1,
    .action = MprisActionNext,
    .pointer = true
  };
  segments[3] = SegmentLabel(StrBufText(&shown), themePurple);
  segments[3].leftPadding = 8;
  segments[3].optional = true;
  return 4;
}

static void
Click(
  [[maybe_unused]] BarOutput *output,
  [[maybe_unused]] Box pill,
  [[maybe_unused]] Box segment,
  u32 action,
  u32 button
)
{
  Player *player = Chosen();
  if (button != BTN_LEFT || !player) return;

  const char *method = action == MprisActionPrevious ? "Previous"
    : action == MprisActionNext                      ? "Next"
                                                     : "PlayPause";
  if (action == MprisActionPrevious && !player->canGoPrevious) return;
  if (action == MprisActionNext && !player->canGoNext) return;
  sd_bus_call_method_async(
    BarUserBus(),
    nullptr,
    player->name,
    playerPath,
    playerInterface,
    method,
    nullptr,
    nullptr,
    ""
  );
}

const BarModule mprisModule = {
  .name = "mpris",
  .Start = Start,
  .Segments = Segments,
  .Click = Click,
  .padding = 30,
  .spacing = 8
};
