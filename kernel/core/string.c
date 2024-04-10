#include <kernel/string.h>

void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    uint8_t v = (uint8_t)c;
    while (n--) {
        *p++ = v;
    }
    return s;
}

void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else if (d > s) {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    while (n--) {
        if (*p1 != *p2) {
            return *p1 - *p2;
        }
        p1++;
        p2++;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len]) {
        len++;
    }
    return len;
}

char *strcpy(char *dest, const char *src) {
    char *ret = dest;
    while ((*dest++ = *src++))
        ;
    return ret;
}

char *strncpy(char *dest, const char *src, size_t n) {
    char *ret = dest;
    while (n && (*dest++ = *src++)) {
        n--;
    }
    while (n--) {
        *dest++ = '\0';
    }
    return ret;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

char *strcat(char *dest, const char *src) {
    char *ret = dest;
    while (*dest) {
        dest++;
    }
    while ((*dest++ = *src++))
        ;
    return ret;
}

char *strncat(char *dest, const char *src, size_t n) {
    char *ret = dest;
    while (*dest) {
        dest++;
    }
    while (n && (*dest++ = *src++)) {
        n--;
    }
    if (n == 0) {
        *dest = '\0';
    }
    return ret;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) {
            return (char *)s;
        }
        s++;
    }
    return (c == 0) ? (char *)s : NULL;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    do {
        if (*s == (char)c) {
            last = s;
        }
    } while (*s++);
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        if (*haystack == *needle) {
            const char *h = haystack;
            const char *n = needle;
            while (*h && *n && *h == *n) {
                h++;
                n++;
            }
            if (!*n) return (char *)haystack;
        }
    }
    return NULL;
}

static void fmt_number(char **buf, size_t *rem, uint64_t n, int base, bool sign, int width, char pad, bool upper) {
    char tmp[65];
    int idx = 0;
    bool negative = false;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (sign && (int64_t)n < 0) {
        negative = true;
        n = 0 - n;
    }

    if (n == 0) {
        tmp[idx++] = '0';
    } else {
        while (n != 0) {
            tmp[idx++] = digits[n % (unsigned)base];
            n /= (unsigned)base;
        }
    }

    if (negative) {
        if (pad == '0') {
            if (*rem > 1) {
                *(*buf)++ = '-';
                (*rem)--;
            }
            width--;
        } else {
            tmp[idx++] = '-';
        }
    }

    while (width-- > idx && *rem > 1) {
        *(*buf)++ = pad;
        (*rem)--;
    }

    while (idx > 0) {
        idx--;
        if (*rem > 1) {
            *(*buf)++ = tmp[idx];
            (*rem)--;
        }
    }
}

int vsnprintf(char *str, size_t size, const char *format, va_list ap) {
    if (!str || size == 0) {
        return 0;
    }

    char *buf = str;
    size_t rem = size;

    while (*format) {
        if (*format != '%') {
            if (rem > 1) {
                *buf++ = *format;
                rem--;
            }
            format++;
            continue;
        }

        format++; /* Skip '%' */
        if (*format == '\0') {
            break;
        }

        if (*format == '%') {
            if (rem > 1) {
                *buf++ = '%';
                rem--;
            }
            format++;
            continue;
        }

        char pad = ' ';
        if (*format == '0') {
            pad = '0';
            format++;
        }

        int width = 0;
        while (*format >= '0' && *format <= '9') {
            if (width < 1024) width = width * 10 + (*format - '0');
            format++;
        }

        int length_mod = 0; /* 0 = normal, 1 = l (long), 2 = ll (long long), 3 = z (size_t) */
        if (*format == 'l') {
            length_mod = 1;
            format++;
            if (*format == 'l') {
                length_mod = 2;
                format++;
            }
        } else if (*format == 'z') {
            length_mod = 3;
            format++;
        }

        switch (*format) {
            case 'c': {
                char c = (char)va_arg(ap, int);
                if (rem > 1) {
                    *buf++ = c;
                    rem--;
                }
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) {
                    s = "(null)";
                }
                size_t slen = strlen(s);
                while (width > (int)slen) {
                    if (rem > 1) {
                        *buf++ = ' ';
                        rem--;
                    }
                    width--;
                }
                while (*s) {
                    if (rem > 1) {
                        *buf++ = *s;
                        rem--;
                    }
                    s++;
                }
                break;
            }
            case 'd':
            case 'i': {
                int64_t val;
                if (length_mod == 2 || length_mod == 1 || length_mod == 3) {
                    val = va_arg(ap, int64_t);
                } else {
                    val = va_arg(ap, int);
                }
                fmt_number(&buf, &rem, (uint64_t)val, 10, true, width, pad, false);
                break;
            }
            case 'u': {
                uint64_t val;
                if (length_mod == 2 || length_mod == 1 || length_mod == 3) {
                    val = va_arg(ap, uint64_t);
                } else {
                    val = va_arg(ap, unsigned int);
                }
                fmt_number(&buf, &rem, val, 10, false, width, pad, false);
                break;
            }
            case 'x': {
                uint64_t val;
                if (length_mod == 2 || length_mod == 1 || length_mod == 3) {
                    val = va_arg(ap, uint64_t);
                } else {
                    val = va_arg(ap, unsigned int);
                }
                fmt_number(&buf, &rem, val, 16, false, width, pad, false);
                break;
            }
            case 'X': {
                uint64_t val;
                if (length_mod == 2 || length_mod == 1 || length_mod == 3) {
                    val = va_arg(ap, uint64_t);
                } else {
                    val = va_arg(ap, unsigned int);
                }
                fmt_number(&buf, &rem, val, 16, false, width, pad, true);
                break;
            }
            case 'p': {
                void *ptr = va_arg(ap, void *);
                if (rem > 2) {
                    *buf++ = '0';
                    *buf++ = 'x';
                    rem -= 2;
                }
                fmt_number(&buf, &rem, (uint64_t)ptr, 16, false, 16, '0', false);
                break;
            }
            default:
                if (rem > 1) {
                    *buf++ = *format;
                    rem--;
                }
                break;
        }
        format++;
    }

    *buf = '\0';
    return (int)(buf - str);
}

int snprintf(char *str, size_t size, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vsnprintf(str, size, format, ap);
    va_end(ap);
    return ret;
}
