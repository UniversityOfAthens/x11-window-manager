#include "window_manager.h"
#include "utils.h"
#include "clients.h"
#include "config.h"
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>

#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>

bool are_keys_equal(wm_key_t a, wm_key_t b)
{
    return a.keysym == b.keysym && a.modifiers == b.modifiers;
}

// Inlining this is definitely useless, I just want to be sure
static inline workspace_t* get_workspace(wm_t *wm)
{
    return &wm->workspaces[wm->active_workspace];
}

static wm_key_t key_event_to_key(wm_t *wm, const XKeyEvent *event)
{
    // XKeycodeToKeysym is deprecated, we need to use this instead
    KeySym keysym = XkbKeycodeToKeysym(wm->conn, event->keycode, 0, 0);

    wm_key_t key = {
        .keysym = keysym,
        .modifiers = event->state,
    };

    return key;
}

// Notifies the server that the given window expects the given key binding
static void grab_key(wm_t *wm, wm_key_t key, Window window)
{
    XGrabKey(wm->conn,
        XKeysymToKeycode(wm->conn, key.keysym),
        key.modifiers, window, False, GrabModeAsync, GrabModeAsync);
}

// Temporary error handler used solely during the initialization phase
static int on_wm_error(Display *display, XErrorEvent *error)
{
    // We will not be able to perform Substructure Redirection if another
    // window manager is already running on our display. Only one client
    // is granted this privilege at a time
    if (error->error_code == BadAccess)
        log_fatal("substructure redirection failed, is a WM already running?");

    return 0;
}

// An error handler that will just ignore all failures
static int dummy_error_handler(Display *display, XErrorEvent *error)
{
    return 0;
}

static int on_x_error(Display *display, XErrorEvent *error)
{
    char error_message[1024];
    XGetErrorText(display, error->error_code, error_message, sizeof(error_message));

    log_fatal("Received X error: %s", error_message);
    return 0;
}

static void create_bindings(wm_t *wm)
{
    // Iterate over all key bindings and register their presence
    for (int i = 0; i < ARRAY_LEN(wm_bindings); i++)
    {
        wm_binding_t *binding = &wm_bindings[i];
        grab_key(wm, binding->key, wm->root);
    }
}

// Will return false if the client does not participate in the protocol (README.md)
static bool try_send_wm_protocol(wm_t *wm, Window window, Atom protocol)
{
    bool is_supported = false;
    Atom *protocols;
    int total_protocols;

    // Search the supported protocols list for a match
    if (XGetWMProtocols(wm->conn, window, &protocols, &total_protocols))
    {
        for (int i = 0; i < total_protocols; i++)
            if (is_supported = (protocols[i] == protocol))
                break;

        XFree(protocols);
    }

    if (is_supported)
    {
        // The protocol is supported, send the message!
        XEvent event = {
            .xclient = {
                .type = ClientMessage,
                .window = window,
                .message_type = wm->atoms[ATOM_WM_PROTOCOLS],
                // The data consists of 5 32-bit values. Hence data.l[] (long)
                // All client message events follow this format
                .format = 32,
            }
        };

        event.xclient.data.l[0] = protocol;
        // CurrentTime is most likely used by the server to combat race conditions.
        // I couldn't find any reference to it in the manual, but everybody does it
        event.xclient.data.l[1] = CurrentTime;
        XSendEvent(wm->conn, window, false, NoEventMask, &event);
    }

    return is_supported;
}

// This function returns None if the specified property does not exist on the given window
static Atom get_window_prop(wm_t *wm, Window w, Atom prop)
{
    Atom type, value;
    unsigned char *data = NULL;
    // These can all be ignored for now, we won't be needing them
    int format;
    unsigned long items, rem_bytes;

    if (XGetWindowProperty(wm->conn, w, prop, 0L, sizeof(Atom), false,
            XA_ATOM, &type, &format, &items, &rem_bytes, &data) == Success && data)
    {
        value = *(Atom*) data;
        XFree(data);
    }

    // X11 will indicate that the property does not exist
    // by filling in the type variable with the value of None
    return type != None ? value : None;
}

static void set_window_prop(wm_t *wm, Window w, Atom a, Atom type,
                            unsigned long *values, unsigned long total)
{
    // Set the property, overriding the previous value if set
    XChangeProperty(wm->conn, w, a, type, 32, PropModeReplace, (unsigned char*) values, total);
}

static bool should_client_float(wm_t *wm, client_t *c)
{
    // If the client is fixed in size, float it
    // It does not expect to live inside a tiling window manager
    if (c->max_width != -1 && c->max_width == c->min_width &&
        c->max_height != -1 && c->max_height == c->min_height)
    {
        return true;
    }

    Atom type = get_window_prop(wm, c->window, wm->atoms[ATOM_WM_WINDOW_TYPE]);

    // _NET_WM_WINDOW_TYPE_DIALOG indicates that this is a dialog window.
    if (type == wm->atoms[ATOM_WM_DIALOG_TYPE])
        return true;

    else if (type == None)
    {
        // Quoting from freedesktop.org: If _NET_WM_WINDOW_TYPE is not set,
        // then managed windows with WM_TRANSIENT_FOR set MUST be taken as this type.
        Window trans = None;
        if (XGetTransientForHint(wm->conn, c->window, &trans))
            return true;
    }

    return false;
}

static void visually_reflect_focus(wm_t *wm, workspace_t *space)
{
    client_t *c = clients_get_focused(&space->clients);

    if (!c)
    {
        XSetInputFocus(wm->conn, wm->root, RevertToPointerRoot, CurrentTime);
        // Clear the property so that clients understand that no window is currently in focus
        XDeleteProperty(wm->conn, wm->root, wm->atoms[ATOM_NET_ACTIVE_WINDOW]);
    }
    else
    {
        XSetWindowBorder(wm->conn, c->window, wm->focused_border_color.pixel);

        set_window_prop(wm, wm->root, wm->atoms[ATOM_NET_ACTIVE_WINDOW], XA_WINDOW, &c->window, 1);
        // The server will generate FocusIn and FocusOut events
        XSetInputFocus(wm->conn, c->window, RevertToPointerRoot, CurrentTime);
        try_send_wm_protocol(wm, c->window, wm->atoms[ATOM_WM_TAKE_FOCUS]);
    }
}

// Sounds like a magic trick
static void visually_unfocus_focused(wm_t *wm, workspace_t *space)
{
    client_t *c = clients_get_focused(&space->clients);

    if (c)
        XSetWindowBorder(wm->conn, c->window, wm->border_color.pixel);
}

static void focus_client(wm_t *wm, workspace_t *space, client_t *c)
{
    client_t *cur = clients_get_focused(&space->clients);
    if (cur == c) return;

    visually_unfocus_focused(wm, space);
    clients_push_focus(&space->clients, c);
    visually_reflect_focus(wm, space);
}

static void try_load_named_color(wm_t *wm, const char *id, XColor *color)
{
    if (!XAllocNamedColor(wm->conn, wm->colormap, id, color, color))
        log_fatal("failed to load focused border color");
}

void wm_setup(wm_t *wm)
{
    struct sigaction sa;

    // Prevent the creation of child zombie processes
    // This is important because we're going to spawn launchers and terminals
    //  using window manager key bindings, and we certainly don't want to wait() on them
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NOCLDSTOP | SA_NOCLDWAIT | SA_RESTART;
    // SIG_IGN = ignore. Don't execute any code, just apply the flag side-effects
    sa.sa_handler = SIG_IGN;
    sigaction(SIGCHLD, &sa, NULL);

    // Connect to an X server
    // Use the $DISPLAY environment variable as a default
    wm->conn = XOpenDisplay(NULL);
    if (!wm->conn)
        log_fatal("failed to connect to X server: %s", XDisplayName(NULL));

    // An initial root window will always be present
    wm->root = DefaultRootWindow(wm->conn);
    wm->is_running = true;
    wm->dragged_client = NULL;
    wm->gap = WM_INITIAL_GAP;

    /*
     * Checking whether we've got a right for Substructure Redirection
     * Using a temporary error handler for this special initialization phase
     */
    XSetErrorHandler(on_wm_error);
    // For substructure redirection, check out page 361 of the programming manual!
    XSelectInput(wm->conn, wm->root, PointerMotionMask | SubstructureRedirectMask | SubstructureNotifyMask);
    // Wait until all pending requests have been fully processed by the X server.
    // the second argument must always be false, since we don't want to discard incoming queue events
    XSync(wm->conn, false);

    XSetErrorHandler(on_x_error);

    // Setting the cursor for our root window. The default is a big X
    Cursor cursor = XCreateFontCursor(wm->conn, XC_left_ptr);
    XDefineCursor(wm->conn, wm->root, cursor);

    wm->atoms[ATOM_WM_PROTOCOLS] = XInternAtom(wm->conn, "WM_PROTOCOLS", false);
    wm->atoms[ATOM_WM_DELETE_WINDOW] = XInternAtom(wm->conn, "WM_DELETE_WINDOW", false);
    wm->atoms[ATOM_NET_ACTIVE_WINDOW] = XInternAtom(wm->conn, "_NET_ACTIVE_WINDOW", false);
    wm->atoms[ATOM_WM_WINDOW_TYPE] = XInternAtom(wm->conn, "_NET_WM_WINDOW_TYPE", false);
    wm->atoms[ATOM_WM_DIALOG_TYPE] = XInternAtom(wm->conn, "_NET_WM_WINDOW_TYPE_DIALOG", false);
    create_bindings(wm);

    int screen = DefaultScreen(wm->conn);
    wm->width = DisplayWidth(wm->conn, screen);
    wm->height = DisplayHeight(wm->conn, screen);

    for (int i = 0; i < TOTAL_WORKSPACES; i++)
    {
        wm->workspaces[i].special_width = wm->width / 2;
        clients_initialize(&wm->workspaces[i].clients);
    }
    wm->active_workspace = 0;

    // Load in some colors
    wm->colormap = DefaultColormap(wm->conn, screen);

    try_load_named_color(wm, "red", &wm->focused_border_color);
    try_load_named_color(wm, "black", &wm->border_color);

    puts("WM was initialized successfully");
}

static void get_size_hints(wm_t *wm, client_t *c)
{
    XSizeHints hints;
    // We can ignore this value safely
    long supplied_return;

    if (XGetWMNormalHints(wm->conn, c->window, &hints, &supplied_return))
    {
        if (hints.flags & PMinSize)
        {
            c->min_width = hints.min_width;
            c->min_height = hints.min_height;
        }
        if (hints.flags & PMaxSize)
        {
            c->max_width = hints.max_width;
            c->max_height = hints.max_height;
        }
    }
}

/*
 * Re-calculate all tiling positions in a single workspace
 * This should generally be called after ground-breaking layout changes
 */
static void tile(wm_t *wm, workspace_t *space)
{
    // Setting to false to prevent EnterNotify events from firing because of
    // the cursor now being above a brand new window.
    wm->has_moved_cursor = false;

    // Do not consider floating windows
    int tiled_clients = 0;
    for (client_t *c = space->clients.head; c; c = c->next)
        tiled_clients += (!c->is_floating);

    if (tiled_clients == 0) return;

    const int max_width = wm->width - 2 * wm->gap;
    const int max_height = wm->height - 2 * wm->gap;

    // Find the first non-floating window
    client_t *special = space->clients.head;
    while (special->is_floating) special = special->next;

    if (tiled_clients == 1)
    {
        XMoveResizeWindow(wm->conn, special->window, wm->gap, wm->gap, max_width, max_height);
    }
    else
    {
        // The non-floating head of the clients list, also known as the special window,
        // will capture a whole pane on its own.
        XMoveResizeWindow(wm->conn, special->window, wm->gap, wm->gap, space->special_width, max_height);
        const int rem_width = max_width - space->special_width - wm->gap;

        // The other windows will just share the remaining space
        // x * total + gap * (total - 1) = max_height, solve for x
        int other_height = (max_height - wm->gap * (tiled_clients - 2)) / (tiled_clients - 1);
        int i = 0;

        for (client_t *c = special->next; c; c = c->next)
        {
            if (c->is_floating)
                continue;

            XMoveResizeWindow(wm->conn, c->window,
                    space->special_width + 2 * wm->gap, wm->gap + i * (wm->gap + other_height),
                    rem_width, other_height);
            i++;
        }
    }
}

static client_t* manage_window(wm_t *wm, Window window)
{
    workspace_t *space = get_workspace(wm);

    // Start tracking the window inside our internal state
    client_t *client = create_client(window);
    clients_insert(&space->clients, client);

    // Create a border around the window to indicate whether it's focused
    XWindowChanges wc = { .border_width = WM_BORDER_WIDTH };
    XConfigureWindow(wm->conn, window, CWBorderWidth, &wc);

    XSelectInput(wm->conn, window, EnterWindowMask);

    // Stores the list of all client windows managed by the window manager
    // This information is important, particularly during window-manager cleanup
    XAddToSaveSet(wm->conn, window);

    get_size_hints(wm, client);
    client->is_floating = should_client_float(wm, client);

    /*
     * Registering some special key bindings
     * These are unique in some way and do not follow the conventions of config.h
     */

    grab_key(wm, wm_kill_client_key, window);

    // Capture move and resize bindings
    XGrabButton(wm->conn, Button1, WM_MOD_MASK, window, false,
            ButtonPressMask | ButtonReleaseMask | ButtonMotionMask,
            GrabModeAsync, GrabModeAsync, None, None);

    XGrabButton(wm->conn, Button3, WM_MOD_MASK, window, false,
            ButtonPressMask | ButtonReleaseMask | ButtonMotionMask,
            GrabModeAsync, GrabModeAsync, None, None);

    return client;
}

/*
 * Carefully ignore the errors instead of attempting to asynchronously
 * determine if a window is still valid. This is a common pattern in many WM
 * implementations. That's because the window might have already destroyed
 * itself before the initial Unmap event arrives at our end.
 */
static void unmanage_client(wm_t *wm, client_t *client)
{
    workspace_t *space = get_workspace(wm);
    XSetErrorHandler(dummy_error_handler);

    // Remove client from save set, we don't have to deal with them anymore
    XRemoveFromSaveSet(wm->conn, client->window);
    // Destroy window and delete client entry from state
    XDestroyWindow(wm->conn, client->window);

    clients_destroy_client(&space->clients, client);
    visually_reflect_focus(wm, space);

    if (client == wm->dragged_client)
        wm->dragged_client = NULL;

    XSync(wm->conn, false);
    XSetErrorHandler(on_x_error);
}

static void on_unmap_notify(wm_t *wm, const XUnmapEvent *event)
{
    workspace_t *space = get_workspace(wm);
    // First, ensure that the unmapped window is actually a client that we manage
    client_t *client = clients_find_by_window(&space->clients, event->window);

    if (!client)
        return;

    /*
     * The window is invisible, so get rid of it. Since minimized windows will
     * not be supported, unmap is pretty much identical to destruction
     *
     * Workspace switching will thankfully never reach this line, since the
     * window does not belong on the current workspace anymore.
     */
    unmanage_client(wm, client);
    tile(wm, space);
}

static void kill_client(wm_t *wm, Window window)
{
    // Try to be civil and use a WM protocol
    // If that's not supported, just kill it violently
    if (!try_send_wm_protocol(wm, window, wm->atoms[ATOM_WM_DELETE_WINDOW]))
        XKillClient(wm->conn, window);
}

/*
 * Just iterate over our global key bindings as defined in config.h.
 * If a match is found, call the callback function
 */
static void on_key_press(wm_t *wm, const XKeyEvent *event)
{
    const wm_key_t key = key_event_to_key(wm, event);

    if (are_keys_equal(wm_kill_client_key, key))
        return kill_client(wm, event->window);

    for (int i = 0; i < ARRAY_LEN(wm_bindings); i++)
    {
        if (are_keys_equal(wm_bindings[i].key, key))
            return wm_bindings[i].callback(wm, wm_bindings[i].argument);
    }
}

/*
 * A toplevel window (substructure redirection) requests to be mapped
 * Map it and start keeping track of it
 */
static void on_map_request(wm_t *wm, const XMapRequestEvent *event)
{
    workspace_t *space = get_workspace(wm);

    client_t *c = manage_window(wm, event->window);
    XMapWindow(wm->conn, event->window);

    // Wait until the mapping request is done, and only then change focus!
    XSync(wm->conn, false);
    focus_client(wm, space, c);

    tile(wm, space);
}

static void on_configure_request(wm_t *wm, const XConfigureRequestEvent *event)
{
    XWindowChanges changes = {
        .x = event->x,
        .y = event->y,
        .width = event->width,
        .height = event->height,
        .border_width = event->border_width,
        .sibling = event->above,
        .stack_mode = event->detail,
    };

    // TODO: Should all requests be allowed?
    XConfigureWindow(wm->conn, event->window, event->value_mask, &changes);
}

static void on_enter_notify(wm_t *wm, const XCrossingEvent *event)
{
    if (!wm->has_moved_cursor) return;
    workspace_t *space = get_workspace(wm);

    client_t *client = clients_find_by_window(&space->clients, event->window);
    
    if (client)
        focus_client(wm, space, client);
}

static void on_button_press(wm_t *wm, const XButtonEvent *event)
{
    /*
     * Will trigger manual floating window resizing and positioning. We're going
     * to be storing the initial position and size as a reference point. 
     */
    workspace_t *space = get_workspace(wm);
    client_t *c = clients_find_by_window(&space->clients, event->window);
    if (!c)
        return;

    wm->drag_cursor_x = event->x_root;
    wm->drag_cursor_y = event->y_root;

    Window root;
    unsigned int border_width, depth;

    if (!XGetGeometry(wm->conn, c->window, &root,
            &wm->drag_window_x, &wm->drag_window_y,
            &wm->drag_window_w, &wm->drag_window_h, &border_width, &depth))
    {
        log_fatal("failed to fetch geometry of client during button press");
    }

    XRaiseWindow(wm->conn, c->window);
    wm->dragged_client = c;

    // The window should now be floating if it isn't already
    if (!c->is_floating)
    {
        c->is_floating = true;
        tile(wm, space);
    }
}

static void on_button_release(wm_t *wm, const XButtonEvent *event)
{
    wm->dragged_client = NULL;
}

static void on_motion_notify(wm_t *wm, const XMotionEvent *event)
{
    wm->has_moved_cursor = true;
    client_t *c = wm->dragged_client;

    if (!c)
        return;

    // The user is trying to move the window
    if (event->state & Button1Mask)
    {
        XMoveWindow(wm->conn, c->window,
            wm->drag_window_x + (event->x_root - wm->drag_cursor_x),
            wm->drag_window_y + (event->y_root - wm->drag_cursor_y));
    }
    else if (event->state & Button3Mask)
    {
        int new_w = wm->drag_window_w + (event->x_root - wm->drag_cursor_x);
        int new_h = wm->drag_window_h + (event->y_root - wm->drag_cursor_y);

        // If the client has an explicit size range, respect it
        if (c->max_width != -1) new_w = MIN(new_w, c->max_width);
        if (c->min_width != -1) new_w = MAX(new_w, c->min_width);
        if (c->max_height != -1) new_h = MIN(new_h, c->max_height);
        if (c->min_height != -1) new_h = MAX(new_h, c->min_height);

        new_w = MAX(5, new_w);
        new_h = MAX(5, new_h);

        XResizeWindow(wm->conn, c->window, new_w, new_h);
    }
}

void wm_loop(wm_t *wm)
{
    while (wm->is_running)
    {
        XEvent event;
        XNextEvent(wm->conn, &event);

        switch (event.type)
        {
            case KeyPress: on_key_press(wm, &event.xkey); break;
            case ButtonPress: on_button_press(wm, &event.xbutton); break;
            case ButtonRelease: on_button_release(wm, &event.xbutton); break;

            // Requests refer to actions that have not yet been executed
            // It's the window manager's duty to either ignore or apply them
            case ConfigureRequest: on_configure_request(wm, &event.xconfigurerequest); break;
            case MapRequest: on_map_request(wm, &event.xmaprequest); break;

            // Notifications will just inform the WM that a decision has been made
            // We can't recall them, we just react to them
            case UnmapNotify: on_unmap_notify(wm, &event.xunmap); break;
            case EnterNotify: on_enter_notify(wm, &event.xcrossing); break;
            case MotionNotify: on_motion_notify(wm, &event.xmotion); break;
        }
    }
}

void wm_cleanup(wm_t *wm)
{
    XCloseDisplay(wm->conn);
}

void wm_quit(wm_t *wm, const wm_arg_t arg)
{
    wm->is_running = false;
}

// Create a new window manager child process
void wm_spawn(wm_t *wm, const wm_arg_t arg)
{
    if (fork() == 0)
    {
        // By convention, the first argument should be the path to the invoked command
        execvp((char*) arg.strs[0], (char**) arg.strs);

        // If we've reached this point, it means that the call failed!
        // Normally, the program would have been replaced by the new process
        log_fatal("failed to replace process image in fork()");
    }
}

void wm_adjust_special_width(wm_t *wm, const wm_arg_t arg)
{
    workspace_t *space = get_workspace(wm);
    const int padding = 40;

    int new_width = space->special_width + arg.amount;
    if (new_width < padding || new_width > wm->width - 2 * wm->gap - padding) return;

    space->special_width = new_width;
    tile(wm, space);
}

void wm_reset_special_width(wm_t *wm, const wm_arg_t arg)
{
    workspace_t *space = get_workspace(wm);

    if (space->special_width != wm->width / 2)
    {
        space->special_width = wm->width / 2;
        tile(wm, space);
    }
}

void wm_adjust_gap(wm_t *wm, const wm_arg_t arg)
{
    wm->gap = MAX(0, wm->gap + arg.amount);
    tile(wm, get_workspace(wm));
}

// This is once again inspired by dwm and vim
void wm_focus_on_next(wm_t *wm, const wm_arg_t arg)
{
    workspace_t *space = get_workspace(wm);
    client_t *f = clients_get_focused(&space->clients);

    if (space->clients.length > 1)
    {
        // Wrap around if you've gone past the limit
        client_t *next = (f->next ? f->next : space->clients.head);
        focus_client(wm, space, next);
    }
}

void wm_focus_on_previous(wm_t *wm, const wm_arg_t arg)
{
    workspace_t *space = get_workspace(wm);
    client_t *f = clients_get_focused(&space->clients);

    if (space->clients.length > 1)
    {
        client_t *next = (f->previous ? f->previous : space->clients.tail);
        focus_client(wm, space, next);
    }
}

void wm_make_focused_special(wm_t *wm, const wm_arg_t arg)
{
    workspace_t *space = get_workspace(wm);
    client_t *f = clients_get_focused(&space->clients);

    if (f && !f->is_floating && space->clients.length > 1)
    {
        // Remove from list and then insert again as root (special window)
        clients_remove_client(&space->clients, f);
        clients_insert(&space->clients, f);

        tile(wm, space);
    }
}

void wm_switch_to_workspace(wm_t *wm, const wm_arg_t arg)
{
    if (wm->active_workspace == arg.amount) return;
    workspace_t *space = get_workspace(wm);

    // Unmap all clients in the current workspace, making them temporarily invisible
    for (client_t *c = space->clients.head; c; c = c->next)
    {
        XUnmapWindow(wm->conn, c->window);
    }

    wm->active_workspace = arg.amount;
    // Prevent expected enter notify events from changing focus
    wm->has_moved_cursor = false;
    space = get_workspace(wm);

    for (client_t *c = space->clients.head; c; c = c->next)
    {
        XMapWindow(wm->conn, c->window);
    }

    // Focus back on the window that was active last time we left
    visually_reflect_focus(wm, space);
}

// Send the application currently in focus to the provided workspace
void wm_send_to_workspace(wm_t *wm, const wm_arg_t arg)
{
    if (wm->active_workspace == arg.amount) return;
    workspace_t *source = get_workspace(wm);
    workspace_t *target = &wm->workspaces[arg.amount];

    client_t *client = clients_get_focused(&source->clients);
    // If no client is currently focused, ignore
    if (!client) return;

    if (client == wm->dragged_client)
        wm->dragged_client = NULL;

    // Remove entry from list and add to target
    clients_remove_client(&source->clients, client);
    clients_insert(&target->clients, client);
        
    // The window is gone, focus on the next one on the stack
    clients_remove_focus(&source->clients, client);
    visually_reflect_focus(wm, source);

    XUnmapWindow(wm->conn, client->window);
    // WARNING: We don't want to focus_client since the window is currently unmapped
    // If you try to do this, X11 will explode
    visually_unfocus_focused(wm, target);
    clients_push_focus(&target->clients, client);

    tile(wm, source);
    tile(wm, target);
}

void wm_toggle_float(wm_t *wm, const wm_arg_t arg)
{
    workspace_t *s = get_workspace(wm);
    client_t *target = clients_get_focused(&s->clients);

    if (target)
    {
        target->is_floating = !target->is_floating;
        tile(wm, s);
    }
}
