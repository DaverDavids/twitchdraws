#pragma once

#include <stdint.h>

// Exact named-color set honored by the v1 Python server (matrix_server.py).
// Pairs name -> packed 0x00RRGGBB.
typedef struct {
    const char* name;
    uint32_t    value;
} NamedColor;

extern const NamedColor kNamedColors[];
extern const int        kNamedColorCount;

// Resolves a color token to a packed 0x00RRGGBB pixel value.
// Accepts:
//   - a named color from kNamedColors (case-insensitive)
//   - a 6-digit hex string, with or without a leading '#', case-insensitive
//     (e.g. "#FF00AA", "ff00aa")
// Returns 0xFFFFFFFF (SENTINEL_INVALID_COLOR) when the token is not a color.
uint32_t parse_color(const char* token);

extern const uint32_t SENTINEL_INVALID_COLOR;