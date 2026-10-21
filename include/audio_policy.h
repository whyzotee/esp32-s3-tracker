#pragma once

// Map trip state to feedback. The audio driver independently enforces shutdown.
namespace AudioPolicy {
void refresh();
void transmissionResult(bool acknowledged);
}
