#!/bin/sh
# export_github_repo.sh -- refresh github_repo/ (published as the port's GitHub repository) from the working folders.
#
# github_repo/.git (the published history) always stays: only the other top-level entries are deleted and written
# again. What goes in (the port's own code and its scripts, nothing made from the game's exe or its files):
#   icons/    the app icons and the Steam library cover
#   recomp/   Makefile, host/ (not port_art.c: the pictures as C arrays), runtime/, nx/icon.jpg,
#             tools/x86recomp (the recompiler) and embed_art.py, third_party/ stb_image.h, stb_vorbis/ and
#             libopenmpt_sources.txt (libopenmpt itself is downloaded: README, "Building")
#   android/  AndroidManifest.xml, java/ (never the signing key or its password)
#   tools/    the build, package and picture scripts (this one too)
#   release/  the players' READMEs and stage/licenses, which the package scripts put in the zips
#   + tools/github/: README.md, THIRD_PARTY_NOTICES.md, LICENSE (GPL-3.0), gitignore (as .gitignore), docs/ (the
#     README's screenshots)
# Never: gen/, analysis/, gamedata/, port_art/ (the pictures go in the art download: tools/package_art.sh),
# NOTES.md, builds, SD_CARD, saves, logs.
# Ends with a scan: no home path, user name or e-mail address anywhere.
#
# usage: tools/export_github_repo.sh
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/github_repo"
R="$ROOT/recomp"
mkdir -p "$OUT"
find "$OUT" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +

mkdir -p "$OUT/recomp/nx" "$OUT/recomp/tools" "$OUT/recomp/third_party"
cp -p "$R/Makefile" "$OUT/recomp/"
rsync -a --exclude='.DS_Store' --exclude='port_art.c' "$R/host" "$OUT/recomp/"
rsync -a --exclude='.DS_Store' "$R/runtime" "$OUT/recomp/"
cp -p "$R/nx/icon.jpg" "$OUT/recomp/nx/"
cp -p "$R/tools/embed_art.py" "$OUT/recomp/tools/"
rsync -a --exclude='.DS_Store' --exclude='__pycache__' "$R/tools/x86recomp" "$OUT/recomp/tools/"
cp -p "$R/third_party/stb_image.h" "$R/third_party/libopenmpt_sources.txt" "$OUT/recomp/third_party/"
rsync -a --exclude='.DS_Store' "$R/third_party/stb_vorbis" "$OUT/recomp/third_party/"

mkdir -p "$OUT/android"
cp -p "$ROOT/android/AndroidManifest.xml" "$OUT/android/"
rsync -a --exclude='.DS_Store' "$ROOT/android/java" "$OUT/android/"

mkdir -p "$OUT/tools"
cp -p "$ROOT"/tools/*.sh "$ROOT"/tools/*.py "$OUT/tools/"
# (not the analysis tools: recover_dlc.py rebuilds the DLC's files from its patches without the game, dlc_save.py
# writes a finished save; neither is needed to build)
rm -f "$OUT/tools/recover_dlc.py" "$OUT/tools/dlc_save.py"
# the Linux build's Docker image (tools/build_linux.sh makes it from this)
mkdir -p "$OUT/tools/linux"
cp -p "$ROOT/tools/linux/Dockerfile" "$OUT/tools/linux/"

# the icons (the user's art): the Android launcher (mobile.png), the Mac / Windows icon (pc_icon.png), the Switch's
# source (switch.png -> recomp/nx/icon.jpg), the Steam library cover (pc_cover.png)
rsync -a --exclude='.DS_Store' "$ROOT/icons" "$OUT/"

mkdir -p "$OUT/release/stage"
cp -p "$ROOT"/release/README.txt "$ROOT"/release/README_mac.txt "$ROOT"/release/README_windows.txt \
      "$ROOT"/release/README_android.txt "$ROOT"/release/README_linux.txt "$OUT/release/"
rsync -a --exclude='.DS_Store' "$ROOT/release/stage/licenses" "$OUT/release/stage/"

G="$ROOT/tools/github"
cp -p "$G/README.md" "$G/THIRD_PARTY_NOTICES.md" "$G/LICENSE" "$OUT/"
cp -p "$G/gitignore" "$OUT/.gitignore"
rsync -a --exclude='.DS_Store' "$G/docs" "$OUT/"

# never: the signing key, the recompiled code, the game's files
for f in android/release.keystore android/keystore.pass recomp/gen recomp/host/port_art.c analysis gamedata port_art; do
  if [ -e "$OUT/$f" ]; then echo "export: $f must not be in the repository" >&2; exit 1; fi
done
# nothing personal: the home path, the account name, the e-mail address
user=$(basename "$HOME")
email=$(git config --global user.email 2>/dev/null || true)
name=$(git config --global user.name 2>/dev/null || true)
if LC_ALL=C grep -rliI --exclude-dir=.git -e "$HOME" -e "$user" ${email:+-e "$email"} ${name:+-e "$name"} "$OUT"; then
  echo "export: personal data found in the files above" >&2
  exit 1
fi
echo "exported: $OUT ($(find "$OUT" -type f ! -path '*/.git/*' | wc -l | tr -d ' ') files)"
