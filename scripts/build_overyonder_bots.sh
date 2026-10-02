#!/usr/bin/env bash
# Builds over|yonder's example bots as C bots the judge can run, to spar with:
#
#   scripts/build_overyonder_bots.sh [OUT_DIR]          # default build/sparring
#   python -m export.evaluate --bot bots/rl_bot_v4 --opponent build/sparring/tactics-bot
#
# The bots come from github.com/overyonder/the-loong-game (the source of the
# Loong Game blog), pinned below. Its tactics-bot, roles-bot and first-bot are
# Nim strategies over its behaviour repertoire; `nim c` writes them out as C
# (each bot's nim.cfg sets compileOnly for the judge's wasm32 clang), which
# the toolkit then builds like any C bot. room-c is its flood-fill C bot. On
# its own ladder (under the rules before Slay the Queen) these rated 1750,
# 1762, 1569 and 1539, the C starter 1024.
#
# Needs git, the unswbc toolkit and Nim 2.2 or newer on PATH. Nim's own site
# may be unreachable; it builds from source with
#   git clone --depth 1 --branch version-2-2 https://github.com/nim-lang/Nim && (cd Nim && sh build_all.sh)
set -euo pipefail

REPO=https://github.com/overyonder/the-loong-game
COMMIT=7f8717bb9ccb2f0c52d7de9f6f28313938259406
OUT=${1:-build/sparring}

command -v nim >/dev/null || { echo "nim is not on PATH (see the comment at the top)" >&2; exit 1; }
command -v unswbc >/dev/null || { echo "the unswbc toolkit is not installed" >&2; exit 1; }

mkdir -p "$OUT"
cd "$OUT"
if [ ! -d the-loong-game ]; then
    git init -q the-loong-game
    git -C the-loong-game fetch -q --depth 1 "$REPO" "$COMMIT"
    git -C the-loong-game checkout -q FETCH_HEAD
fi
examples=the-loong-game/examples

[ -d starter-c ] || unswbc init c starter-c >/dev/null
rm -rf repertoire
cp -r "$examples/repertoire" repertoire
nimbase="$(nim dump 2>&1 | grep -m1 '/lib$')/nimbase.h"

for bot in first-bot roles-bot tactics-bot; do
    rm -rf "$bot"
    cp -r "$examples/$bot" "$bot"
    cp starter-c/helper.c starter-c/helper.h "$nimbase" "$bot/"
    nim c "$bot/strategy.nim" >"$bot.build.log" 2>&1 || { tail -20 "$bot.build.log" >&2; exit 1; }
    echo "built $OUT/$bot"
done

rm -rf room-c
cp -r "$examples/the-choice/c" room-c
cp starter-c/helper.c starter-c/helper.h room-c/
echo "built $OUT/room-c"
