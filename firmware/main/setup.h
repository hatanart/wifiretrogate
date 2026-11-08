/*
 * Setup mode: an access point with a one-page web form for adding and
 * forgetting Wi-Fi networks. Forwarding is off in this mode.
 */
#pragma once

/* Never returns; leaves setup mode by restarting: "finish" on the web page,
 * the button, the console's reboot, or 10 minutes without activity (only
 * when a network is saved, else it would come straight back). */
void setup_run(void);

/* Something happened (a console command): restarts the 10-minute timeout */
void setup_activity(void);
