/*
 * Status screens on the OLED, with burn-in protection.
 */
#pragma once

void display_init(void);

/* true if a display answered at start-up */
bool display_present(void);

/* Bridge mode: refresh the status screen (call once a second) */
void display_bridge_status(void);

/* Setup mode: show how to join the setup access point (call once a second) */
void display_setup(const char *ap_ssid, const char *ap_password, const char *message);

/* Turn the screen on again (button press) */
void display_wake(void);
