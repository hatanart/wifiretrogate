/*
 * Telnet console (port 23), line mode: the client echoes locally and sends
 * whole lines, which suits DOS and Amiga Telnet clients. Reachable on the
 * service address (and on 192.168.4.1 in setup mode).
 */
#pragma once

void telnet_start(void);

/* Open Telnet sessions */
int telnet_sessions(void);
