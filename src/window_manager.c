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

// Will return false if the client does not participate in the protocol (x_notes.md)
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

static void set_window_prop(wm_t *wm, Window w, Atom a, Atom type,
                            unsigned long *values, unsigned long total)
{
    // Set the property, overriding the previous value if set
    XChangeProperty(wm->conn, w, a, type, 32, PropModeReplace, (unsigned char*) values, total);
}

// Call with NULL to clear focus
static void focus_client(wm_t *wm, workspace_t *space, client_t *c)
{
    // The previously focused window's border must be reset 
    if (space->focused_client)
        XSetWindowBorder(wm->conn, space->focused_client->frame, wm->border_color.pixel);

    if (!c)
    {
        XSetInputFocus(wm->conn, wm->root, RevertToPointerRoot, CurrentTime);
        // Clear the property so that clients understand that no window is currently in focus
        XDeleteProperty(wm->conn, wm->root, wm->atoms[ATOM_NET_ACTIVE_WINDOW]);
        space->focused_client = NULL;
    }
    else
    {
        XSetWindowBorder(wm->conn, c->frame, wm->focused_border_color.pixel);
        // Raise the window so that the entire border is visible
        XRaiseWindow(wm->conn, c->frame);

        set_window_prop(wm, wm->root, wm->atoms[ATOM_NET_ACTIVE_WINDOW], XA_WINDOW, &c->window, 1);
        // The server will generate FocusIn and FocusOut events
        XSetInputFocus(wm->conn, c->window, RevertToPointerRoot, CurrentTime);
        try_send_wm_protocol(wm, c->window, wm->atoms[ATOM_WM_TAKE_FOCUS]);
        space->focused_client = c;
    }
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
    create_bindings(wm);

    int screen = DefaultScreen(wm->conn);
    wm->width = DisplayWidth(wm->conn, screen);
    wm->height = DisplayHeight(wm->conn, screen);

    for (int i = 0; i < TOTAL_WORKSPACES; i++)
    {
        wm->workspaces[i].special_width = wm->width / 2;
        wm->workspaces[i].clients.data = NULL;
        wm->workspaces[i].clients.length = 0;
        wm->workspaces[i].focused_client = NULL;
    }
    wm->active_workspace = 0;

    // Load in some colors
    wm->colormap = DefaultColormap(wm->conn, screen);

    try_load_named_color(wm, "red", &wm->focused_border_color);
    try_load_named_color(wm, "white", &wm->border_color);

    puts("WM was initialized successfully");
}

static void move_and_resize_client(wm_t *wm, client_t *c, int x, int y, int w, int h)
{
    XMoveResizeWindow(wm->conn, c->frame, x, y, w, h);
    // The x-y position is relative to that of the parent
    XMoveResizeWindow(wm->conn, c->window, 0, 0, w, h);
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

    if (space->clients.length == 0) return;

    const int max_width = wm->width - 2 * WM_BORDER_WIDTH;
    const int max_height = wm->height - 2 * WM_BORDER_WIDTH;

    if (space->clients.length == 1)
    {
        move_and_resize_client(wm, space->clients.data, 0, 0, max_width, max_height);
    }
    else
    {
        // The head of the clients list, also known as the special window,
        // will capture a whole pane on its own.
        move_and_resize_client(wm, space->clients.data, 0, 0, space->special_width, max_height);
        const int remaining_width = max_width - space->special_width;

        // The other windows will just share the remaining space
        int other_height = max_height / (space->clients.length - 1);
        int i = 0;

        for (client_t *c = space->clients.data->next; c; c = c->next, i++)
        {
            move_and_resize_client(wm, c,
                    space->special_width, i * other_height,
                    remaining_width, other_height);
        }
    }
}

static void frame_window(wm_t *wm, Window window)
{
    workspace_t *space = get_workspace(wm);

    // Fetch the window's attributes so that we can create a matching frame with a border
    XWindowAttributes window_attrs;
    XGetWindowAttributes(wm->conn, window, &window_attrs);

    const Window frame = XCreateSimpleWindow(
        wm->conn, wm->root,
        window_attrs.x, window_attrs.y,
        window_attrs.width, window_attrs.height,
        WM_BORDER_WIDTH, WM_BORDER_COLOR, 0x000000);

    /*
     * Forward substructure (geometry, configuration) events of child window to frame,
     * so that they're collected by the root which has enabled substructure
     * redirection. (at least that's my interpretation!)
     */
    XSelectInput(wm->conn, frame, SubstructureNotifyMask | EnterWindowMask);

    // Stores the list of all client windows managed by the window manager
    // This information is important, particularly during window-manager cleanup
    XAddToSaveSet(wm->conn, window);

    XReparentWindow(wm->conn, window, frame, 0, 0);
    // The frame needs to be mapped as well
    // Any mapped child with an unmapped ancestor will behave as if unmapped
    XMapWindow(wm->conn, frame);

    // The change should be reflected to our internal state
    client_t *client = create_client(frame, window);
    clients_insert(&space->clients, client);

    // Register possible bindings
    grab_key(wm, wm_kill_client_key, window);
}

/*
 * Carefully ignore the errors instead of attempting to asynchronously
 * determine if a window is still valid. This is a common pattern in many WM
 * implementations. That's because the window might have already completely destroyed
 * itself before the initial Unmap event arrives at our end.
 */
static void unframe_client(wm_t *wm, client_t *client)
{
    workspace_t *space = get_workspace(wm);
    XSetErrorHandler(dummy_error_handler);

    // Unmap frame and reparent window
    XUnmapWindow(wm->conn, client->frame);
    XReparentWindow(wm->conn, client->window, wm->root, 0, 0);
    // Remove client from save set, we don't have to deal with them anymore
    XRemoveFromSaveSet(wm->conn, client->window);
    // Destroy frame and delete client entry from state
    XDestroyWindow(wm->conn, client->frame);

    if (space->focused_client == client)
        focus_client(wm, space, client->previous ? client->previous : client->next);

    clients_destroy_client(&space->clients, client);

    XSync(wm->conn, false);
    XSetErrorHandler(on_x_error);
}

static void on_unmap_notify(wm_t *wm, const XUnmapEvent *event)
{
    workspace_t *space = get_workspace(wm);
    // First, ensure that the unmapped window is actually a client that we manage
    client_t *client = clients_find_by_window(space->clients, event->window, CLIENT_WINDOW);

    if (!client)
        return;

    // The window is invisible, so get rid of the frame
    // Since minimized windows will not be supported, unmap is pretty much identical to destruction
    // We might unmap frame windows during workspace switching,
    //   but notice that we won't reach this line of code
    unframe_client(wm, client);
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
 * A window requests to be mapped, so get ready for it being visible.
 * We need to enclose the window in a decorative frame (title bar, border)
 */
static void on_map_request(wm_t *wm, const XMapRequestEvent *event)
{
    workspace_t *space = get_workspace(wm);

    frame_window(wm, event->window);
    XMapWindow(wm->conn, event->window);

    // Wait until the mapping request is done, and only then change focus!
    // The new window will always be the root of our list
    XSync(wm->conn, false);
    focus_client(wm, space, space->clients.data);

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

    // TODO: Why does this work? Also, why does this event get called so rarely?
    // More specifically, why does it only get called when xterm is trying to start?!
    XConfigureWindow(wm->conn, event->window, event->value_mask, &changes);
}

static void on_enter_notify(wm_t *wm, const XCrossingEvent *event)
{
    if (!wm->has_moved_cursor) return;
    workspace_t *space = get_workspace(wm);

    // Frames are top-level, we should be searching for those
    client_t *client = clients_find_by_window(space->clients, event->window, CLIENT_FRAME);

    if (client)
        focus_client(wm, space, client);
}

static void on_motion_notify(wm_t *wm, const XMotionEvent *event)
{
    wm->has_moved_cursor = true;
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
        execvp((char*) arg.str_array[0], (char**) arg.str_array);

        // If we've reached this point, it means that the call failed!
        // Normally, the program would have been replaced by the new process
        log_fatal("failed to replace process image in fork()");
    }
}

void wm_adjust_special_width(wm_t *wm, const wm_arg_t arg)
{
    workspace_t *space = get_workspace(wm);

    int new_width = space->special_width + arg.amount;
    if (new_width < WM_SPECIAL_PADDING || new_width > wm->width - WM_SPECIAL_PADDING) return;

    space->special_width = new_width;
    tile(wm, space);
}

// This is once again inspired by dwm and vim
void wm_focus_on_next(wm_t *wm, const wm_arg_t arg)
{
    workspace_t *space = get_workspace(wm);

    if (space->focused_client && space->focused_client->next)
        focus_client(wm, space, space->focused_client->next);
}

void wm_focus_on_previous(wm_t *wm, const wm_arg_t arg)
{
    workspace_t *space = get_workspace(wm);

    if (space->focused_client && space->focused_client->previous)
        focus_client(wm, space, space->focused_client->previous);
}

void wm_make_focused_special(wm_t *wm, const wm_arg_t arg)
{
    workspace_t *space = get_workspace(wm);

    if (space->focused_client && space->clients.length > 1)
    {
        // Remove from list and then insert again as root (special window)
        clients_remove_client(&space->clients, space->focused_client);
        clients_insert(&space->clients, space->focused_client);

        tile(wm, space);
    }
}

void wm_switch_to_workspace(wm_t *wm, const wm_arg_t arg)
{
    if (wm->active_workspace == arg.amount) return;
    workspace_t *space = get_workspace(wm);

    // Unmap all clients in the current workspace, making them temporarily invisible
    for (client_t *c = space->clients.data; c; c = c->next)
    {
        XUnmapWindow(wm->conn, c->frame);
    }

    wm->active_workspace = arg.amount;
    // Prevent expected enter notify events from changing focus
    wm->has_moved_cursor = false;
    space = get_workspace(wm);

    for (client_t *c = space->clients.data; c; c = c->next)
    {
        XMapWindow(wm->conn, c->frame);
    }

    // Focus back on the window that was active last time we left
    focus_client(wm, space, space->focused_client);
}

// Send the application currently in focus to the provided workspace
void wm_send_to_workspace(wm_t *wm, const wm_arg_t arg)
{
    if (wm->active_workspace == arg.amount) return;
    workspace_t *source = get_workspace(wm);
    workspace_t *target = &wm->workspaces[arg.amount];

    // If no client is currently focused, ignore
    if (!source->focused_client) return;
    client_t *client = source->focused_client;

    // Remove entry from list and add to target
    clients_remove_client(&source->clients, client);
    clients_insert(&target->clients, client);
        
    // The window is gone, focus on special and make the old one invisible
    focus_client(wm, source, source->clients.data);
    XUnmapWindow(wm->conn, client->frame);
    focus_client(wm, target, client);

    tile(wm, source);
    tile(wm, target);
}
