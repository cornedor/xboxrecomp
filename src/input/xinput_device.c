/**
 * Xbox Input Compatibility Layer
 *
 * Translates the Xbox controller API to a host gamepad backend.
 * Handles the structural differences between the Xbox gamepad
 * (analog buttons as bytes, separate trigger channels) and the host
 * (digital face buttons, trigger axes).
 *
 *   _WIN32 -> Windows XInput
 *   POSIX  -> SDL2 GameController
 */

#include "xinput_xbox.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>


/* ---- bindings: keys.ini ---------------------------------------------------
 *
 * Every control's keyboard key and controller source, with today's defaults,
 * overridable from keys.ini in the working directory (RECOMP_KEYS=<file>
 * names another). One binding per line, '#' or ';' starts a comment:
 *
 *   a = Space            keyboard: a letter/digit, Enter Space Backspace Tab
 *   start = Enter        Shift Ctrl Alt Esc Up Down Left Right Home End
 *   lstick_left = Left   PageUp PageDown Insert Delete Numpad0-9 F1-F12, 0xNN
 *   pad.black = rb       controller: up down left right start back ls rs
 *   pad.white = lb       a b x y lb rb lt rt (buttons only; sticks stay)
 *
 * Controls: up down left right start back ls rs a b x y black white lt rt,
 * and for keys also lstick_left/right/up/down, rstick_left/right/up/down.
 * Read once, the first time input is asked for; each line is logged. */
enum { C_UP, C_DOWN, C_LEFT, C_RIGHT, C_START, C_BACK, C_LS, C_RS,
       C_A, C_B, C_X, C_Y, C_BLACK, C_WHITE, C_LT, C_RT,
       C_LSL, C_LSR, C_LSD, C_LSU, C_RSL, C_RSR, C_RSD, C_RSU, C_COUNT };
static const char *const k_ctl[C_COUNT] = {
    "up", "down", "left", "right", "start", "back", "ls", "rs",
    "a", "b", "x", "y", "black", "white", "lt", "rt",
    "lstick_left", "lstick_right", "lstick_down", "lstick_up",
    "rstick_left", "rstick_right", "rstick_down", "rstick_up" };
/* VK codes (numeric: the SDL build has no winuser.h) */
static int s_key[C_COUNT] = {
    0x26, 0x28, 0x25, 0x27, 0x0D, 0x08, 0x10, 0x11,
    'Z', 'X', 'A', 'S', 'E', 'Q', '1', '3',
    0x64, 0x66, 0x62, 0x68, 'J', 'L', 'K', 'I' };
enum { P_UP, P_DOWN, P_LEFT, P_RIGHT, P_START, P_BACK, P_LS, P_RS,
       P_A, P_B, P_X, P_Y, P_LB, P_RB, P_LT, P_RT, P_COUNT };
static const char *const k_src[P_COUNT] = {
    "up", "down", "left", "right", "start", "back", "ls", "rs",
    "a", "b", "x", "y", "lb", "rb", "lt", "rt" };
static int s_pad[C_RT + 1] = {
    P_UP, P_DOWN, P_LEFT, P_RIGHT, P_START, P_BACK, P_LS, P_RS,
    P_A, P_B, P_X, P_Y, P_LB, P_RB, P_LT, P_RT };

static int key_code(const char *n)
{
    static const struct { const char *n; int vk; } t[] = {
        {"enter", 0x0D}, {"return", 0x0D}, {"space", 0x20}, {"backspace", 0x08},
        {"tab", 0x09}, {"shift", 0x10}, {"ctrl", 0x11}, {"control", 0x11},
        {"alt", 0x12}, {"esc", 0x1B}, {"escape", 0x1B}, {"up", 0x26},
        {"down", 0x28}, {"left", 0x25}, {"right", 0x27}, {"home", 0x24},
        {"end", 0x23}, {"pageup", 0x21}, {"pagedown", 0x22}, {"insert", 0x2D},
        {"delete", 0x2E},
    };
    size_t i;
    if (n[0] && !n[1]) {
        char c = n[0];
        if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
        if (c >= '0' && c <= '9') return c;
        return 0;
    }
    if (n[0] == '0' && n[1] == 'x') return (int)strtol(n, NULL, 16);
    if (!strncmp(n, "numpad", 6) && n[6] >= '0' && n[6] <= '9' && !n[7]) return 0x60 + n[6] - '0';
    if (n[0] == 'f' && n[1] >= '1' && n[1] <= '9') {
        int k = atoi(n + 1);
        if (k >= 1 && k <= 12) return 0x6F + k;
    }
    for (i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!strcmp(n, t[i].n)) return t[i].vk;
    return 0;
}

static void bindings_load(void)
{
    static int done;
    const char *path;
    char line[256];
    FILE *f;
    int n = 0;
    if (done)
        return;
    done = 1;
    path = getenv("RECOMP_KEYS");
    if (!path || !*path)
        path = "keys.ini";
    if (!(f = fopen(path, "r")))
        return;
    while (fgets(line, sizeof line, f)) {
        char *p, *name, *val, *e;
        int i, c = -1, pad = 0;
        for (p = line; *p; p++) {
            if (*p == '#' || *p == ';' || *p == '\r' || *p == '\n') { *p = 0; break; }
            if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
        }
        if (!(e = strchr(line, '=')))
            continue;
        *e = 0;
        name = line; val = e + 1;
        while (*name == ' ' || *name == '\t') name++;
        while (*val == ' ' || *val == '\t') val++;
        for (e = name + strlen(name); e > name && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        for (e = val + strlen(val); e > val && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        if (!strncmp(name, "pad.", 4)) { pad = 1; name += 4; }
        for (i = 0; i < C_COUNT; i++)
            if (!strcmp(name, k_ctl[i])) { c = i; break; }
        if (c < 0 || (pad && c > C_RT)) {
            fprintf(stderr, "[INPUT] %s: unknown control \"%s%s\"\n", path, pad ? "pad." : "", name);
            continue;
        }
        if (pad) {
            int src = -1;
            for (i = 0; i < P_COUNT; i++)
                if (!strcmp(val, k_src[i])) { src = i; break; }
            if (src < 0) {
                fprintf(stderr, "[INPUT] %s: unknown controller button \"%s\"\n", path, val);
                continue;
            }
            s_pad[c] = src;
        } else {
            int vk = key_code(val);
            if (!vk) {
                fprintf(stderr, "[INPUT] %s: unknown key \"%s\"\n", path, val);
                continue;
            }
            s_key[c] = vk;
        }
        fprintf(stderr, "[INPUT] %s%s = %s\n", pad ? "pad." : "", name, val);
        n++;
    }
    fclose(f);
    fprintf(stderr, "[INPUT] %s: %d binding(s)\n", path, n);
    fflush(stderr);
}

/* ======================================================================== */
#if defined(_WIN32)
/* ====================  XInput backend  ================================== */
/* ======================================================================== */

#include <xinput.h>
#pragma comment(lib, "xinput.lib")

static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS] = { FALSE };
static DWORD g_last_packet[XBOX_MAX_CONTROLLERS] = { 0 };

/* ---- keyboard, when there is no pad --------------------------------------
 *
 * A title that waits on PRESS START is unreachable on a machine with no
 * controller plugged in, which is most machines someone brings this up on.
 * The whole point of a recompilation is to be able to look at the thing
 * running, and a build nobody can press a button in cannot be looked at.
 *
 * Off by default, because a keyboard silently acting as player 1 is
 * surprising when a real pad is what you meant to use. RECOMP_KEYBOARD=1
 * turns it on. It only ever answers for port 0, and is merged on top of a
 * pad connected there, so a real controller keeps working.
 *
 * The keys are the ones a Dreamcast or Saturn emulator would pick, which is
 * the closest thing to a convention here:
 *
 *   arrows        d-pad             Enter      START
 *   Z X A S       A B X Y           Backspace  BACK
 *   Q E           white black       1 3        triggers
 *   numpad 8/2/4/6 left thumb       I/K/J/L    right thumb
 *
 * Keys come from the framebuffer window, which only receives them while it
 * has the focus, so typing in another window does not drive the game.
 */
static BOOL keyboard_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_KEYBOARD");
        on = (v && *v && *v != '0') ? 1 : 0;
    }
    return on ? TRUE : FALSE;
}

/* The framebuffer window records what is held; see src/video/fb_present.c.
 *
 * GetAsyncKeyState was the first attempt and reads nothing here -- under
 * Wine it answers about a state a GDI-drawing guest never touches. Going
 * through the window is also the better gate: a window only receives keys
 * while it has the focus, so there is no separate focus check to get wrong
 * and no way for typing in another application to drive the game.
 *
 * Declared rather than included so src/input does not depend on the video
 * module's include path. */
extern int xbox_FramebufferKeyDown(int vk);

static BOOL key_down(int vk)
{
    return xbox_FramebufferKeyDown(vk) ? TRUE : FALSE;
}

/* A thumb axis from two keys, at the full deflection a digital key implies. */
static SHORT axis_from_keys(int negative, int positive)
{
    int v = 0;
    if (key_down(negative)) v -= 32767;
    if (key_down(positive)) v += 32767;
    return (SHORT)v;
}

static void keyboard_state(XBOX_INPUT_STATE *pState)
{
    static DWORD packet;
    static const WORD dig[8] = {
        XBOX_GAMEPAD_DPAD_UP, XBOX_GAMEPAD_DPAD_DOWN, XBOX_GAMEPAD_DPAD_LEFT,
        XBOX_GAMEPAD_DPAD_RIGHT, XBOX_GAMEPAD_START, XBOX_GAMEPAD_BACK,
        XBOX_GAMEPAD_LEFT_THUMB, XBOX_GAMEPAD_RIGHT_THUMB };
    static const int ana[8] = {
        XBOX_BUTTON_A, XBOX_BUTTON_B, XBOX_BUTTON_X, XBOX_BUTTON_Y,
        XBOX_BUTTON_BLACK, XBOX_BUTTON_WHITE, XBOX_BUTTON_LTRIGGER, XBOX_BUTTON_RTRIGGER };
    WORD b = 0;
    int c;

    bindings_load();
    memset(pState, 0, sizeof(*pState));
    for (c = C_UP; c <= C_RS; c++)
        if (key_down(s_key[c])) b |= dig[c];
    pState->Gamepad.wButtons = b;
    /* Analog on the console, so a key is 255 rather than a flag -- a title
     * that reads these as a pressure never sees a press if they are 1. */
    for (c = C_A; c <= C_RT; c++)
        pState->Gamepad.bAnalogButtons[ana[c - C_A]] = key_down(s_key[c]) ? 255 : 0;
    pState->Gamepad.sThumbLX = axis_from_keys(s_key[C_LSL], s_key[C_LSR]);
    pState->Gamepad.sThumbLY = axis_from_keys(s_key[C_LSD], s_key[C_LSU]);
    pState->Gamepad.sThumbRX = axis_from_keys(s_key[C_RSL], s_key[C_RSR]);
    pState->Gamepad.sThumbRY = axis_from_keys(s_key[C_RSD], s_key[C_RSU]);

    /* The title's input layer looks for button edges, so the packet number
     * has to move whenever the state does or a press is never noticed. */
    pState->dwPacketNumber = ++packet;
}

/* A controller source (k_src) as 0..255. */
static int pad_src(const XINPUT_GAMEPAD *g, int src)
{
    static const WORD bit[P_LT] = {
        XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT,
        XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_BACK,
        XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB, XINPUT_GAMEPAD_A,
        XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
        XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER };
    if (src == P_LT) return g->bLeftTrigger;
    if (src == P_RT) return g->bRightTrigger;
    return (g->wButtons & bit[src]) ? 255 : 0;
}

void xbox_InputInit(void)
{
    for (DWORD i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        XINPUT_STATE state;
        DWORD result = XInputGetState(i, &state);
        g_controller_connected[i] = (result == ERROR_SUCCESS);
    }
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    XINPUT_STATE xi_state;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    /* RECOMP_NO_HOST_PAD=1: ignore the machine's real controllers (scripted
     * test runs, so a pad in someone's hands does not steer them; the
     * scripted presses and the keyboard still work). */
    {
        static int no_pad = -1;
        if (no_pad < 0) {
            const char *v = getenv("RECOMP_NO_HOST_PAD");
            no_pad = v && *v && *v != '0';
        }
        result = no_pad ? ERROR_DEVICE_NOT_CONNECTED : XInputGetState(dwPort, &xi_state);
    }
    if (result != ERROR_SUCCESS) {
        g_controller_connected[dwPort] = FALSE;
        if (dwPort == 0 && keyboard_enabled()) {
            keyboard_state(pState);
            return ERROR_SUCCESS;
        }
        return result;
    }

    g_controller_connected[dwPort] = TRUE;
    g_last_packet[dwPort] = xi_state.dwPacketNumber;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = xi_state.dwPacketNumber;
    /* Through the bindings (keys.ini pad.<control>); by default the
     * usual layout: shoulders = black (LB) / white (RB). */
    bindings_load();
    {
        static const WORD dig[8] = {
            XBOX_GAMEPAD_DPAD_UP, XBOX_GAMEPAD_DPAD_DOWN, XBOX_GAMEPAD_DPAD_LEFT,
            XBOX_GAMEPAD_DPAD_RIGHT, XBOX_GAMEPAD_START, XBOX_GAMEPAD_BACK,
            XBOX_GAMEPAD_LEFT_THUMB, XBOX_GAMEPAD_RIGHT_THUMB };
        static const int ana[8] = {
            XBOX_BUTTON_A, XBOX_BUTTON_B, XBOX_BUTTON_X, XBOX_BUTTON_Y,
            XBOX_BUTTON_BLACK, XBOX_BUTTON_WHITE, XBOX_BUTTON_LTRIGGER, XBOX_BUTTON_RTRIGGER };
        WORD w = 0;
        int c;
        for (c = C_UP; c <= C_RS; c++)
            if (pad_src(&xi_state.Gamepad, s_pad[c]) > 30) w |= dig[c];
        pState->Gamepad.wButtons = w;
        for (c = C_A; c <= C_RT; c++)
            pState->Gamepad.bAnalogButtons[ana[c - C_A]] = (BYTE)pad_src(&xi_state.Gamepad, s_pad[c]);
    }

    pState->Gamepad.sThumbLX = xi_state.Gamepad.sThumbLX;
    pState->Gamepad.sThumbLY = xi_state.Gamepad.sThumbLY;
    pState->Gamepad.sThumbRX = xi_state.Gamepad.sThumbRX;
    pState->Gamepad.sThumbRY = xi_state.Gamepad.sThumbRY;

    /* Merge the keyboard on top rather than only standing in for a missing
     * pad.
     *
     * The first version put the keyboard behind XInput's failure, on the
     * assumption that with nothing plugged in the call would fail. It does
     * not: under Wine XInputGetState returns ERROR_SUCCESS and a gamepad
     * with every button at rest, so the fallback was unreachable and
     * pressing a key did nothing at all.
     *
     * Merging is also the better rule. A real pad keeps working -- its
     * buttons are already in pState and the keyboard only adds to them --
     * and there is no special case left to get wrong. */
    if (dwPort == 0 && keyboard_enabled()) {
        XBOX_INPUT_STATE kb;
        int i;
        keyboard_state(&kb);
        pState->Gamepad.wButtons |= kb.Gamepad.wButtons;
        for (i = 0; i < 8; i++)
            if (kb.Gamepad.bAnalogButtons[i] > pState->Gamepad.bAnalogButtons[i])
                pState->Gamepad.bAnalogButtons[i] = kb.Gamepad.bAnalogButtons[i];
        if (kb.Gamepad.sThumbLX) pState->Gamepad.sThumbLX = kb.Gamepad.sThumbLX;
        if (kb.Gamepad.sThumbLY) pState->Gamepad.sThumbLY = kb.Gamepad.sThumbLY;
        if (kb.Gamepad.sThumbRX) pState->Gamepad.sThumbRX = kb.Gamepad.sThumbRX;
        if (kb.Gamepad.sThumbRY) pState->Gamepad.sThumbRY = kb.Gamepad.sThumbRY;
        /* The input layer records edges, so an unchanged packet number is
         * read as the same state and the press never happens. */
        pState->dwPacketNumber = kb.dwPacketNumber;
    }

    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    XINPUT_VIBRATION xi_vib;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;
    {
        /* RECOMP_NO_HOST_PAD (scripted runs): no rumble on someone's pad. */
        const char *v = getenv("RECOMP_NO_HOST_PAD");
        if (v && *v && *v != '0')
            return ERROR_DEVICE_NOT_CONNECTED;
    }

    xi_vib.wLeftMotorSpeed = pVibration->wLeftMotorSpeed;
    xi_vib.wRightMotorSpeed = pVibration->wRightMotorSpeed;
    return XInputSetState(dwPort, &xi_vib);
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    XINPUT_CAPABILITIES xi_caps;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;

    result = XInputGetCapabilities(dwPort, dwFlags, &xi_caps);
    if (result != ERROR_SUCCESS) return result;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type = xi_caps.Type;
    pCaps->SubType = xi_caps.SubType;
    pCaps->Flags = xi_caps.Flags;
    return ERROR_SUCCESS;
}

/* ======================================================================== */
#else /* !_WIN32 */
/* ====================  SDL2 GameController backend  ===================== */
/* ======================================================================== */

#include <SDL.h>

static SDL_GameController *g_pads[XBOX_MAX_CONTROLLERS];
static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS];
static DWORD g_packet[XBOX_MAX_CONTROLLERS];

/* RECOMP_KEYBOARD=1: the keyboard as player 1, merged over any pad. Same map
 * as the Windows backend; keys come from the framebuffer window as VK codes. */
extern int xbox_FramebufferKeyDown(int vk);

static int keyboard_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_KEYBOARD");
        on = (v && *v && *v != '0') ? 1 : 0;
    }
    return on;
}

static SHORT kb_axis(int neg, int pos)
{
    int v = 0;
    if (xbox_FramebufferKeyDown(neg)) v -= 32767;
    if (xbox_FramebufferKeyDown(pos)) v += 32767;
    return (SHORT)v;
}

/* OR the keyboard into *pState. */
static void keyboard_merge(XBOX_INPUT_STATE *pState)
{
    static const WORD dig[8] = {
        XBOX_GAMEPAD_DPAD_UP, XBOX_GAMEPAD_DPAD_DOWN, XBOX_GAMEPAD_DPAD_LEFT,
        XBOX_GAMEPAD_DPAD_RIGHT, XBOX_GAMEPAD_START, XBOX_GAMEPAD_BACK,
        XBOX_GAMEPAD_LEFT_THUMB, XBOX_GAMEPAD_RIGHT_THUMB };
    static const int ana[8] = {
        XBOX_BUTTON_A, XBOX_BUTTON_B, XBOX_BUTTON_X, XBOX_BUTTON_Y,
        XBOX_BUTTON_BLACK, XBOX_BUTTON_WHITE, XBOX_BUTTON_LTRIGGER, XBOX_BUTTON_RTRIGGER };
    WORD b = 0;
    int c;

    bindings_load();
    for (c = C_UP; c <= C_RS; c++)
        if (xbox_FramebufferKeyDown(s_key[c])) b |= dig[c];
    pState->Gamepad.wButtons |= b;
    for (c = C_A; c <= C_RT; c++)
        if (xbox_FramebufferKeyDown(s_key[c]))
            pState->Gamepad.bAnalogButtons[ana[c - C_A]] = 255;
    if (!pState->Gamepad.sThumbLX) pState->Gamepad.sThumbLX = kb_axis(s_key[C_LSL], s_key[C_LSR]);
    if (!pState->Gamepad.sThumbLY) pState->Gamepad.sThumbLY = kb_axis(s_key[C_LSD], s_key[C_LSU]);
    if (!pState->Gamepad.sThumbRX) pState->Gamepad.sThumbRX = kb_axis(s_key[C_RSL], s_key[C_RSR]);
    if (!pState->Gamepad.sThumbRY) pState->Gamepad.sThumbRY = kb_axis(s_key[C_RSD], s_key[C_RSU]);
}

/* Open attached game controllers into free slots (up to
 * XBOX_MAX_CONTROLLERS); a pad that went away frees its slot. */
static void open_controllers(void)
{
    for (int slot = 0; slot < XBOX_MAX_CONTROLLERS; slot++)
        if (g_pads[slot] && !SDL_GameControllerGetAttached(g_pads[slot])) {
            SDL_GameControllerClose(g_pads[slot]);
            g_pads[slot] = NULL;
            g_controller_connected[slot] = FALSE;
        }
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        int slot, open = 0;
        if (!SDL_IsGameController(i))
            continue;
        for (slot = 0; slot < XBOX_MAX_CONTROLLERS; slot++)
            if (g_pads[slot] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pads[slot])) == id)
                open = 1;
        if (open)
            continue;
        for (slot = 0; slot < XBOX_MAX_CONTROLLERS && g_pads[slot]; slot++)
            ;
        if (slot == XBOX_MAX_CONTROLLERS)
            break;
        g_pads[slot] = SDL_GameControllerOpen(i);
        g_controller_connected[slot] = (g_pads[slot] != NULL);
        if (g_pads[slot])
            fprintf(stderr, "[INPUT] controller %d: %s\n", slot, SDL_GameControllerName(g_pads[slot]));
    }
}

void xbox_InputInit(void)
{
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER))
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    open_controllers();
}

/* Set up on first use (nothing else calls xbox_InputInit on POSIX), then
 * look for newly attached pads once a second while a slot is free. */
static void input_poll_devices(void)
{
    static int inited;
    static Uint32 next_scan;
    int slot;
    if (!inited) {
        inited = 1;
        xbox_InputInit();
        next_scan = SDL_GetTicks() + 1000;
        return;
    }
    if ((Sint32)(SDL_GetTicks() - next_scan) < 0)
        return;
    next_scan = SDL_GetTicks() + 1000;
    for (slot = 0; slot < XBOX_MAX_CONTROLLERS; slot++)
        if (!g_pads[slot] || !SDL_GameControllerGetAttached(g_pads[slot])) {
            SDL_GameControllerUpdate();     /* lets SDL notice new devices */
            open_controllers();
            break;
        }
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (dwPort == 0)
        input_poll_devices();

    SDL_GameController *c = g_pads[dwPort];
    if (!c || !SDL_GameControllerGetAttached(c)) {
        g_controller_connected[dwPort] = FALSE;
        if (dwPort == 0 && keyboard_enabled()) {
            static DWORD kb_packet;
            memset(pState, 0, sizeof(*pState));
            keyboard_merge(pState);
            pState->dwPacketNumber = ++kb_packet;   /* must move for edges */
            return ERROR_SUCCESS;
        }
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    SDL_GameControllerUpdate();
    g_controller_connected[dwPort] = TRUE;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = ++g_packet[dwPort];

    WORD btn = 0;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP))    btn |= XBOX_GAMEPAD_DPAD_UP;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  btn |= XBOX_GAMEPAD_DPAD_DOWN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  btn |= XBOX_GAMEPAD_DPAD_LEFT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) btn |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START))      btn |= XBOX_GAMEPAD_START;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK))       btn |= XBOX_GAMEPAD_BACK;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSTICK))  btn |= XBOX_GAMEPAD_LEFT_THUMB;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) btn |= XBOX_GAMEPAD_RIGHT_THUMB;
    pState->Gamepad.wButtons = btn;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 255 : 0;

    /* SDL trigger axes are 0..32767 -> Xbox analog button 0..255 */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);

    /* SDL Y axis points down; the Xbox Y axis points up -- invert.
     * Use (-1 - v) so v = -32768 does not overflow SHORT. */
    pState->Gamepad.sThumbLX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    pState->Gamepad.sThumbLY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
    pState->Gamepad.sThumbRX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    pState->Gamepad.sThumbRY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY));

    if (dwPort == 0 && keyboard_enabled())
        keyboard_merge(pState);
    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    SDL_GameController *c = g_pads[dwPort];
    if (!c) return ERROR_DEVICE_NOT_CONNECTED;

    /* SDL rumble needs a duration; refresh for ~1s on each call (the game
     * polls vibration continuously). */
    SDL_GameControllerRumble(c, pVibration->wLeftMotorSpeed,
                             pVibration->wRightMotorSpeed, 1000);
    return ERROR_SUCCESS;
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    (void)dwFlags;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (!g_pads[dwPort])
        return ERROR_DEVICE_NOT_CONNECTED;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type    = 1;   /* XINPUT_DEVTYPE_GAMEPAD */
    pCaps->SubType = 1;   /* XINPUT_DEVSUBTYPE_GAMEPAD */
    pCaps->Flags   = 0;
    return ERROR_SUCCESS;
}

#endif /* _WIN32 */
