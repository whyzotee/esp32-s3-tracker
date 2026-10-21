#pragma once

// Shutdown is terminal until the next deep-sleep wake or reset.
namespace DeviceLifecycle {
bool shuttingDown();
[[noreturn]] void powerOff();
}
