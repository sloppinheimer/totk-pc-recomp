// Shared by the system overrides: runtime/ov_system.c, ov_input.c, ov_media.c and ov_gfx.c, and their probe.
// oe_push_message, oe_set_focus and ov_nv_state are in gfx.h (the window posts focus messages, the NVN layer
// allocates through the recorded callbacks).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "gfx.h"

// ---- coop_spawn.c
void coop_tick(struct Cpu *c);   // co-op spawn test, from the gamepad poll (ov_input.c npad_states)
int coop_p2_connected(void);     // player 2's controller: Npad id 1
void coop_local_input(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);   // coop_players.c
int coop_p2_npad(void);          // report player 2 as Npad id 1 to the game (TOTK_COOP_P2_NPAD=1)
int coop_p2_poll(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);   // 1 with a state

// ---- ov_system.c
int ov_handheld(void);                     // TOTK_OPERATION_MODE=handheld (default docked); fatal on another value
const char *ov_language(void);             // the game's language, a Switch language code ("en-US", "zh-Hans", ...)
const char *ov_shared_font_dir(char *buf, size_t n);   // TOTK_SHARED_FONTS, else <romfs>/../../firmware/shared-fonts
uint64_t ov_bfttf_size(const uint8_t *p, uint64_t len); // decoded font size of a BFTTF image, 0 if it is not one
