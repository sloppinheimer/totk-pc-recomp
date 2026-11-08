// Character mods in co-op (coop_chars.c; docs/coop/character-mods.md): which character each player is, the stand-in
// actor and bare pieces this game has for a character, and the GameBanana fetch of a character a party member uses.
#pragma once
#include <stdint.h>

#define COOP_CHAR_KEY 24   // NetState.character: "Link", "gb:<GameBanana file id>" or the mod's folder name

typedef struct {
    char key[COOP_CHAR_KEY], tag[8], actor[48], folder[256], name[128], page[128];
    uint32_t mod_id, file_id;
    char bare[3][48];   // the bare head, upper and lower actors (tools/coop/charmod.py BARE)
} CoopChar;

void coop_chars_init(void);                  // once, after mods_scan; reads mods/zz-coop-characters/characters.tsv
const CoopChar *coop_char_local(void);       // this PC's Link's character (the switched-on character mod, else Link)
const CoopChar *coop_char_find(const char *key);   // NULL when this PC has no set for it
int coop_char_count(void);
const CoopChar *coop_char_at(int i);
// The stand-in actor for a player whose character is key: that character's PlayerPuppet_<tag>, else PlayerPuppet.
const char *coop_char_actor(const char *key);
// For armor slot k (0 head, 1 body, 2 legs): the bare piece a player of character key wears here in place of their
// armor, or NULL when their armor is shown as it is (the same character as this game's own Link, or no set for it).
const char *coop_char_bare(const char *key, int k);
// 1 when an actor name is a stand-in (PlayerPuppet or PlayerPuppet_<tag>).
int coop_char_is_standin(const char *name);

// ---- the party's characters and fetching them (the Co-op tab). A player's character (key, GameBanana mod id) as their
// states say; called by coop_players.c on every state.
void coop_chars_seen(int player_id, const char *key, uint32_t mod_id);
const char *coop_chars_of(int player_id);    // "" unknown

// A fetch job: tools/coop/charmod.py run in the background (info, fetch, sets), its status read from a file.
enum { COOP_JOB_NONE, COOP_JOB_RUNNING, COOP_JOB_DONE, COOP_JOB_FAILED };
typedef struct {
    int state;
    char what[32];            // "info", "fetch", "sets"
    char text[512];           // the last status line (an error, or the result's JSON)
    uint64_t got, size;       // fetch progress, bytes
} CoopJob;
void coop_chars_job(CoopJob *out);   // a copy of the current job's state
void coop_chars_tick(void);          // every frame, the game thread (coop_players.c): polls the job, starts asks
// What to ask the player for: a party member's character this PC has no set for, with GameBanana's details of it once
// an info job got them (the prompt shows name, page, file and size). Returns 0 when nothing is to be asked.
typedef struct {
    int player_id;
    uint32_t mod_id, file_id;
    char name[128], page[128], file[128];
    uint64_t size;
    int ready;               // the details are in (an info job finished)
} CoopCharAsk;
int coop_chars_ask(CoopCharAsk *out);
void coop_chars_answer(int yes);             // the player's answer to the current ask: yes fetches it, no skips it
// A GameBanana link or mod id typed into the menu: its info job, then the same ask (player_id 0: for this PC).
void coop_chars_lookup(const char *link);
int coop_chars_restart_needed(void);         // a set was rebuilt and not added while the game runs: it needs a restart
int coop_chars_live_loaded(void);            // a character was added while the game runs (coop_chars.c live_load)
