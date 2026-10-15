#!/usr/bin/env bash
# Package the Windows cross-built bundle dir as a zip.
# Usage: package-windows.sh [RESULT_LINK] [OUT_DIR]
# Requires: zip, unzip
set -euo pipefail

RESULT="${1:-result}"
OUT_DIR="${2:-pkg}"
DST="logosctl-x86_64-windows"

src=$(readlink -f "$RESULT")
staged=$(find -L "$src" -type f | wc -l | tr -d ' ')
[ "$staged" -gt 0 ] || { echo "Bundle is empty" >&2; exit 1; }

# -L dereferences: the bundle's DLLs are symlinks into the nix store.
rm -rf "$DST" && mkdir "$DST"
cp -rL "$src"/. "$DST/"
chmod -R u+w "$DST"

mkdir -p "$OUT_DIR"
zip -qr "$OUT_DIR/$DST.zip" "$DST"

zipped=$(unzip -l "$OUT_DIR/$DST.zip" | tail -1 | awk '{print $2}')
echo "bundle $staged file(s) -> zip $zipped entr(ies)"
[ "$zipped" -ge "$staged" ] || { echo "Zip is missing files (undereferenced symlinks?)" >&2; exit 1; }

n_exe=$(find "$DST" -name '*.exe' | wc -l | tr -d ' ')
[ "$n_exe" -gt 0 ] || { echo "No .exe in bundle" >&2; exit 1; }
echo "exe(s): $n_exe"