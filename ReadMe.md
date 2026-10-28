# shell

Three small Wayland programs for Hyprland, written in C23. Together they replace a
[Quickshell](https://quickshell.org) config:

- **bar**: a status bar on every output, plus the desktop wallpaper.
- **launcher**: an application launcher with search.
- **hyprshorts**: a searchable list of every keybind in the Hyprland Lua config.

They draw with cairo and pango straight onto `wlr-layer-shell` surfaces through
libwayland-client, with no toolkit. They share one colour scheme (Tokyo Night) and one font,
*IoskeleyMonoTerm Nerd Font Mono*. They only target Linux on x86-64, running Hyprland on
Arch Linux.

## Building

Install the build dependencies:

```sh
sudo pacman -S --needed clang pkgconf wayland wayland-protocols libxkbcommon cairo pango \
  systemd-libs libpulse lua librsvg
```

| Package | Used by | For |
|---|---|---|
| `wayland`, `wayland-protocols` | all | the compositor connection, and the protocol xml (`wlr-layer-shell` is vendored in `protocols/`) |
| `libxkbcommon` | all | keyboard layouts |
| `cairo`, `pango` | all | drawing and text |
| `systemd-libs` | all | the event loop (sd-event) and d-bus (sd-bus) |
| `libpulse` | bar (volume) | the default output's volume, through `pipewire-pulse` |
| `librsvg` | bar (tray) | svg tray icons |
| `lua` (5.5) | hyprshorts | running the Hyprland config to find its keybinds |

Then build:

```sh
./build.sh            # build/bar, build/launcher and build/hyprshorts, plus compile_commands.json
./build.sh --debug    # build/debug/..., unoptimised, with the address and ub sanitizers
```

Each app also has its own script (`bar/build.sh`, `launcher/build.sh`,
`hyprshorts/build.sh`), which takes `--debug` too. The code builds with `-std=c23 -Wall -Wextra
-Wpedantic -Wshadow -Wconversion -Werror`. It follows `STYLE.md` and `.clang-format` from the
editor project (clang-format 22 or later).

Install by copying the binaries somewhere on your `PATH`:

```sh
install -Dm755 build/bar build/launcher build/hyprshorts -t ~/.local/bin
```

### Choosing the bar's modules

Each pill on the bar is a module in `bar/modules/`. `bar/build.sh` compiles only the modules
listed (with the libraries they need) and generates the table that orders them. There's no
`#ifdef`, so a module that is left out is not in the binary at all.

```sh
bar/build.sh                                   # the defaults
bar/build.sh --right "cpu memory volume"       # only these, left to right, on the right
bar/build.sh --left "workspaces window" --center "clock" --no-wallpaper
```

| Section | Default modules (left to right) |
|---|---|
| left | `workspaces mpris window` |
| center | `clock` |
| right | `tray dnd battery temp cpu memory disk connectivity volume` |

## Running

### Runtime dependencies

The bar works with whatever of these is present, and hides or dims a pill when its source is
missing:

- **Hyprland**: workspaces and the window title, over its IPC sockets.
- **pipewire-pulse** (or pulseaudio): volume.
- **mako**: do not disturb.
- **upower**: battery.
- **bluez**: bluetooth.
- **iwd**: the Wi-Fi network's name.
- Applications offering tray items (the StatusNotifierItem protocol).

Clicking a pill opens a terminal UI in `foot`. These need to be installed: `btop`
(temperature, cpu, memory), `dua` (disk), `bluetui` (bluetooth), `impala` (network) and
`wiremix` (volume).

### Hyprland config

The changes to `~/.config/hypr/hyprland.lua` to switch from Quickshell:

```lua
-- autostart: the bar replaces both qs and swaybg (it draws the wallpaper too). it goes in the
-- hyprland.start handler, which runs once the compositor is up, not at the top level, which
-- runs each time the config is read.
hl.on("hyprland.start", function ()
  hl.exec_cmd("uwsm app -- bar")
end)

-- keybinds
hl.bind("SUPER + SPACE", hl.dsp.exec_cmd("uwsm-app -- launcher"), { description = "App launcher" })
hl.bind("SUPER + SHIFT + K", hl.dsp.exec_cmd("uwsm-app -- hyprshorts"), { description = "Keyboard shortcuts" })
hl.bind("SUPER + SHIFT + SPACE", hl.dsp.exec_cmd("bar toggle"), { description = "Toggle bar" })
```

The launcher and hyprshorts are toggles. Running one while it is open closes it, so the same
key opens and closes it. Opening one also closes the other, so only one overlay shows at a time.

The running bar takes two commands:

```sh
bar toggle      # hide or show the bar
bar calendar    # open or close the calendar (on the focused output)
```

## The bar

| Pill | Shows | Input |
|---|---|---|
| workspaces | workspaces 1–5: lit when they exist, underlined when focused | click: switch to it |
| mpris | the playing (or paused) track, with previous, play/pause and next | click the buttons |
| window | the focused window's title (up to 320 px wide) | |
| clock | date and time, to the minute | click: open the month calendar |
| tray | status notifier items | left click: activate; right click: the item's menu |
| dnd | mako's do-not-disturb mode (hidden on narrow outputs) | click: turn it on or off |
| battery | charge and charging state (hidden without a battery) | |
| temp | cpu package temperature | click: btop |
| cpu | cpu use over the last 2 s | click: btop |
| memory | memory in use | click: btop |
| disk | how full `/` is | click: dua |
| bluetooth | adapter on or off, and connected devices | click: bluetui |
| network | the wired interface, else the Wi-Fi network, else "offline" (name hidden on narrow outputs) | click: impala |
| connectivity | bluetooth and network in one pill (instead of listing them separately) | click either half: as its own pill |
| volume | the default output's volume | wheel: ±5%; right click: mute; left click: wiremix |

Outputs narrower than 1920 logical pixels get the compact bar.

The wallpaper is `~/.config/backgrounds/background.png`, scaled to cover each output.

Everything that can tell the bar about a change does, so the bar is event driven:

- Hyprland's event socket, for workspaces and the window title.
- D-Bus signals, for mpris, mako, upower, bluez, iwd and the tray.
- Pulseaudio's subscription, for volume.
- Netlink link events, for the wired network.

Only the readings with no change notification are polled: cpu, memory and temperature every
2 s, and disk every 30 s.

### Writing a module

A module is one file, `bar/modules/<name>.c`, defining a `const BarModule <name>Module` (see
`bar/module.h`):

- `Start` subscribes to its source and calls `BarRedraw()` when it changes.
- `Segments` describes the pill's content: runs of text, or icons.
- `Click`, `Scroll` and `Hide` are optional.

The bar does all the layout, drawing and hit testing. `bar/modules/cpu.c` is the simplest
example.

## The launcher

The launcher lists the desktop entries in `$XDG_DATA_HOME/applications` and each
`$XDG_DATA_DIRS/applications`. It skips hidden entries, entries not shown in menus, and any
that the blacklist matches.

- **Searching:** each word must match the name, command, keywords, categories or comment.
  Matches in the name rank highest.
- **Keys:**
  - Enter or a double click runs the selected entry.
  - Up/Down or Ctrl+P/Ctrl+N move the selection; Page Up/Page Down move a page.
  - Escape, Ctrl+C or a click outside the panel closes it.
- **Running:** entries with `Terminal=true` run in `foot`.

The blacklist and an optional whitelist live in `~/.config/launcher/config.ini`
(`launcher/config.ini.example` documents the format). Without that file, the launcher uses the
built-in blacklist from the old Quickshell config. `launcher --list [query]` prints what would
be listed, with the command each entry would run.

## hyprshorts

hyprshorts runs `~/.config/hypr/hyprland.lua` with an embedded Lua 5.5, against a stub `hl`
table. The stub records every `hl.bind()` and accepts everything else, so binds made in loops
or helper functions are listed just as Hyprland makes them.

Each bind shows its keys, made readable, next to its description:

- `code:10` becomes `1`.
- `XF86AudioMute` becomes `Audio Mute`.
- `mouse:272` becomes `Mouse Left`.

Search matches the keys and the description. `hyprshorts --list [query] [config]` prints the
list.

## Layout

```
common/      shared by all three apps
  wayland.c    globals, outputs, layer/popup/sub surfaces, shm buffers, the event loop
  input.c      pointer, keyboard (xkb, key repeat), cursor shapes, clipboard paste
  draw.c       rounded rectangles and text, drawn as Qt draws them
  picker.c     the search overlay used by the launcher and hyprshorts
  match.c      query normalising, scoring and ranking
  theme.h      the colours and font
  process.c, instance.c, memory.c, strbuf.c
bar/         main.c (layout, drawing, input), module.h, calendar.c, wallpaper.c,
             hyprland.c (ipc), json.c, dbus.c, pulse.c (libpulse on sd-event),
             modules/ (one file per pill), tray/ (icon themes, dbusmenu popups)
launcher/    main.c, config.ini.example
hyprshorts/  main.c
protocols/   wlr-layer-shell-unstable-v1.xml
```

## Differences from the Quickshell config

- **Network:** the pill no longer depends on interface names passed to a script, so it can't
  get stuck on "offline". Interfaces are found in `/sys/class/net`, and the Wi-Fi name comes
  from iwd over D-Bus.
- **Tray menus:** these are drawn in the bar's own style (like the calendar), not as Qt
  menus.
- **Tray order:** items appear in the order they registered.
- **Terminal entries:** launcher entries marked `Terminal=true` run in `foot`.
- **Memory:** the bar uses about 6 MB of its own memory and about 0% CPU when idle. Its RSS is
  about 29 MB, most of it shared library pages. The overlays use about 24 MB while open.
