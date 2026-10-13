# Windows OpenGL presentation

VideoDisplay and SkiaAudioDisplay retain their existing OpenGL rendering. On Windows, a supported context now copies the completed default back buffer into a D3D11 texture through WGL_NV_DX_interop2 and presents a two-buffer DXGI flip-discard swap chain for the same canvas HWND. The transfer stays on the GPU. It does not modify driver profiles.

This is a compatibility mitigation for a measured native WGL presentation interaction with Windows DWM. The affected trace used Composed_Copy_GPU_GDI and showed DWM spending substantial CPU time in OpenDxBltEvent's NtQueryDirectoryObject fallback. Changing the NVIDIA application's presentation method improved playback and desktop responsiveness. Source inspection did not identify invalid window flags, a forced software pixel format, the reason the DWM event was unavailable, or an Aegisub commit that introduced the defect. The change must not be described as proof of a particular vendor's bug.

The presenter selects a hardware D3D11 adapter compatible with the current OpenGL context, restores framebuffer bindings, read-buffer selection, scissor and framebuffer-sRGB state, and resizes to physical client dimensions. Resources are unregistered before deletion and released while their owning OpenGL context is current. Video uses sync interval 1; audio uses the existing AEGISUB_SKIA_AUDIO_SWAP_INTERVAL setting (0 by default, clamped to 0–4). Unsupported contexts or initialization/presentation errors retain the native SwapBuffers path. An exceptional driver failure that prevents interop unlock/unregistration may retain references rather than free a still-registered resource.

Set AEGISUB_ENABLE_DXGI_PRESENTATION=0 before starting the process to compare the native backend. The default is enabled on supported Windows contexts; other platforms retain their existing path. Performance traces record video_display.presentation_backend and audio_display.presentation_backend (1 = DXGI, 0 = native), including runtime fallback. Audio tile diagnostics read GL_BACK with DXGI and GL_FRONT with native presentation.

## Repeatable validation

From the repository root, using the existing configured build directory:

```powershell
cmake --build build-dir --config RelWithDebInfo --target Aegisub video-presenter-d3d11-smoke skia-audio-display-smoke test-aegisub --parallel
build-dir/RelWithDebInfo/video-presenter-d3d11-smoke.exe dxgi build-dir/artifacts/presenter-dxgi
pwsh -File tools/video-presentation-probe.ps1 -Executable build-dir/RelWithDebInfo/Aegisub.exe -Artifacts build-dir/artifacts/playback-dxgi -Ffmpeg <ffmpeg-executable> -Presentation Dxgi -WithAudio
```

Use a new empty artifacts directory. The presenter smoke has a 25-second internal deadline and checks displayed colors, orientation, animated content, resize, hide/show, context recreation and GL state. The GUI probe has a 45-second fixture preparation timeout and a 40-second GUI deadline, creates an isolated profile, and exercises attached playback, pause, detached playback/resize, reattach and playback with subtitles. It closes only its own test process. Inspect its three screenshots in addition to the backend/frame assertions. Use -Presentation Native for the comparison. Optional -SourceVideo and -StartSeconds remux an eight-second excerpt into the artifacts directory without modifying the source media.

These tests establish candidate rendering and lifecycle behavior on the tested hardware. They do not replace a same-project comparison on an affected system or a fresh DWM trace proving disappearance of the original event-lookup hotspot. A short synthetic native test may work even when the original project stutters.
