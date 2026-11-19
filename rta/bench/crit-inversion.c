/*
 * Priority inversion benchmark for critical sections and SRW locks in wine-rta.
 *
 * All threads run on one CPU:
 *   L  normal priority, holds the lock (exclusive) for about HOLD_MS of work
 *   M  MMCSS "Audio" (real-time, medium), busy for HOG_MS
 *   H  MMCSS "Pro Audio" (real-time, high), then takes the lock
 *
 * Without priority inheritance M starves L, so H waits for about HOG_MS.
 * With it, L runs at H's priority and H waits for about HOLD_MS.
 * Needs WINE_RT_PRIO=1; compare WINE_RT_PI=0 against the default.
 *
 * Build: x86_64-w64-mingw32-gcc -O2 -o crit-inversion.exe crit-inversion.c -lavrt
 * Run:   wine crit-inversion.exe [rounds] [cs|srw|srw-shared]
 *        cs         critical section (default)
 *        srw        SRW lock, H takes it exclusive
 *        srw-shared SRW lock, H takes it shared
 */

#include <windows.h>
#include <avrt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOLD_MS 20
#define HOG_MS  300

static CRITICAL_SECTION cs;
static SRWLOCK srw = SRWLOCK_INIT;
static enum { LOCK_CS, LOCK_SRW, LOCK_SRW_SHARED } lock_type;

static void lock_exclusive(void)
{
    if (lock_type == LOCK_CS) EnterCriticalSection(&cs);
    else AcquireSRWLockExclusive(&srw);
}

static void unlock_exclusive(void)
{
    if (lock_type == LOCK_CS) LeaveCriticalSection(&cs);
    else ReleaseSRWLockExclusive(&srw);
}

/* the high-priority thread's acquisition */
static void lock_high(void)
{
    if (lock_type == LOCK_SRW_SHARED) AcquireSRWLockShared(&srw);
    else lock_exclusive();
}

static void unlock_high(void)
{
    if (lock_type == LOCK_SRW_SHARED) ReleaseSRWLockShared(&srw);
    else unlock_exclusive();
}
static HANDLE low_holds, low_go, medium_started;
static LARGE_INTEGER freq;
static volatile LONGLONG sink;
static volatile LONG low_done;
static LONGLONG work_per_ms;

static LONGLONG now_us(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000000 / freq.QuadPart;
}

static void work(LONGLONG units)
{
    LONGLONG i;
    for (i = 0; i < units; i++) sink += i;
}

static void calibrate(void)
{
    LONGLONG units = 1000000, start, elapsed;

    for (;;)
    {
        start = now_us();
        work(units);
        elapsed = now_us() - start;
        if (elapsed > 50000) break;
        units *= 2;
    }
    work_per_ms = units * 1000 / elapsed;
}

static HANDLE enter_mmcss(const WCHAR *task)
{
    DWORD index = 0;
    HANDLE handle = AvSetMmThreadCharacteristicsW(task, &index);
    if (!handle) fprintf(stderr, "AvSetMmThreadCharacteristicsW(%ls) failed: %lu\n", task, GetLastError());
    return handle;
}

static DWORD WINAPI low_thread(void *arg)
{
    lock_exclusive();
    SetEvent(low_holds);
    /* start working only once the medium thread runs, so the two always overlap */
    WaitForSingleObject(low_go, INFINITE);
    work(HOLD_MS * work_per_ms);  /* a fixed amount of CPU work, not wall time */
    low_done = 1;
    unlock_exclusive();
    return 0;
}

static DWORD WINAPI medium_thread(void *arg)
{
    HANDLE mmcss = enter_mmcss(L"Audio");
    LONGLONG end;

    SetEvent(medium_started);
    end = now_us() + HOG_MS * 1000;
    while (now_us() < end) work(1000);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    return 0;
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 5, i;
    const char *names[] = { "critical section", "SRW lock, exclusive", "SRW lock, shared" };

    if (argc > 2 && !strcmp(argv[2], "srw")) lock_type = LOCK_SRW;
    else if (argc > 2 && !strcmp(argv[2], "srw-shared")) lock_type = LOCK_SRW_SHARED;
    HANDLE mmcss, low, medium;
    LONGLONG start, waited;

    QueryPerformanceFrequency(&freq);
    SetProcessAffinityMask(GetCurrentProcess(), 1);
    Sleep(10);
    calibrate();

    InitializeCriticalSection(&cs);
    low_holds = CreateEventW(NULL, FALSE, FALSE, NULL);
    low_go = CreateEventW(NULL, FALSE, FALSE, NULL);
    medium_started = CreateEventW(NULL, FALSE, FALSE, NULL);
    mmcss = enter_mmcss(L"Pro Audio");

    printf("%s wait of the high-priority thread (hold %d ms, hog %d ms):\n", names[lock_type], HOLD_MS, HOG_MS);
    for (i = 0; i < rounds; i++)
    {
        low_done = 0;
        low = CreateThread(NULL, 0, low_thread, NULL, 0, NULL);
        WaitForSingleObject(low_holds, INFINITE);
        medium = CreateThread(NULL, 0, medium_thread, NULL, 0, NULL);
        WaitForSingleObject(medium_started, INFINITE);

        SetEvent(low_go);
        start = now_us();
        lock_high();
        waited = now_us() - start;
        unlock_high();
        /* the kernel's real-time throttling can let the low thread finish before we even try */
        if (low_done && waited < 1000)
            printf("  round %d: invalid, the lock was already free\n", i + 1);
        else
            printf("  round %d: %.1f ms\n", i + 1, waited / 1000.0);

        WaitForSingleObject(medium, INFINITE);
        WaitForSingleObject(low, INFINITE);
        CloseHandle(medium);
        CloseHandle(low);
        Sleep(500);  /* let the real-time budget recover */
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    DeleteCriticalSection(&cs);
    return 0;
}
