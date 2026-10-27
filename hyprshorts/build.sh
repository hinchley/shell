#!/usr/bin/env bash
# builds build/hyprshorts (or build/debug/hyprshorts with --debug).
# usage: hyprshorts/build.sh [--debug]
source "$(dirname "$0")/../common/build.sh"
BuildArguments "$@"

packages=(wayland-client xkbcommon cairo pangocairo libsystemd lua5.5)
sources=(
  hyprshorts/main.c
  common/draw.c
  common/input.c
  common/instance.c
  common/match.c
  common/memory.c
  common/picker.c
  common/strbuf.c
  common/wayland.c
)
BuildApp hyprshorts
