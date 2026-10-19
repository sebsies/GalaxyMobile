#pragma once

#include <jni.h>

namespace mobile_haptics {

enum class HapticType {
    Click,       // Short tactile click for virtual buttons (A, B, Z, D-pad)
    Tick,        // Light micro-tick for UI controls, slider adjustments, tab switches
    Spin,        // Energetic pulse for Mario spin attacks & Launch Stars
    RumblePulse  // Engine rumble motor engagement
};

void init(JavaVM* vm, jobject activity);
void shutdown();
void trigger(HapticType type);
void updateRumble(int rumbleActive);

} // namespace mobile_haptics
