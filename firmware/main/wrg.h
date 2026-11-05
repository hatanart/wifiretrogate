/*
 * Device-wide helpers implemented in main.c.
 */
#pragma once

#include <stdbool.h>

bool wrg_setup_mode(void);

/* Restart into setup mode (true) or bridge mode (false) */
void wrg_restart(bool setup_mode) __attribute__((noreturn));
