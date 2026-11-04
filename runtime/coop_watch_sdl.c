// The co-op read watchpoint is Windows-only (coop_watch_win.c).
#include <stdint.h>
int coop_watch_arm(uint64_t addr, uint64_t len) { (void)addr, (void)len; return 0; }
void coop_watch_disarm(void) {}
void coop_hide_console(void) {}
