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
    if (*list != NULL)
    {
        client->next = *list;
        (*list)->previous = client;
    }    
        
    *list = client;
}

void clients_destroy_client(client_list_t *list, client_t *client)
{
    assert(list && client);
    
    // If we're at the beginning of the list, just move forward
    if (client->previous == NULL)
    {
        if (client->next)
            client->next->previous = NULL;

        *list = client->next;
    }
    else
    {
        client->previous->next = client->next;

        if (client->next)
            client->next->previous = client->previous;
    }

    free(client);
}

client_t* clients_find_by_window(const client_list_t list, Window window)
{
    if (list == NULL)
        return NULL;

    // Just return the first match, performing a linear search
    for (client_t *c = list; c != NULL; c = c->next)
        if (c->window == window)
            return c;

    return NULL;
}
