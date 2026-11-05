// Co-op tracing: a read watchpoint in software. coop_watch_arm makes the pages of a guest range inaccessible; every
// access to them faults, the handler counts the lifted function that touched the range (rt_lifted_function), lets the
// instruction run once with the page restored (trap flag) and protects the page again. Test code for finding which
// game code reads an object (docs/coop/re-notes.md); slow while armed.

#include <windows.h>
#include <stdint.h>
#include <stdio.h>

#include "host.h"
#include "layout.h"

uint64_t rt_lifted_function(uintptr_t rip);
int coop_watch_counts(void);   // coop_spawn.c: 0 skips this fault (the camera-only watch)

#define SLOTS 64
static volatile LONG armed;
static uint64_t lo, hi, page_lo, page_hi;   // the watched range and its pages
static DWORD old_prot;
static struct { volatile LONG64 fn; volatile LONG hits; volatile LONG min_off, max_off; } seen[SLOTS];
int coop_watch_writes;   // set: only writes to the range are counted
static volatile LONG outside;                // faults on the pages outside the range
static __declspec(thread) int stepping;      // this thread runs one instruction with the page open

static void note(uint64_t fn, uint64_t addr) {
    LONG off = (LONG)(addr - lo);
    for (int i = 0; i < SLOTS; i++) {
        LONG64 have = seen[i].fn;
        if (!have) have = InterlockedCompareExchange64(&seen[i].fn, (LONG64)fn, 0);
        if (!have || (uint64_t)have == fn) {
            InterlockedIncrement(&seen[i].hits);
            if (seen[i].hits == 1 || off < seen[i].min_off) seen[i].min_off = off;
            if (off > seen[i].max_off) seen[i].max_off = off;
            return;
        }
    }
}

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    // Every single-step trap is ours: nothing else in the game sets the trap flag, and a thread can take the trap after
    // its stepping mark went stale (the page was protected again by another thread before it ran the instruction).
    if (er->ExceptionCode == EXCEPTION_SINGLE_STEP) {
        stepping = 0;
        DWORD o;
        if (armed) VirtualProtect((void *)(uintptr_t)page_lo, page_hi - page_lo, PAGE_NOACCESS, &o);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uint64_t a = (uint64_t)er->ExceptionInformation[1];
    if (a < page_lo || a >= page_hi || !page_lo) return EXCEPTION_CONTINUE_SEARCH;
    if (a >= lo && a < hi && coop_watch_counts() && (!coop_watch_writes || er->ExceptionInformation[0] == 1)) note(rt_lifted_function((uintptr_t)ep->ContextRecord->Rip), a);
    else InterlockedIncrement(&outside);
    DWORD o;
    VirtualProtect((void *)(uintptr_t)page_lo, page_hi - page_lo, old_prot, &o);
    stepping = 1;
    ep->ContextRecord->EFlags |= 0x100;   // trap after the instruction: protect again in the single-step handler
    return EXCEPTION_CONTINUE_EXECUTION;
}

int coop_watch_arm(uint64_t addr, uint64_t len) {
    static int installed;
    if (armed) return 0;
    rt_lifted_function(1);   // builds its table now, not inside the handler
    if (!installed) { AddVectoredExceptionHandler(1, veh); installed = 1; }
    memset((void *)seen, 0, sizeof seen);
    outside = 0;
    lo = addr, hi = addr + len, page_lo = addr & ~0xfffull, page_hi = (addr + len + 0xfff) & ~0xfffull;
    if (!VirtualProtect((void *)(uintptr_t)page_lo, page_hi - page_lo, PAGE_NOACCESS, &old_prot)) {
        rt_log("coop: watch %llx: VirtualProtect failed (%lu)", (unsigned long long)addr, GetLastError());
        page_lo = 0;
        return 0;
    }
    armed = 1;
    rt_log("coop: watch armed on %llx+%#llx (old protection %#lx)", (unsigned long long)addr,
           (unsigned long long)len, old_prot);
    return 1;
}

void coop_watch_disarm(void) {
    if (!armed) return;
    armed = 0;
    DWORD o;
    VirtualProtect((void *)(uintptr_t)page_lo, page_hi - page_lo, old_prot, &o);
    char d[160];
    rt_log("coop: watch on %llx: %ld faults on the same pages outside it", (unsigned long long)lo, outside);
    for (int i = 0; i < SLOTS && seen[i].fn; i++)
        rt_log("coop:   %6ld reads/writes by %s at +%#lx..+%#lx", seen[i].hits,
               ldr_describe((uint64_t)seen[i].fn, d, sizeof d), seen[i].min_off, seen[i].max_off);
}

// TOTK_COOP_HIDE_CONSOLE=1: hides the console window totk.exe (a console program) opens, for co-op sessions with
// several game windows; the log still goes to logs\totk.log.
void coop_hide_console(void) {
    HWND w = GetConsoleWindow();
    if (w) ShowWindow(w, SW_HIDE);
}
