/*
 * Unit tests for avrt (MMCSS) functions
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winternl.h"
#include "avrt.h"

#include "wine/test.h"

static void test_set_characteristics(void)
{
    HANDLE handle;
    DWORD index;
    BOOL ret;

    /* A known task with a zero index registers a new task. */
    index = 0;
    SetLastError(0xdeadbeef);
    handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
    ok(handle != NULL, "AvSetMmThreadCharacteristicsW failed, error %lu\n", GetLastError());
    ok(index != 0, "expected a non-zero task index\n");
    ret = AvRevertMmThreadCharacteristics(handle);
    ok(ret, "AvRevertMmThreadCharacteristics failed, error %lu\n", GetLastError());

    /* Task names are registry key names, so the lookup is case-insensitive. */
    index = 0;
    handle = AvSetMmThreadCharacteristicsW(L"pro audio", &index);
    ok(handle != NULL, "lowercase task name failed, error %lu\n", GetLastError());
    if (handle) AvRevertMmThreadCharacteristics(handle);

    index = 0;
    handle = AvSetMmThreadCharacteristicsA("Audio", &index);
    ok(handle != NULL, "AvSetMmThreadCharacteristicsA failed, error %lu\n", GetLastError());
    ok(index != 0, "expected a non-zero task index\n");
    if (handle) AvRevertMmThreadCharacteristics(handle);

    /* Unknown task names are rejected. */
    index = 0;
    SetLastError(0xdeadbeef);
    handle = AvSetMmThreadCharacteristicsW(L"wine-rta nonexistent task", &index);
    ok(handle == NULL, "expected failure for an unknown task\n");
    ok(GetLastError() == ERROR_INVALID_TASK_NAME, "got error %lu\n", GetLastError());
    if (handle) AvRevertMmThreadCharacteristics(handle);

    SetLastError(0xdeadbeef);
    handle = AvSetMmThreadCharacteristicsW(NULL, &index);
    ok(handle == NULL, "expected failure for a NULL task name\n");
    ok(GetLastError() == ERROR_INVALID_TASK_NAME, "got error %lu\n", GetLastError());
}

static DWORD shared_index;
static HANDLE thread_handle;

static DWORD WINAPI join_task_thread(void *arg)
{
    DWORD index = shared_index;

    thread_handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
    ok(thread_handle != NULL, "AvSetMmThreadCharacteristicsW failed, error %lu\n", GetLastError());
    ok(index == shared_index, "expected index %lu, got %lu\n", shared_index, index);
    if (thread_handle) AvRevertMmThreadCharacteristics(thread_handle);
    return 0;
}

static void test_task_index(void)
{
    HANDLE handle, thread;

    /* A second thread passing an existing index joins the same task, and
     * gets its own handle. */
    shared_index = 0;
    handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &shared_index);
    ok(handle != NULL, "AvSetMmThreadCharacteristicsW failed, error %lu\n", GetLastError());
    ok(shared_index != 0, "expected a non-zero task index\n");

    thread = CreateThread(NULL, 0, join_task_thread, NULL, 0, NULL);
    ok(thread != NULL, "CreateThread failed, error %lu\n", GetLastError());
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);

    ok(thread_handle != handle, "expected distinct handles for distinct threads\n");

    if (handle) AvRevertMmThreadCharacteristics(handle);
}

static void test_max_characteristics(void)
{
    HANDLE handle;
    DWORD index;

    /* The thread joins whichever of the two tasks has the higher priority. */
    index = 0;
    SetLastError(0xdeadbeef);
    handle = AvSetMmMaxThreadCharacteristicsW(L"Audio", L"Pro Audio", &index);
    ok(handle != NULL, "AvSetMmMaxThreadCharacteristicsW failed, error %lu\n", GetLastError());
    ok(index != 0, "expected a non-zero task index\n");
    if (handle) AvRevertMmThreadCharacteristics(handle);

    index = 0;
    handle = AvSetMmMaxThreadCharacteristicsA("Audio", "Pro Audio", &index);
    ok(handle != NULL, "AvSetMmMaxThreadCharacteristicsA failed, error %lu\n", GetLastError());
    if (handle) AvRevertMmThreadCharacteristics(handle);
}

static LONG get_current_priority(void)
{
    THREAD_BASIC_INFORMATION info;
    NTSTATUS status;

    status = NtQueryInformationThread(GetCurrentThread(), ThreadBasicInformation, &info, sizeof(info), NULL);
    ok(!status, "NtQueryInformationThread failed, status %#lx\n", status);
    return info.Priority;
}

static void test_thread_priority(void)
{
    LONG before, priority;
    HANDLE handle;
    DWORD index = 0;
    BOOL ret;

    before = get_current_priority();
    handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
    ok(handle != NULL, "AvSetMmThreadCharacteristicsW failed, error %lu\n", GetLastError());
    if (!handle) return;

    /* Windows temporarily drops MMCSS threads out of the realtime band to honour
     * SystemResponsiveness, so only check the exact values on Wine. */
    priority = get_current_priority();
    trace("priority before MMCSS: %ld, after: %ld\n", before, priority);
    if (winetest_platform_is_wine)
        ok(priority >= 16, "expected a realtime band priority, got %ld\n", priority);

    ret = AvSetMmThreadPriority(handle, AVRT_PRIORITY_CRITICAL);
    ok(ret, "AvSetMmThreadPriority(CRITICAL) failed, error %lu\n", GetLastError());
    if (winetest_platform_is_wine)
        ok(get_current_priority() > priority, "expected CRITICAL to raise the priority\n");
    ret = AvSetMmThreadPriority(handle, AVRT_PRIORITY_LOW);
    ok(ret, "AvSetMmThreadPriority(LOW) failed, error %lu\n", GetLastError());
    if (winetest_platform_is_wine)
        ok(get_current_priority() < priority, "expected LOW to lower the priority\n");

    SetLastError(0xdeadbeef);
    ret = AvSetMmThreadPriority(handle, AVRT_PRIORITY_CRITICAL + 1);
    ok(!ret, "expected failure for an out of range priority\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "got error %lu\n", GetLastError());

    /* Only trace the Windows thread priority: the exact boost MMCSS applies is
     * not documented. */
    trace("thread priority after MMCSS: %d\n", GetThreadPriority(GetCurrentThread()));

    ret = AvRevertMmThreadCharacteristics(handle);
    ok(ret, "AvRevertMmThreadCharacteristics failed, error %lu\n", GetLastError());
    priority = get_current_priority();
    ok(priority == before, "expected priority %ld after reverting, got %ld\n", before, priority);

    /* Tasks that are not audio tasks leave the priority alone. */
    index = 0;
    handle = AvSetMmThreadCharacteristicsW(L"Playback", &index);
    ok(handle != NULL, "AvSetMmThreadCharacteristicsW failed, error %lu\n", GetLastError());
    if (winetest_platform_is_wine)
        ok(get_current_priority() == before, "expected unchanged priority\n");
    if (handle) AvRevertMmThreadCharacteristics(handle);
}

static void test_revert(void)
{
    HANDLE handle;
    DWORD index = 0;
    BOOL ret;

    handle = AvSetMmThreadCharacteristicsW(L"Audio", &index);
    ok(handle != NULL, "AvSetMmThreadCharacteristicsW failed, error %lu\n", GetLastError());
    ret = AvRevertMmThreadCharacteristics(handle);
    ok(ret, "AvRevertMmThreadCharacteristics failed, error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    ret = AvRevertMmThreadCharacteristics(handle);
    ok(!ret, "expected failure for an already reverted handle\n");
    ok(GetLastError() == ERROR_INVALID_HANDLE, "got error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    ret = AvSetMmThreadPriority(handle, AVRT_PRIORITY_HIGH);
    ok(!ret, "expected failure for an already reverted handle\n");
    ok(GetLastError() == ERROR_INVALID_HANDLE, "got error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    ret = AvRevertMmThreadCharacteristics(NULL);
    ok(!ret, "expected failure for a NULL handle\n");
    ok(GetLastError() == ERROR_INVALID_HANDLE, "got error %lu\n", GetLastError());
}

static void test_system_responsiveness(void)
{
    HANDLE handle;
    DWORD index = 0;
    ULONG value = 0xdeadbeef;
    BOOL ret;

    handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
    ok(handle != NULL, "AvSetMmThreadCharacteristicsW failed, error %lu\n", GetLastError());
    if (!handle) return;

    ret = AvQuerySystemResponsiveness(handle, &value);
    ok(ret, "AvQuerySystemResponsiveness failed, error %lu\n", GetLastError());
    if (ret)
    {
        ok(value <= 100, "unexpected responsiveness value %lu\n", value);
        trace("system responsiveness: %lu\n", value);
    }

    AvRevertMmThreadCharacteristics(handle);
}

START_TEST(avrt)
{
    test_set_characteristics();
    test_task_index();
    test_max_characteristics();
    test_thread_priority();
    test_revert();
    test_system_responsiveness();
}
