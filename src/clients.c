#include "clients.h"
#include "utils.h"
#include <assert.h>
#include <stdlib.h>

client_t* create_client(Window frame, Window window)
{
    client_t *c = malloc(sizeof(client_t));
    if (!c)
        log_fatal("failed to allocate memory for client");

    c->previous = NULL;
    c->next = NULL;
    c->window = window;
    c->frame = frame;
    // Initialize everything to negative one to mark them as disabled
    c->min_width = c->max_width = -1;
    c->min_height = c->max_height = -1;

    return c;
}

void clients_initialize(client_list_t *list)
{
    list->head = NULL;
    list->length = 0;
    list->focus_stack = NULL;
}

void clients_insert(client_list_t *list, client_t *client)
{
    // Just insert the client at the beginning
    if (list->head != NULL)
    {
        client->next = list->head;
        list->head->previous = client;
    }    
    else
        list->tail = client;
        
    list->head = client;
    list->length++;
}

void clients_remove_client(client_list_t *list, client_t *client)
{
    assert(list && client);
    
    // If we're at the beginning of the list, just move forward
    if (client->previous == NULL)
        list->head = client->next;

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
    // Remove from focus stack as well, automatically!
    clients_remove_focus(list, client);
    free(client);
}

client_t* clients_find_by_window(const client_list_t *list,
                                 Window window, client_window_e type)
{
    if (list->head == NULL)
        return NULL;

    // Just return the first match, performing a linear search
    for (client_t *c = list->head; c != NULL; c = c->next)
    {
        if ((type == CLIENT_WINDOW && c->window == window) ||
            (type == CLIENT_FRAME && c->frame == window))
        {
            return c;
        }
    }

    return NULL;
}

client_t* clients_get_focused(client_list_t *list)
{
    if (!list->focus_stack) return NULL;
    return list->focus_stack->client;
}

// A private method that removes the specified element from the singly linked list
static focus_stack_t* remove_from_focus(client_list_t *list, client_t *c)
{
    if (!list->focus_stack) return NULL;

    // If it's the root, be careful
    if (list->focus_stack->client == c)
    {
        focus_stack_t *temp = list->focus_stack;
        list->focus_stack = temp->next;
        return temp;
    }

    focus_stack_t *prev = list->focus_stack;
    for (focus_stack_t *f = prev->next; f; f = f->next)
    {
        if (f->client == c)
        {
            // Just jump over the current node, effectively deleting it
            prev->next = f->next;
            return f;
        }
    }

    return NULL;
}

void clients_remove_focus(client_list_t *list, client_t *c)
{
    // Free up the heap-allocated memory as well
    free(remove_from_focus(list, c));
}

void clients_push_focus(client_list_t *list, client_t *c)
{
    focus_stack_t *item = remove_from_focus(list, c);

    // If the item was not already inside the stack, create it
    if (item == NULL)
    {
        item = malloc(sizeof(focus_stack_t));
        assert(item);
        item->client = c;
    }

    item->next = list->focus_stack;
    list->focus_stack = item;
}
