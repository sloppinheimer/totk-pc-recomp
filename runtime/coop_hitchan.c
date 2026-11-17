// The HIT channel's reader: every packet that comes on it is handed to the sync it belongs to. The channel carries
// the shared world progress (coop_gamedata.c), the rotating ground (coop_ground.c), a player's blow sent to the game
// that runs the enemy (coop_contact.c), the sync of enemies made at run time (coop_spawn_sync.c) and the travel
// notice (coop_notices.c); a packet is told by its size and the kind word each of them carries. Split out of
// coop_world.c (docs/coop/enemy-sync-spec.md B.28).
#include <stddef.h>
#include <stdint.h>

#include "coop_game.h"
#include "coop_net.h"
#include "coop_notices.h"
#include "coop_world_int.h"

void coop_ent_tick(Cpu *c, uint64_t me);                         // coop_ent_hooks.c: the entity sync
int coop_gamedata_take(const void *data, int n, int from);       // coop_gamedata.c: 1 when the packet was its own

// Once a frame while another player is in the session (from coop_world_tick).
void coop_hitchan_tick(Cpu *c, uint64_t me) {
    coop_ent_tick(c, me);
    int my_id = coop_net_id(), from = 0, n;
    static _Alignas(16) uint8_t in[COOP_HIT_MAX];
    while ((n = coop_net_hit_take(in, sizeof in, &from)) != 0) {
        if (coop_gamedata_take(in, n, from)) continue;
        const ContactPacket *cp = (const ContactPacket *)in;
        const SpawnPacket *sp = (const SpawnPacket *)in;
        if (n >= (int)offsetof(GroundPacket, g) && ((const GroundPacket *)in)->kind == GROUND_KIND) ground_take((const GroundPacket *)in, n);
        else if (n >= (int)offsetof(ContactPacket, e) && (cp->kind == ECONTACT_KIND || cp->kind == ECONTACT_ARROW) && (int)cp->to == my_id && cp->n <= CONTACT_MAX &&
                 n == (int)(offsetof(ContactPacket, e) + cp->n * CONTACT_SZ))
            coop_contact_take(cp, from);
        else if (n == (int)sizeof(SpawnPacket) && (sp->kind == SPAWN_KIND || sp->kind == SPAWN_GONE)) spawn_take(c, sp, from);
        else if (n == (int)sizeof(CoopTravelPacket) && ((const CoopTravelPacket *)in)->kind == COOP_TRAVEL_KIND)
            coop_notice_travel_take((const CoopTravelPacket *)in, n, from);
    }
    spawn_tick(c);
}
