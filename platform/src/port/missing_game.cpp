// Definitions the original binary had (or never needed) that the decomp does
// not provide as out-of-line code.
#include <string.h>

#include "Game/Effect/ParticleEmitter.hpp"
#include "Game/System/StationedArchiveLoader.hpp"
#include "JSystem/J3DGraphBase/J3DPacket.hpp"
#include "nw4r/lyt/pane.h"
#include "nw4r/ut/TextWriterBase.h"

// Base-class draw of the J3D packet hierarchy does nothing.
void J3DPacket::draw() {}

// Only derived conditions are ever instantiated; the base accepts everything.
bool StationedArchiveLoader::Condition::isExecute(const MR::StationedFileInfo*) const { return true; }

namespace nw4r {
    namespace lyt {
        Pane::Pane() {
            Init();
            mBasePosition = 4;  // centre/centre
            memset(mName, 0, sizeof(mName));
            memset(mUserData, 0, sizeof(mUserData));
            mTranslate = math::VEC3(0.0f, 0.0f, 0.0f);
            mRotate = math::VEC3(0.0f, 0.0f, 0.0f);
            mScale = math::VEC2(1.0f, 1.0f);
            mSize = Size(0.0f, 0.0f);
            mAlpha = 0xFF;
            mGlbAlpha = mAlpha;
            mFlag = 0;
            SetVisible(true);
        }
    }  // namespace lyt

    namespace ut {
        // The game only prints wide strings (see CustomTagProcessor.cpp).
        template <>
        f32 TextWriterBase< char >::PrintImpl(StreamType str, int length) {
            (void)str;
            (void)length;
            return 0.0f;
        }
    }  // namespace ut
}  // namespace nw4r

extern "C" {
__attribute__((weak)) int port_is_fullscreen_aspect(void) { return 0; }
__attribute__((weak)) float port_get_display_aspect(void) { return 16.0f / 9.0f; }
__attribute__((weak)) int port_is_plus_edition(void) { return 0; }
__attribute__((weak)) int port_is_early_luigi_unlocked(void) { return 0; }
__attribute__((weak)) int port_is_modern_boo_flight(void) { return 0; }
__attribute__((weak)) int port_is_skip_cutscenes(void) { return 0; }
__attribute__((weak)) int port_is_infinite_health(void) { return 0; }
__attribute__((weak)) int port_is_infinite_powerup(void) { return 0; }
__attribute__((weak)) int port_is_flight_everywhere(void) { return 0; }
__attribute__((weak)) int port_is_hd_ui_enabled(void) { return 0; }
__attribute__((weak)) int port_input_real_a(void) { return 0; }
__attribute__((weak)) void port_request_warp(const char* stage, int scenario) { (void)stage; (void)scenario; }
__attribute__((weak)) int port_take_warp_request(char* outStage, size_t maxLen, int* outScenario) { (void)outStage; (void)maxLen; (void)outScenario; return 0; }
__attribute__((weak)) void port_request_powerup(int type) { (void)type; }
__attribute__((weak)) int port_take_powerup_request(void) { return -1; }
__attribute__((weak)) void port_set_current_powerup(int mode) { (void)mode; }
__attribute__((weak)) int port_get_current_powerup(void) { return 0; }
__attribute__((weak)) void port_request_unlock_spin(void) {}
__attribute__((weak)) int port_take_unlock_spin_request(void) { return 0; }
__attribute__((weak)) void port_set_spin_unlocked(int unlocked) { (void)unlocked; }
__attribute__((weak)) int port_is_spin_unlocked(void) { return 0; }
__attribute__((weak)) int port_has_game_data(void) { return 0; }
__attribute__((weak)) void port_open_installer(void) {}
__attribute__((weak)) int port_is_plus_installed(void) { return 0; }
__attribute__((weak)) void port_install_plus(void) {}
}
