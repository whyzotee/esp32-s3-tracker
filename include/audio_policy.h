#pragma once

// Map trip state to feedback. The audio driver independently enforces shutdown.
namespace AudioPolicy {
void refresh();
// Success means local send completion for status 0, network ACK for other statuses.
void transmissionResult(bool successful);
}
