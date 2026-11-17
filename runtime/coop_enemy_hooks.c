// Co-op: the enemy sync's entry hooks. coop_hooks.h COOP_HOOKED_EN registers the game functions the enemy sync uses,
// and coop_spawn.c coop_hook sends each call to coop_enemy_hook; the actor's calc (main+0x41bb20) and the action
// request (main+0x50ad74), which other co-op code hooks too, come through coop_enemy_shared first.
#include <stdint.h>

#include "coop_enemy.h"

int coop_enemy_calc(Cpu *c);   // coop_enemy.c

// The actor's calc and the action request. Nonzero: the function is not run.
int coop_enemy_shared(Cpu *c, uint64_t off) {
    if (!coop_enemy_new()) return 0;
    if (off == 0x41bb20) {
        coop_enemy_calc(c);
        return 0;
    }
    return en_copy_hook(c, off);
}

// Every other hook of COOP_HOOKED_EN. Nonzero: the function is not run.
int coop_enemy_hook(Cpu *c, uint64_t off) {
    int r = coop_enemy_new() && en_copy_hook(c, off);
    if (off == 0x1bf20cc && !r) coop_check_arm(c);   // an attack armed, and the arm call runs: the mismatch checker's record (coop_check.c)
    return r;
}
