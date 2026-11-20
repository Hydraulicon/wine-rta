# Design note 01: real-time scheduling in wine-rta

Status: steps 1-3 implemented (see "Implementation status"); step 4 (yabridge) not started.

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

## Implementation status

Implemented in these commits on the development branch:

- `avrt: Fix the task2 check in AvSetMmMaxThreadCharacteristicsW.`
- `server: Use real-time scheduling for realtime band priorities on Linux.`
  (proposal 1)
- `server: Add an MMCSS thread priority.` A private `ThreadWineMmcssPriority`
  information class carried by `set_thread_info` (`SET_THREAD_INFO_MMCSS_PRIORITY`),
  also passed through wow64 for 32-bit plugins. Server protocol 982 -> 983.
- `avrt: Implement MMCSS thread characteristics.` (proposals 2 and 3, option (a))

Environment variables (read by wineserver at startup):

| Variable | Values | Default |
| --- | --- | --- |
| `WINE_RT_PRIO` | `1` enables real-time scheduling | off |
| `WINE_RT_PRIO_MAX` | highest Linux RT priority used | 10 |
| `WINE_RT_POLICY` | `fifo` or `rr` | `fifo` |

Resulting mapping with the defaults: NT priority `p` in [16,31] maps to
`1 + (p - 16) * 9 / 15`. MMCSS "Pro Audio" is NT 24 (22-26 with
`AvSetMmThreadPriority`), giving `SCHED_FIFO` 5 (4-7). "Audio" is NT 19
(17-21), giving 2 (1-4). `THREAD_PRIORITY_TIME_CRITICAL` in a normal-class
process stays at NT 15 and does not become real-time.

Verified in the cloud container (Linux 6.18, root with `CAP_SYS_NICE`):
the avrt tests pass with `WINE_RT_PRIO` off and on; `chrt` shows the expected
policies and priorities, a revert back to `SCHED_OTHER`, wineserver staying on
`SCHED_OTHER`, and a thread started with `chrt -f 3` staying at FIFO 3 through
MMCSS registration and revert. Latency was not measured.

Known limitations:

- The kernel checks the target thread's `RLIMIT_RTPRIO`, not wineserver's.
  wineserver raises only its own soft limit, so a Wine process with a soft
  limit below the hard limit can still be refused. Then the niceness mapping
  is used. Raising the soft limit in ntdll at startup would fix this.
- MMCSS priorities only affect Linux scheduling when real-time scheduling is
  enabled and permitted. Otherwise Windows code still sees the MMCSS priority
  (`NtQueryInformationThread`), but the thread keeps the niceness it would have
  in upstream Wine. An earlier version also raised niceness with real-time
  scheduling off. MMCSS threads then starved normal threads holding locks they
  waited for (about 300 ms in the inversion benchmark), which priority
  inheritance cannot fix, since it only boosts lock owners for real-time waiters.
  macOS keeps the upstream mapping and ignores MMCSS.
- Reverting one of several handles on the same thread drops that thread's MMCSS
  priority entirely.
- An unknown non-zero task index is accepted (Windows returns
  `ERROR_INVALID_TASK_INDEX`). Task settings are hard-coded, not read from
  the registry.
- The avrt test expectations come from Microsoft's documentation and have not
  been run on Windows.

## Revised plan after the VST3 SDK survey

See `rta/reference/vst3-sdk.md`. The SDK code that runs on the audio thread
makes no Windows API calls. Its only Windows synchronisation primitives are
critical sections, auto-reset events (`SetEvent`, `PulseEvent`) and
`WaitForSingleObject`. That changes the order of the remaining work.

### Next: ntsync (in-process events)

Wine 11 handles events, semaphores and mutexes in-process through
`/dev/ntsync` (`server/inproc_sync.c`, `dlls/ntdll/unix/sync.c`) only when
it was built with `linux/ntsync.h` and the device exists at runtime.
Otherwise every `SetEvent` and wait is a wineserver round trip. wineserver
stays on `SCHED_OTHER` by design, so a `SCHED_FIFO` audio thread waiting on an
event would depend on a normally scheduled process: a priority inversion.

Our first build had no ntsync: Ubuntu 24.04's kernel headers predate
`linux/ntsync.h`, so `HAVE_LINUX_NTSYNC_H` was undefined.

Work:

1. Build ntsync support even when the system headers lack `linux/ntsync.h`.
2. When `WINE_RT_PRIO=1` and ntsync is unavailable at runtime, warn loudly.
3. Measure event wake-up latency with ntsync versus the server fallback.

Status: 1 and 2 are done. The header is bundled as `include/wine/ntsync.h`
(Linux 6.14, unchanged through 6.18) and used on all Linux builds. When
`WINE_RT_PRIO=1` is set and ntsync is unusable, wineserver prints a warning
naming the cause. For 3, `rta/bench/event-pingpong.c` ping-pongs two threads
through auto-reset events and reports the round-trip distribution.

Baseline for the wineserver fallback, from the cloud container (a VM whose
kernel lacks `CONFIG_NTSYNC`; 20000 round trips, microseconds, two runs each).
This only shows the order of magnitude, not real-time behaviour:

| Configuration | min | median | p99 | p99.9 | max |
| --- | --- | --- | --- | --- | --- |
| `WINE_RT_PRIO=0` | 30-31 | 36 | 81-86 | 203-271 | 1420-2253 |
| `WINE_RT_PRIO=1` | 29-31 | 36 | 69-140 | 144-436 | 880-5565 |
| `WINE_RT_PRIO=1`, `--mmcss` | 31 | 36 | 70-74 | 96-128 | 540-1028 |

Still to do: the same runs with `/dev/ntsync` on real hardware (Linux 6.14 or
later with the `ntsync` module loaded), ideally on a low-latency kernel.

### Then: priority inversion in critical sections (was step 3)

Contended critical sections wait on a plain futex in-process
(`RtlWaitOnAddress`, `dlls/ntdll/sync.c`): no wineserver, but no priority
inheritance either. A real-time thread blocked on a lock held by a
`SCHED_OTHER` thread can be starved indefinitely. Candidate: priority-inheriting
futexes (`FUTEX_LOCK_PI`) for critical sections when real-time scheduling is
enabled. This is invasive, which the upstream-fit decision allows when needed.

Status: implemented. With `WINE_RT_PRIO=1`, and when the kernel supports PI
futexes (probed at startup), critical sections use `FUTEX_LOCK_PI` /
`FUTEX_UNLOCK_PI`. `WINE_RT_PI=0` turns this off while keeping real-time
scheduling.

- The Unix side stores the thread's Linux tid in `struct teb_data::pi_tid`
  (TEB offset 0x384 on 64-bit, 0x220 on 32-bit). A tid of 0 means the
  existing implementation, so the mode is decided once per process.
- The low 32 bits of `LockSemaphore` hold the PI futex word: 0 when free, the
  owner's tid otherwise. Uncontended enter and leave are one compare-and-swap
  each, in user space.
- Contended waiters spin up to 4000 iterations (or `SpinCount`, if larger)
  before sleeping. Without that, `FUTEX_UNLOCK_PI` hands the lock to the sleeping
  waiter and every contended acquisition becomes a context switch.

Measurements in the cloud container (4 vCPUs):

| Benchmark | PI off | PI on |
| --- | --- | --- |
| Inversion, `rta/bench/crit-inversion.c` (wait of the high-priority thread) | ~320 ms | ~20 ms (the low thread's 20 ms of work) |
| Uncontended enter and leave | 17.0 ns | 18.6 ns |
| Two threads hammering one lock (per pair) | 136-147 ns | 97 ns |

The first PI version, without spinning and with atomic `LockCount` updates,
measured 30 ns uncontended and about 2400 ns contended.

Wine's kernel32 `sync`, `thread`, `heap` and `loader` and ntdll `sync` and
`rtl` tests give the same results with PI off and on. (`loader` fails when the
test prefix holds a stale copy of a rebuilt DLL; run `wineboot -u` after
rebuilding ntdll.)

Known limitations:

- PI futexes only boost real-time waiters. With `WINE_RT_PRIO` off there is no
  priority inheritance, but MMCSS no longer raises niceness either, so the
  inversion benchmark behaves as in upstream Wine: the low thread shares the CPU
  with the medium one, and the high thread waits about 40 ms for 20 ms of work.
  Inversion between threads whose priorities applications set themselves with
  `SetThreadPriority` is upstream behaviour and not addressed.
- In PI mode `LockCount` is written by the owner only and no longer counts
  waiters. Nothing in Wine reads it.
- A `LockSemaphore` that holds a real semaphore handle (`MakeCriticalSectionGlobal`,
  or applications that set it themselves) is not supported in PI mode.
- If an owner exits while holding the lock, a waiter gets it (Windows would block
  forever).
- 32-bit processes in the classic setup, where a separate 32-bit Wine is built
  `--with-wine64`, are covered: 32-bit ntdll has its own 32-bit Unix side, which
  stores `pi_tid` at TEB offset 0x220. This is the setup yabridge's bitbridge
  needs; its README says the bitbridge does not work with Wine's new WoW64 mode.
  Measured with a 32-bit build in the cloud container: the critical section
  inversion benchmark drops from ~312 ms to ~15 ms, and Wine's 32-bit kernel32
  `sync`, `thread`, `heap` and `loader`, ntdll `sync`, `rtl` and `threadpool`
  and avrt tests pass with `WINE_RT_PRIO` off, on, and on with `WINE_RT_PI=0`.
- 32-bit code under the new WoW64 mode keeps the existing implementation: the
  64-bit Unix side does not set `pi_tid` in the 32-bit TEB, and the wow64 entries
  for the PI Unix calls return `STATUS_NOT_SUPPORTED`. Supporting it would need
  32-bit parameter thunks and setting the field in the 32-bit TEB.
- Condition variables and `WaitOnAddress` are not priority-inheriting. They
  have no owner to boost, so this is inherent.

#### SRW locks

Status: implemented for exclusive owners. MSVC's `std::mutex` and
`std::shared_mutex` are built on SRW locks, so this covers most C++ plugins.

- 64-bit only: the upper 32 bits of `SRWLOCK`, unused by Wine so far, hold a
  PI futex owned by the exclusive owner while it holds the lock. The lower half
  keeps bit 0 as the exclusive flag (an application-compatibility test relies on
  it), counts shared owners in bits 1-29, and has two waiting flags.
- Exclusive waiters queue on the PI futex and boost the owner. Shared waiters do
  so only when they run with a real-time policy (checked with
  `sched_getscheduler`, only on the slow path). Other shared waiters wait for
  the release in user space. Queueing them all on the PI futex hands it from
  waiter to waiter, a context switch each, and made one writer with three
  readers 18 times slower.
- An exclusive owner waiting for shared owners to leave cannot boost them, since a
  PI futex has a single owner.
- Acquiring a lock the thread already holds blocks until another thread releases
  it, as on Windows. Wine's tests do this to check deadlocks. Release by another
  thread works unless other threads are queued in the kernel; Microsoft documents
  that SRW locks must be released by the thread that acquired them.
- Locks that are not 4-byte aligned, and 32-bit code, keep the existing
  implementation. A 32-bit `SRWLOCK` has only 4 bytes, so there is no room for a
  separate PI word, and keeping PI words outside the lock (for instance in a table
  hashed by address) would let unrelated locks block each other. So 32-bit plugins
  get priority inheritance for critical sections but not for `std::mutex`.

Measurements in the cloud container (`rta/bench/lock-throughput.c`,
`rta/bench/crit-inversion.c`), `WINE_RT_PRIO=1`:

| Benchmark | `WINE_RT_PI=0` | PI on |
| --- | --- | --- |
| Inversion, SRW exclusive | ~320 ms | ~20 ms |
| Inversion, high-priority thread waiting shared behind the writer | ~320 ms | ~20 ms |
| SRW exclusive, uncontended | 54 ns | 32 ns |
| SRW shared, uncontended | 38 ns | 17 ns |
| SRW exclusive, 2 threads | 207 ns | 220 ns |
| SRW 1 writer + 3 readers, writer | 2.8-3.3 us | 0.9-1.4 us |
| SRW 1 writer + 3 readers, reads | 5.6-6.3 M/s | 1.3-1.8 M/s |

The last two rows show that PI mode favours writers more: about 1.5 reads per
write instead of about 17. Neither is wrong, but code that relies on readers
making progress against a busy writer gets less of it.

### Then: `TIME_CRITICAL` worker threads (revisits proposal 2)

The SDK never sets thread priorities or calls MMCSS. The host's audio thread
is yabridge's, which yabridge already makes real-time and which the server
leaves alone. Plugins' own DSP worker threads typically call
`SetThreadPriority(TIME_CRITICAL)`, which under option (a) stays at NT 15
and never becomes real-time. Options: (b) make `TIME_CRITICAL` real-time when
`WINE_RT_PRIO=1`, or let threads created by an MMCSS thread inherit
eligibility. Decide after measuring multi-threaded plugins.

### Deprioritised

- Wine's audio stack (`winmm`, `dsound`, `mmdevapi`, the ALSA and PulseAudio
  drivers): plugins do not use it.
- `SetTimer` precision: GUI refresh only, through the server message queue.
- The GUI stack (Direct2D, DirectWrite, WIC, OLE drag and drop, hooks, DPI):
  compatibility work, a separate track from real-time.
