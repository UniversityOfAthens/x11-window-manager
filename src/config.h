#ifndef _WM_CONFIG_H
#define _WM_CONFIG_H

// Using the super (windows) key as a binding prefix
#define WM_MOD_MASK Mod4Mask

#define WM_BORDER_WIDTH 1
#define WM_BORDER_COLOR 0xffffff

// The -c option indicates that the commands should be read from the argument list
// /bin/sh is a symlink to our default POSIX-compliant shell (probably Bash)
#define SHELL_CMD(cmd) { "/bin/sh", "-c", cmd, NULL }

static const char* wm_app_launcher[] = { "dmenu_run", NULL };
static const char* wm_terminal_cmd[] = { "xterm", NULL };

typedef struct
{
    unsigned int modifiers;
    KeySym keysym;
} wm_key_t;

static wm_key_t wm_kill_client = { WM_MOD_MASK | ShiftMask, XK_q };

#endif
