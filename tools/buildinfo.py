#!/usr/bin/env python3
"""PlatformIO pre-build script: inject FW_BUILD = the git build id, so a device can identify the
exact build on it (RCs reuse the FW_VERSION integer, so the version alone can't tell rc1 from rc2).

`git describe --tags --always --dirty` at a tagged commit yields the tag (e.g. "fw-v19-rc2"); we
strip the "fw-" prefix -> "v19-rc2". Off a tag it's "v19-rc2-3-g<sha>"; a dirty/dev tree gets
"-dirty"; no git -> short sha or "nogit". Defined as a C string macro FW_BUILD.
"""
import subprocess
Import("env")  # noqa: F821  (provided by PlatformIO/SCons)


def sh(cmd):
    try:
        return subprocess.check_output(cmd, shell=True, stderr=subprocess.DEVNULL).decode().strip()
    except Exception:
        return ""


desc = sh("git describe --tags --always --dirty") or "nogit"
if desc.startswith("fw-"):
    desc = desc[3:]                       # fw-v19-rc2 -> v19-rc2
env.Append(CPPDEFINES=[("FW_BUILD", '\\"%s\\"' % desc)])
print("FW_BUILD = %s" % desc)
