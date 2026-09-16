#include "colors.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

const uint32_t SENTINEL_INVALID_COLOR = 0xFFFFFFFFu;

const NamedColor kNamedColors[] = {
    { "black",     0x000000 }, // alias of "off"
    { "white",     0xFFFFFF },
    { "red",       0xFF0000 },
    { "green",     0x00FF00 },
    { "blue",      0x0000FF },
    { "yellow",    0xFFFF00 },
    { "cyan",      0x00FFFF },
    { "magenta",   0xFF00FF },
    { "purple",    0x800080 },
    { "orange",    0xFFA500 },
    { "pink",      0xFFC0CB },
    { "gray",      0x808080 },
    { "off",       0x000000 },
    { "grey",      0x808080 },
    { "lime",      0xBFFF00 },
    { "teal",      0x008080 },
    { "navy",      0x000080 },
    { "maroon",    0x800000 },
    { "olive",     0x808000 },
    { "aqua",      0x00FFFF },
    { "fuchsia",   0xFF00FF },
    { "indigo",    0x4B0082 },
    { "violet",    0xEE82EE },
    { "gold",      0xFFD700 },
    { "silver",    0xC0C0C0 },
    { "brown",     0xCC9B65 },
    { "beige",     0xF5F5DC },
    { "coral",     0xFF7F50 },
    { "salmon",    0xFA8072 },
    { "chocolate", 0xD2691E },
    { "crimson",   0xDC143C },
    { "turquoise", 0x40E0D0 },
    { "skyblue",   0x87CEEB },
    { "lavender",  0xE6E6FA },
    { "mint",      0x98FF98 },
    { "peach",     0xFFDAB9 },
    { "hotpink",   0xFF69B4 },
    { "plum",      0xDDA0DD },
    { "orchid",    0xDA70D6 },
    { "khaki",     0xF0E68C },
};

const int kNamedColorCount = (int)(sizeof(kNamedColors) / sizeof(kNamedColors[0]));

static void str_to_lower(char* s) {
    for (; *s; ++s) {
        if (*s >= 'A' && *s <= 'Z') {
            *s = (char)(*s - 'A' + 'a');
        }
    }
}

// NULL-terminates the first token, trims surrounding whitespace/#/commas.
static const char* next_color_token(char* buf) {
    // trim leading whitespace
    while (*buf != '\0' && isspace((unsigned char)*buf)) {
        ++buf;
    }
    if (*buf == '#') {
        ++buf;  // a '#' may prefix a hex color
    }
    // trim trailing whitespace / '#' / ',' / '\r' / '\n'
    size_t len = strlen(buf);
    while (len > 0) {
        char c = buf[len - 1];
        if (isspace((unsigned char)c) || c == ',' || c == '#') {
            buf[--len] = '\0';
        } else {
            break;
        }
    }
    return buf;
}

uint32_t parse_color(const char* token) {
    if (token == NULL || *token == '\0') {
        return SENTINEL_INVALID_COLOR;
    }

    char buf[48];
    strncpy(buf, token, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    const char* clean = next_color_token(buf);
    if (*clean == '\0') {
        return SENTINEL_INVALID_COLOR;
    }

    char work[64];
    strncpy(work, clean, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';
    str_to_lower(work);

    // 1. Named color lookup (case-insensitive).
    for (int i = 0; i < kNamedColorCount; ++i) {
        if (strcmp(work, kNamedColors[i].name) == 0) {
            return kNamedColors[i].value;
        }
    }

    // 2. 6-digit hex.
    if (strlen(work) == 6) {
        uint32_t value = 0;
        for (int i = 0; i < 6; ++i) {
            char c = work[i];
            int nibble;
            if (c >= '0' && c <= '9') {
                nibble = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                nibble = c - 'a' + 10;
            } else {
                return SENTINEL_INVALID_COLOR;
            }
            value = (value << 4) | (uint32_t)nibble;
        }
        return value;
    }

    return SENTINEL_INVALID_COLOR;
}