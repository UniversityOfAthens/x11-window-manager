#include "window_manager.h"

int main()
{
    wm_t w_manager;

    wm_setup(&w_manager);
    wm_loop(&w_manager);
    wm_cleanup(&w_manager);
}
