// Co-op: session state the syncs share (coop_sess.c).
#pragma once
#include <stdint.h>

void coop_sess_phase0(void);              // once a frame, first thing at GameDataMgr::calc: the frame stamp
uint64_t coop_sess_frame_ns(void);        // the performance counter as read at this frame's phase 0
uint32_t coop_sess_frame_no(void);
int coop_debug(void);                     // TOTK_COOP_DEBUG=1: the records and periodic measurements tests read
int coop_sess_paused(void);               // phase 0 has not run for 0.3 s
int coop_sess_loading(void);              // the game's teardown flag, or no Link yet
void coop_sess_net(void);                 // the net tick

enum { COOP_SESS_RUNS, COOP_SESS_PAUSED, COOP_SESS_LOADING, COOP_SESS_SILENT, COOP_SESS_GONE };
int coop_sess_peer(int id);               // a peer by relay id, from its heartbeat: silent 1 s is paused, 3 s gone
double coop_sess_hold_s(void);            // how long a claim waits for a rival's

// The clock estimate of a peer: a sample is a stamp that peer put on a state this game took in this frame.
void coop_sess_clock_sample(int id, int64_t peer_stamp_ns);
// Once a frame per peer: the time on that peer's clock at which its states are shown now, the delay in it, and how
// often the estimate was set at once (a change of that count is a teleport for every copy). 0: no estimate yet.
int coop_sess_clock(int id, uint64_t interval_ns, int64_t *t_ns, int64_t *delay_ns, uint32_t *teleports);
