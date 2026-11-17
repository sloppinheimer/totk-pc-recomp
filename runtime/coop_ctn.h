// Co-op: contraptions, the gates and notes the hooks call (coop_ctn.c; the entity sync is coop_ent.h, and
// docs/coop/contraption-sync-v4-spec.md is its specification).
#pragma once
#include <stdint.h>

#include "cpu.h"

int coop_ctn_core(void);                     // on unless TOTK_COOP_CONTRAPTION=0

// The entity sync's three entry points (coop_ent.c) and the mirror's answers (coop_ent_id.c).
void coop_ent_phase0(Cpu *c);                // from GameDataMgr::calc, actor phase 0
void coop_ent_drive(Cpu *c);                 // from the co-op tick: the copies moved, their devices set
void coop_ent_mirror_tick(void);             // from the co-op tick: what the object mirror knows of an actor
void coop_ent_net(void);                     // beside the transport's tick: heartbeat flags, the pause queue

// Read from any game thread: the sets coop_ent.c publishes each frame.
int coop_ent_is(uint64_t a);                 // an entity's actor (or a part of the batch being made)
int coop_contraption_made(uint64_t a);       // a copy this code made
int coop_contraption_follower(uint64_t a);   // a live entity another game owns, held under a request or not (G4)
int coop_ctn_driven(uint64_t a);             // a copy this game moves by the drive (not held under a request)
int coop_ctn_follows(uint64_t a);            // as the follower set: a hit's toggle goes to its owner
int coop_ctn_no_charge(uint64_t a);          // its devices are paid by another player
int coop_ctn_part(uint64_t a);               // a member of an island of two or more
int coop_ctn_runs_part(uint64_t a);          // an entity this game owns
int coop_ctn_parts_all(uint64_t *out, int max);   // every entity's actor here (the riders)
int coop_ctn_emitter_of_followed(uint64_t a);     // a device followed here, or a shot of one (its hits are the owner's to count)
int coop_ctn_sender_entry(uint64_t group);        // the entry of the group's on/off sender main+0x32eb90
int coop_g1(Cpu *c, uint64_t off);           // G1: writes kept off the bodies of copies
int coop_held_here(uint64_t a);              // this game's Link holds it with Ultrahand

// Notes from the hooks.
void coop_ctn_structure_event(void);         // a bond made or an edge removed here
void coop_ctn_toggle_asked(uint64_t comp, uint64_t actor);   // a hit here on a device another game owns
void coop_ctn_event_seen(uint64_t actor);
void coop_ctn_death_seen(uint64_t actor, uint32_t kind);
void coop_ctn_note_onoff(uint64_t node, uint64_t actor);
void coop_ctn_note_fuse(uint64_t node, uint64_t actor);
void coop_ctn_gauge_seen(uint64_t mgr);
