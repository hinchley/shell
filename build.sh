#!/usr/bin/env bash
# builds the bar, the launcher and hyprshorts into build/, and joins the compile commands
# each records into compile_commands.json (for clangd).
#
# usage: ./build.sh [--debug]
#   --debug  builds into build/debug, without optimisation, with debug info and the address
#            and undefined behaviour sanitizers.
#
# each app has its own build script too (bar/build.sh takes options choosing its modules).
set -euo pipefail
cd "$(dirname "$0")"

mode=release
for argument in "$@"; do
  case $argument in
    --debug) mode=debug ;;
    *) echo "usage: ./build.sh [--debug]" >&2; exit 2 ;;
  esac
done

for app in bar launcher hyprshorts; do
  "$app/build.sh" "$@"
done

{
  echo "["
  # one entry per line: each loses its comma, then all but the last get one back.
  cat build/"$mode"/*.commands.json | sed 's/},$/}/' | sed '$!s/}$/},/'
  echo "]"
} > compile_commands.json
