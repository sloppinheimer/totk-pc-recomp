// The co-op texts' ids (runtime/ui_text.h includes this file at the top of UiTextId; the texts are in
// coop_text_<lang>.inc). They sit before UI_MENU_FIRST, so ov_probe.c's check of the menu's texts leaves them out.
    // the Co-op tab (ui/coop_tab.cpp): host, join, leave, who is here
    UI_MENU_TAB_COOP, UI_MENU_COOP_NAME, UI_MENU_COOP_ADDRESS,
    UI_MENU_COOP_HOST, UI_MENU_COOP_HOST_DESC, UI_MENU_COOP_JOIN, UI_MENU_COOP_JOIN_DESC,
    UI_MENU_COOP_LEAVE, UI_MENU_COOP_LEAVE_DESC,
    UI_MENU_COOP_IDLE,
    UI_MENU_COOP_HOSTING,     // "%s": this PC's LAN address
    UI_MENU_COOP_JOINING,     // "%s": the address
    UI_MENU_COOP_JOINED,      // "%s": the address, "%d": this player's number
    UI_MENU_COOP_PLAYERS,     // "%s": the other players' names
    UI_MENU_COOP_ALONE,
    UI_MENU_COOP_FAILED,      // "%s": why
    UI_MENU_COOP_NOTE,
    UI_MENU_COOP_COPY_LAN, UI_MENU_COOP_COPY_LAN_DESC, UI_MENU_COOP_COPY_NET, UI_MENU_COOP_COPY_NET_DESC,
    UI_MENU_COOP_COPIED,      // a copy button's label for a moment after it was pressed
    UI_MENU_COOP_JOIN_NEED,   // Join pressed with no address
    // the router (coop_upnp.c): "%s" the internet address, or (FAILED) why, in English
    UI_MENU_COOP_UPNP_WORKING, UI_MENU_COOP_UPNP_OPEN, UI_MENU_COOP_UPNP_OPEN_NOIP, UI_MENU_COOP_UPNP_SHARED,
    UI_MENU_COOP_UPNP_FAILED,
    // the on-screen keyboard for the name and the address
    UI_MENU_COOP_EDIT_DESC, UI_MENU_COOP_KEY_SPACE, UI_MENU_COOP_KEY_DELETE, UI_MENU_COOP_KEY_DONE, UI_MENU_COOP_KEY_PASTE,
    UI_MENU_COOP_TAGS, UI_MENU_COOP_TAGS_DESC,   // the other players' names over their Links
    // the co-op notices (coop_notices.c): %s the player's name, then the place's
    UI_MENU_COOP_NOTICE_JOINED, UI_MENU_COOP_NOTICE_HERE, UI_MENU_COOP_NOTICE_LEFT, UI_MENU_COOP_NOTICE_TRAVELLED,
    UI_MENU_COOP_NOTICE_NEAR, UI_MENU_COOP_NOTICE_TRAVELLED_SOMEWHERE, UI_MENU_COOP_NOTICE_ENTERED,
    UI_MENU_COOP_NOTICE_ENEMY_SYNC,   // that player's game runs another enemy sync (coop_enemy.c)
    // character mods (coop_chars.c): the section, this PC's, a player's missing one; the download prompt (%s player,
    // %s mod, %s file, %.0f MB, %s page); the lookup by link; progress (%.0f of %.0f MB), failure, done; a mod's toggle
    UI_MENU_COOP_CHARS, UI_MENU_COOP_CHAR_SELF, UI_MENU_COOP_CHAR_MISSING, UI_MENU_COOP_CHAR_ASK, UI_MENU_COOP_CHAR_LOOKING,
    UI_MENU_COOP_CHAR_YES, UI_MENU_COOP_CHAR_YES_DESC, UI_MENU_COOP_CHAR_NO, UI_MENU_COOP_CHAR_NO_DESC,
    UI_MENU_COOP_CHAR_LINK, UI_MENU_COOP_CHAR_LOOKUP, UI_MENU_COOP_CHAR_LOOKUP_DESC, UI_MENU_COOP_CHAR_PROGRESS,
    UI_MENU_COOP_CHAR_FAILED, UI_MENU_COOP_CHAR_RESTART, UI_MENU_COOP_CHAR_USE_DESC,
