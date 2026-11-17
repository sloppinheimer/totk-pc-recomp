// Co-op: where the entity sync (coop_ent*.c, the contraption sync) is run from each frame. The Autobuild research
// tests (coop_autobuild.c) run after it from the same places.
#include <stdint.h>

#include "coop_ctn.h"
#include "coop_ent.h"

void coop_ent_saved_tick(Cpu *c);                 // coop_ent_id.c: the devices of a save
void coop_ent_bench_tick(Cpu *c, uint64_t me);    // coop_ent_bench.c: TOTK_COOP_BENCH
void coop_ent_log_tick(Cpu *c, uint64_t me);      // coop_ent_log.c: the session record
void coop_autobuild_seq_tests(Cpu *c);            // coop_autobuild.c
void coop_autobuild_tick(Cpu *c, uint64_t me);    // coop_autobuild.c

// From GameDataMgr::calc, at actor phase 0 outside the parallel actor update, where every actor link resolves
// (coop_spawn.c coop_hook).
void coop_ent_seq_calc(Cpu *c) {
    coop_ent_phase0(c);
    coop_autobuild_seq_tests(c);
}

// The co-op tick while another player is in the session (coop_hitchan.c coop_hitchan_tick).
void coop_ent_tick(Cpu *c, uint64_t me) {
    coop_ent_mirror_tick();   // the object mirror's answers to the entity sync (this is the tick its tables live on)
    coop_ent_drive(c);
    coop_ent_saved_tick(c);
    coop_ent_bench_tick(c, me);
    coop_ent_log_tick(c, me);
    coop_autobuild_tick(c, me);
}
