# Reference: how the VST3 SDK uses Windows and Linux APIs

SDK version: **v3.8.1_build_84** (fetch with `rta/tools/fetch-vst3sdk.sh`).
`base`, `pluginterfaces`, `public.sdk` and `cmake` are MIT. `vstgui4` has its
own BSD-3-style licence. Yabridge currently builds against v3.7.7, the older
GPLv3 release, through a patched fork.

Method: every identifier followed by `(` in the SDK's Windows-specific files,
matched against the functions exported by Wine's DLL `.spec` files. Files under
`samples/`, tests and the AAX wrapper are excluded. COM interfaces (Direct2D,
DirectWrite, WIC) were counted separately, because they are not exports.

## Main finding for real-time work

The SDK code that runs on the audio thread (`process()`, parameter and event
plumbing) makes **no Windows API calls**. Windows APIs appear only in:

| Area | Where | Windows APIs | Relevance |
| --- | --- | --- | --- |
| Locks | `base/thread/source/flock.cpp` | `InitializeCriticalSection`, `Enter/Leave/TryEnterCriticalSection` | **High**: plugins share these locks between the audio and GUI or worker threads |
| Conditions | `base/thread/source/fcondition.cpp` | `CreateEventA` (auto-reset), `SetEvent`, `ResetEvent`, **`PulseEvent`**, `WaitForSingleObject` | **High**: wake-up latency; `PulseEvent` is racy by design and is only used on Windows (Linux uses `pthread_cond_*`) |
| Atomics | `pluginterfaces/base/funknown.cpp` | `InterlockedExchangeAdd` (reference counting) | Low: compiler intrinsics, no Wine code |
| Thread checks | `public.sdk/source/common/threadchecker_win32.cpp` | `GetCurrentThreadId` | Low |
| Timers | `base/source/timer.cpp` | `SetTimer(NULL, 0, ms, TimerProc)`, `KillTimer` | GUI thread only; needs a message loop on the creating thread |
| Module load | `public.sdk/source/vst/hosting/module_win32.cpp` | `LoadLibraryW`, `GetProcAddress`, `FreeLibrary`, `GetFileAttributesW`, `SHGetKnownFolderPath` | Load time only |
| Misc | `public.sdk/source/common/*` | clipboard (`OpenClipboard`, `GlobalAlloc`, ...), `ShellExecuteA`, `OutputDebugStringA`, `FormatMessageA` | Not on the audio path |

So for the scheduling and synchronisation work (design note 01, step 2), the
Windows APIs worth tuning first are critical sections, auto-reset events and
`WaitForSingleObject`, plus `PulseEvent` semantics. A plugin's audio path then
mostly hits Wine through its own code (allocators, `QueryPerformanceCounter`,
TLS), not through the SDK.

## Timers: Windows versus Linux

- **Windows:** `SetTimer` with no window and a `TimerProc`. The timer fires as
  `WM_TIMER` only while the creating thread pumps messages, so yabridge's
  Wine host must keep a message loop running on that thread. Accuracy is
  limited to the message loop and Wine's timer resolution.
- **Linux:** the plugin gets fd and timer callbacks from the host through
  `Steinberg::Linux::IRunLoop` (`pluginterfaces/gui/iplugview.h`:
  `registerEventHandler`, `registerTimer`). There is no plugin-owned event loop.

That difference is what yabridge bridges today: Windows plugins expect a
thread with a message queue, native Linux hosts offer `IRunLoop`.

## Module loading: Windows versus Linux

| | Windows (`module_win32.cpp`) | Linux (`module_linux.cpp`) |
| --- | --- | --- |
| Loader | `LoadLibraryW` | `dlopen` |
| Entry and exit | `InitDll` / `ExitDll`, both **optional** | `ModuleEntry(void *handle)` / `ModuleExit`, both **mandatory** |
| Bundle folders | `Contents/x86_64-win`, `arm64-win`, `arm64ec-win`, `arm64x-win` | `Contents/x86_64-linux` (and other arches) |
| Search paths | `FOLDERID_UserProgramFilesCommon\VST3`, `FOLDERID_ProgramFilesCommon\VST3` | `~/.vst3`, `/usr/lib/vst3`, `/usr/local/lib/vst3`, ... |

ARM64EC and ARM64X bundle folders are new compared with yabridge's 3.7.7.

## GUI (VSTGUI, Windows platform layer)

Most VST3 plugin GUIs use VSTGUI, so this is the Windows GUI surface Wine has to
get right for plugin editors. It matters for compatibility, not real-time.

- **Rendering:** Direct2D (`ID2D1Factory1`, `ID2D1DeviceContext`, `ID2D1Device`,
  bitmaps, path geometries, gradient and image brushes), DirectWrite
  (`IDWriteFactory5`, font sets and collections, text layouts, custom
  `IDWriteTextRenderer`), WIC (`IWICImagingFactory`, bitmaps and locks), and
  optional OpenGL through `ChoosePixelFormat`, `SetPixelFormat` and `SwapBuffers`.
- **Windows:** `CreateWindowEx`, `RegisterClassEx`, `SetWindowPos`,
  `SetLayeredWindowAttributes`, `DwmExtendFrameIntoClientArea`, `GetUpdateRgn`,
  `BeginPaint`/`EndPaint`, `MapWindowPoints`.
- **Input:** `GetAsyncKeyState` (17 sites), `SetCapture`/`ReleaseCapture`,
  `TrackMouseEvent`, `SetWindowsHookEx`/`CallNextHookEx`, IMM composition
  (`ImmGetContext`, `ImmGetCompositionStringW`).
- **Drag and drop, clipboard:** OLE `RegisterDragDrop`, `RevokeDragDrop`,
  `DoDragDrop`, `OleSetClipboard`, `OleGetClipboard`, `CreateStreamOnHGlobal`,
  `DragQueryFile`. Yabridge translates these to XDND itself today.
- **DPI:** `SetProcessDpiAwareness`, `SetThreadDpiAwarenessContext`.
- **Resources:** `FindResourceA`, `LoadResource`, `LockResource`, `SHCreateStreamOnFileEx`.

## Reproducing the scan

```sh
rta/tools/fetch-vst3sdk.sh /path/to/vst3sdk
# Wine exports: name -> DLL, from every dlls/*/*.spec
# SDK identifiers: grep -ohE '\b[A-Z][A-Za-z0-9_]+ *\(' over files that test
#   SMTG_OS_WINDOWS/_WIN32, plus vstgui4/vstgui/lib/platform/win32
# then join the two lists on the identifier.
```
