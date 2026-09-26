# Lua Workspace S1 source round-trip E2E

This executable exercises the production Lua source utilities and the ASS read/write boundary with the bundled LuaJIT. It is an S1 integration E2E, **not** completion of the Lua Workspace GUI, Run/Debug, or editor integration planned for S2 and later stages.

The harness accepts a repository-relative fixture directory and an explicit artifact directory (which may be absolute). CTest invokes it from the repository root with a finite process timeout. Each executable fixture is evaluated four times against its own hand-written `.expected` business result: original source, formatted source, serialized source, and serialized source after a physical ASS save/reopen. JIT compilation is disabled for each execution, and an instruction-count hook bounds interpreted Lua execution. The `validate_only` fixture deliberately contains an error call and an unbounded loop; it is compiled, formatted, and serialized but never executed. Invalid escape and binary-chunk fixtures must fail validation and conversion without changing their source buffers.

Each case retains source variants, actual and expected results, diagnostics where applicable, the input and output ASS files, and `status.txt`. The top-level `manifest.txt` records all cases and remains available after a failure. `classifier` writes several effect lines to ASS, reopens them, runs the repository's `kara-templater.lua` parser with minimal Automation host stubs, and compares its parsed code entries to the C++ classifier. The comment text in lexical fixtures is synthetic tokenizer input, including delimiter-shaped bytes; it is not an issue citation or project documentation.

From the repository root, with the configured `build-dir` and `RelWithDebInfo` configuration:

```powershell
cmake --build build-dir --config RelWithDebInfo --target lua-workspace-source-e2e --parallel
ctest --test-dir build-dir -C RelWithDebInfo -R '^lua_workspace_source_roundtrip$' --output-on-failure
```
