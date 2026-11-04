// The Co-op tab of the in-game menu: this player's name, Host (a relay in this game, coop_relay.c, joined at
// 127.0.0.1), the host's address and Join, Leave, and who is here. The name and the last address are kept in
// totk.ini (COOP_NAME, COOP_JOIN under TOTK_); TOTK_COOP_RELAY still joins at start for the test scripts.
// The name and the address open an on-screen keyboard (the D-pad and A, or type), so a gamepad is enough.
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "ui/option_rows.h"

extern "C" {
#include "cfg.h"
#include "coop_net.h"
#include "ui_text.h"
void menu_page_modal(bool on);   // ui/menu.cpp: B and the tab keys stay with the keyboard while it is open
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

// ---- the on-screen keyboard: a popup over the page, for the field picked (kb_buf)
static char *kb_buf;
static size_t kb_cap;
static bool kb_upper, kb_open;

static void keyboard(float scale) {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("##coop_kbd", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) {
        if (kb_open) menu_page_modal(false), kb_open = false;
        return;
    }
    kb_open = true;
    menu_page_modal(true);
    ImGui::SetNextItemWidth(560 * scale);
    ImGui::InputText("##kbd_text", kb_buf, kb_cap);   // a keyboard types here (click it); the keys below for a pad
    static const char *const rows[] = {"1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm.:_"};
    float kw = 50 * scale, kh = 46 * scale;
    bool first = ImGui::IsWindowAppearing();
    for (const char *row : rows) {
        for (const char *k = row; *k; k++) {
            char c = kb_upper && *k >= 'a' && *k <= 'z' ? (char)(*k - 32) : *k, label[8];
            snprintf(label, sizeof label, "%c##k%c", c, *k);
            if (k != row) ImGui::SameLine();
            if (ImGui::Button(label, ImVec2(kw, kh))) {
                size_t l = strlen(kb_buf);
                if (l + 1 < kb_cap) kb_buf[l] = c, kb_buf[l + 1] = 0;
            }
            if (first && k == rows[1]) ImGui::SetItemDefaultFocus(), first = false;
        }
    }
    float wide = kw * 2.5f + ImGui::GetStyle().ItemSpacing.x;
    if (ImGui::Button("Aa##kshift", ImVec2(kw * 1.5f, kh))) kb_upper = !kb_upper;
    ImGui::SameLine();
    if (ImGui::Button(menu_text(UI_MENU_COOP_KEY_SPACE), ImVec2(wide, kh))) {
        size_t l = strlen(kb_buf);
        if (l + 1 < kb_cap) kb_buf[l] = ' ', kb_buf[l + 1] = 0;
    }
    ImGui::SameLine();
    if (ImGui::Button(menu_text(UI_MENU_COOP_KEY_DELETE), ImVec2(wide, kh))) {
        size_t l = strlen(kb_buf);
        if (l) kb_buf[l - 1] = 0;
    }
    ImGui::SameLine();
    bool done = ImGui::Button(menu_text(UI_MENU_COOP_KEY_DONE), ImVec2(wide, kh));
    if (done || ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
        save_fields();
        ImGui::CloseCurrentPopup();
        menu_page_modal(false), kb_open = false;
    }
    ImGui::EndPopup();
}

// A field: its row shows the text; A or a click opens the keyboard on it.
static void field(const char *id, UiTextId label, char *buf, size_t n) {
    if (opt_text_row(id, menu_text(label), buf[0] ? buf : "-", nullptr, menu_text(UI_MENU_COOP_EDIT_DESC), false) == 2) {
        kb_buf = buf, kb_cap = n, kb_upper = false;
        ImGui::OpenPopup("##coop_kbd");
    }
}

// An action that looks like one: a filled button the width of the page, with what it does under it.
static bool action(const char *id, UiTextId label, UiTextId desc, float scale, bool danger = false) {
    char l[160];
    snprintf(l, sizeof l, "%s%s", menu_text(label), id);
    ImVec4 base = danger ? ImVec4(0.55f, 0.20f, 0.18f, 1) : ImVec4(0.80f, 0.60f, 0.20f, 1);
    ImGui::PushStyleColor(ImGuiCol_Button, base);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(base.x + 0.12f, base.y + 0.12f, base.z + 0.08f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(base.x + 0.18f, base.y + 0.18f, base.z + 0.12f, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, danger ? ImVec4(1, 1, 1, 1) : ImVec4(0.10f, 0.08f, 0.05f, 1));
    ImGui::Spacing();
    bool pressed = ImGui::Button(l, ImVec2(ImGui::GetContentRegionAvail().x, 52 * scale));
    ImGui::PopStyleColor(4);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", menu_text(desc));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    return pressed;
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
        if (action("##coop_leave", UI_MENU_COOP_LEAVE, UI_MENU_COOP_LEAVE_DESC, scale, true)) coop_end();
    } else {
        field("##coop_name", UI_MENU_COOP_NAME, g_name, sizeof g_name);
        if (action("##coop_host", UI_MENU_COOP_HOST, UI_MENU_COOP_HOST_DESC, scale)) {
            save_fields();
            coop_host(g_name);
        }
        field("##coop_addr", UI_MENU_COOP_ADDRESS, g_addr, sizeof g_addr);
        if (g_addr[0] && action("##coop_join", UI_MENU_COOP_JOIN, UI_MENU_COOP_JOIN_DESC, scale)) {
            save_fields();
            coop_net_join(g_addr, g_name);
        }
    }
    // Show player names (coop_tags; drawn by ui/coop_tags.cpp): on unless set to off.
    const char *tags = cfg_get("TOTK_COOP_TAGS");
    bool tags_on = !(tags && !strcmp(tags, "off"));
    if (opt_toggle_text("##coop_tags", menu_text(UI_MENU_COOP_TAGS), menu_text(UI_MENU_COOP_TAGS_DESC), tags_on)) {
        cfg_set("TOTK_COOP_TAGS", tags_on ? "off" : "on");
        opt_save_soon();
    }
    opt_text(UI_MENU_COOP_NOTE);
    static bool test_kbd;   // TOTK_COOP_TEST_KBD: the keyboard opens on the name the first time the tab shows (tests)
    if (!test_kbd && cfg_get("TOTK_COOP_TEST_KBD")) {
        test_kbd = true, kb_buf = g_name, kb_cap = sizeof g_name;
        ImGui::OpenPopup("##coop_kbd");
    }
    keyboard(scale);
    opt_end();
}
