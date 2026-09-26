# Lua Workspace S1 source round-trip E2E

This executable exercises the production Lua source utilities and the ASS read/write boundary with the bundled LuaJIT. It is an S1 integration E2E, **not** completion of the Lua Workspace GUI, Run/Debug, or editor integration planned for S2 and later stages.

The harness accepts a repository-relative fixture directory and an explicit artifact directory (which may be absolute). CTest invokes it from the repository root with a finite process timeout. Each executable fixture is evaluated four times against its own hand-written `.expected` business result: original source, formatted source, serialized source, and serialized source after a physical ASS save/reopen. JIT compilation is disabled for each execution, and an instruction-count hook bounds interpreted Lua execution. The `validate_only` fixture deliberately contains an error call and an unbounded loop; it is compiled, formatted, and serialized but never executed. Invalid escape and binary-chunk fixtures must fail validation and conversion without changing their source buffers.

Each case retains source variants, actual and expected results, diagnostics where applicable, the input and output ASS files, and `status.txt`. The top-level `manifest.txt` records all cases and remains available after a failure. `classifier` writes several effect lines to ASS, reopens them, runs the repository's `kara-templater.lua` parser with minimal Automation host stubs, and compares its parsed code entries to the C++ classifier. The comment text in lexical fixtures is synthetic tokenizer input, including delimiter-shaped bytes; it is not an issue citation or project documentation.

From the repository root, with the configured `build-dir` and `RelWithDebInfo` configuration:

```powershell
cmake --build build-dir --config RelWithDebInfo --target lua-workspace-source-e2e --parallel
ctest --test-dir build-dir -C RelWithDebInfo -R '^lua_workspace_source_roundtrip$' --output-on-failure
```

## S2 editor GUI E2E

`editor-uia.cs` is a separate, bounded Windows UIA driver for the real Lua Workspace frame. It starts `--gui-test host` with an isolated profile, copies `fixtures/editor.ass` and its project-local Automation macro into the artifact directory, opens code through the real Automation menu, edits the Lua source control through guarded input, and inspects the physically saved ASS. The scenario covers local and main undo isolation, multi-code-line decisions, layered close/open cancellation, metadata conflict and invalidation, deletion/undo, and Lua-file persistence and conflict. The read-only Lua-file fault affects only an artifact copy and its original attributes are restored. The manifest records every required step as `passed`, `failed`, or `not-run`; compilation or a partial pass does not establish S2 completion.

From the repository root after building the GUI target:

```powershell
dotnet run tests/gui-automation/lua-workspace/editor-uia.cs -- --exe build-dir/RelWithDebInfo/Aegisub.exe --artifacts build-dir/artifacts/lua-workspace/editor-uia
```

The driver has a 120-second supervising process timeout and per-discovery deadlines. It retains `manifest.json`, `input.ass`, `output.ass` when saved, source buffers, UIA trees, screenshots, clipboard ownership/sequence receipts, and a supervisor cleanup status under the explicit artifact directory. The user clipboard is snapshotted before the worker runs and restored only after the worker and GUI host stop if no external clipboard change intervened. A failed or unrun step must not be inferred successful from a compiled driver.

`applied.ass` is frozen immediately after Apply and main Undo/Redo, before later lifecycle scenarios mutate the test document. Use that artifact, not the final `output.ass`, as the subtitle input for `verify-editor-save.json`; the real headless Automation macro in `fixtures/verify-editor-save.lua` checks the persisted code-once business result `38` and the untouched surrounding lines.

The GUI driver requires Windows, .NET 10, the currently built English application UI, and an unlocked interactive desktop. Do not use the keyboard, mouse, or clipboard concurrently with a run. TaskDialog button aliases cover only the English and Simplified Chinese labels observed during development; another localization fails closed rather than assuming button positions. The file-conflict branches exercise the real choice dialog, including a second external file revision between conflict display and overwrite confirmation. These desktop-global input and clipboard constraints are prerequisites, not a claim of safety under concurrent human or automation activity.
