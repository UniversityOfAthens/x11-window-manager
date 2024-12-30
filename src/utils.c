#include "utils.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

void log_fatal(const char *format, ...)
{
    va_list args;
    va_start(args, format);

    // Just print out the supplied message with an informative prefix
    fprintf(stderr, "{TestWM error}: ");
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    
    va_end(args);
    exit(EXIT_FAILURE);
}
