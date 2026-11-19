# Design note 01: real-time scheduling in wine-rta

Status: draft for review. Nothing here is implemented yet.

## Goal

Let Windows audio plugins (and the hosts that bridge them, such as yabridge)
get real-time Linux scheduling for audio threads through the Windows APIs they
already call, without the bridge having to bypass Wine's own priority model.

## Current behaviour (Wine 11.22)

### Server: `server/thread.c`

- `init_threading()` (line ~188) probes `setpriority(-20)` and `RLIMIT_NICE` to
  compute `nice_limit`. If it is `>= 0`, `apply_thread_priority()` returns early
  and no priority is applied at all.
- `apply_thread_priority()` (line ~206) maps NT priorities `[1,15]` linearly to
  nice values. Priorities `>= LOW_REALTIME_PRIORITY` (16) are clamped to 15,
  with an upstream `FIXME: handle realtime priorities using SCHED_RR`.
- `get_effective_thread_priority()` (line ~779) clamps non-realtime-class
  processes to 15 and adds +1 for the first thread of a process.
- `set_thread_priority()` (line ~792) refuses priorities >= 16 unless the
  process class is `REALTIME_PRIORITY_CLASS` (`STATUS_PRIVILEGE_NOT_HELD`).
- The macOS branch of the same file already maps realtime priorities to a
  time-constraint policy. Linux has nothing equivalent.

### MMCSS: `dlls/avrt/main.c`

`AvSetMmThreadCharacteristics{A,W}`, `AvSetMmMaxThreadCharacteristics*`,
`AvSetMmThreadPriority` and `AvRevertMmThreadCharacteristics` are all stubs that
log a FIXME and return success. The task handle is a constant (`0x12345678`).

### Yabridge (`src/common/utils.{h,cpp}`)

`set_realtime_priority(bool, int prio = 5)` calls `sched_setscheduler(0, ...)`
directly, from the plugin side and from the Wine host. It is called from
~20 sites (VST2/VST3/CLAP bridges, `host-process.cpp`, `communication/common.h`).
Priorities are copied from the host's audio threads. It is deliberately not
applied to wineserver ("can actually increase latencies").

Because Wine does not know about this, Wine's `setpriority` calls and the
bridge's `SCHED_FIFO` can interact: a plugin calling `SetThreadPriority` on a
thread that the bridge made `SCHED_FIFO` has no useful effect, and threads
created by plugins inherit policy only incidentally.

## Proposal

### 1. A real-time mapping in the server (opt-in)

Add a Linux `SCHED_FIFO` (default) / `SCHED_RR` path to
`apply_thread_priority()`:

- Gate behind `WINE_RT_PRIO` (name TBD), read once in `init_threading()`.
  Values: unset/`0` = current behaviour; `1` = enable.
- Also require the capability to succeed: probe `sched_setscheduler` on the
  server thread (and restore) and read `RLIMIT_RTPRIO`. If the probe fails, log
  once at debug level and fall back to nice. Never fail a Windows call because
  the Linux side refused.
- Map NT priority to a Linux RT priority in a configurable band, e.g.
  `WINE_RT_PRIO_MAX` (default 10, well below IRQ threads and below what
  PipeWire/JACK typically use) so that Wine cannot starve the system:
  - NT 1..15 stay on `SCHED_OTHER` with nice (unchanged).
  - NT >= 16 (`THREAD_PRIORITY_TIME_CRITICAL` in the realtime class, and
    `SetThreadPriority` values 22..31) map linearly into `[1, WINE_RT_PRIO_MAX]`.
  - Set `SCHED_RESET_ON_FORK` so helper processes do not inherit it.
- Do not override a thread that is already `SCHED_FIFO`/`SCHED_RR` from outside
  Wine (read via `sched_getscheduler` before changing). This is the key
  interoperability point with yabridge's current behaviour and with hosts that
  assign priorities themselves.
- Keep wineserver's own main thread at `SCHED_OTHER`, matching yabridge's
  finding.

Safety: RT threads that spin can lock a machine. Document the need for
`RLIMIT_RTTIME` and keep the band low. Default stays off.

### 2. Relax the class check for audio threads

`set_thread_priority()` currently denies priorities >= 16 outside the realtime
process class. Plugins usually run in the normal class and call
`SetThreadPriority(TIME_CRITICAL)`, which maps to 15 and gets the maximum
*nice* only. Two options:

- (a) Leave the NT semantics alone and make 15 (time-critical, non-realtime
  class) eligible for the RT band when MMCSS "Pro Audio" is active on the
  thread (see 3).
- (b) Treat time-critical in the normal class as RT when `WINE_RT_PRIO=1`.

Recommend (a): it keeps Windows-visible behaviour unchanged and ties RT to an
explicit audio signal.

### 3. Implement MMCSS properly in `avrt`

- Introduce a server request (or reuse `set_thread_info` with a new mask bit,
  `SET_THREAD_INFO_MMCSS`) carrying: task name (`"Pro Audio"`, `"Audio"`,
  `"Games"`, ...) and an `AVRT_PRIORITY`.
- Keep a per-thread MMCSS state on the server (`struct thread`), a real task
  index and handle, and make `AvRevertMmThreadCharacteristics` undo it.
- Task table, initially hard-coded: `Pro Audio` and `Audio` -> RT eligible;
  everything else -> no change. Later: read overrides from the registry under
  `HKLM\Software\Microsoft\Windows NT\CurrentVersion\Multimedia\SystemProfile\Tasks`
  as Windows does.
- `AvSetMmThreadPriority` adjusts the thread within the band
  (`AVRT_PRIORITY_LOW`..`CRITICAL`).

### 4. Yabridge side (later)

- Detect the custom Wine (version string or an exported marker), and when
  present stop calling `set_realtime_priority()` on Wine-created threads;
  keep it only on native-side threads that Wine does not manage.
- Mirror the host's audio-thread priority onto the Wine host thread through
  `SetThreadPriority`/MMCSS instead of `sched_setscheduler`.

## Test plan

- Unit-ish tests in `dlls/ntdll/tests/info.c` and a new `dlls/avrt/tests/`
  (the dir does not exist yet) for the Windows-visible behaviour: handles,
  index, revert semantics, invalid names, `AvSetMmThreadPriority` bounds.
  These run anywhere.
- Linux-specific verification needs a host with `RLIMIT_RTPRIO` > 0 and
  ideally a PREEMPT_RT or low-latency kernel: a small test program that sets
  MMCSS "Pro Audio" and reads `/proc/<pid>/task/<tid>/sched` or uses
  `chrt -p`. This cannot be validated in the cloud container; it needs your
  hardware.
- Latency measurement: cyclictest-style loop in a Windows binary (timer wake
  jitter) comparing nice vs RT paths.

## Decisions (resolved)

1. Enable switch: `WINE_RT_PRIO=1` environment variable, off by default.
2. Priority band: capped at 10, configurable with `WINE_RT_PRIO_MAX`.
3. Policy: `SCHED_FIFO` by default (matches yabridge, JACK, PipeWire). Not yet
   confirmed with the maintainer; keep it switchable.
4. When `RLIMIT_RTPRIO` is 0 or the probe fails: log once at debug level and
   keep the existing nice mapping. rtkit support is a later, separate change.
5. Upstream fit: small, flag-gated, separable commits for now (scheduling and
   MMCSS kept upstreamable; yabridge-specific parts kept apart). More invasive
   refactoring is not ruled out where a change needs it.
