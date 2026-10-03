# Normalized recipe validation

The commit-facing recipe was checked separately from the original completed
Linux package. Only new validation directories outside both source checkouts
and previously verified outputs were used.

Passed:

- Bash syntax checks for every shell entry point; Python compile syntax checks
  without writing bytecode into the recipe tree
- All six prepared-source Git pins, tracked cleanliness and upstream patch/header
  SHA-256 checks
- Private source preparation; patched HOE audio_output.c and FFmpeg mov.c exactly
  matched the previously verified provider inputs
- zlib CMake configuration against the private copy, confirming the external
  prepared zlib checkout remains pristine and retains zconf.h
- Actual Clang 19 rebuild/link/install of the native provider through normalized
  build-native.sh, reusing the already verified pinned static dependency prefix
- Rebuilt provider's stripped bytes matched the original validated runtime:
  b31a4b95eaad98a2730fc10613a1f1ab262ddd39ccbfddc9e006a3bbff86094a
- Generated five real media fixtures; fresh/cache-reload video/audio random seeks
  and all six video output formats passed
- All 16 upstream audio regressions and 13 upstream build-identity tests passed
- All 39 public exports and 39 Aegisub ABI entries passed; exact API 1.1.0
- No dynamic FFmpeg/dav1d/zlib dependency or build RPATH/RUNPATH leaked
- Notices and build manifest collection ran against the actual prepared inputs

Not rerun during normalization: complete zlib/dav1d/FFmpeg dependency compilation
through the consolidated build.sh. Those dependency configure flags/source pins
are unchanged from the prior successful full and clean-source rebuilds. Syntax,
source preparation, zlib configuration and the real final native rebuild were
checked; this is not described as a second full dependency-stack rebuild.

Validation logs/artifacts are intentionally outside the importable recipe tree.
No downloaded source, fixture binary, cache or generated build product belongs
in this commit directory.

Final patch-preparation hardening additionally passed in both a normal output
folder and a build directory inside an enclosing Git worktree, including
inherited GIT_DIR/GIT_WORK_TREE pointing at that enclosing repository. Git state
was isolated; all five patched-file hashes matched the known-good runtime
inputs before a marker was written. Reuse also verified those hashes, and a
negative test modifying mov.c under an existing valid marker was rejected.
Both provider verify.sh entry points unset PYTHONOPTIMIZE so inherited optimized
Python mode cannot disable assert-based checks.
