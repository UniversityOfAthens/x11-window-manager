#ifndef _WM_UTILS_H
#define _WM_UTILS_H

#define ARRAY_LEN(a) (sizeof(a) / sizeof(a[0]))

// Prints out an error message and panics
void log_fatal(const char *format, ...);

#endif
