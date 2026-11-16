// Co-op: the transport's inbox and peer events shared by its readers (coop_msg.c).
#pragma once
#include <stdint.h>

#include "coop_ctr.h"

enum { COOP_MSG_ENT, COOP_MSG_ENEMY, COOP_MSG_READERS };

void coop_msg_open(int reader);                       // from then on the reader's messages and events are kept for it
int coop_msg_recv(int reader, CoopCtrMsg *m);         // as coop_ctr_recv, for that reader's types only; 1 one taken
int coop_msg_peer_event(int reader, uint64_t *inc, int *id, int *joined);   // as coop_ctr_peer_event, per reader
