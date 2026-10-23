#!/usr/bin/env bash
# Puts a package made by tools/release_update.py into the update repository (GitHub Pages serves it): ONE commit, force-pushed, so the repository never grows with
# the history of every release.
#   tools/publish_pages.sh SITE_DIR [REPO_URL] [BRANCH]
#   SITE_DIR   a directory that holds updates/manifest.cosup (release_update.py --out SITE_DIR/updates); its content becomes the repository's content
#   REPO_URL   default https://github.com/kawanakatomoya374-pixel/C-OS-UPDATE
#   BRANCH     default main (Settings > Pages > Deploy from a branch > main / (root), once)
# Run it yourself, from a machine that may push to that repository; the private signing key is not needed here (the files are already signed).
set -euo pipefail
SITE="${1:?usage: publish_pages.sh SITE_DIR [REPO_URL] [BRANCH]}"; URL="${2:-https://github.com/kawanakatomoya374-pixel/C-OS-UPDATE}"; BRANCH="${3:-main}"
[ -f "$SITE/updates/manifest.cosup" ] || { echo "no $SITE/updates/manifest.cosup (run tools/release_update.py --out $SITE/updates first)"; exit 1; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cp -r "$SITE"/. "$T"/; touch "$T/.nojekyll"
( cd "$T" && git init -q -b "$BRANCH" && git add -A && git -c user.name="C-OS release" -c user.email="release@invalid" commit -q -m "C-OS update package $(date -u +%Y-%m-%dT%H:%MZ)" && git push -f "$URL" "$BRANCH" )
echo "published: $URL (Pages: https://${URL#https://github.com/}" | sed 's|github.com/\([^/]*\)/|\1.github.io/|' 
