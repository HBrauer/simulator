#ifndef UTIL_H
#define UTIL_H

#include <stdio.h>
#include <string.h>

static inline void sim_strlcpy(char *dst, const char *src, size_t dst_size)
{
    if (dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

#endif
