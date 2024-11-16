#ifndef _WM_CLIENTS_H
#define _WM_CLIENTS_H

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

    struct client_t *next;
    struct client_t *previous;
} client_t;

typedef client_t* client_list_t;

void clients_insert(client_list_t *list, client_t *client);
void clients_destroy_client(client_list_t *list, client_t *client);

client_t* create_client(Window frame, Window window);
// Returns NULL upon search failure
client_t* clients_find_by_window(const client_list_t list, Window window);

#endif
