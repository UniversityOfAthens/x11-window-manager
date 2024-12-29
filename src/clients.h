#ifndef _WM_CLIENTS_H
#define _WM_CLIENTS_H

#include <stdbool.h>
#include <X11/Xutil.h>

/*
 * A doubly-linked list of all top-level windows that our WM is responsible of
 * managing. We'll usually not deal with more than a hundred clients, so I
 * BELIEVE this is efficient enough. :)
 */
typedef struct client_t
{
    Window frame;
    Window window;
    bool is_floating;

    // These will be left to -1 when disabled
    int min_width, min_height;
    int max_width, max_height;

    struct client_t *next;
    struct client_t *previous;
} client_t;

typedef struct
{
    client_t *data;
    // Storing last item for faster access
    client_t *tail;

    int length;
} client_list_t;

void clients_insert(client_list_t *list, client_t *client);
// NOTE: Will just remove it from the list, you need to destroy it yourself!
void clients_remove_client(client_list_t *list, client_t *client);
// Will remove from list and then actually free up the resources
void clients_destroy_client(client_list_t *list, client_t *client);

client_t* create_client(Window frame, Window window);

typedef enum
{
    CLIENT_FRAME,
    CLIENT_WINDOW,
} client_window_e;

// Returns NULL upon search failure
client_t* clients_find_by_window(const client_list_t list,
                                 Window window, client_window_e type);

#endif
