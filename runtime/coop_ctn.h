// Co-op: contraptions (coop_ctn.c; docs/coop/contraption-sync-v3.md section 27).
#pragma once
#include <stdint.h>

#include "cpu.h"

void coop_ctn_phase0(Cpu *c);   // from GameDataMgr::calc, actor phase 0: records and copy making
void coop_ctn_tick(Cpu *c);     // from the co-op tick: messages, authority, drive, devices

// Read from any game thread.
int coop_contraption_made(uint64_t a);       // a part made here from a record
int coop_contraption_follower(uint64_t a);   // a made part of a build this game does not run, or one being made
int coop_ctn_driven(uint64_t a);             // a part of a build another game runs (not held here)
int coop_ctn_no_charge(uint64_t a);          // a part whose devices another player pays for
int coop_g1(Cpu *c, uint64_t off);           // G1: writes kept off the bodies of builds another game runs
void coop_g1_counts(uint32_t out[9]);
void coop_ctn_toggle_asked(uint64_t comp, uint64_t actor);   // a hit here on a device of a build another game runs
void coop_ctn_sender_entry(uint64_t group);                  // the entry of the group's on/off sender main+0x32eb90

// A made part by the numbers of the game whose object it was (coop_players.c, coop_bonds.c).
int coop_contraption_ident(uint64_t a, int *owner, uint32_t *id);
uint64_t coop_contraption_find(int owner, uint32_t id);
int coop_contraption_owns(int from, uint32_t id);

int coop_ctn_first(int run_here, uint64_t parts[21]);   // (tests) the first live build this game runs / follows
