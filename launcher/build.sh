#!/usr/bin/env bash
# builds build/launcher (or build/debug/launcher with --debug).
# usage: launcher/build.sh [--debug]
source "$(dirname "$0")/../common/build.sh"
BuildArguments "$@"

packages=(wayland-client xkbcommon cairo pangocairo libsystemd)
sources=(
  launcher/main.c
  common/draw.c
  common/input.c
  common/instance.c
  common/match.c
  common/memory.c
  common/picker.c
  common/process.c
  common/strbuf.c
  common/wayland.c
)
BuildApp launcher
