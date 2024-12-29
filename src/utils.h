#ifndef _WM_UTILS_H
#define _WM_UTILS_H

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) < (b) ? (b) : (a))

#define ARRAY_LEN(a) (sizeof(a) / sizeof(a[0]))

// Prints out an error message and panics
void log_fatal(const char *format, ...);

#endif
