/*
 * Setup mode: an access point with a one-page web form for adding and
 * forgetting Wi-Fi networks. Forwarding is off in this mode.
 */
#pragma once

/* Never returns; leaves setup mode by restarting. */
void setup_run(void);
