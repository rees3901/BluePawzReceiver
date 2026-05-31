"""
PlatformIO pre-build hook: bake the current git commit hash into the
firmware AND into the LittleFS image.

Why both?
  - The firmware binary gets `-DBLUEPAWZ_GIT_HASH="abc1234"` so code can
    reference it as a string literal at compile time (no FS read needed
    to print it on the TFT or in /version).
  - The web UI is served from LittleFS and reads /version.json on load.
    We rewrite data/version.json each build so its `fs_git_hash` matches
    the firmware's `BLUEPAWZ_GIT_HASH`. The web UI surfaces both
    alongside the existing fw/fs semver strings so you can immediately
    tell if firmware and filesystem were built from the same commit
    (and whether either has been "edited locally" since that commit).

Behaviour:
  - Runs `git rev-parse --short=7 HEAD` from the project root.
  - If the working tree has uncommitted changes, suffixes "-dirty".
  - If git isn't available or this isn't a git checkout, falls back to
    "unknown" — build still succeeds.
  - Idempotent: only rewrites version.json if the hash actually changed,
    so we don't churn the file's mtime on every build.
"""

import json
import subprocess
from pathlib import Path

Import("env")  # noqa: F821  (provided by PlatformIO's SCons env)

PROJECT_DIR = Path(env["PROJECT_DIR"])  # noqa: F821


def _git(*args: str) -> str:
    return subprocess.check_output(
        ["git", *args], cwd=str(PROJECT_DIR), stderr=subprocess.DEVNULL
    ).decode().strip()


def get_git_hash() -> str:
    try:
        short = _git("rev-parse", "--short=7", "HEAD")
        # `git status --porcelain` is empty iff the working tree is clean.
        dirty = bool(_git("status", "--porcelain"))
        return f"{short}-dirty" if dirty else short
    except Exception:
        return "unknown"


git_hash = get_git_hash()
print(f"[inject_git_hash] BLUEPAWZ_GIT_HASH = {git_hash}")

# 1. Inject into firmware as a string-literal macro.
#    The extra quoting (\"...\") is required so the C preprocessor sees
#    a string literal, not a bare identifier.
env.Append(CPPDEFINES=[("BLUEPAWZ_GIT_HASH", env.StringifyMacro(git_hash))])  # noqa: F821

# 2. Stamp data/version.json so the FS image carries the same hash.
version_json = PROJECT_DIR / "data" / "version.json"
if version_json.exists():
    try:
        current = json.loads(version_json.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        current = {}
    if current.get("fs_git_hash") != git_hash:
        current["fs_git_hash"] = git_hash
        # Preserve human-friendly key ordering by writing fs_version first,
        # then fs_git_hash, then everything else.
        ordered = {}
        for key in ("fs_version", "fs_git_hash"):
            if key in current:
                ordered[key] = current[key]
        for key, value in current.items():
            if key not in ordered:
                ordered[key] = value
        version_json.write_text(
            json.dumps(ordered, indent=2) + "\n", encoding="utf-8"
        )
        print(f"[inject_git_hash] wrote fs_git_hash={git_hash} → {version_json}")
