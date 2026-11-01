#!/bin/sh
# Fetch GNU Unifont's bitmap .hex (cached) and rebuild kernel/fontcjk.h.
#
#   tools/getunifont.sh              # download if needed, regenerate the header
#   UNIFONT_VERSION=17.0.05 tools/getunifont.sh
#
# The generated kernel/fontcjk.h is committed, so this is only needed when new
# Chinese strings are added to the sources.
set -e

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VER=${UNIFONT_VERSION:-18.0.01}
CACHE=${UNIFONT_CACHE:-$HOME/.cache/aptos-unifont}
HEX=$CACHE/unifont_all-$VER.hex
URL=https://ftp.gnu.org/gnu/unifont/unifont-$VER/unifont_all-$VER.hex.gz

mkdir -p "$CACHE"

if [ ! -f "$HEX" ]; then
    echo "getunifont: downloading $URL"
    python3 - "$URL" "$HEX" <<'PY'
import gzip, shutil, sys, urllib.request
url, out = sys.argv[1], sys.argv[2]
with urllib.request.urlopen(url, timeout=120) as r, gzip.open(r, "rb") as gz:
    with open(out, "wb") as f:
        shutil.copyfileobj(gz, f)
PY
fi

python3 "$ROOT/tools/mkcjkfont.py" --hex "$HEX" \
    --src "$ROOT/kernel" --src "$ROOT/apps" --out "$ROOT/kernel/fontcjk.h"
