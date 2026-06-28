#pragma once
// Firmware version — the single source of truth for OTA. Tracked in git (config.h is gitignored),
// so tools/release.sh can bump + commit + tag this file. Integer, monotonically increasing.
#define FW_VERSION 8    // fresh planepuck-fw line: this build is the v0 baseline; first release auto-bumps to v1

// Build id (e.g. "v19-rc2") injected at build time by tools/buildinfo.py from `git describe`, so a
// device can distinguish RC builds that share FW_VERSION. Fallback when the script didn't run.
#ifndef FW_BUILD
#define FW_BUILD "?"
#endif
