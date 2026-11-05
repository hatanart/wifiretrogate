/*
 * The status line: "does it work, and if not, why?" Shared by the display
 * and the console.
 */
#pragma once

#include <stddef.h>

void status_line(char *text, size_t size);
