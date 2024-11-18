#ifndef _WM_CONFIG_H
#define _WM_CONFIG_H

#include "window_manager.h"

// Using the super (windows) key as a binding prefix
#define WM_MOD_MASK Mod4Mask

#define WM_BORDER_WIDTH 1
#define WM_BORDER_COLOR 0xffffff

// The -c option indicates that the commands should be read from the argument list
// /bin/sh is a symlink to our default POSIX-compliant shell (probably Bash)
#define SHELL_CMD(cmd) { "/bin/sh", "-c", cmd, NULL }

static const char* wm_app_launcher[] = { "dmenu_run", NULL };
static const char* wm_terminal_cmd[] = { "xterm", NULL };

static wm_key_t wm_kill_client_key = { WM_MOD_MASK | ShiftMask, XK_q };

static wm_binding_t wm_bindings[] = {
    { {WM_MOD_MASK,             XK_p}, wm_spawn, wm_app_launcher },
    { {WM_MOD_MASK | ShiftMask, XK_Return}, wm_spawn, wm_terminal_cmd },
    { {WM_MOD_MASK | ShiftMask, XK_e}, wm_quit, NULL },
};

#endif
