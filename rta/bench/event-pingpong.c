/*
 * Event wake-up latency benchmark for wine-rta.
 *
 * Two threads ping-pong through a pair of auto-reset events (the primitives
 * the VST3 SDK's FCondition uses on Windows) and report round-trip latency.
 * Compare runs with and without /dev/ntsync, and with WINE_RT_PRIO=1.
 *
 * Build: x86_64-w64-mingw32-gcc -O2 -o event-pingpong.exe event-pingpong.c -lavrt
 * Run:   wine event-pingpong.exe [iterations] [--mmcss]
 */

#include <windows.h>
#include <avrt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HANDLE ping, pong;
static int iterations = 20000;
static int use_mmcss;

static HANDLE enter_mmcss(void)
{
    DWORD index = 0;
    HANDLE handle;

    if (!use_mmcss) return NULL;
    if (!(handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index)))
        fprintf(stderr, "AvSetMmThreadCharacteristicsW failed: %lu\n", GetLastError());
    return handle;
}

static DWORD WINAPI responder(void *arg)
{
    HANDLE mmcss = enter_mmcss();
    int i;

    for (i = 0; i < iterations; i++)
    {
        WaitForSingleObject(ping, INFINITE);
        SetEvent(pong);
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    return 0;
}

static int compare(const void *a, const void *b)
{
    LONGLONG x = *(const LONGLONG *)a, y = *(const LONGLONG *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    LARGE_INTEGER freq, start, end;
    LONGLONG *samples;
    HANDLE thread, mmcss;
    double us;
    int i;

    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--mmcss")) use_mmcss = 1;
        else iterations = atoi(argv[i]);
    }
    if (iterations < 100) iterations = 100;

    samples = malloc(iterations * sizeof(*samples));
    ping = CreateEventW(NULL, FALSE, FALSE, NULL);
    pong = CreateEventW(NULL, FALSE, FALSE, NULL);
    QueryPerformanceFrequency(&freq);
    us = 1e6 / (double)freq.QuadPart;

    mmcss = enter_mmcss();
    thread = CreateThread(NULL, 0, responder, NULL, 0, NULL);

    for (i = 0; i < iterations; i++)
    {
        QueryPerformanceCounter(&start);
        SetEvent(ping);
        WaitForSingleObject(pong, INFINITE);
        QueryPerformanceCounter(&end);
        samples[i] = end.QuadPart - start.QuadPart;
    }

    WaitForSingleObject(thread, INFINITE);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);

    qsort(samples, iterations, sizeof(*samples), compare);
    printf("event round trip over %d iterations%s (microseconds):\n",
           iterations, use_mmcss ? ", MMCSS Pro Audio" : "");
    printf("  min %.1f  median %.1f  p99 %.1f  p99.9 %.1f  max %.1f\n",
           samples[0] * us, samples[iterations / 2] * us, samples[iterations * 99 / 100] * us,
           samples[iterations * 999 / 1000] * us, samples[iterations - 1] * us);
    return 0;
}
