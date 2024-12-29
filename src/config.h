#ifndef _WM_CONFIG_H
#define _WM_CONFIG_H

#include "window_manager.h"
#include <X11/XF86keysym.h>

// Using the super (windows) key as a binding prefix
#define WM_MOD_MASK Mod4Mask

#define WM_BORDER_WIDTH 1
#define WM_BORDER_COLOR 0xffffff
#define WM_SPECIAL_PADDING 50

#define SWITCH_WORK(k, n)                                                  \
    { {WM_MOD_MASK, k}, wm_switch_to_workspace, {.amount = n} },           \
    { {WM_MOD_MASK | ShiftMask, k}, wm_send_to_workspace, {.amount = n} }  \

// The -c option indicates that the commands should be read from the argument list
// /bin/sh is a symlink to our default POSIX-compliant shell (probably Bash)
#define SHELL(cmd) { .strs = (const char*[]) { "/bin/sh", "-c", cmd, NULL } }

static wm_key_t wm_kill_client_key = { WM_MOD_MASK | ShiftMask, XK_q };

static wm_binding_t wm_bindings[] = {
    { {WM_MOD_MASK,             XK_p}, wm_spawn, SHELL("dmenu_run") },
    { {WM_MOD_MASK | ShiftMask, XK_Return}, wm_spawn, SHELL("xterm") },
    { {WM_MOD_MASK | ShiftMask, XK_e}, wm_quit, NULL },

    { {WM_MOD_MASK, XK_l}, wm_adjust_special_width, {.amount = 20} },
    { {WM_MOD_MASK, XK_h}, wm_adjust_special_width, {.amount = -20} },
    { {WM_MOD_MASK, XK_j}, wm_focus_on_next, NULL},
    { {WM_MOD_MASK, XK_k}, wm_focus_on_previous, NULL},
    { {WM_MOD_MASK, XK_Return}, wm_make_focused_special, NULL},

    // Workspace switching bindings, this is going to be repetitive
    SWITCH_WORK(XK_1, 0), SWITCH_WORK(XK_2, 1), SWITCH_WORK(XK_3, 2),
    SWITCH_WORK(XK_4, 3), SWITCH_WORK(XK_5, 4), SWITCH_WORK(XK_6, 5),
    SWITCH_WORK(XK_7, 6), SWITCH_WORK(XK_8, 7), SWITCH_WORK(XK_9, 8),

    { {WM_MOD_MASK, XK_t}, wm_toggle_float },

    // Audio volume controls
    { {0, XF86XK_AudioLowerVolume}, wm_spawn, SHELL("pactl set-sink-volume 0 -5%") },
    { {0, XF86XK_AudioRaiseVolume}, wm_spawn, SHELL("pactl set-sink-volume 0 +5%") },
    { {0, XF86XK_AudioMute},        wm_spawn, SHELL("pactl set-sink-mute 0 toggle") },
};

#endif
