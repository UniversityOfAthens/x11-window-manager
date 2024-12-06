#ifndef _WM_WINDOW_MANAGER_H
#define _WM_WINDOW_MANAGER_H

#include <stdbool.h>
#include <X11/Xutil.h>
#include "clients.h"

#define TOTAL_WORKSPACES 9

// Creating an enum-array to store non-predefined atom values
// Server queries are expensive, so we should cache them!
typedef enum
{
    ATOM_WM_PROTOCOLS,
    ATOM_WM_DELETE_WINDOW,
    ATOM_WM_TAKE_FOCUS,
    ATOM_NET_ACTIVE_WINDOW,
    TOTAL_ATOMS,
} wm_atom_e;

typedef struct
{
    client_list_t clients;
    client_t *focused_client;

    // The width of the special window, initially set to half the screen width
    int special_width;
} workspace_t;

typedef struct
{
    Display *conn;
    Colormap colormap;

    int active_workspace;
    workspace_t workspaces[TOTAL_WORKSPACES];

    // Dimensions of the entire monitor in pixels
    int width, height;

    Atom atoms[TOTAL_ATOMS];
    // We're only dealing with simple, single-monitor setups (as of now)
    Window root;
    bool has_moved_cursor;
    bool is_running;

    // Cache color indices
    XColor border_color;
    XColor focused_border_color;
} wm_t;

void wm_setup(wm_t *wm);
void wm_loop(wm_t *wm);
void wm_cleanup(wm_t *wm);

/*
 * This design is inspired by dwm.
 * The argument type can be easily deduced from the implementation of the
 * callback function. No need for an `type` enum here.
 */
typedef union
{
    const char **str_array;
    int amount;
} wm_arg_t;

typedef struct
{
    unsigned int modifiers;
    KeySym keysym;
} wm_key_t;

bool are_keys_equal(wm_key_t a, wm_key_t b);

typedef struct
{
    wm_key_t key;

    void (*callback)(wm_t*, const wm_arg_t);
    const wm_arg_t argument;
} wm_binding_t;

/*
 * Functions that should be accessible to our configuration file
 * They should match the type of binding callbacks
 */
void wm_spawn(wm_t *wm, const wm_arg_t arg);
void wm_quit(wm_t *wm, const wm_arg_t arg);

// The argument represents dx, min and max bound checking will be applied
void wm_adjust_special_width(wm_t *wm, const wm_arg_t arg);

void wm_focus_on_next(wm_t *wm, const wm_arg_t arg);
void wm_focus_on_previous(wm_t *wm, const wm_arg_t arg);
void wm_make_focused_special(wm_t *wm, const wm_arg_t arg);
void wm_switch_to_workspace(wm_t *wm, const wm_arg_t arg);

#endif
