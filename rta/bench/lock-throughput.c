/*
 * Lock throughput benchmark for wine-rta: critical sections and SRW locks,
 * uncontended and contended. Compare WINE_RT_PRIO=1 with and without WINE_RT_PI=0.
 *
 * Build: x86_64-w64-mingw32-gcc -O2 -o lock-throughput.exe lock-throughput.c
 * Run:   wine lock-throughput.exe
 */

#include <windows.h>
#include <stdio.h>

#define ITERATIONS        10000000
#define THREAD_ITERATIONS 2000000

static CRITICAL_SECTION cs;
static SRWLOCK srw = SRWLOCK_INIT;
static volatile LONG counter, stop, reads;
static LARGE_INTEGER freq;

static double now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / freq.QuadPart;
}

static DWORD WINAPI cs_hammer(void *arg)
{
    int i;
    for (i = 0; i < THREAD_ITERATIONS; i++)
    {
        EnterCriticalSection(&cs);
        counter++;
        LeaveCriticalSection(&cs);
    }
    return 0;
}

static DWORD WINAPI srw_hammer(void *arg)
{
    int i;
    for (i = 0; i < THREAD_ITERATIONS; i++)
    {
        AcquireSRWLockExclusive(&srw);
        counter++;
        ReleaseSRWLockExclusive(&srw);
    }
    return 0;
}

static DWORD WINAPI srw_reader(void *arg)
{
    while (!stop)
    {
        AcquireSRWLockShared(&srw);
        if (counter < 0) counter = 0;
        ReleaseSRWLockShared(&srw);
        InterlockedIncrement(&reads);
    }
    return 0;
}

static void run_two(LPTHREAD_START_ROUTINE func, const char *name)
{
    HANDLE threads[2];
    double t;

    counter = 0;
    t = now();
    threads[0] = CreateThread(NULL, 0, func, NULL, 0, NULL);
    threads[1] = CreateThread(NULL, 0, func, NULL, 0, NULL);
    WaitForMultipleObjects(2, threads, TRUE, INFINITE);
    printf("%-40s %8.1f ns/pair  (counter %ld, expected %d)\n", name,
           (now() - t) * 1e9 / (2 * THREAD_ITERATIONS), counter, 2 * THREAD_ITERATIONS);
    CloseHandle(threads[0]);
    CloseHandle(threads[1]);
}

int main(void)
{
    HANDLE threads[4];
    double t;
    int i;

    QueryPerformanceFrequency(&freq);
    InitializeCriticalSection(&cs);

    t = now();
    for (i = 0; i < ITERATIONS; i++) { EnterCriticalSection(&cs); LeaveCriticalSection(&cs); }
    printf("%-40s %8.1f ns/pair\n", "critical section, uncontended", (now() - t) * 1e9 / ITERATIONS);
    run_two(cs_hammer, "critical section, 2 threads");

    t = now();
    for (i = 0; i < ITERATIONS; i++) { AcquireSRWLockExclusive(&srw); ReleaseSRWLockExclusive(&srw); }
    printf("%-40s %8.1f ns/pair\n", "SRW exclusive, uncontended", (now() - t) * 1e9 / ITERATIONS);
    t = now();
    for (i = 0; i < ITERATIONS; i++) { AcquireSRWLockShared(&srw); ReleaseSRWLockShared(&srw); }
    printf("%-40s %8.1f ns/pair\n", "SRW shared, uncontended", (now() - t) * 1e9 / ITERATIONS);
    run_two(srw_hammer, "SRW exclusive, 2 threads");

    counter = reads = stop = 0;
    for (i = 1; i < 4; i++) threads[i] = CreateThread(NULL, 0, srw_reader, NULL, 0, NULL);
    t = now();
    threads[0] = CreateThread(NULL, 0, srw_hammer, NULL, 0, NULL);
    WaitForSingleObject(threads[0], INFINITE);
    t = now() - t;
    stop = 1;
    WaitForMultipleObjects(3, threads + 1, TRUE, INFINITE);
    printf("%-40s %8.1f ns/pair  (counter %ld, expected %d), %.1f M reads/s\n",
           "SRW 1 writer + 3 readers, writer", t * 1e9 / THREAD_ITERATIONS, counter, THREAD_ITERATIONS,
           reads / t / 1e6);

    DeleteCriticalSection(&cs);
    return 0;
}
