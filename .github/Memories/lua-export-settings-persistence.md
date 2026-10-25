# Lua Export settings in ASS projects

- Ordinary ASS project writes persist the existing `automation_settings` map in
  `[Aegisub Project Garbage]` using `Automation Settings <encoded Name>: <payload>`.
  Unknown filters and empty payloads remain in the map and on disk. Settings are
  project metadata, not view state: `write_project_garbage` controls their output;
  `write_ui_state` does not. ASS Export and the core facade retain their default
  no-project-metadata contract.
- These settings can contain user-entered text and are now visible in saved ASS
  projects. This restores the project persistence channel lost in
  `9c7119fdc29cf2bb306e8684d280cfe7ee78bd96`: that metadata migration moved settings
  out of Script Info without writing the new map, and its parser retained the
  protocol separator in the filter key.
- Both legacy Script Info and Project Garbage parse the same prefix. Remove only
  its single separator space; leading spaces belonging to a filter Name are part
  of its identity. Keep the encoded Name and serialized payload opaque here.
  Duplicate keys retain the last value read.
- The ASS reader still trims lines and property-value leading whitespace.
  Lua Edit/Textbox/Dropdown serialization protects its trailing whitespace with
  existing `#XX` escapes, including `#20` for spaces. Do not change the global
  inline codec or double-encode the complete `name:value|...` payload.
- This is Export-filter configuration, not automatic macro persistence. Parameter
  acceptance, Save enablement, history/dirty tracking and autosave scheduling are
  unchanged. Actual project-save snapshots keep the map; settings-only changes
  do not gain a new guarantee that a save or autosave will run.
- Independent literal reader/writer tests are in
  `tests/tests/ass_project_garbage.cpp`; typed Lua values and fresh-file disk
  restoration are in `tests/tests/lua_dialog_serialization.cpp`. These tests do
  not establish GUI Save/Save As or native recovery execution.
