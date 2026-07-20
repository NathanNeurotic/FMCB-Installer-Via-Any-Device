#ifndef SIOPRINTF_H
#define SIOPRINTF_H

/* Modern ps2sdk removed sio_printf() from <sio.h>. Provide a small drop-in shim
   so the serial (SIO) debug feedback used across the installer keeps working.
   This header is force-included for every translation unit (see the Makefile's
   -include flag) so files that call sio_printf() directly do not each need to
   pull in main.h. The include guard makes double-inclusion harmless. */

#include <sio.h>
#include <stdio.h>
#include <stdarg.h>

static inline int sio_printf(const char *format, ...)
{
    char buffer[512];
    va_list args;
    int result;

    va_start(args, format);
    result = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    sio_puts(buffer);
    return result;
}

#endif /* SIOPRINTF_H */
