"""
PlatformIO PRE-build hook. Three responsibilities:

  1. Resolve the current git commit hash (and dirty flag).
  2. Write it to the GIT-IGNORED data/build_info.json so the LittleFS image
     carries it WITHOUT dirtying any tracked file (see the long note below).
  3. Prune RadioLib's source list so PIO doesn't compile every radio
     driver + digital-mode encoder in the library every build.

The BLUEPAWZ_GIT_HASH macro itself is added to the compile command in the
POST-build hook (scripts/inject_git_hash_post.py), because `projenv`
(needed to scope the macro to src/*.cpp only) is exposed only to post
scripts. We stash the hash on the env here so the post script can read it
without re-running `git rev-parse`.

Why per-file scoping for the macro matters:
   The old behaviour was `env.Append(CPPDEFINES=...)` against the GLOBAL
   env, which embedded the current git hash in every framework and
   library .o file's recorded compile command. Every commit flipped the
   hash, every flip invalidated every cached .o, every "press upload"
   became a 15-20 minute full rebuild — even though no framework or
   library source had changed. Scoping the macro to projenv (project
   sources only) keeps framework/library caches warm across commits.

Why the RadioLib filter matters:
   RADIOLIB_EXCLUDE_* macros in platformio.ini make those modules'
   implementations EMPTY at preprocessing time, but PIO still walks every
   .cpp in the library and compiles each one to a near-empty .o file.
   That's why builds were showing dozens of `Compiling RadioLib/.../
   CC1101.cpp.o`, `LR11x0/...`, `APRS.cpp.o`, etc. despite the excludes.

   Approach: physically rename the unused .cpp files to .cpp.skip in the
   installed RadioLib copy under .pio/libdeps/. PIO compiles only *.cpp;
   the .cpp.skip files are invisible to it. This is blunter than fiddling
   with SCons SRC_FILTER (which we tried and it broke the lib env's
   compiler config -- 'CC' is not recognized errors), but completely
   bulletproof: the unused TUs simply do not exist as far as PIO is
   concerned. Idempotent and reversible (rename back to .cpp if anything
   needs them). RadioLib reinstall undoes the rename, in which case this
   script re-applies it on the next build. The RADIOLIB_EXCLUDE_* flags
   remain in platformio.ini as a belt-and-braces fallback in case any
   neighbouring .cpp tries to reference symbols from a skipped file.
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

# Stash for the post-script so it doesn't re-run git rev-parse.
env["BLUEPAWZ_GIT_HASH"] = git_hash  # noqa: F821

# V3.6.3: write the FS git hash to data/build_info.json, NOT version.json.
#
# build_info.json is GIT-IGNORED (see .gitignore). It still gets packed into
# the LittleFS image (PIO images the whole data/ dir), so /build_info.json is
# readable at runtime by the /version handler. Because it's never committed,
# regenerating it every build no longer leaves a tracked file "dirty after
# every build", which was the perpetual re-commit loop we're killing here.
#
# (Previously we stamped fs_git_hash INTO the tracked data/version.json. A
# tracked file can never hold its own commit's hash — the hash isn't known
# until after the commit — so every build re-dirtied it and demanded another
# commit, forever. version.json is now hand-edited ONLY when bumping the
# semver, in lockstep with include/version.h.)
build_info = PROJECT_DIR / "data" / "build_info.json"
build_info.write_text(
    json.dumps({"fs_git_hash": git_hash}, indent=2) + "\n", encoding="utf-8"
)
print(f"[inject_git_hash] wrote fs_git_hash={git_hash} -> {build_info} (git-ignored)")


# Rename unused RadioLib .cpp files to .cpp.skip so PIO doesn't see them.
# Keep: modules/SX126x/ (our SX1262 family) + base infrastructure
# (Hal, Module, hal/Arduino, utils, protocols/PhysicalLayer, Print).
# Skip: all other radio drivers + all digital-mode encoders +
#       HAL implementations for non-Arduino platforms.
# Verified against RadioLib 7.x layout in .pio/libdeps/.
RADIOLIB_SKIP_DIRS = [
    # Other radio driver families (we use SX126x only)
    "modules/CC1101", "modules/LLCC68", "modules/LR11x0", "modules/LR2021",
    "modules/RF69", "modules/SX123x", "modules/SX127x", "modules/SX128x",
    "modules/Si443x", "modules/nRF24",
    # HAL implementations for other platforms (we use hal/Arduino)
    "hal/RPiPico", "hal/Stm32duino",
    # Digital-mode encoders we never use
    "protocols/ADSB", "protocols/AX25", "protocols/APRS", "protocols/BellModem",
    "protocols/FSK4", "protocols/Hellschreiber", "protocols/LoRaWAN",
    "protocols/Morse", "protocols/Pager", "protocols/RTTY", "protocols/SSTV",
    "protocols/ExternalRadio",
]

pioenv = env["PIOENV"]  # noqa: F821  e.g. heltec_wireless_tracker_v2
radiolib_src = PROJECT_DIR / ".pio" / "libdeps" / pioenv / "RadioLib" / "src"

renamed_count = 0
if radiolib_src.is_dir():
    for sub in RADIOLIB_SKIP_DIRS:
        sub_path = radiolib_src / sub
        if not sub_path.is_dir():
            continue
        for cpp in sub_path.rglob("*.cpp"):
            cpp.rename(cpp.with_suffix(".cpp.skip"))
            renamed_count += 1
    if renamed_count:
        print(f"[inject_git_hash] RadioLib pruned: renamed {renamed_count} unused .cpp -> .cpp.skip")
    else:
        print("[inject_git_hash] RadioLib already pruned (nothing to rename)")
else:
    # Lib not installed yet; PIO will fetch on first build, this script
    # runs again next time and applies the rename. Non-fatal.
    print(f"[inject_git_hash] RadioLib not installed yet at {radiolib_src} -- skip applied on next build")
