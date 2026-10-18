#include <stdbool.h>
#include <ultra64.h>
#include <time.h>
#include <string.h>
#include "pc/configfile.h"
#include "controller_api.h"

#include "../configfile.h"
#include "controller_keyboard.h"

#include "pc/gfx/gfx_window_manager_api.h"
#include "pc/pc_main.h"
#include "engine/math_util.h"
#include "menu/file_select.h"
#include "pc/djui/djui.h"
#include "pc/djui/djui_panel_pause.h"

#ifdef TARGET_WEB
#include <emscripten.h>
#include "pc/djui/djui_chat_box.h"

// ---------------------------------------------------------------------------
// Touch controls (phones and tablets)
//
// The web page shows an on-screen N64 pad and keeps its state in
// globalThis.__sm64touch = { buttons, stickX, stickY }, where "buttons" is the
// usual N64 bit mask (A_BUTTON, Z_TRIG, U_CBUTTONS...) and the stick goes from
// -128 to 127. Reading it here, instead of faking key presses, means the page
// never has to know which key each button is bound to, and the stick stays
// analog (proper walking speed) instead of "full speed or nothing".
//
// It is merged with the keyboard, so both can even be used at the same time.
// ---------------------------------------------------------------------------

EM_JS(int, web_touch_buttons, (void), {
    var T = globalThis.__sm64touch;
    return T ? (T.buttons | 0) : 0;
});

EM_JS(int, web_touch_axis, (int which), {
    var T = globalThis.__sm64touch;
    if (!T) { return 0; }
    var v = which ? T.stickY : T.stickX;
    if (typeof v !== 'number' || !isFinite(v)) { return 0; }
    var r = Math.round(v);
    if (r > 127) { r = 127; }
    if (r < -128) { r = -128; }
    return r | 0;
});

// Acciones de un solo toque (de momento, el chat). La pagina las encola en
// globalThis.__sm64touch.actions y el juego las consume aqui abajo.
#define WEB_TOUCH_ACTION_CHAT 1

EM_JS(int, web_touch_action, (void), {
    var T = globalThis.__sm64touch;
    if (!T || !T.actions || T.actions.length === 0) { return 0; }
    return T.actions.shift() | 0;
});

// La pagina necesita saber si el chat esta abierto para ensenar u ocultar el
// teclado del movil a la vez (Android lo mantiene mientras un input tenga el
// foco). El juego es el unico que lo sabe: el chat se cierra solo al enviar.
EM_JS(void, web_touch_report_chat, (int open), {
    var T = globalThis.__sm64touch;
    if (T) { T.chatOpen = !!open; }
});
#endif

static int keyboard_buttons_down;

#define MAX_KEYBINDS 64
static int keyboard_mapping[MAX_KEYBINDS][2];
static int num_keybinds = 0;

static u32 keyboard_lastkey = VK_INVALID;

static int keyboard_map_scancode(int scancode) {
    int ret = 0;
    for (int i = 0; i < num_keybinds; i++) {
        if (keyboard_mapping[i][0] == scancode) {
            ret |= keyboard_mapping[i][1];
        }
    }
    return ret;
}

bool keyboard_on_key_down(int scancode) {
    djui_panel_pause_disconnect_key_update(scancode);

    // see if interactable captures this scancode
    if (djui_interactable_on_key_down(scancode)) {
        keyboard_lastkey = scancode;
        return FALSE;
    }

    int mapped = keyboard_map_scancode(scancode);
    keyboard_buttons_down |= mapped;
    keyboard_lastkey = scancode;
    return mapped != 0;
}

bool keyboard_on_key_up(int scancode) {
    djui_interactable_on_key_up(scancode);

    int mapped = keyboard_map_scancode(scancode);
    keyboard_buttons_down &= ~mapped;
    if (keyboard_lastkey == (u32) scancode)
        keyboard_lastkey = VK_INVALID;
    return mapped != 0;
}

void keyboard_on_all_keys_up(void) {
    keyboard_buttons_down = 0;
}

void keyboard_on_text_input(char* text) {
    djui_interactable_on_text_input(text);
}

void keyboard_on_text_editing(char* text, int cursorPos) {
    djui_interactable_on_text_editing(text, cursorPos);
}

static void keyboard_add_binds(int mask, unsigned int *scancode) {
    for (int i = 0; i < MAX_BINDS && num_keybinds < MAX_KEYBINDS; ++i) {
        if (scancode[i] < VK_BASE_KEYBOARD + VK_SIZE) {
            keyboard_mapping[num_keybinds][0] = scancode[i];
            keyboard_mapping[num_keybinds][1] = mask;
            num_keybinds++;
        }
    }
}

static void keyboard_bindkeys(void) {
    bzero(keyboard_mapping, sizeof(keyboard_mapping));
    num_keybinds = 0;

    keyboard_add_binds(STICK_UP,     configKeyStickUp);
    keyboard_add_binds(STICK_LEFT,   configKeyStickLeft);
    keyboard_add_binds(STICK_DOWN,   configKeyStickDown);
    keyboard_add_binds(STICK_RIGHT,  configKeyStickRight);
    keyboard_add_binds(A_BUTTON,     configKeyA);
    keyboard_add_binds(B_BUTTON,     configKeyB);
    keyboard_add_binds(X_BUTTON,     configKeyX);
    keyboard_add_binds(Y_BUTTON,     configKeyY);
    keyboard_add_binds(Z_TRIG,       configKeyZ);
    keyboard_add_binds(U_CBUTTONS,   configKeyCUp);
    keyboard_add_binds(L_CBUTTONS,   configKeyCLeft);
    keyboard_add_binds(D_CBUTTONS,   configKeyCDown);
    keyboard_add_binds(R_CBUTTONS,   configKeyCRight);
    keyboard_add_binds(L_TRIG,       configKeyL);
    keyboard_add_binds(R_TRIG,       configKeyR);
    keyboard_add_binds(START_BUTTON, configKeyStart);
    keyboard_add_binds(U_JPAD,       configKeyDUp);
    keyboard_add_binds(D_JPAD,       configKeyDDown);
    keyboard_add_binds(L_JPAD,       configKeyDLeft);
    keyboard_add_binds(R_JPAD,       configKeyDRight);
}

static void keyboard_init(void) {
    keyboard_bindkeys();
}

static void keyboard_read(OSContPad *pad) {
    pad->button |= keyboard_buttons_down;
    const u32 xstick = keyboard_buttons_down & STICK_XMASK;
    const u32 ystick = keyboard_buttons_down & STICK_YMASK;
    if (xstick == STICK_LEFT)
        pad->stick_x = -128;
    else if (xstick == STICK_RIGHT)
        pad->stick_x = 127;
    if (ystick == STICK_DOWN)
        pad->stick_y = -128;
    else if (ystick == STICK_UP)
        pad->stick_y = 127;

#ifdef TARGET_WEB
    // On-screen pad of the web page (touch controls). It wins over the keyboard
    // only for what the player is actually pressing.
    const int touchButtons = web_touch_buttons();
    if (touchButtons != 0) {
        pad->button |= (u16) touchButtons;
    }
    const int touchX = web_touch_axis(0);
    const int touchY = web_touch_axis(1);
    if (touchX != 0) { pad->stick_x = (s8) touchX; }
    if (touchY != 0) { pad->stick_y = (s8) touchY; }

    // One shot actions coming from the page. This is the one place the game
    // walks through once per frame, so it is where the page's queued actions
    // are drained. The chat button opens and closes the chat exactly like the
    // chat key does, so it keeps working if the player rebinds that key.
    web_touch_report_chat(gDjuiChatBoxFocus ? 1 : 0);
    int touchAction = web_touch_action();
    while (touchAction != 0) {
        if (touchAction == WEB_TOUCH_ACTION_CHAT) {
            djui_chat_box_toggle();
        }
        touchAction = web_touch_action();
    }
#endif
}

static u32 keyboard_rawkey(void) {
    const u32 ret = keyboard_lastkey;
    keyboard_lastkey = VK_INVALID;
    return ret;
}

static void keyboard_shutdown(void) {
}

struct ControllerAPI controller_keyboard = {
    VK_BASE_KEYBOARD,
    keyboard_init,
    keyboard_read,
    keyboard_rawkey,
    NULL,
    NULL,
    keyboard_bindkeys,
    keyboard_shutdown
};
