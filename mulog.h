/*
 * Copyright 2021, Michael Clark <micheljclark@mac.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#pragma once

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Debug
 */

extern int debug;

static void mu_set_debug(int level) { debug = level; }
static void log_printf(const char* fmt, ...);
#define debugf(...) if(debug > 0) log_printf(__VA_ARGS__)
#define tracef(...) if(debug > 1) log_printf(__VA_ARGS__)


/*
 * logging implementation
 */

static void log_printf(const char* fmt, ...)
{
    int len;
    char buf[128];
    char *pbuf = buf;
    char *hbuf = NULL;
    va_list ap;
    va_start(ap, fmt);
    len = vsnprintf(pbuf, sizeof(buf)-1, fmt, ap);
    pbuf[sizeof(buf)-1] = '\0';
    va_end(ap);
    if (len >= sizeof(buf)) {
        pbuf = hbuf = (char*)malloc(len + 1);
        va_start(ap, fmt);
        len = vsnprintf(pbuf, len + 1, fmt, ap);
        pbuf[len] = '\0';
        va_end(ap);
    }
    fwrite(pbuf, 1, len, stderr);
    if (hbuf) free(hbuf);
}

#ifdef __cplusplus
}
#endif
