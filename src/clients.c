#include "clients.h"
#include "utils.h"
#include <assert.h>
#include <stdlib.h>

client_t* create_client(Window frame, Window window)
{
    client_t *client = malloc(sizeof(client_t));
    if (!client)
        log_fatal("failed to allocate memory for client");

    client->previous = NULL;
    client->next = NULL;
    client->window = window;
    client->frame = frame;

    return client;
}

void clients_insert(client_list_t *list, client_t *client)
{
    // Just insert the client at the beginning
    if (list->data != NULL)
    {
        client->next = list->data;
        list->data->previous = client;
    }    
    else
        list->tail = client;
        
    list->data = client;
    list->length++;
}

void clients_remove_client(client_list_t *list, client_t *client)
{
    assert(list && client);
    
    // If we're at the beginning of the list, just move forward
    if (client->previous == NULL)
        list->data = client->next;

    if (client->next == NULL)
        list->tail = client->previous;
    else
        client->next->previous = client->previous;

    if (client->previous)
        client->previous->next = client->next;

    // The client might now be inserted on a brand new list,
    // we can't just leave it into an invalid state
    client->next = client->previous = NULL;
    list->length--;
}

void clients_destroy_client(client_list_t *list, client_t *client)
{
    clients_remove_client(list, client);
    free(client);
}

client_t* clients_find_by_window(const client_list_t list, Window window, client_window_e type)
{
    if (list.data == NULL)
        return NULL;

    // Just return the first match, performing a linear search
    for (client_t *c = list.data; c != NULL; c = c->next)
    {
        if ((type == CLIENT_WINDOW && c->window == window) ||
            (type == CLIENT_FRAME && c->frame == window))
        {
            return c;
        }
    }

    return NULL;
}
