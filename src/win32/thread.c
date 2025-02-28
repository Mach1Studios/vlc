/*****************************************************************************
 * thread.c : Win32 back-end for LibVLC
 *****************************************************************************
 * Copyright (C) 1999-2016 VLC authors and VideoLAN
 *
 * Authors: Jean-Marc Dressler <polux@via.ecp.fr>
 *          Samuel Hocevar <sam@zoy.org>
 *          Gildas Bazin <gbazin@netcourrier.com>
 *          Clément Sténac
 *          Rémi Denis-Courmont
 *          Pierre Ynard
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#define _DECL_DLLMAIN
#include <vlc_common.h>

#include "libvlc.h"
#include <stdarg.h>
#include <stdatomic.h>
#include <stdnoreturn.h>
#include <assert.h>
#include <limits.h>
#include <errno.h>
#include <time.h>
#ifndef VLC_WINSTORE_APP
#include <mmsystem.h>
#endif
#include <vlc_atomic.h>

/*** Static mutex and condition variable ***/
static SRWLOCK super_lock = SRWLOCK_INIT;

#ifndef VLC_WINSTORE_APP
# define IS_INTERRUPTIBLE (1)
#else
# define IS_INTERRUPTIBLE (_WIN32_WINNT >= 0x0A00)
#endif

/*** Threads ***/
static thread_local struct vlc_thread *current_thread_ctx = NULL;

struct vlc_thread
{
    HANDLE         id;

    bool           killable;
    atomic_uint    killed;
    vlc_cleanup_t *cleaners;

    void        *(*entry) (void *);
    void          *data;
};

/*** Thread-specific variables (TLS) ***/
struct vlc_threadvar
{
    DWORD                 id;
    void                (*destroy) (void *);
    struct vlc_threadvar *prev;
    struct vlc_threadvar *next;
} *vlc_threadvar_last = NULL;

int vlc_threadvar_create (vlc_threadvar_t *p_tls, void (*destr) (void *))
{
    struct vlc_threadvar *var = malloc (sizeof (*var));
    if (unlikely(var == NULL))
        return errno;

    var->id = TlsAlloc();
    if (var->id == TLS_OUT_OF_INDEXES)
    {
        free (var);
        return EAGAIN;
    }
    var->destroy = destr;
    var->next = NULL;
    *p_tls = var;

    AcquireSRWLockExclusive(&super_lock);
    var->prev = vlc_threadvar_last;
    if (var->prev)
        var->prev->next = var;

    vlc_threadvar_last = var;
    ReleaseSRWLockExclusive(&super_lock);
    return 0;
}

void vlc_threadvar_delete (vlc_threadvar_t *p_tls)
{
    struct vlc_threadvar *var = *p_tls;

    AcquireSRWLockExclusive(&super_lock);
    if (var->prev != NULL)
        var->prev->next = var->next;

    if (var->next != NULL)
        var->next->prev = var->prev;
    else
        vlc_threadvar_last = var->prev;

    ReleaseSRWLockExclusive(&super_lock);

    TlsFree (var->id);
    free (var);
}

int vlc_threadvar_set (vlc_threadvar_t key, void *value)
{
    int saved = GetLastError ();

    if (!TlsSetValue(key->id, value))
        return ENOMEM;

    SetLastError(saved);
    return 0;
}

void *vlc_threadvar_get (vlc_threadvar_t key)
{
    int saved = GetLastError ();
    void *value = TlsGetValue (key->id);

    SetLastError(saved);
    return value;
}

static void vlc_threadvars_cleanup(void)
{
    vlc_threadvar_t key;
retry:
    /* TODO: use RW lock or something similar */
    AcquireSRWLockExclusive(&super_lock);
    for (key = vlc_threadvar_last; key != NULL; key = key->prev)
    {
        void *value = vlc_threadvar_get(key);
        if (value != NULL && key->destroy != NULL)
        {
            ReleaseSRWLockExclusive(&super_lock);
            vlc_threadvar_set(key, NULL);
            key->destroy(value);
            goto retry;
        }
    }
    ReleaseSRWLockExclusive(&super_lock);
}

/*** Futeces^WAddress waits ***/
#if (_WIN32_WINNT < _WIN32_WINNT_WIN8)
static BOOL (WINAPI *WaitOnAddress_)(VOID volatile *, PVOID, SIZE_T, DWORD);
#define WaitOnAddress (*WaitOnAddress_)
static VOID (WINAPI *WakeByAddressAll_)(PVOID);
#define WakeByAddressAll (*WakeByAddressAll_)
static VOID (WINAPI *WakeByAddressSingle_)(PVOID);
#define WakeByAddressSingle (*WakeByAddressSingle_)

static struct wait_addr_bucket
{
    SRWLOCK lock;
    CONDITION_VARIABLE wait;
} wait_addr_buckets[32];

static struct wait_addr_bucket *wait_addr_get_bucket(void volatile *addr)
{
    uintptr_t u = (uintptr_t)addr;

    return wait_addr_buckets + ((u >> 3) % ARRAY_SIZE(wait_addr_buckets));
}

static BOOL WINAPI WaitOnAddressFallback(void volatile *addr, void *value,
                                         SIZE_T size, DWORD ms)
{
    struct wait_addr_bucket *bucket = wait_addr_get_bucket(addr);
    uint64_t futex, val = 0;
    BOOL ret = FALSE;

    AcquireSRWLockExclusive(&bucket->lock);

    switch (size)
    {
        case 1:
            futex = atomic_load_explicit((atomic_char *)addr,
                                         memory_order_relaxed);
            val = *(const char *)value;
            break;
        case 2:
            futex = atomic_load_explicit((atomic_short *)addr,
                                         memory_order_relaxed);
            val = *(const short *)value;
            break;
        case 4:
            futex = atomic_load_explicit((atomic_int *)addr,
                                         memory_order_relaxed);
            val = *(const int *)value;
            break;
        case 8:
            futex = atomic_load_explicit((atomic_llong *)addr,
                                         memory_order_relaxed);
            val = *(const long long *)value;
            break;
        default:
            vlc_assert_unreachable();
    }

    if (futex == val)
        ret = SleepConditionVariableSRW(&bucket->wait, &bucket->lock, ms, 0);

    ReleaseSRWLockExclusive(&bucket->lock);
    return ret;
}

static void WINAPI WakeByAddressFallback(void *addr)
{
    struct wait_addr_bucket *bucket = wait_addr_get_bucket(addr);

    /* Acquire the bucket critical section (only) to enforce proper sequencing.
     * The critical section does not protect any actual memory object. */
    AcquireSRWLockExclusive(&bucket->lock);
    /* No other threads can hold the lock for this bucket while it is held
     * here. Thus any other thread either:
     * - is already sleeping in SleepConditionVariableCS(), and to be woken up
     *   by the following WakeAllConditionVariable(), or
     * - has yet to retrieve the value at the wait address (with the
     *   'switch (size)' block). */
    ReleaseSRWLockExclusive(&bucket->lock);
    /* At this point, other threads can retrieve the value at the wait address.
     * But the value will have already been changed by our call site, thus
     * (futex == val) will be false, and the threads will not go to sleep. */

    /* Wake up any thread that was already sleeping. Since there are more than
     * one wait address per bucket, all threads must be woken up :-/ */
    WakeAllConditionVariable(&bucket->wait);
}

static void vlc_wait_addr_init(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(wait_addr_buckets); i++)
    {
        struct wait_addr_bucket *bucket = wait_addr_buckets + i;

        InitializeSRWLock(&bucket->lock);
        InitializeConditionVariable(&bucket->wait);
    }
    WaitOnAddress_ = WaitOnAddressFallback;
    WakeByAddressAll_ = WakeByAddressFallback;
    WakeByAddressSingle_ = WakeByAddressFallback;
}
#endif

void vlc_atomic_wait(void *addr, unsigned val)
{
    WaitOnAddress(addr, &val, sizeof (val), INFINITE);
}

int vlc_atomic_timedwait(void *addr, unsigned val, vlc_tick_t deadline)
{
    vlc_tick_t delay;

    do
    {
        long ms;

        delay = deadline - vlc_tick_now();

        if (delay < 0)
            break; // deadline passed
        if (delay >= VLC_TICK_FROM_MS(LONG_MAX))
            ms = LONG_MAX;
        else
            ms = MS_FROM_VLC_TICK(delay);

        if (WaitOnAddress(addr, &val, sizeof (val), ms))
            return 0;
    }
    while (delay > 0);

    return ETIMEDOUT;
}

int vlc_atomic_timedwait_daytime(void *addr, unsigned val, time_t deadline)
{
    long delay;

    do
    {
        long ms;

        delay = deadline - time(NULL);

        if (delay < 0)
            break; // deadline passed
        if (delay >= (LONG_MAX / 1000))
            ms = LONG_MAX;
        else
            ms = delay * 1000;

        if (WaitOnAddress(addr, &val, sizeof (val), ms))
            return 0;
    }
    while (delay > 0);

    return ETIMEDOUT;
}

void vlc_atomic_notify_one(void *addr)
{
    WakeByAddressSingle(addr);
}

void vlc_atomic_notify_all(void *addr)
{
    WakeByAddressAll(addr);
}

/*** Threads ***/
static
#ifdef VLC_WINSTORE_APP
DWORD
#else // !VLC_WINSTORE_APP
unsigned
#endif // !VLC_WINSTORE_APP
__stdcall vlc_entry (void *p)
{
    struct vlc_thread *th = p;

    current_thread_ctx = th;
    th->killable = true;
    th->data = th->entry (th->data);
    current_thread_ctx = NULL;

    return 0;
}

int vlc_clone (vlc_thread_t *p_handle, void *(*entry) (void *),
               void *data, int priority)
{
    struct vlc_thread *th = malloc (sizeof (*th));
    if (unlikely(th == NULL))
        return ENOMEM;
    th->entry = entry;
    th->data = data;
    th->killable = false; /* not until vlc_entry() ! */
    atomic_init(&th->killed, false);
    th->cleaners = NULL;

    HANDLE h;
#ifdef VLC_WINSTORE_APP
    h = CreateThread(NULL, 0, vlc_entry, th, 0, NULL);
#else // !VLC_WINSTORE_APP
    /* When using the MSVCRT C library you have to use the _beginthreadex
     * function instead of CreateThread, otherwise you'll end up with
     * memory leaks and the signal functions not working (see Microsoft
     * Knowledge Base, article 104641) */
    h = (HANDLE)(uintptr_t) _beginthreadex (NULL, 0, vlc_entry, th, 0, NULL);
#endif // !VLC_WINSTORE_APP
    if (h == 0)
    {
        int err = errno;
        free (th);
        return err;
    }

    th->id = h;

    if (p_handle != NULL)
        *p_handle = th;

    if (priority)
        SetThreadPriority (th->id, priority);

    return 0;
}

void vlc_join (vlc_thread_t th, void **result)
{
    DWORD ret;

    do
    {
        ret = WaitForSingleObject(th->id, INFINITE);
        assert(ret != WAIT_ABANDONED_0);
    }
    while (ret == WAIT_FAILED);

    if (result != NULL)
        *result = th->data;
    CloseHandle (th->id);
    free(th);
}

unsigned long vlc_thread_id (void)
{
    return GetCurrentThreadId ();
}

int vlc_set_priority (vlc_thread_t th, int priority)
{
    if (!SetThreadPriority (th->id, priority))
        return VLC_EGENERIC;
    return VLC_SUCCESS;
}

/*** Thread cancellation ***/

#if IS_INTERRUPTIBLE
/* APC procedure for thread cancellation */
static void CALLBACK vlc_cancel_self (ULONG_PTR self)
{
    (void) self;
}
#endif

void vlc_cancel (vlc_thread_t th)
{
    atomic_store_explicit(&th->killed, true, memory_order_release);
    vlc_atomic_notify_one(&th->killed);

#if IS_INTERRUPTIBLE
    QueueUserAPC (vlc_cancel_self, th->id, (uintptr_t)th);
#endif
}

int vlc_savecancel (void)
{
    struct vlc_thread *th = current_thread_ctx;
    if (th == NULL)
        return false; /* Main thread - cannot be cancelled anyway */

    int state = th->killable;
    th->killable = false;
    return state;
}

void vlc_restorecancel (int state)
{
    struct vlc_thread *th = current_thread_ctx;
    assert (state == false || state == true);

    if (th == NULL)
        return; /* Main thread - cannot be cancelled anyway */

    assert (!th->killable);
    th->killable = state != 0;
}

noreturn static void vlc_docancel(struct vlc_thread *th)
{
    th->killable = false; /* Do not re-enter cancellation cleanup */

    for (vlc_cleanup_t *p = th->cleaners; p != NULL; p = p->next)
        p->proc (p->data);

    th->data = VLC_THREAD_CANCELED;
#ifdef VLC_WINSTORE_APP
    ExitThread(0);
#else // !VLC_WINSTORE_APP
    _endthreadex(0);
#endif // !VLC_WINSTORE_APP
}

void vlc_testcancel (void)
{
    struct vlc_thread *th = current_thread_ctx;
    if (th == NULL)
        return; /* Main thread - cannot be cancelled anyway */
    if (!th->killable)
        return;
    if (!atomic_load_explicit(&th->killed, memory_order_relaxed))
        return;

    vlc_docancel(th);
}

void vlc_control_cancel (vlc_cleanup_t *cleaner)
{
    /* NOTE: This function only modifies thread-specific data, so there is no
     * need to lock anything. */

    struct vlc_thread *th = current_thread_ctx;
    if (th == NULL)
        return; /* Main thread - cannot be cancelled anyway */

    if (cleaner != NULL)
    {
        /* cleaner is a pointer to the caller stack, no need to allocate
            * and copy anything. As a nice side effect, this cannot fail. */
        cleaner->next = th->cleaners;
        th->cleaners = cleaner;
    }
    else
    {
        th->cleaners = th->cleaners->next;
    }
}

/*** Clock ***/
static union
{
    struct
    {
        LARGE_INTEGER freq;
    } perf;
#ifndef VLC_WINSTORE_APP
    struct
    {
        MMRESULT (WINAPI *timeGetDevCaps)(LPTIMECAPS ptc,UINT cbtc);
        DWORD (WINAPI *timeGetTime)(void);
    } multimedia;
#endif
} clk;

static vlc_tick_t mdate_interrupt (void)
{
    ULONGLONG ts;
    BOOL ret;

    ret = QueryUnbiasedInterruptTime (&ts);
    if (unlikely(!ret))
        abort ();

    /* hundreds of nanoseconds */
    static_assert ((10000000 % CLOCK_FREQ) == 0, "Broken frequencies ratio");
    return ts / (10000000 / CLOCK_FREQ);
}

static vlc_tick_t mdate_tick (void)
{
    ULONGLONG ts = GetTickCount64 ();

    /* milliseconds */
    static_assert ((CLOCK_FREQ % 1000) == 0, "Broken frequencies ratio");
    return VLC_TICK_FROM_MS( ts );
}
#ifndef VLC_WINSTORE_APP
static vlc_tick_t mdate_multimedia (void)
{
    DWORD ts = clk.multimedia.timeGetTime ();

    /* milliseconds */
    static_assert ((CLOCK_FREQ % 1000) == 0, "Broken frequencies ratio");
    return VLC_TICK_FROM_MS( ts );
}
#endif

static vlc_tick_t mdate_perf (void)
{
    /* We don't need the real date, just the value of a high precision timer */
    LARGE_INTEGER counter;
    if (!QueryPerformanceCounter (&counter))
        abort ();

    /* Convert to from (1/freq) to microsecond resolution */
    /* We need to split the division to avoid 63-bits overflow */
    return vlc_tick_from_frac(counter.QuadPart, clk.perf.freq.QuadPart);
}

static vlc_tick_t mdate_wall (void)
{
    FILETIME ts;
    ULARGE_INTEGER s;

#if (_WIN32_WINNT >= _WIN32_WINNT_WIN8) && (!defined(VLC_WINSTORE_APP) || _WIN32_WINNT >= 0x0A00)
    GetSystemTimePreciseAsFileTime (&ts);
#else
    GetSystemTimeAsFileTime (&ts);
#endif
    s.LowPart = ts.dwLowDateTime;
    s.HighPart = ts.dwHighDateTime;
    /* hundreds of nanoseconds */
    static_assert ((10000000 % CLOCK_FREQ) == 0, "Broken frequencies ratio");
    return VLC_TICK_FROM_MSFTIME(s.QuadPart);
}

static vlc_tick_t mdate_default(void)
{
    vlc_threads_setup(NULL);
    return mdate_perf();
}

static vlc_tick_t (*mdate_selected) (void) = mdate_default;

vlc_tick_t vlc_tick_now (void)
{
    return mdate_selected ();
}

void (vlc_tick_wait)(vlc_tick_t deadline)
{
    vlc_tick_t delay;
#if (_WIN32_WINNT >= _WIN32_WINNT_WIN8)
    struct vlc_thread *th = current_thread_ctx;

    if (th != NULL && th->killable)
    {
        do
        {
            if (atomic_load_explicit(&th->killed, memory_order_acquire))
                vlc_docancel(th);
        }
        while (vlc_atomic_timedwait(&th->killed, false, deadline) == 0);

        return;
    }
#else
    vlc_testcancel();
#endif

    while ((delay = (deadline - vlc_tick_now())) > 0)
    {
        delay = (delay + (1000-1)) / 1000;
        if (unlikely(delay > 0x7fffffff))
            delay = 0x7fffffff;

#if (_WIN32_WINNT >= _WIN32_WINNT_WIN8)
        Sleep(delay);
#else
        SleepEx(delay, TRUE);
        vlc_testcancel();
#endif
    }
}

void (vlc_tick_sleep)(vlc_tick_t delay)
{
    vlc_tick_wait (vlc_tick_now () + delay);
}

static BOOL SelectClockSource(vlc_object_t *obj)
{
#ifdef VLC_WINSTORE_APP
    const char *name = "perf";
#else
    const char *name = "multimedia";
#endif
    char *str = NULL;
    if (obj != NULL)
        str = var_InheritString(obj, "clock-source");
    if (str != NULL)
        name = str;
    if (!strcmp (name, "interrupt"))
    {
        msg_Dbg (obj, "using interrupt time as clock source");
        mdate_selected = mdate_interrupt;
    }
    else
    if (!strcmp (name, "tick"))
    {
        msg_Dbg (obj, "using Windows time as clock source");
        mdate_selected = mdate_tick;
    }
#ifndef VLC_WINSTORE_APP
    else
    if (!strcmp (name, "multimedia"))
    {
        TIMECAPS caps;
        MMRESULT (WINAPI * timeBeginPeriod)(UINT);

        HMODULE hWinmm = LoadLibrary(TEXT("winmm.dll"));
        if (!hWinmm)
            goto perf;

        clk.multimedia.timeGetDevCaps = (void*)GetProcAddress(hWinmm, "timeGetDevCaps");
        clk.multimedia.timeGetTime = (void*)GetProcAddress(hWinmm, "timeGetTime");
        if (!clk.multimedia.timeGetDevCaps || !clk.multimedia.timeGetTime)
            goto perf;

        msg_Dbg (obj, "using multimedia timers as clock source");
        if (clk.multimedia.timeGetDevCaps (&caps, sizeof (caps)) != MMSYSERR_NOERROR)
            goto perf;
        msg_Dbg (obj, " min period: %u ms, max period: %u ms",
                 caps.wPeriodMin, caps.wPeriodMax);
        mdate_selected = mdate_multimedia;

        timeBeginPeriod = (void*)GetProcAddress(hWinmm, "timeBeginPeriod");
        if (timeBeginPeriod != NULL)
            timeBeginPeriod(5);
    }
#endif
    else
    if (!strcmp (name, "perf"))
    {
    perf:
        msg_Dbg (obj, "using performance counters as clock source");
        if (!QueryPerformanceFrequency (&clk.perf.freq))
            abort ();
        msg_Dbg (obj, " frequency: %llu Hz", clk.perf.freq.QuadPart);
        mdate_selected = mdate_perf;
    }
    else
    if (!strcmp (name, "wall"))
    {
        msg_Dbg (obj, "using system time as clock source");
        mdate_selected = mdate_wall;
    }
    else
    {
        msg_Err (obj, "invalid clock source \"%s\"", name);
        abort ();
    }
    free (str);
    return TRUE;
}


/*** CPU ***/
unsigned vlc_GetCPUCount (void)
{
    SYSTEM_INFO systemInfo;

    GetNativeSystemInfo(&systemInfo);

    return systemInfo.dwNumberOfProcessors;
}


/*** Initialization ***/
static SRWLOCK setup_lock = SRWLOCK_INIT; /* FIXME: use INIT_ONCE */

void vlc_threads_setup(libvlc_int_t *vlc)
{
    AcquireSRWLockExclusive(&setup_lock);
    if (mdate_selected != mdate_default)
    {
        ReleaseSRWLockExclusive(&setup_lock);
        return;
    }

    if (!SelectClockSource((vlc != NULL) ? VLC_OBJECT(vlc) : NULL))
        abort();
    assert(mdate_selected != mdate_default);

#ifndef VLC_WINSTORE_APP
    /* Raise default priority of the current process */
#ifndef ABOVE_NORMAL_PRIORITY_CLASS
#   define ABOVE_NORMAL_PRIORITY_CLASS 0x00008000
#endif
    if (var_InheritBool(vlc, "high-priority"))
    {
        if (SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS)
         || SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS))
            msg_Dbg(vlc, "raised process priority");
        else
            msg_Dbg(vlc, "could not raise process priority");
    }
#endif
    ReleaseSRWLockExclusive(&setup_lock);
}

#define LOOKUP(s) (((s##_) = (void *)GetProcAddress(h, #s)) != NULL)

BOOL WINAPI DllMain (HANDLE hinstDll, DWORD fdwReason, LPVOID lpvReserved)
{
    (void) hinstDll;
    (void) lpvReserved;

    switch (fdwReason)
    {
        case DLL_PROCESS_ATTACH:
        {
#if (_WIN32_WINNT < _WIN32_WINNT_WIN8)
            HMODULE h = GetModuleHandle(TEXT("api-ms-win-core-synch-l1-2-0.dll"));
            if (h == NULL || !LOOKUP(WaitOnAddress)
             || !LOOKUP(WakeByAddressAll) || !LOOKUP(WakeByAddressSingle))
            {
                vlc_wait_addr_init();
            }
#endif
            break;
        }

        case DLL_THREAD_DETACH:
            vlc_threadvars_cleanup();
            break;
    }
    return TRUE;
}
