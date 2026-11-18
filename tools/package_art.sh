#!/bin/sh
# package_art.sh -- the pictures for building the port from its repository, as a release download:
#   release/PvZ_DLC_Edition_Art_<version>.zip: port_art/ (the overlay's pictures, embedded into the program by
#   recomp/tools/embed_art.py) and gamedata/wide/ (the widescreen pictures: HenryJk's PvZWidescreen art, GPL-3.0, its
#   licence inside; PvZ Touch's bushes; the port's own, made from the game's art by tools/). Unzipped at the top of the
#   repository it puts both where the build looks.
#   tools/package_art.sh "Beta 2.4"
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION="${1:?version, e.g. \"Beta 2.4\"}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
rsync -a --exclude='.DS_Store' "$ROOT/port_art" "$TMP/"
mkdir -p "$TMP/gamedata"
rsync -a --exclude='.DS_Store' --exclude='*.off' "$ROOT/gamedata/wide" "$TMP/gamedata/"
cat > "$TMP/README.txt" <<EOF
Plants vs. Zombies: DLC Edition port, $VERSION: the pictures for building it from its repository.
Unzip this at the top of the repository (it gives port_art/ and gamedata/wide/), then see the repository's README,
"Building". The pictures are PopCap's and EA's art or made from it (HenryJk's PvZWidescreen art is GPL-3.0:
gamedata/wide/HENRYJK_LICENSE.txt); they are not part of the port's GPL-3.0 code.
EOF
ZIP="$ROOT/release/PvZ_DLC_Edition_Art_$(echo "$VERSION" | tr -d ' ').zip"
rm -f "$ZIP"
(cd "$TMP" && zip -q -r -X "$ZIP" README.txt port_art gamedata)
echo "built $ZIP"
