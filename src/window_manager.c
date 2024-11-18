#include "window_manager.h"
#include "utils.h"
#include "clients.h"
#include "config.h"
#include <X11/XKBlib.h>
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
    wm->clients = NULL;
    wm->is_running = true;

    /*
     * Checking whether we've got a right for Substructure Redirection
     * Using a temporary error handler for this special initialization phase
     */
    XSetErrorHandler(on_wm_error);
    // For substructure redirection, check out page 361 of the programming manual!
    XSelectInput(wm->conn, wm->root, SubstructureRedirectMask | SubstructureNotifyMask);
    // Wait until all pending requests have been fully processed by the X server.
    // the second argument must always be false, since we don't want to discard incoming queue events
    XSync(wm->conn, false);

    XSetErrorHandler(on_x_error);

    // Setting the cursor for our root window. The default is a big X
    Cursor cursor = XCreateFontCursor(wm->conn, XC_left_ptr);
    XDefineCursor(wm->conn, wm->root, cursor);

    wm->atoms[ATOM_WM_PROTOCOLS] = XInternAtom(wm->conn, "WM_PROTOCOLS", false);
    wm->atoms[ATOM_WM_DELETE_WINDOW] = XInternAtom(wm->conn, "WM_DELETE_WINDOW", false);
    create_bindings(wm);

    puts("WM was initialized successfully");
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

static void frame_window(wm_t *wm, Window window)
{
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
    XSelectInput(wm->conn, frame, SubstructureNotifyMask);

    // Stores the list of all client windows managed by the window manager
    // This information is important, particularly during window-manager cleanup
    XAddToSaveSet(wm->conn, window);

    XReparentWindow(wm->conn, window, frame, 0, 0);
    // The frame needs to be mapped as well
    // Any mapped child with an unmapped ancestor will behave as if unmapped
    XMapWindow(wm->conn, frame);

    // The change should be reflected to our internal state
    clients_insert(&wm->clients, create_client(frame, window));

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
    XSetErrorHandler(dummy_error_handler);

    // Unmap frame and reparent window
    XUnmapWindow(wm->conn, client->frame);
    XReparentWindow(wm->conn, client->window, wm->root, 0, 0);
    // Remove client from save set, we don't have to deal with them anymore
    XRemoveFromSaveSet(wm->conn, client->window);
    // Destroy frame and delete client entry from state
    XDestroyWindow(wm->conn, client->frame);
    clients_destroy_client(&wm->clients, client);

    XSync(wm->conn, false);

    XSetErrorHandler(on_x_error);
}

static void on_unmap_notify(wm_t *wm, const XUnmapEvent *event)
{
    // First, ensure that the unmapped window is actually a client that we manage
    client_t *client = clients_find_by_window(wm->clients, event->window);

    if (!client)
        return;

    // The window is invisible, so get rid of the frame
    // Since minimized windows will not be supported, unmap is pretty much identical to destruction
    unframe_client(wm, client);
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
    wm_key_t key = key_event_to_key(wm, event);

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
    frame_window(wm, event->window);
    XMapWindow(wm->conn, event->window);
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

    // Just forward the event. TODO: Enforce the layout policy
    // Remember: requests, unlike notify's, require some action on our part
    XConfigureWindow(wm->conn, event->window, event->value_mask, &changes);
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

