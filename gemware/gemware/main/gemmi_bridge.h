#pragma once

/* Start the background HTTP poller after Wi-Fi is connected. */
void gemmi_bridge_start(void);

/* Queue a device reply for the poll task to POST. A NULL or empty choice is
 * a plain acknowledgement; a non-empty choice answers an ask. */
void gemmi_bridge_answer(int event_id, const char *choice);
