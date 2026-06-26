#!/usr/bin/env bash
# Cut a release: bump src/version.h, commit, and push the commit + a fw-v<N> tag.
# The GitHub Action (.github/workflows/release.yml), triggered by that tag, then BUILDS with secrets
# from the GH Actions vault and PUBLISHES the firmware + manifest to the droplet. This script does
# NOT build or deploy — it only tags. See tools/OTA-SETUP.md ("CI") for the required secrets + remote.
#
#   Usage:    tools/release.sh ["release notes"]         # FINAL: auto-bump = (latest final fw-v tag) + 1
#             tools/release.sh <version-int> ["notes"]   # FINAL: force a specific version
#             tools/release.sh rc ["notes"]              # RC: candidate for the next version (fw-v<N>-rc<M>)
#             tools/release.sh rc <version-int> ["notes"]# RC: candidate for a specific version
#
# An RC publishes firmware-v<N>.bin + adds it to versions.json (so you can install it on TEST devices
# via `fleet.py send <code> <N>` or Settings -> Versions) but does NOT move the fleet's version.json /
# manifest, and sends no broadcast. Promote to the whole fleet later with a FINAL cut of the same N.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
VER_FILE="src/version.h"

git rev-parse --git-dir >/dev/null 2>&1 || { echo "error: not a git repo (run: git init)"; exit 1; }
git remote get-url origin >/dev/null 2>&1 || { echo "error: no 'origin' remote. Add it, e.g.:"; echo "  git remote add origin git@github.com:<you>/planepuck.git"; exit 1; }
if ! git diff --quiet || ! git diff --cached --quiet; then
  echo "error: uncommitted changes — commit or stash them first (release only bumps the version)."; exit 1
fi

# CI rebuilds config.h from config.h.example, so every #define in config.h must also exist in the
# example (values may differ for secrets). A missing one makes the CI build fail — this bit fw-v5
# (MAX_WEATHER_CITIES). Catch it here, before tagging.
if [ -f src/config.h ]; then
  MISS=$(comm -23 <(grep -oE '^#define +[A-Za-z_0-9]+' src/config.h         | awk '{print $2}' | sort -u) \
                  <(grep -oE '^#define +[A-Za-z_0-9]+' src/config.h.example | awk '{print $2}' | sort -u))
  if [ -n "$MISS" ]; then
    echo "error: these #defines are in config.h but missing from config.h.example (CI would fail to build):"
    echo "$MISS" | sed 's/^/  /'
    echo "add them to src/config.h.example (blank any secret values), commit, and retry."; exit 1
  fi
fi

# First arg "rc" selects a release candidate (staged, not promoted to the fleet).
RC=0
if [ "${1:-}" = "rc" ]; then RC=1; shift; fi

# A numeric first arg forces the version; otherwise it's release notes and we auto-pick:
#   - the highest version among ALL tags (rc or final) is the "current" line;
#   - if it's already finalised (a plain fw-v<N> exists), start the next version (N+1);
#   - else keep N — so `rc` makes the next rc<M>, and a final PROMOTES that in-flight candidate.
if [[ "${1:-}" =~ ^[0-9]+$ ]]; then
  NEW="$1"; NOTES="${2:-}"
else
  NOTES="${1:-}"
  MAXV=$(git tag --list 'fw-v*' | sed -nE 's/^fw-v([0-9]+)(-rc[0-9]+)?$/\1/p' | sort -n | tail -1)
  if [ -z "$MAXV" ]; then NEW=1
  elif git rev-parse -q --verify "refs/tags/fw-v$MAXV" >/dev/null 2>&1; then NEW=$(( MAXV + 1 ))
  else NEW=$MAXV; fi
  echo "==> $([ "$RC" -eq 1 ] && echo 'candidate' || echo 'release') for v$NEW (highest tagged: v${MAXV:-none})"
fi

if [ "$RC" -eq 1 ]; then                          # next rc<M> for version NEW
  M=$(git tag --list "fw-v$NEW-rc*" | sed -E "s/^fw-v$NEW-rc//" | sort -n | tail -1)
  M=$(( ${M:-0} + 1 )); TAG="fw-v$NEW-rc$M"; LABEL="v$NEW rc$M"
else
  TAG="fw-v$NEW"; LABEL="v$NEW"
fi

if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null 2>&1; then
  echo "error: tag $TAG already exists"; exit 1
fi

echo "==> set FW_VERSION -> $NEW  (tag $TAG)"
sed -i.bak -E "s/^#define[[:space:]]+FW_VERSION[[:space:]]+[0-9]+/#define FW_VERSION $NEW/" "$VER_FILE"
rm -f "$VER_FILE.bak"
grep -qE "^#define FW_VERSION $NEW( |$)" "$VER_FILE" || { echo "error: failed to bump $VER_FILE to a valid integer"; git checkout -- "$VER_FILE"; exit 1; }
grep -E "^#define FW_VERSION" "$VER_FILE"

git add "$VER_FILE"
if git diff --cached --quiet; then
  # Promoting an in-flight candidate (or re-cutting an rc): version.h is unchanged. Make an EMPTY commit
  # anyway — it keeps the exact tree/bits the rc tested — so $TAG carries its OWN clean release notes
  # instead of inheriting the prior rc's commit subject (CI publishes `git log -1 --pretty=%s` into
  # version.json, which is shown on every device's update prompt).
  echo "==> version.h already at $NEW — empty release commit so $TAG carries its own notes (same bits)"
  git commit --allow-empty -m "release: firmware $LABEL${NOTES:+ — $NOTES}"
else
  git commit -m "release: firmware $LABEL${NOTES:+ — $NOTES}"
fi
git tag "$TAG"
echo "==> pushing commit + tag $TAG (the 'release' Action builds + publishes)"
git push origin HEAD
git push origin "$TAG"
echo "==> done — watch the build under the repo's Actions tab."
