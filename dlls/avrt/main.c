/* Avrt dll implementation
 *
 * Copyright (C) 2009 Maarten Lankhorst
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winternl.h"
#include "wine/debug.h"
#include "wine/list.h"
#include "avrt.h"

WINE_DEFAULT_DEBUG_CHANNEL(avrt);

static inline WCHAR *strdupAW(const char *src)
{
    int len;
    WCHAR *dst;
    if (!src) return NULL;
    len = MultiByteToWideChar(CP_ACP, 0, src, -1, NULL, 0);
    if ((dst = malloc(len * sizeof(*dst)))) MultiByteToWideChar(CP_ACP, 0, src, -1, dst, len);
    return dst;
}

/* Default MMCSS tasks, from HKLM\\Software\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks.
 * Only the audio tasks raise the thread priority. Their NT priority approximates the Windows
 * scheduling categories: "High" (Pro Audio) maps to the 23-26 band, "Medium" (Audio) to 16-22.
 * AvSetMmThreadPriority() adds an offset of -2 to +2. */
struct mmcss_task
{
    const WCHAR *name;
    int priority;  /* NT priority for AVRT_PRIORITY_NORMAL, 0 to leave the thread priority alone */
};

static const struct mmcss_task tasks[] =
{
    { L"Audio",          19 },
    { L"Capture",         0 },
    { L"DisplayPostProcessing", 0 },
    { L"Distribution",    0 },
    { L"Games",           0 },
    { L"Playback",        0 },
    { L"Pro Audio",      24 },
    { L"Window Manager",  0 },
};

#define MMCSS_HANDLE_MAGIC 0x4d4d4353  /* "MMCS" */

struct mmcss_handle
{
    DWORD magic;
    struct list entry;
    const struct mmcss_task *task;
    HANDLE thread;          /* real handle to the registered thread */
    AVRT_PRIORITY priority;
};

static struct list handles = LIST_INIT(handles);
static LONG next_task_index;

static CRITICAL_SECTION handles_cs;
static CRITICAL_SECTION_DEBUG handles_cs_debug =
{
    0, 0, &handles_cs,
    { &handles_cs_debug.ProcessLocksList, &handles_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": handles_cs") }
};
static CRITICAL_SECTION handles_cs = { &handles_cs_debug, -1, 0, 0, 0, 0 };

static const struct mmcss_task *find_task(const WCHAR *name)
{
    unsigned int i;

    if (!name) return NULL;
    for (i = 0; i < ARRAY_SIZE(tasks); i++)
        if (!wcsicmp(name, tasks[i].name)) return &tasks[i];
    return NULL;
}

/* returns the handle with handles_cs held, or NULL with the last error set */
static struct mmcss_handle *grab_handle(HANDLE AvrtHandle)
{
    struct mmcss_handle *handle;

    EnterCriticalSection(&handles_cs);
    LIST_FOR_EACH_ENTRY(handle, &handles, struct mmcss_handle, entry)
        if (handle == AvrtHandle && handle->magic == MMCSS_HANDLE_MAGIC) return handle;
    LeaveCriticalSection(&handles_cs);

    SetLastError(ERROR_INVALID_HANDLE);
    return NULL;
}

static void release_handle(void)
{
    LeaveCriticalSection(&handles_cs);
}

static BOOL set_mmcss_priority(HANDLE thread, DWORD priority)
{
    NTSTATUS status = NtSetInformationThread(thread, ThreadWineMmcssPriority, &priority, sizeof(priority));

    if (status)
    {
        WARN("failed to set MMCSS priority %lu, status %#lx\n", priority, status);
        SetLastError(RtlNtStatusToDosError(status));
        return FALSE;
    }
    return TRUE;
}

static HANDLE register_thread(const struct mmcss_task *task, DWORD *index)
{
    struct mmcss_handle *handle;

    if (!index)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return NULL;
    }

    if (!(handle = calloc(1, sizeof(*handle))))
    {
        SetLastError(ERROR_OUTOFMEMORY);
        return NULL;
    }
    handle->magic = MMCSS_HANDLE_MAGIC;
    handle->task = task;
    handle->priority = AVRT_PRIORITY_NORMAL;

    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle->thread,
                         THREAD_SET_INFORMATION, FALSE, 0))
    {
        free(handle);
        return NULL;
    }

    if (task->priority && !set_mmcss_priority(handle->thread, task->priority))
    {
        CloseHandle(handle->thread);
        free(handle);
        return NULL;
    }

    /* a zero index registers a new task, a non-zero index joins an existing one */
    if (!*index) *index = InterlockedIncrement(&next_task_index);

    EnterCriticalSection(&handles_cs);
    list_add_tail(&handles, &handle->entry);
    LeaveCriticalSection(&handles_cs);

    TRACE("registered thread %04lx for %s, index %lu, handle %p\n",
          GetCurrentThreadId(), debugstr_w(task->name), *index, handle);
    return handle;
}

HANDLE WINAPI AvSetMmThreadCharacteristicsA(const char *name, DWORD *index)
{
    WCHAR *nameW = NULL;
    HANDLE ret;

    if (name && !(nameW = strdupAW(name)))
    {
        SetLastError(ERROR_OUTOFMEMORY);
        return NULL;
    }

    ret = AvSetMmThreadCharacteristicsW(nameW, index);

    free(nameW);
    return ret;
}

HANDLE WINAPI AvSetMmThreadCharacteristicsW(const WCHAR *name, DWORD *index)
{
    const struct mmcss_task *task;

    TRACE("(%s,%p)\n", debugstr_w(name), index);

    if (!(task = find_task(name)))
    {
        SetLastError(ERROR_INVALID_TASK_NAME);
        return NULL;
    }
    return register_thread(task, index);
}

BOOL WINAPI AvQuerySystemResponsiveness(HANDLE AvrtHandle, ULONG *value)
{
    TRACE("(%p, %p)\n", AvrtHandle, value);

    if (!grab_handle(AvrtHandle)) return FALSE;
    release_handle();

    /* the Windows default for the SystemResponsiveness registry value */
    *value = 20;
    return TRUE;
}

BOOL WINAPI AvRevertMmThreadCharacteristics(HANDLE AvrtHandle)
{
    struct mmcss_handle *handle;

    TRACE("(%p)\n", AvrtHandle);

    if (!(handle = grab_handle(AvrtHandle))) return FALSE;
    list_remove(&handle->entry);
    release_handle();

    if (handle->task->priority) set_mmcss_priority(handle->thread, 0);
    CloseHandle(handle->thread);
    free(handle);
    return TRUE;
}

BOOL WINAPI AvSetMmThreadPriority(HANDLE AvrtHandle, AVRT_PRIORITY prio)
{
    struct mmcss_handle *handle;
    BOOL ret = TRUE;

    TRACE("(%p)->(%d)\n", AvrtHandle, prio);

    if (prio < AVRT_PRIORITY_VERYLOW || prio > AVRT_PRIORITY_CRITICAL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!(handle = grab_handle(AvrtHandle))) return FALSE;
    handle->priority = prio;
    if (handle->task->priority) ret = set_mmcss_priority(handle->thread, handle->task->priority + prio);
    release_handle();
    return ret;
}

HANDLE WINAPI AvSetMmMaxThreadCharacteristicsA(const char *task1, const char *task2, DWORD *index)
{
    WCHAR *task1W = NULL, *task2W = NULL;
    HANDLE ret;

    if (task1 && !(task1W = strdupAW(task1)))
    {
        SetLastError(ERROR_OUTOFMEMORY);
        return NULL;
    }

    if (task2 && !(task2W = strdupAW(task2)))
    {
        free(task1W);
        SetLastError(ERROR_OUTOFMEMORY);
        return NULL;
    }

    ret = AvSetMmMaxThreadCharacteristicsW(task1W, task2W, index);

    free(task2W);
    free(task1W);
    return ret;
}

HANDLE WINAPI AvSetMmMaxThreadCharacteristicsW(const WCHAR *task1, const WCHAR *task2, DWORD *index)
{
    const struct mmcss_task *first, *second;

    TRACE("(%s,%s,%p)\n", debugstr_w(task1), debugstr_w(task2), index);

    if (!(first = find_task(task1)) || !(second = find_task(task2)))
    {
        SetLastError(ERROR_INVALID_TASK_NAME);
        return NULL;
    }
    return register_thread(second->priority > first->priority ? second : first, index);
}
