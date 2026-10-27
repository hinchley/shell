# shared by the apps' build scripts, which source it.
# it generates the wayland protocol code, compiles an app with the project's warnings, and
# records how each file was compiled in a fragment that the top level build.sh joins into
# compile_commands.json (for clangd).
#
# an app's script sets three arrays and calls BuildApp with the app's name:
#   packages   the pkg-config packages it links against.
#   sources    its c files, relative to the repository root.
#   linkFlags  anything else the link needs (optional).
# BuildArguments handles the options every script takes: --debug builds without
# optimisation, with debug info and the address and undefined behaviour sanitizers.

set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

mode=release
linkFlags=()

# the protocols every app uses (through common/wayland.c), and where their xml is.
waylandProtocols=$(pkg-config --variable=pkgdatadir wayland-protocols)
protocolFiles=(
  "protocols/wlr-layer-shell-unstable-v1.xml"
  "$waylandProtocols/stable/xdg-shell/xdg-shell.xml"
  "$waylandProtocols/stable/viewporter/viewporter.xml"
  "$waylandProtocols/stable/tablet/tablet-v2.xml"
  "$waylandProtocols/staging/fractional-scale/fractional-scale-v1.xml"
  "$waylandProtocols/staging/cursor-shape/cursor-shape-v1.xml"
)

common=(-std=c23 -D_GNU_SOURCE -Icommon -Ibuild/protocols)
warnings=(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wformat-nonliteral -Werror)

# handles --debug, and fails on anything else.
BuildArguments() {
  for argument in "$@"; do
    case $argument in
      --debug) mode=debug ;;
      *) echo "usage: $0 [--debug]" >&2; exit 2 ;;
    esac
  done
}

# true when target is missing or older than source.
Stale() {
  [[ ! -f $1 || $2 -nt $1 ]]
}

# generates each protocol's header and code into build/protocols, when stale.
GenerateProtocols() {
  mkdir -p build/protocols
  local file name
  for file in "${protocolFiles[@]}"; do
    name=$(basename "$file" .xml)
    if Stale "build/protocols/$name-client-protocol.h" "$file"; then
      wayland-scanner client-header "$file" "build/protocols/$name-client-protocol.h"
    fi
    if Stale "build/protocols/$name-protocol.c" "$file"; then
      wayland-scanner private-code "$file" "build/protocols/$name-protocol.c"
    fi
  done
}

# the compile flags for a package list, with pkg-config's include folders marked as system
# folders so that warnings in library headers are not ours.
PackageFlags() {
  pkg-config --cflags "$@" | sed 's/-I/-isystem /g'
}

# writes build/<mode>/<name>.commands.json: one compile_commands entry per source.
WriteCommands() {
  local name=$1
  shift
  local output="build/$mode/$name.commands.json"
  local first=1 source argument
  {
    for source in "${sources[@]}"; do
      ((first)) || printf ',\n'
      first=0
      printf '  {"directory": "%s", "file": "%s", "arguments": ["clang"' "$root" "$source"
      for argument in "$@"; do printf ', "%s"' "$argument"; done
      printf ', "-c", "%s"]}' "$source"
    done
    printf '\n'
  } > "$output"
}

# compiles and links build/<name> (or build/debug/<name>).
BuildApp() {
  local name=$1
  GenerateProtocols
  mkdir -p "build/$mode"

  local optimise
  if [[ $mode == debug ]]; then
    optimise=(-O0 -g3 -fno-omit-frame-pointer -fsanitize=address,undefined)
  else
    optimise=(-O2)
  fi

  # the generated protocol code is not ours, so it is compiled once without the warnings.
  local objects=() file object
  for file in build/protocols/*-protocol.c; do
    object="build/$mode/$(basename "$file" .c).o"
    if Stale "$object" "$file"; then
      clang "${common[@]}" "${optimise[@]}" -w -c "$file" -o "$object"
    fi
    objects+=("$object")
  done

  local packageFlags
  read -ra packageFlags <<< "$(PackageFlags "${packages[@]}")"
  local flags=("${common[@]}" "${warnings[@]}" "${packageFlags[@]}")
  WriteCommands "$name" "${flags[@]}"

  local binary="build/$name"
  [[ $mode == debug ]] && binary="build/debug/$name"
  local libraries
  read -ra libraries <<< "$(pkg-config --libs "${packages[@]}")"
  clang "${flags[@]}" "${optimise[@]}" "${sources[@]}" "${objects[@]}" \
    "${libraries[@]}" "${linkFlags[@]}" -lm -o "$binary"
  echo "built $binary"
}
