#ifndef GNSS_H
#define GNSS_H

#include <board_pins.h>

void setup_gnss(bool rawNmeaDebug);
void stop_gnss();
// Start/reuse an acquisition on the dedicated GPS task.
void start_gnss_acquisition(bool restart = false);
// Main-task only: consume an immutable fix snapshot, never read the UART/parser.
void service_gnss();
enum class GnssStatus { Unavailable, Searching, FixReady, TimedOut };
// Main-task snapshot outcome; the driver never changes trip state.
GnssStatus gnss_status();
// True only when a new fix passes the quality/stability checks this acquisition.
bool get_location();

#endif
