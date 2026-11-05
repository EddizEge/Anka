#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Profile-guided optimization links of the NX2Perf release program (BUILDING_NX2PERF.md).
# Run from Git Bash started inside a Visual Studio 2022 x64 Developer shell (link.exe, pgomgr.exe
# and ninja.exe on PATH), after `cmake --build <build> --target yuzu-cmd` with -DENABLE_LTO=ON.
# Nothing is compiled again: the same /GL objects are linked with other /LTCG options.
#
#   pgo_link.sh <build dir> instrument <name>
#       bin/<name>.exe linked with /FASTGENPROFILE:PGD=bin\<name>.pgd. Play with it (pgort140.dll
#       from the MSVC tools next to it); it writes <name>!N.pgc files next to the .pgd, or into
#       the folder in VCPROFILE_PATH.
#   pgo_link.sh <build dir> optimize <name> <instrumented .pgd> <folder with .pgc files>
#       copies the .pgd to bin/<name>.pgd, merges every .pgc into it (pgomgr /merge), and links
#       bin/<name>.exe with /USEPROFILE:PGD=bin\<name>.pgd.
set -euo pipefail
export MSYS2_ARG_CONV_EXCL='*'   # keep /options away from Git Bash's path conversion

[ $# -ge 3 ] || { sed -n '5,17p' "$0"; exit 2; }
B=$1 MODE=$2 NAME=$3
[[ "$NAME" =~ ^[A-Za-z0-9._-]+$ ]] || { echo "error: name"; exit 2; }

link_with() {   # $1 = what replaces "/LTCG " in yuzu-cmd's own link line
  local cmd
  cmd=$(ninja -C "$B" -t commands yuzu-cmd | tail -1)
  case "$cmd" in *'/out:bin\eden-cli.exe'*'/LTCG '*) ;; *) echo "error: unexpected link line"; exit 1 ;; esac
  cmd=${cmd#*link.exe }
  cmd=${cmd% && cd .\"}
  cmd=${cmd//'/out:bin\eden-cli.exe'/"/out:bin\\$NAME.exe"}
  cmd=${cmd//'/implib:src\yuzu_cmd\eden-cli.lib'/"/implib:bin\\$NAME.lib"}
  cmd=${cmd//'/pdb:bin\eden-cli.pdb'/"/pdb:bin\\$NAME.pdb"}
  cmd=${cmd//'/LTCG '/"$1 "}
  printf '%s\n' "$cmd" > "$B/$NAME.rsp"
  (cd "$B" && link.exe "@$NAME.rsp")
}

case "$MODE" in
  instrument)
    link_with "/LTCG /FASTGENPROFILE:PGD=bin\\$NAME.pgd"
    ;;
  optimize)
    [ $# -ge 5 ] || { echo "error: optimize needs <instrumented .pgd> <pgc folder>"; exit 2; }
    cp "$4" "$B/bin/$NAME.pgd"
    shopt -s nullglob
    pgcs=("$5"/*.pgc)
    [ ${#pgcs[@]} -gt 0 ] || { echo "error: no .pgc in $5"; exit 1; }
    for f in "${pgcs[@]}"; do pgomgr.exe /merge "$f" "$B/bin/$NAME.pgd"; done
    link_with "/LTCG /USEPROFILE:PGD=bin\\$NAME.pgd"
    ;;
  *) echo "error: mode is instrument or optimize"; exit 2 ;;
esac
echo "done: $B/bin/$NAME.exe"
