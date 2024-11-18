#ifndef _WM_WINDOW_MANAGER_H
#define _WM_WINDOW_MANAGER_H

#include <stdbool.h>
#include <X11/Xutil.h>
#include "clients.h"

// Creating an enum-array to store non-predefined atom values
// Server queries are expensive, so we should cache them!
typedef enum
{
    ATOM_WM_PROTOCOLS,
    ATOM_WM_DELETE_WINDOW,
    TOTAL_ATOMS,
} wm_atom_e;

typedef struct
{
    Display *conn;
    client_list_t clients;

    Atom atoms[TOTAL_ATOMS];
    // We're only dealing with simple, single-monitor setups (as of now)
    Window root;
    bool is_running;
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

#endif
