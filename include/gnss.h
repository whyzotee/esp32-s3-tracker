#ifndef GNSS_H
#define GNSS_H

#include <board_pins.h>

void setup_gnss(bool rawNmeaDebug);
void stop_gnss();
// True only when a new fix passes the quality/stability checks this acquisition.
bool get_location();

#endif
