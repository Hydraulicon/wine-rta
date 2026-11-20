# Handoff: wine-rta real-time audio work

For the next session, running on the maintainer's own machine. Written
2026-11-20 at the end of a cloud session that had no access to real hardware.
Read this, then `rta/design/01-realtime-scheduling.md` (the design note: every
decision, measurement and known limitation), then `rta/reference/vst3-sdk.md`.

## Goal

Patch Wine for real-time audio, prioritising the Windows APIs audio plugins use,
and later rebase yabridge (github.com/Hydraulicon/yabridge, a fork of
robbert-vdh/yabridge) onto it. The `wine-rta` `master` branch is a plain
mirror of upstream Wine (11.22). All the work so far is on branch
`ccr-b77ff3cf-7zpw85` and has not been merged. yabridge has no changes yet.

```sh
git clone -b ccr-b77ff3cf-7zpw85 https://github.com/Hydraulicon/wine-rta
git clone https://github.com/Hydraulicon/yabridge
```

## Decisions already made by the maintainer

- Real-time scheduling is opt-in: `WINE_RT_PRIO=1`, off by default.
- Linux real-time priorities are capped at 10 by default (`WINE_RT_PRIO_MAX`).
- `SCHED_FIFO` by default (`WINE_RT_POLICY=rr` for `SCHED_RR`). This was chosen
  by the previous session, not confirmed by the maintainer; keep it switchable.
- When real-time scheduling is not permitted: fall back to the niceness mapping and
  log once. rtkit support is a separate, later change.
- Keep patches small, flag-gated and in separable commits, so rebasing onto new
  upstream Wine stays cheap. More invasive work is allowed when needed.
- The VST3 SDK upgrade for yabridge (3.7.7 to the MIT-licensed 3.8.1) waits for
  the yabridge stage.

## What is done

With `WINE_RT_PRIO` off, Linux scheduling matches upstream Wine. With it on:

| Area | Where | Summary |
| --- | --- | --- |
| Real-time scheduling | `server/thread.c` | Thread priorities 16-31 map to `SCHED_FIFO`/`SCHED_RR` 1..`WINE_RT_PRIO_MAX`, with `SCHED_RESET_ON_FORK`. Threads already made real-time outside Wine (yabridge does this) are left alone. wineserver stays on `SCHED_OTHER`. |
| MMCSS | `dlls/avrt/main.c`, `server/thread.c`, `dlls/ntdll/unix/thread.c`, `dlls/wow64/process.c` | Real `AvSetMmThreadCharacteristics` and related calls. "Pro Audio" is thread priority 24 (22-26), giving FIFO 5 (4-7); "Audio" is 19. Carried by the private `ThreadWineMmcssPriority` info class (server protocol 983). Windows code always sees the MMCSS priority; Linux scheduling uses it only when real-time scheduling is enabled and permitted. |
| ntsync | `include/wine/ntsync.h`, `server/inproc_sync.c`, `dlls/ntdll/unix/sync.c` | Bundled Linux 6.14 uapi header, so in-process events and mutexes always build. wineserver warns on stderr when `WINE_RT_PRIO=1` and `/dev/ntsync` is unusable. |
| PI critical sections | `dlls/ntdll/sync.c`, `dlls/ntdll/unix/sync.c` | `FUTEX_LOCK_PI` on the low 32 bits of `LockSemaphore`; Linux tid in `struct teb_data::pi_tid` (TEB +0x384 on 64-bit, +0x220 on 32-bit). `WINE_RT_PI=0` turns it off. Works for 64-bit and classic 32-bit (`--with-wine64`). |
| PI SRW locks | `dlls/ntdll/sync.c` | Exclusive owners hold a PI futex in the upper 32 bits of `SRWLOCK`. 64-bit only. Covers MSVC `std::mutex`. |
| Tests | `dlls/avrt/tests/avrt.c` | 48 checks. Expected values come from Microsoft's documentation and have not been run on Windows. |
| Benchmarks | `rta/bench/` | `event-pingpong.c` (event wake-up latency), `crit-inversion.c` (priority inversion, `cs`/`srw`/`srw-shared`), `lock-throughput.c`. |

Measured in the cloud VM (4 vCPUs, no ntsync, root with `CAP_SYS_NICE`):
inversion ~320 ms without PI and ~20 ms with it, for critical sections and SRW
locks; with real-time scheduling off, ~40 ms, as in upstream. Full tables are in
the design note.

## Do this first on real hardware

The cloud session could not do any of this, and it decides what matters next.

1. Check the machine:
   ```sh
   uname -r                     # 6.14 or later for ntsync
   sudo modprobe ntsync && ls -l /dev/ntsync
   ulimit -r                    # RLIMIT_RTPRIO; must be >= 1 (limits.conf or PAM, e.g. @audio - rtprio 95)
   ```
2. Build (see below) and run the benchmarks with `/dev/ntsync` present and absent
   (rename or `chmod` it), each with `WINE_RT_PRIO=0`, `WINE_RT_PRIO=1` and
   `WINE_RT_PRIO=1 WINE_RT_PI=0`:
   ```sh
   x86_64-w64-mingw32-gcc -O2 -o event-pingpong.exe rta/bench/event-pingpong.c -lavrt
   x86_64-w64-mingw32-gcc -O2 -o crit-inversion.exe rta/bench/crit-inversion.c -lavrt
   x86_64-w64-mingw32-gcc -O2 -o lock-throughput.exe rta/bench/lock-throughput.c
   WINE_RT_PRIO=1 ./wine event-pingpong.exe 20000 --mmcss
   WINE_RT_PRIO=1 ./wine crit-inversion.exe 10 srw
   WINE_RT_PRIO=1 ./wine lock-throughput.exe
   ```
   Record the results in the design note next to the cloud numbers. A low-latency
   or PREEMPT_RT kernel is the interesting case.
3. Try a real multi-threaded plugin through yabridge to settle the `TIME_CRITICAL`
   question (design note, "`TIME_CRITICAL` worker threads"). Plugins' DSP worker
   threads call `SetThreadPriority(TIME_CRITICAL)`, which stays at thread priority 15
   and never becomes real-time under the current design.

## Building

Dependencies on Ubuntu 24.04: the usual Wine build dependencies plus `flex`,
`bison`, `gcc-mingw-w64` (both i686 and x86_64), `libasound2-dev`,
`libpulse-dev`, X11 dev packages, and `gcc-multilib` for the 32-bit build.
Ubuntu 24.04's kernel headers lack `linux/ntsync.h`; the bundled header covers it.

```sh
mkdir build64 && cd build64 && ../wine-rta/configure --enable-win64 && make -j"$(nproc)"
# classic 32-bit, needed by yabridge's bitbridge (it does not work with the new WoW64 mode):
mkdir build32 && cd build32 && ../wine-rta/configure --with-wine64=../build64 && make -j"$(nproc)"
```

Gotchas found along the way:

- `make dlls/ntdll` does not rebuild everything. After editing ntdll, build
  `dlls/ntdll/ntdll.so` (Unix side) and `dlls/ntdll/x86_64-windows/ntdll.dll`
  (PE side) explicitly, or run a full `make`.
- After a server protocol change (`server/protocol.def`, then run
  `tools/make_requests`), run `wineserver -k` before testing.
- A new header must be listed in `include/Makefile.in`, or configuring a new build
  directory fails.
- Run `wineboot -u` after rebuilding DLLs, or kernel32's `loader` test fails on stale
  copies in the prefix. A prefix created before the 32-bit build was linked
  (`--with-wine64`) cannot load kernel32; create a fresh one.
- `configure` was generated by Autoconf 2.73. Avoid regenerating it with an older
  Autoconf; one-line edits by hand are fine.

## Testing

Run from the build directory with a throwaway prefix:

```sh
export WINEPREFIX=$HOME/.wine-rta-test
for mode in "WINE_RT_PRIO=0" "WINE_RT_PRIO=1" "WINE_RT_PRIO=1 WINE_RT_PI=0"; do
  ./server/wineserver -k
  for t in "kernel32 sync" "ntdll sync" "kernel32 thread" "kernel32 heap" "ntdll rtl" \
           "ntdll threadpool" "kernel32 loader" "avrt avrt"; do
    set -- $t
    env $mode ./wine dlls/$1/tests/x86_64-windows/$1_test.exe $2 | tail -1
  done
done
```

All of these passed in the cloud VM in every mode, 64-bit and 32-bit
(`i386-windows`). kernel32 `process` had 48 failures in every mode, all about
console handles in a headless container; it should pass on a desktop.

To check scheduling directly, run a program that registers MMCSS "Pro Audio" and
look at its threads with `chrt -p <tid>`: `SCHED_FIFO|SCHED_RESET_ON_FORK 5`
with `WINE_RT_PRIO=1`, `SCHED_OTHER` otherwise.

## Open items, in suggested order

1. Hardware runs above, and the `TIME_CRITICAL` decision.
2. Raise the soft `RLIMIT_RTPRIO` to the hard limit in ntdll at startup. The kernel
   checks the target thread's limit, not wineserver's, so a soft limit below the
   hard limit makes real-time scheduling fail silently into niceness.
3. If ntsync turns out to matter as much as expected: decide whether `WINE_RT_PRIO=1`
   without ntsync should stay a warning or become something stronger.
4. MMCSS task settings from the registry
   (`HKLM\Software\Microsoft\Windows NT\CurrentVersion\Multimedia\SystemProfile\Tasks`)
   instead of the hard-coded table; return `ERROR_INVALID_TASK_INDEX` for unknown
   non-zero indices.
5. Run `dlls/avrt/tests` on real Windows (or WineTestBot) and fix expectations.
6. 32-bit code in Wine's new WoW64 mode: thunks for the PI Unix calls and setting
   `pi_tid` in the 32-bit TEB. Low priority, since yabridge's bitbridge needs the
   classic setup.
7. The yabridge stage: VST3 SDK 3.8.1 (`rta/tools/fetch-vst3sdk.sh`); detect wine-rta
   and stop calling `set_realtime_priority()` on threads Wine manages; mirror the
   host's audio-thread priority through MMCSS instead of `sched_setscheduler`.

## Limitations to keep in mind

All in the design note. The ones most likely to bite:

- 32-bit `SRWLOCK` (4 bytes) has no room for a PI word, so 32-bit `std::mutex` is
  not priority-inheriting. 32-bit critical sections are.
- An exclusive SRW owner waiting for shared owners cannot boost them.
- PI-mode SRW locks favour writers more than before (about 1.5 reads per write
  against about 17 in a 1-writer, 3-reader benchmark).
- In PI mode, critical section `LockCount` no longer counts waiters, and a
  `LockSemaphore` holding a real semaphore handle is not supported.

## Conventions

- Commit messages follow Wine's style: `component: Summary sentence.` and a short
  body. Documentation and benchmarks for this fork live under `rta/` and use the
  `rta:` prefix.
- Keep Wine code changes and their `rta/` documentation in separate commits.
- Update the design note's status and measurements when something lands.
