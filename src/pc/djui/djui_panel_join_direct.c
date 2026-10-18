#include <stdio.h>
#include "djui.h"
#include "djui_panel.h"
#include "djui_panel_menu.h"
#include "djui_panel_modlist.h"
#include "djui_panel_join_message.h"
#include "djui_lobby_entry.h"
#include "pc/network/network.h"
#include "pc/network/socket/socket.h"
#include "pc/network/coopnet/coopnet.h"
#include "pc/utils/misc.h"
#include "pc/configfile.h"
#include "pc/debuglog.h"
#include "macros.h"

static struct DjuiInputbox* sInputboxIp = NULL;

#ifdef TARGET_WEB
// Lista de salas abiertas que devuelve el relay.
// MUY IMPORTANTE: no se crean ni se destruyen elementos en marcha (eso bloquea
// el juego). Se crean 4 botones fijos al abrir el panel y despues SOLO se les
// cambia el texto y la visibilidad.
#define WEB_ROOM_SLOTS 4

static struct DjuiButton* sRoomButtons[WEB_ROOM_SLOTS] = { NULL };
static struct DjuiText* sRoomsStateText = NULL;
static char sShownRoomLabel[WEB_ROOM_SLOTS][96] = { { 0 } };
static bool sRoomsWaiting = false;
#endif

static bool djui_panel_join_direct_ip_parse_numbers(char** msg) {
    int num = 0;
    for (int i = 0; i < 3; i++) {
        char c = **msg;
        if (c >= '0' && c <= '9') {
            // is number
            num *= 10;
            num += (c - '0');
            *msg = *msg + 1;
        } else if (i == 0) {
            return false;
        } else {
            break;
        }
    }

    return num >= 0 && num <= 255;
}

static bool djui_panel_join_direct_ip_parse_period(char** msg) {
    char c = **msg;
    bool isPeriod = (c == '.');
    if (isPeriod) { *msg = *msg + 1; }
    return isPeriod;
}

static bool djui_panel_join_direct_ip_parse_spacer(char** msg) {
    char c = **msg;
    bool isSpacer = (c == ':' || c == ' ');
    if (isSpacer) { *msg = *msg + 1; }
    return isSpacer;
}

static bool djui_panel_join_direct_ip_parse_port(char** msg) {
    int port = 0;
    for (int i = 0; i < 5; i++) {
        char c = **msg;
        if (c >= '0' && c <= '9') {
            // is number
            port *= 10;
            port += (c - '0');
            *msg = *msg + 1;
        } else if (i == 0) {
            return false;
        } else {
            break;
        }
    }

    return port <= 65535;
}

UNUSED static bool djui_panel_join_direct_ip_valid(char* buffer) {
    char** msg = &buffer;

    if (!djui_panel_join_direct_ip_parse_numbers(msg)) { return false; }
    if (!djui_panel_join_direct_ip_parse_period(msg))  { return false; }
    if (!djui_panel_join_direct_ip_parse_numbers(msg)) { return false; }
    if (!djui_panel_join_direct_ip_parse_period(msg))  { return false; }
    if (!djui_panel_join_direct_ip_parse_numbers(msg)) { return false; }
    if (!djui_panel_join_direct_ip_parse_period(msg))  { return false; }
    if (!djui_panel_join_direct_ip_parse_numbers(msg)) { return false; }
    if (djui_panel_join_direct_ip_parse_spacer(msg)) {
        if (!djui_panel_join_direct_ip_parse_port(msg)) { return false; }
    }

    return (**msg == '\0');
}

static void djui_panel_join_direct_ip_text_change(struct DjuiBase* caller) {
    struct DjuiInputbox* inputbox1 = (struct DjuiInputbox*)caller;
    struct DjuiTheme* theme = gDjuiThemes[configDjuiTheme];
    struct DjuiColor* textColor = &theme->interactables.textColor;
    if (strlen(inputbox1->buffer) > 2) {
        djui_inputbox_set_text_color(inputbox1, textColor->r, textColor->g, textColor->b, textColor->a);
    } else {
        djui_inputbox_set_text_color(inputbox1, 255, 0, 0, 255);
    }
}

static void djui_panel_join_direct_ip_text_set_new(void) {
    char buffer[256] = { 0 };
    char orig_ip[256] = { 0 };
    if (snprintf(buffer, 256, "%s", sInputboxIp->buffer) < 0) {
        LOG_INFO("truncating IP");
    }

    // copy original buffer for storing to gGetHostName
    memcpy(&orig_ip, &buffer, 256);

    bool afterSpacer = false;
    bool is_ipv6 = false;
    int port = 0;

    // check if address starts with [ (meaning it's a direct IPv6 address.
    // This is needed because we need to know when to get the port number. Example: [2001:db8::1000]:7777
    // If this character is not in the first character in the buffer, it will be treated as an IPv4 address or hostname.
    if (buffer[0] == '[') {
        memcpy(&buffer, &buffer[1], 255);
        is_ipv6 = true;
    }

    if (is_ipv6) {
        LOG_INFO("Detected direct IPv6 address");
    } else {
        LOG_INFO("Detected direct IPv4 address or hostname");
    }

    // this needs cleaning
    for (int i = 0; i < 256; i++) {
        // Direct IPv6 address
        if (is_ipv6 == true) {
            // Check if it reached end of address "]:", or a space as a fail safe.
            if ((buffer[i] == ']') || buffer[i] == ' ') {
                afterSpacer = true;
                memset(&orig_ip, 0, 256);
                memcpy(&orig_ip[1], &buffer, i+1);
                buffer[i] = '\0';
                orig_ip[0] = '[';
                // skip over the port separator
                if (buffer[i+1] == ':') {
                    i += 1;
                }
            } else if (buffer[i] == '\0') {
                break;
            } else if (afterSpacer && buffer[i] >= '0' && buffer[i] <= '9') {
                port *= 10;
                port += buffer[i] - '0';
            }
        } else {
            // Direct IPv4 address or hostname
            // Check if it reached end of address ":", or a space as a fail safe.
            if (buffer[i] == ' ' || buffer[i] == ':') {
                afterSpacer = true;
                buffer[i] = '\0';
                memcpy(&orig_ip, &buffer, i+1);
            } else if (buffer[i] == '\0') {
                break;
            } else if (afterSpacer && buffer[i] >= '0' && buffer[i] <= '9') {
                port *= 10;
                port += buffer[i] - '0';
            }
        }
    }

    snprintf(gGetHostName, MAX_CONFIG_STRING, "%s", orig_ip);
    if (snprintf(configJoinIp, MAX_CONFIG_STRING, "%s", buffer) < 0) {
        LOG_INFO("truncating IP");
    }
    if (port >= 1 && port <= 65535) {
        configJoinPort = port;
    } else {
        configJoinPort = DEFAULT_PORT;
    }
}

static void djui_panel_join_direct_ip_text_set(struct DjuiInputbox* inputbox1) {
    char buffer[256] = { 0 };
#ifdef TARGET_WEB
    // En web el campo es el NOMBRE DE LA SALA (no hay IP ni puerto): recordamos
    // la ultima sala a la que te uniste.
    snprintf(buffer, 256, "%s", configJoinIp);
#else
    if (strlen(configJoinIp) > 0 && configJoinPort != DEFAULT_PORT) {
        if (snprintf(buffer, 256, "%s:%d", configJoinIp, configJoinPort) < 0) { LOG_INFO("truncating IP"); }
    } else if (strlen(configJoinIp) > 0) {
        if (snprintf(buffer, 256, "%s", configJoinIp) < 0) { LOG_INFO("truncating IP"); }
    } else {
        if (snprintf(buffer, 256, "localhost") < 0) { LOG_INFO("truncating IP"); }
    }
#endif

    djui_inputbox_set_text(inputbox1, buffer);
}

void djui_panel_join_direct_do_join(struct DjuiBase* caller);

#ifdef TARGET_WEB
// --------------------------- lista de salas (web) ---------------------------

static void djui_panel_join_direct_room_click(struct DjuiBase* caller) {
    // Al pulsar una sala: la ponemos en el campo y entramos directamente.
    char room[64] = { 0 };
    int players = 0;
    webnet_rooms_get(caller->tag, room, sizeof(room), &players);
    if (room[0] == '\0') { return; }
    djui_inputbox_set_text(sInputboxIp, room);
    djui_panel_join_direct_do_join(NULL);
}

static void djui_panel_join_direct_rooms_refresh(UNUSED struct DjuiBase* caller) {
    webnet_rooms_fetch();
    sRoomsWaiting = true;
}

// Se llama cada fotograma desde djui_panel_update(). Solo cambia TEXTOS y
// visibilidad: crear o destruir elementos aqui (o dibujando) bloquea el juego.
void djui_panel_join_direct_poll(void) {
    if (sRoomsStateText == NULL) { return; }        // el panel no esta abierto
    if (sRoomsWaiting) {
        if (webnet_rooms_state() == 0) { return; }  // el relay aun no ha respondido
        sRoomsWaiting = false;
    }

    const int count = webnet_rooms_count();
    for (int i = 0; i < WEB_ROOM_SLOTS; i++) {
        struct DjuiButton* button = sRoomButtons[i];
        if (button == NULL) { continue; }

        char room[64] = { 0 };
        int players = 0;
        if (i < count) { webnet_rooms_get(i, room, sizeof(room), &players); }

        char label[96] = { 0 };
        if (room[0] != '\0') { snprintf(label, sizeof(label), "%s  (%d)", room, players); }

        if (strcmp(sShownRoomLabel[i], label) != 0) {
            snprintf(sShownRoomLabel[i], sizeof(sShownRoomLabel[i]), "%s", label);
            djui_text_set_text(button->text, label);
            djui_base_set_visible(&button->base, (label[0] != '\0'));
            djui_base_set_enabled(&button->base, (label[0] != '\0'));
        }
    }

    // Aviso de "no hay salas" (solo cuando ya ha respondido el relay).
    static char sShownMsg[64] = { 0 };
    const char* msg = (count > 0) ? "" : DLANG(JOIN, NO_ROOMS);
    if (strcmp(sShownMsg, msg) != 0) {
        snprintf(sShownMsg, sizeof(sShownMsg), "%s", msg);
        djui_text_set_text(sRoomsStateText, msg);
    }
}

// Al cerrar el panel soltamos las referencias (si no, quedarian apuntando a
// objetos ya destruidos).
static void djui_panel_join_direct_destroy(UNUSED struct DjuiBase* caller) {
    for (int i = 0; i < WEB_ROOM_SLOTS; i++) {
        sRoomButtons[i] = NULL;
        sShownRoomLabel[i][0] = '\0';
    }
    sRoomsStateText = NULL;
    sRoomsWaiting = false;
}
#endif

void djui_panel_join_direct_do_join(struct DjuiBase* caller) {
#ifdef TARGET_WEB
    // En web basta con el nombre de la sala (no hay IP ni puerto)
    if (!(strlen(sInputboxIp->buffer) > 0)) {
#else
    if (!(strlen(sInputboxIp->buffer) > 2)) {
#endif
        djui_interactable_set_input_focus(&sInputboxIp->base);
        djui_inputbox_select_all(sInputboxIp);
        return;
    }
    network_reset_reconnect_and_rehost();
    djui_panel_join_direct_ip_text_set_new();
    network_set_system(NS_SOCKET);
    network_init(NT_CLIENT, false);
    djui_panel_join_message_create(caller);
}

void djui_panel_join_direct_create(struct DjuiBase* caller) {
    struct DjuiBase* defaultBase = NULL;
    struct DjuiThreePanel* panel = djui_panel_menu_create(DLANG(JOIN, JOIN_TITLE), false);
    struct DjuiBase* body = djui_three_panel_get_body(panel);
    {
#ifdef TARGET_WEB
        struct DjuiText* text1 = djui_text_create(body, DLANG(JOIN, JOIN_ROOM));
#else
        struct DjuiText* text1 = djui_text_create(body, DLANG(JOIN, JOIN_SOCKET));
#endif
        djui_base_set_size_type(&text1->base, DJUI_SVT_RELATIVE, DJUI_SVT_ABSOLUTE);
        djui_base_set_size(&text1->base, 1.0f, 100);
        djui_base_compute_tree(&text1->base);
        u16 directLines = djui_text_count_lines(text1, 12);
        f32 directTextHeight = 32 * 0.8125f * directLines + 8;
        djui_base_set_size(&text1->base, 1.0f, directTextHeight);
        djui_base_set_color(&text1->base, 220, 220, 220, 255);

        struct DjuiInputbox* inputbox1 = djui_inputbox_create(body, 256);
        djui_base_set_size_type(&inputbox1->base, DJUI_SVT_RELATIVE, DJUI_SVT_ABSOLUTE);
        djui_base_set_size(&inputbox1->base, 1.0f, 32.0f);
        djui_interactable_hook_value_change(&inputbox1->base, djui_panel_join_direct_ip_text_change);
        sInputboxIp = inputbox1;
        djui_panel_join_direct_ip_text_set(inputbox1);

#ifdef TARGET_WEB
        // --------- lista de salas abiertas (la da el relay) ---------
        // Se crean 4 botones fijos (ocultos) y luego SOLO se les cambia el
        // texto: asi nunca hay que tocar el arbol mientras el juego corre.
        struct DjuiText* textRooms = djui_text_create(body, DLANG(JOIN, ROOMS_TITLE));
        djui_base_set_size_type(&textRooms->base, DJUI_SVT_RELATIVE, DJUI_SVT_ABSOLUTE);
        djui_base_set_size(&textRooms->base, 1.0f, 24);
        djui_base_set_color(&textRooms->base, 220, 220, 220, 255);
        djui_text_set_drop_shadow(textRooms, 64, 64, 64, 100);

        sRoomsStateText = djui_text_create(body, "");
        djui_base_set_size_type(&sRoomsStateText->base, DJUI_SVT_RELATIVE, DJUI_SVT_ABSOLUTE);
        djui_base_set_size(&sRoomsStateText->base, 1.0f, 22);
        djui_base_set_alignment(&sRoomsStateText->base, DJUI_HALIGN_CENTER, DJUI_VALIGN_TOP);
        djui_base_set_color(&sRoomsStateText->base, 170, 170, 170, 255);
        djui_text_set_drop_shadow(sRoomsStateText, 64, 64, 64, 100);

        for (int i = 0; i < WEB_ROOM_SLOTS; i++) {
            struct DjuiButton* roomButton = djui_button_create(body, "", DJUI_BUTTON_STYLE_NORMAL, djui_panel_join_direct_room_click);
            djui_base_set_size(&roomButton->base, 1.0f, 26);
            djui_base_set_visible(&roomButton->base, false);
            djui_base_set_enabled(&roomButton->base, false);
            roomButton->base.tag = i;   // para saber que sala se ha pulsado
            sRoomButtons[i] = roomButton;
            sShownRoomLabel[i][0] = '\0';
        }

        djui_button_create(body, DLANG(JOIN, ROOMS_REFRESH), DJUI_BUTTON_STYLE_NORMAL, djui_panel_join_direct_rooms_refresh);

        // pedimos la lista (asincrono); el poll la pinta en cuanto llegue
        webnet_rooms_fetch();
        sRoomsWaiting = true;
#endif

        struct DjuiRect* rect2 = djui_rect_container_create(body, 64);
        {
            struct DjuiButton* button1 = djui_button_create(&rect2->base, DLANG(MENU, BACK), DJUI_BUTTON_STYLE_BACK, djui_panel_menu_back);
            djui_base_set_size(&button1->base, 0.485f, 64);
            djui_base_set_alignment(&button1->base, DJUI_HALIGN_LEFT, DJUI_VALIGN_TOP);

            struct DjuiButton* button2 = djui_button_create(&rect2->base, DLANG(JOIN, JOIN), DJUI_BUTTON_STYLE_NORMAL, djui_panel_join_direct_do_join);
            djui_base_set_size(&button2->base, 0.485f, 64);
            djui_base_set_alignment(&button2->base, DJUI_HALIGN_RIGHT, DJUI_VALIGN_TOP);
            defaultBase = &button2->base;
        }
    }

    struct DjuiPanel* p = djui_panel_add(caller, panel, defaultBase);
#ifdef TARGET_WEB
    if (p) {
        p->on_panel_destroy = djui_panel_join_direct_destroy;
    } else {
        djui_panel_join_direct_destroy(NULL);
    }
#endif
}
