// The Co-op tab of the in-game menu: this player's name, Host (a relay in this game, coop_relay.c, joined at
// 127.0.0.1), the host's address and Join, Leave, and who is here. The name and the last address are kept in
// totk.ini (COOP_NAME, COOP_JOIN under TOTK_); TOTK_COOP_RELAY still joins at start for the test scripts.
// The name and the address open an on-screen keyboard (the D-pad and A, or type), so a gamepad is enough.
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "ui/coop_colour.h"
#include "ui/option_rows.h"

extern "C" {
#include "cfg.h"
#include "coop_chars.h"
#include "coop_net.h"
#include "mods.h"
#include "ui_text.h"
#include "platform/platform.h"
void menu_page_modal(bool on);   // ui/menu.cpp: B and the tab keys stay with the keyboard while it is open
}

static char g_name[17], g_addr[128], g_gb[160];
static bool g_loaded;

static void load_fields(void) {
    if (g_loaded) return;
    g_loaded = true;
    const char *n = cfg_get("TOTK_COOP_NAME"), *a = cfg_get("TOTK_COOP_JOIN");
    snprintf(g_name, sizeof g_name, "%s", n && *n ? n : "Player");
    snprintf(g_addr, sizeof g_addr, "%s", a ? a : "");
    const char *gb = cfg_get("TOTK_COOP_CHAR_LINK");   // a test sets the link (the keyboard has no '/')
    if (gb) snprintf(g_gb, sizeof g_gb, "%s", gb);
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

// A field: its row shows the text. Picked with the gamepad (A), it opens the on-screen keyboard; with the mouse or
// the keyboard (a click, Enter), the row becomes a text box to type in, done on Enter or a click elsewhere.
static const char *inline_id;   // the field being typed in, NULL none
static void field(const char *id, UiTextId label, char *buf, size_t n, float scale) {
    if (inline_id && !strcmp(inline_id, id)) {
        static bool focused;
        ImGui::TextUnformatted(menu_text(label));
        ImGui::SameLine(260 * scale);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (!focused) ImGui::SetKeyboardFocusHere(), focused = true;
        char iid[64];
        snprintf(iid, sizeof iid, "##in%s", id);
        bool enter = ImGui::InputText(iid, buf, n, ImGuiInputTextFlags_EnterReturnsTrue);
        if (enter || ImGui::IsItemDeactivated()) {
            save_fields();
            inline_id = nullptr, focused = false;
            opt_focus_key(id);   // the cursor back on the row
        }
        return;
    }
    if (opt_text_row(id, menu_text(label), buf[0] ? buf : "-", nullptr, menu_text(UI_MENU_COOP_EDIT_DESC), false) == 2) {
        bool pad = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) || ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown);
        if (pad) {
            kb_buf = buf, kb_cap = n, kb_upper = false;
            ImGui::OpenPopup("##coop_kbd");
        } else
            inline_id = id;
    }
}

// An action that looks like one: a filled button the width of the page, with what it does under it.
static bool action(const char *id, UiTextId label, UiTextId desc, float scale, bool danger = false) {
    char l[160];
    // A copy button reads "Copied" for 2 s after it was pressed (copied_id, copied_at: ImGui's clock).
    extern const char *copied_id;
    extern double copied_at;
    bool copied = copied_id && !strcmp(copied_id, id) && ImGui::GetTime() - copied_at < 2;
    snprintf(l, sizeof l, "%s%s", menu_text(copied ? UI_MENU_COOP_COPIED : label), id);
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

const char *copied_id;
double copied_at;
// A button that puts text on the clipboard (the tester: the host's address with a copy button).
static void copy_button(const char *id, UiTextId label, UiTextId desc, const char *text, float scale) {
    if (action(id, label, desc, scale) && plat_clipboard_set(text)) copied_id = id, copied_at = ImGui::GetTime();
}

// Character mods (coop_chars.c; docs/coop/character-mods.md): this PC's character and each player's, the prompt to
// download a party member's character from GameBanana (it waits for a yes), a GameBanana link to look one up, and a
// switch per character mod for this PC's Link (it applies at the next start, as the Mods tab's switches do).
static void characters_section(float scale) {
    static char line[640];
    opt_group(UI_MENU_COOP_CHARS);
    snprintf(line, sizeof line, menu_text(UI_MENU_COOP_CHAR_SELF), coop_char_local()->name);
    opt_text_str(line);
    int ids[COOP_MAX_PLAYERS];
    char names[COOP_MAX_PLAYERS][17];
    int n = *coop_net_target() ? coop_net_players(ids, names, COOP_MAX_PLAYERS) : 0;
    const char *ask_name = "?";
    CoopCharAsk ask;
    int asking = coop_chars_ask(&ask);
    for (int i = 0; i < n; i++) {
        const char *key = coop_chars_of(ids[i]);
        const CoopChar *c = coop_char_find(key[0] ? key : "Link");
        char what[200];
        if (c) snprintf(what, sizeof what, "%s", c->name);
        else snprintf(what, sizeof what, menu_text(UI_MENU_COOP_CHAR_MISSING),
                      asking && ask.ready && ask.player_id == ids[i] && ask.name[0] ? ask.name : key);
        if (asking && ask.player_id == ids[i]) ask_name = names[i];
        ImGui::TextColored(coop_colour_v4(ids[i]), "%s", names[i][0] ? names[i] : "?");
        ImGui::SameLine(0, 0);
        ImGui::Text(": %s", key[0] ? what : "...");
    }
    if (asking) {
        if (!ask.ready) {
            snprintf(line, sizeof line, menu_text(UI_MENU_COOP_CHAR_LOOKING), ask.page[0] ? ask.page : "...");
            opt_text_str(line);
        } else {
            snprintf(line, sizeof line, menu_text(UI_MENU_COOP_CHAR_ASK), ask.player_id ? ask_name : coop_char_local()->name,
                     ask.name, ask.file, (double)ask.size / 1e6, ask.page);
            opt_text_str(line, true);
            if (action("##coop_char_yes", UI_MENU_COOP_CHAR_YES, UI_MENU_COOP_CHAR_YES_DESC, scale)) coop_chars_answer(1);
            if (action("##coop_char_no", UI_MENU_COOP_CHAR_NO, UI_MENU_COOP_CHAR_NO_DESC, scale, true)) coop_chars_answer(0);
        }
    }
    CoopJob jobv;
    coop_chars_job(&jobv);
    const CoopJob *job = &jobv;
    if (job->state == COOP_JOB_RUNNING && !strcmp(job->what, "fetch")) {
        snprintf(line, sizeof line, menu_text(UI_MENU_COOP_CHAR_PROGRESS), (double)job->got / 1e6, (double)job->size / 1e6);
        opt_text_str(line);
        if (job->text[0]) opt_text_str(job->text);
    } else if (job->state == COOP_JOB_FAILED) {
        snprintf(line, sizeof line, menu_text(UI_MENU_COOP_CHAR_FAILED), job->text);
        opt_text_str(line, true);
    } else if (coop_chars_restart_needed())
        opt_text(UI_MENU_COOP_CHAR_RESTART);
    field("##coop_gb", UI_MENU_COOP_CHAR_LINK, g_gb, sizeof g_gb, scale);
    if (g_gb[0] && !asking && job->state != COOP_JOB_RUNNING &&
        action("##coop_gb_look", UI_MENU_COOP_CHAR_LOOKUP, UI_MENU_COOP_CHAR_LOOKUP_DESC, scale))
        coop_chars_lookup(g_gb);
    // A switch per character mod: on makes it this PC's Link's (the others off), from the next start.
    for (int k = 0; k < coop_char_count(); k++) {
        const CoopChar *c = coop_char_at(k);
        if (!c->folder[0]) continue;
        int mi = -1;
        for (int i = 0; i < mods_count(); i++) if (!strcmp(mods_at(i)->name, c->folder)) mi = i;
        if (mi < 0) continue;
        char id[300];
        snprintf(id, sizeof id, "##coop_char_%s", c->folder);
        if (opt_toggle_text(id, c->name, menu_text(UI_MENU_COOP_CHAR_USE_DESC), mods_at(mi)->enabled)) {
            int on = !mods_at(mi)->enabled;
            for (int j = 0; j < coop_char_count(); j++) {   // one character at a time
                const CoopChar *o = coop_char_at(j);
                for (int i = 0; on && o->folder[0] && i < mods_count(); i++)
                    if (i != mi && !strcmp(mods_at(i)->name, o->folder)) mods_set_enabled(i, 0);
            }
            mods_set_enabled(mi, on);
            opt_save_soon();
        }
    }
}

extern "C" void tab_coop(float scale) {
    load_fields();
    opt_begin("##coop", scale);
    static char line[512];
    const char *target = coop_net_target();
    int hosting = coop_relay_port() != 0, id = coop_net_id();
    if (hosting) {
        // This PC's address, asked once a second, not twice a frame (each ask opens and closes a socket; code review,
        // 2026-11-06).
        static char ip_seen[64];
        static int have_ip;
        static uint64_t ip_at;
        uint64_t now_ns = plat_now_ns();
        if (!ip_at || now_ns - ip_at > 1000000000ull) have_ip = coop_local_address(ip_seen, sizeof ip_seen), ip_at = now_ns;
        char ip[64];
        if (have_ip) snprintf(ip, sizeof ip, "%s", ip_seen);
        else snprintf(ip, sizeof ip, "this PC's address");
        snprintf(line, sizeof line, menu_text(UI_MENU_COOP_HOSTING), ip);
        opt_text_str(line);
        char detail[160];
        int up = coop_upnp_status(detail, sizeof detail);
        static const UiTextId kUpnp[] = {UI_MENU_COOP_UPNP_WORKING, UI_MENU_COOP_UPNP_WORKING, UI_MENU_COOP_UPNP_OPEN,
                                         UI_MENU_COOP_UPNP_OPEN_NOIP, UI_MENU_COOP_UPNP_SHARED, UI_MENU_COOP_UPNP_FAILED};
        snprintf(line, sizeof line, menu_text(kUpnp[up]), detail);
        opt_text_str(line, up == COOP_UPNP_FAILED || up == COOP_UPNP_SHARED);
        char addr[80];   // what a joining player types: the address, and the port when it is not the usual one
        if (coop_relay_port() == COOP_PORT) snprintf(addr, sizeof addr, "%s", ip);
        else snprintf(addr, sizeof addr, "%s:%d", ip, coop_relay_port());
        if (have_ip) copy_button("##coop_copy_lan", UI_MENU_COOP_COPY_LAN, UI_MENU_COOP_COPY_LAN_DESC, addr, scale);
        if (up == COOP_UPNP_OPEN && detail[0]) {
            if (coop_relay_port() == COOP_PORT) snprintf(addr, sizeof addr, "%s", detail);
            else snprintf(addr, sizeof addr, "%s:%d", detail, coop_relay_port());
            copy_button("##coop_copy_net", UI_MENU_COOP_COPY_NET, UI_MENU_COOP_COPY_NET_DESC, addr, scale);
        }
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
            // "Here: %s" with each name in its player's colour (ui/coop_colour.h): the catalog text before and after
            // the %s, and between them the names, one piece of text each on the same line
            const char *fmt = menu_text(UI_MENU_COOP_PLAYERS), *at = strstr(fmt, "%s");
            const char *after = at ? at + 2 : "";
            if (!at) ImGui::TextUnformatted(fmt), ImGui::SameLine();
            else if (at > fmt) ImGui::TextUnformatted(fmt, at), ImGui::SameLine(0, 0);
            for (int i = 0; i < n; i++) {
                if (i) ImGui::TextUnformatted(", "), ImGui::SameLine(0, 0);
                ImGui::TextColored(coop_colour_v4(ids[i]), "%s (%d)", names[i][0] ? names[i] : "?", ids[i]);
                if (i + 1 < n || *after) ImGui::SameLine(0, 0);
            }
            if (*after) ImGui::TextUnformatted(after);
        }
        if (action("##coop_leave", UI_MENU_COOP_LEAVE, UI_MENU_COOP_LEAVE_DESC, scale, true)) coop_end();
    } else {
        field("##coop_name", UI_MENU_COOP_NAME, g_name, sizeof g_name, scale);
        if (action("##coop_host", UI_MENU_COOP_HOST, UI_MENU_COOP_HOST_DESC, scale)) {
            save_fields();
            coop_host(g_name);
        }
        field("##coop_addr", UI_MENU_COOP_ADDRESS, g_addr, sizeof g_addr, scale);
        // Join shows from the start (the tester); pressed with no address, it says to type one.
        static double need_at = -10;
        if (action("##coop_join", UI_MENU_COOP_JOIN, UI_MENU_COOP_JOIN_DESC, scale)) {
            if (!g_addr[0]) need_at = ImGui::GetTime();
            else {
                save_fields();
                coop_net_join(g_addr, g_name);
            }
        }
        if (!g_addr[0] && ImGui::GetTime() - need_at < 4) opt_text(UI_MENU_COOP_JOIN_NEED);
    }
    // Show player names (coop_tags; drawn by ui/coop_tags.cpp): on unless set to off.
    const char *tags = cfg_get("TOTK_COOP_TAGS");
    bool tags_on = !(tags && !strcmp(tags, "off"));
    if (opt_toggle_text("##coop_tags", menu_text(UI_MENU_COOP_TAGS), menu_text(UI_MENU_COOP_TAGS_DESC), tags_on)) {
        cfg_set("TOTK_COOP_TAGS", tags_on ? "off" : "on");
        opt_save_soon();
    }
    characters_section(scale);
    opt_text(UI_MENU_COOP_NOTE);
    static bool test_kbd;   // TOTK_COOP_TEST_KBD: the keyboard opens on the name the first time the tab shows (tests)
    if (!test_kbd && cfg_get("TOTK_COOP_TEST_KBD")) {
        test_kbd = true, kb_buf = g_name, kb_cap = sizeof g_name;   // =inline: the text box a mouse or keyboard gets
        if (!strcmp(cfg_get("TOTK_COOP_TEST_KBD"), "inline")) inline_id = "##coop_name";
        else ImGui::OpenPopup("##coop_kbd");
    }
    keyboard(scale);
    opt_end();
}
