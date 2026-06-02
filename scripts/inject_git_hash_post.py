"""
PlatformIO POST-build hook. Counterpart to inject_git_hash.py (the pre
script). Reads the git hash that the pre script stashed on env, then
appends `-DBLUEPAWZ_GIT_HASH="<hash>"` to PROJENV only.

`projenv` is PIO's project-sources build env (src/*.cpp). Touching it
here means only main.cpp.o picks up the macro; the framework + library
.o files' cached compile commands are unchanged, so they don't get
invalidated when the hash flips on commit. That's the whole point of
the post-script split — `projenv` simply isn't exposed to pre scripts.

Why the macro is a string literal:
   The extra quoting via StringifyMacro is required so the C
   preprocessor sees a string literal, not a bare identifier — e.g.
       -DBLUEPAWZ_GIT_HASH="abc1234"
   so `const char *h = BLUEPAWZ_GIT_HASH;` compiles cleanly.
"""

Import("env", "projenv")  # noqa: F821

git_hash = env.get("BLUEPAWZ_GIT_HASH", "unknown")  # noqa: F821
projenv.Append(  # noqa: F821
    CPPDEFINES=[("BLUEPAWZ_GIT_HASH", projenv.StringifyMacro(git_hash))]  # noqa: F821
)
print(f"[inject_git_hash_post] applied BLUEPAWZ_GIT_HASH={git_hash} to projenv")
