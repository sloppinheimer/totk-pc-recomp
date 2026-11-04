// The Co-op tab of the in-game menu: this player's name, Host (a relay in this game, coop_relay.c, joined at
// 127.0.0.1), the host's address and Join, Leave, and who is here. The name and the last address are kept in
// totk.ini (COOP_NAME, COOP_JOIN under TOTK_); TOTK_COOP_RELAY still joins at start for the test scripts.
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "ui/option_rows.h"

extern "C" {
#include "cfg.h"
#include "coop_net.h"
#include "ui_text.h"
}

static char g_name[17], g_addr[128];
static bool g_loaded;

static void load_fields(void) {
    if (g_loaded) return;
    g_loaded = true;
    const char *n = cfg_get("TOTK_COOP_NAME"), *a = cfg_get("TOTK_COOP_JOIN");
    snprintf(g_name, sizeof g_name, "%s", n && *n ? n : "Player");
    snprintf(g_addr, sizeof g_addr, "%s", a ? a : "");
}

static void save_fields(void) {
    cfg_set("TOTK_COOP_NAME", g_name);
    cfg_set("TOTK_COOP_JOIN", g_addr);
    opt_save_soon();
}

// A labelled text field on its own line (keyboard input).
static void field(const char *id, UiTextId label, char *buf, size_t n, float scale) {
    ImGui::TextUnformatted(menu_text(label));
    ImGui::SameLine(260 * scale);
    ImGui::SetNextItemWidth(320 * scale);
    if (ImGui::InputText(id, buf, n)) save_fields();
}

extern "C" void tab_coop(float scale) {
    load_fields();
    opt_begin("##coop", scale);
    static char line[512];
    const char *target = coop_net_target();
    int hosting = coop_relay_port() != 0, id = coop_net_id();
    if (hosting) {
        char ip[64];
        if (!coop_local_address(ip, sizeof ip)) snprintf(ip, sizeof ip, "this PC's address");
        snprintf(line, sizeof line, menu_text(UI_MENU_COOP_HOSTING), ip);
        opt_text_str(line);
        char detail[160];
        int up = coop_upnp_status(detail, sizeof detail);
        static const UiTextId kUpnp[] = {UI_MENU_COOP_UPNP_WORKING, UI_MENU_COOP_UPNP_WORKING, UI_MENU_COOP_UPNP_OPEN,
                                         UI_MENU_COOP_UPNP_OPEN_NOIP, UI_MENU_COOP_UPNP_SHARED, UI_MENU_COOP_UPNP_FAILED};
        snprintf(line, sizeof line, menu_text(kUpnp[up]), detail);
        opt_text_str(line, up == COOP_UPNP_FAILED || up == COOP_UPNP_SHARED);
    } else if (*target && id) {
        snprintf(line, sizeof line, menu_text(UI_MENU_COOP_JOINED), target, id);
        opt_text_str(line);
    } else if (*target) {
        snprintf(line, sizeof line, menu_text(UI_MENU_COOP_JOINING), target);
        opt_text_str(line);
    } else {
        if (*coop_net_error()) {
            snprintf(line, sizeof line, menu_text(UI_MENU_COOP_FAILED), coop_net_error());
            opt_text_str(line, true);
        }
        opt_text(UI_MENU_COOP_IDLE);
    }
    if (*target) {
        int ids[COOP_MAX_PLAYERS];
        char names[COOP_MAX_PLAYERS][17];
        int n = coop_net_players(ids, names, COOP_MAX_PLAYERS);
        if (!n) opt_text(UI_MENU_COOP_ALONE);
        else {
            char list[256] = "";
            for (int i = 0; i < n; i++) {
                size_t l = strlen(list);
                snprintf(list + l, sizeof list - l, "%s%s (%d)", i ? ", " : "", names[i][0] ? names[i] : "?", ids[i]);
            }
            snprintf(line, sizeof line, menu_text(UI_MENU_COOP_PLAYERS), list);
            opt_text_str(line);
        }
        if (opt_button("##coop_leave", UI_MENU_COOP_LEAVE, UI_MENU_COOP_LEAVE_DESC)) {
            coop_end();
        }
    } else {
        field("##coop_name", UI_MENU_COOP_NAME, g_name, sizeof g_name, scale);
        if (opt_button("##coop_host", UI_MENU_COOP_HOST, UI_MENU_COOP_HOST_DESC)) {
            save_fields();
            coop_host(g_name);
        }
        field("##coop_addr", UI_MENU_COOP_ADDRESS, g_addr, sizeof g_addr, scale);
        if (g_addr[0] && opt_button("##coop_join", UI_MENU_COOP_JOIN, UI_MENU_COOP_JOIN_DESC)) {
            save_fields();
            coop_net_join(g_addr, g_name);
        }
    }
    opt_text(UI_MENU_COOP_NOTE);
    opt_end();
}
