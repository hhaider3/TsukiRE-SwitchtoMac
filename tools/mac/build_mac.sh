#!/bin/bash
# build_mac.sh - From a Tsukihime .xci/.nsp to a native Apple silicon build (installs what it needs with Homebrew).
#
#   bash tools/mac/build_mac.sh --edition japan --keys /path/to/prod.keys
#
# Options: --edition japan|global (default japan), --keys <file> (default data/prod.keys), --input <dump> (default:
# the .xci/.nsp in the repository root that matches the edition), --no-updates (ignore updates/), --jobs N.
# Updates and patches: updates/*.nsp (a name starting with _ is skipped), as on Windows and Linux.
# It never copies or prints the keys. Everything it makes stays in folders .gitignore leaves out.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
EDITION=japan; KEYS=""; INPUT=""; NO_UPDATES=0; JOBS=$(( $(sysctl -n hw.ncpu) - 2 ))
while [ $# -gt 0 ]; do
  case "$1" in
    --edition) EDITION="$2"; shift 2;;
    --keys) KEYS="$2"; shift 2;;
    --input) INPUT="$2"; shift 2;;
    --no-updates) NO_UPDATES=1; shift;;
    --jobs) JOBS="$2"; shift 2;;
    *) echo "unknown option: $1" >&2; exit 2;;
  esac
done
step() { printf '\n== %s\n' "$*"; }
fail() { echo "ERROR: $*" >&2; exit 1; }
sha_upper() { shasum -a 256 "$1" | cut -d' ' -f1 | tr a-f A-F; }

[ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ] || fail "this script is for macOS on Apple silicon"
command -v brew >/dev/null || fail "Homebrew is needed: https://brew.sh"
case "$EDITION" in
  japan)  PROGRAM=01001DC01486A000; DATA="$ROOT/data/tsuki";        GAME=tsukihime
          RECOMP_ENV=(NX_RECOMP_JT_LSR=1 NX_RECOMP_JT_MOV=1);;
  global) PROGRAM=010057A01E4D4000; DATA="$ROOT/data/tsuki_global"; GAME=tsukihime_global
          RECOMP_ENV=(NX_RECOMP_JT_LSR=1 NX_RECOMP_JT_FUERA=1 NX_RECOMP_RELR=1 NX_RECOMP_JT_MOV=1);;
  *) fail "--edition is japan or global";;
esac
FW="$ROOT/framework"; PRIV="$FW/private"; VENV="$PRIV/venv"; PY="$VENV/bin/python"
MODS="$ROOT/mods/$EDITION"; UPD="$DATA/updates"
[ -n "$KEYS" ] || KEYS="$ROOT/data/prod.keys"
[ -f "$KEYS" ] || fail "no keys at $KEYS (use --keys)"

step "Tools (Homebrew)"
brew install --quiet cmake ninja pkgconf python@3.13 dotnet spirv-tools sdl3 vulkan-headers vulkan-loader molten-vk ffmpeg opus jpeg-turbo libpng
export DOTNET_ROOT="$(brew --prefix dotnet)/libexec" PATH="$(brew --prefix dotnet)/libexec:$PATH"
mkdir -p "$PRIV"
if [ ! -x "$PY" ]; then "$(brew --prefix python@3.13)/bin/python3.13" -m venv "$VENV"; fi
iconv -f UTF-16 -t UTF-8 "$ROOT/tools/requirements.txt" | tr -d '\r' | grep -v '^#' > "$PRIV/requirements.utf8.txt"
"$PY" -m pip install -q --disable-pip-version-check -r "$PRIV/requirements.utf8.txt"

step "Shader translator (Ryubing, pinned)"
RY="$PRIV/third_party/ryubing"
if [ ! -d "$RY" ]; then
  mkdir -p "$PRIV/third_party"; DL="$(mktemp -d "$PRIV/third_party/_dl.XXXX")"
  curl -fsSL -o "$DL/r.tar.gz" https://codeberg.org/Software_arch/Ryubing/archive/a82350bb77.tar.gz
  tar xzf "$DL/r.tar.gz" -C "$DL"
  N="$(find "$DL/ryubing" -type f | wc -l | tr -d ' ')"
  [ "$N" = 4310 ] || fail "the Ryubing source has $N files, 4310 expected"
  mv "$DL/ryubing" "$RY"; rm -rf "${DL:?}"
fi
HARNESS="$PRIV/build/spirv_harness/bin/Release/net10.0/spirv_harness"
[ -x "$HARNESS" ] || (cd "$FW/tools/maxwell/spirv_harness" && dotnet build -c Release -v quiet --nologo)

step "Dump"
if [ -z "$INPUT" ]; then
  shopt -s nullglob nocaseglob
  for f in "$ROOT"/*.nsp "$ROOT"/*.xci; do
    id="$("$PY" -I "$ROOT/tools/internal/romfs_source.py" info --keys "$KEYS" --input "$f" 2>/dev/null | sed -n 's/^PROGRAMID=//p' || true)"
    if [ "$(echo "$id" | tr a-f A-F)" = "$PROGRAM" ]; then INPUT="$f"; break; fi
  done
  shopt -u nullglob nocaseglob
fi
[ -n "$INPUT" ] || fail "no .xci/.nsp of the $EDITION edition in $ROOT"
INPUT="$(cd "$(dirname "$INPUT")" && pwd)/$(basename "$INPUT")"
basename "$INPUT"
mkdir -p "$DATA"
if [ ! -f "$DATA/romfs_source.bin" ] || [ ! -f "$DATA/exefs/main" ]; then
  rm -rf "${DATA:?}/exefs" "${DATA:?}/romfs_build"
  "$PY" -I "$ROOT/tools/internal/romfs_source.py" prepare --keys "$KEYS" --input "$INPUT" --program-id "$PROGRAM" \
    --source "$DATA/romfs_source.bin" --build-romfs "$DATA/romfs_build" --exefs "$DATA/exefs"
fi
[ "$(dirname "$INPUT")" = "$ROOT" ] || echo "WARNING: the game reads the dump from the repository root at run time: put it there, or set NX_ROMFS_DUMP"

step "Updates and patches"
mkdir -p "$MODS" "$UPD"
if [ "$NO_UPDATES" = 0 ]; then
  shopt -s nullglob
  for f in "$ROOT"/updates/*.nsp; do
    b="$(basename "$f" .nsp)"; case "$b" in _*) continue;; esac
    name="$(echo "$b" | sed 's/[^A-Za-z0-9._()+-]/_/g')"
    if [ -d "$MODS/_$name" ]; then echo "$b: disabled"; continue; fi
    if [ -f "$UPD/$name/mod.json" ] && [ -f "$MODS/$name/from_update.txt" ]; then echo "$b: already processed"; continue; fi
    rm -rf "${UPD:?}/_new_${name:?}" "${UPD:?}/${name:?}" "${MODS:?}/${name:?}"
    set +e
    "$PY" -I "$ROOT/tools/internal/mods_nsp.py" mod --keys "$KEYS" --nsp "$f" --program-id "$PROGRAM" --base-dump "$INPUT" \
      --base-exefs "$DATA/exefs" --out "$UPD/_new_$name"
    code=$?
    set -e
    if [ "$code" = 3 ]; then echo "$b: not for the $EDITION edition"; rm -rf "${UPD:?}/_new_${name:?}"; continue; fi
    [ "$code" = 0 ] || fail "mods_nsp.py failed with $b (code $code)"
    mkdir -p "$MODS/$name"
    if [ -d "$UPD/_new_$name/romfs" ]; then mv "$UPD/_new_$name/romfs" "$MODS/$name/romfs"; else mkdir "$MODS/$name/romfs"; fi
    printf 'romfs of the update %s.nsp (%s edition). Rename this folder to _%s to disable it, then build again.\r\n' "$b" "$EDITION" "$name" > "$MODS/$name/from_update.txt"
    if "$PY" -I -c 'import json,sys; sys.exit(0 if "main" in json.load(open(sys.argv[1]))["exefs_different"] else 1)' "$UPD/_new_$name/mod.json"; then
      printf '%s\r\n' "$(sha_upper "$UPD/_new_$name/exefs/main")" > "$MODS/$name/requires_main.txt"
    fi
    mv "$UPD/_new_$name" "$UPD/$name"
  done
  shopt -u nullglob
fi
MAIN="$DATA/exefs/main"; LAYERS=()
shopt -s nullglob
for d in "$MODS"/*/; do
  name="$(basename "$d")"; case "$name" in _*) continue;; esac
  LAYERS+=("${d%/}")
  if [ "$NO_UPDATES" = 0 ] && [ -f "${d}requires_main.txt" ] && [ -f "$UPD/$name/exefs/main" ]; then
    if [ "$MAIN" != "$DATA/exefs/main" ] && [ "$(sha_upper "$MAIN")" != "$(sha_upper "$UPD/$name/exefs/main")" ]; then
      fail "active updates with different executables cannot be combined: rename all but one in $MODS to _<name>"
    fi
    MAIN="$UPD/$name/exefs/main"
    echo "main from the update $name"
  fi
done
shopt -u nullglob
WANT="$(sha_upper "$MAIN")"

step "Recompile"
mkdir -p "$DATA/analysis"
"$PY" -I "$FW/tools/nso2bin.py" "$MAIN" "$DATA/analysis/main.bin" >/dev/null
"$PY" -I "$FW/tools/nso2bin.py" "$DATA/exefs/sdk" "$DATA/analysis/sdk.bin" >/dev/null
printf '%s\r\n' "$WANT" > "$DATA/analysis/main_source.txt"
HAVE=""; [ -f "$DATA/gen/main_source.txt" ] && HAVE="$(tr -d '\r\n' < "$DATA/gen/main_source.txt")"
if [ "$HAVE" != "$WANT" ]; then
  rm -rf "${DATA:?}/gen"
  (cd "$FW" && env "${RECOMP_ENV[@]}" "$PY" -m recomp "$MAIN" "$DATA/gen/main" --name main --base 0x4000000000 | tail -1)
  (cd "$FW" && env "${RECOMP_ENV[@]}" "$PY" -m recomp "$DATA/exefs/sdk" "$DATA/gen/sdk" --name sdk --base 0x4010000000 | tail -1)
  printf '%s\r\n' "$WANT" > "$DATA/gen/main_source.txt"
fi

step "Font, shaders, HUD icons"
mkdir -p "$DATA/fonts" "$DATA/spirv"
[ -f "$DATA/fonts/shared_0.ttf" ] || cp "$ROOT/resources/fonts/shared_0.ttf" "$DATA/fonts/"
SARGS=(--maxwell "$FW/tools/maxwell" --romfs-shader "$DATA/romfs_build/shader" --main-bin "$DATA/analysis/main.bin" --spirv "$DATA/spirv")
for d in ${LAYERS[@]+"${LAYERS[@]}"}; do
  if [ -d "$d/romfs/shader" ]; then SARGS+=(--romfs-shader "$d/romfs/shader"); fi
done
SH="$(mktemp -d "$DATA/_shaders.XXXX")"
sum="$("$PY" -I "$ROOT/tools/internal/tsuki_shaders_offline.py" "${SARGS[@]}" --out "$SH/in" | tail -1)"; echo "$sum"
case "$sum" in
  *without_spv=0*) ;;
  *)
    "$HARNESS" --batch "$SH/in" --out "$SH/out" --jobs "$(sysctl -n hw.ncpu)" --spirv-val "$(command -v spirv-val)" | tail -1
    cp "$SH"/out/*.spv "$DATA/spirv/"
    if [ -f "$DATA/spirv/results.tsv" ]; then tail -n +2 "$SH/out/results.tsv" >> "$DATA/spirv/results.tsv"; else cp "$SH/out/results.tsv" "$DATA/spirv/results.tsv"; fi
    ;;
esac
rm -rf "${SH:?}"
sum="$("$PY" -I "$ROOT/tools/internal/tsuki_shaders_offline.py" "${SARGS[@]}" | tail -1)"; echo "$sum"
case "$sum" in *without_spv=0*) ;; *) fail "some shaders were not translated";; esac
"$PY" -I "$ROOT/tools/hud/build_hud_assets.py" --edition "$EDITION" --root "$ROOT" | tail -1 || echo "WARNING: no HUD icons by device"

step "Build"
BUILD="$FW/build/$GAME"
cmake --fresh -S "$FW" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DNX_GAME="$GAME" -DNX_GEN_DIR="$DATA/gen" \
  -DCMAKE_PREFIX_PATH="$(brew --prefix)" -DNX_OPUS_ROOT="$(brew --prefix)" | tail -1
ninja -C "$BUILD" -j "$JOBS" | tail -1

LAUNCH="$ROOT/play_$GAME.command"
printf '#!/bin/sh\ncd "$(dirname "$0")/framework/build/%s" && exec ./%s\n' "$GAME" "$GAME" > "$LAUNCH"
chmod +x "$LAUNCH"
step "Done"
echo "Game: $BUILD/$GAME"
echo "Double-click $(basename "$LAUNCH") to play. Keep the dump in the repository root."
