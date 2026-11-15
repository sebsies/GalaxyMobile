#include "mobile_ui.h"

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <string>
#include <vector>

#include "xr/ui_font.h"
#include "port/port.h"
#include "mobile_haptics.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "MobileUI", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "MobileUI", __VA_ARGS__)

namespace mobile_ui {

namespace {

int64_t monotonicNowNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

// Galaxy Database for Galaxy & Scenario Warp Menu (Plus Edition)
struct GalaxyDef {
    const char* displayName;
    const char* stageName;
    int maxScenarios;
    int category; // 1=Terrace, 2=Fountain, 3=Kitchen, 4=Bedroom, 5=Engine, 6=Garden, 7=Special
};

static const GalaxyDef kGalaxies[] = {
    // Terrace (category 1)
    {"Good Egg Galaxy", "EggStarGalaxy", 6, 1},
    {"Honeyhive Galaxy", "HoneyBeeKingdomGalaxy", 6, 1},
    {"Flipswitch Galaxy", "FlipPanelExGalaxy", 1, 1},
    {"Loopdeeloop Galaxy", "SurfingLv1Galaxy", 1, 1},
    {"Sweet Sweet Galaxy", "BeltConveyerExGalaxy", 1, 1},
    {"Bowser Jr. Robot", "TriLegLv1Galaxy", 1, 1},

    // Fountain (category 2)
    {"Space Junk Galaxy", "StarDustGalaxy", 6, 2},
    {"Battlerock Galaxy", "BattleShipGalaxy", 6, 2},
    {"Rolling Green Galaxy", "TamakoroExLv1Galaxy", 1, 2},
    {"Hurry-Scurry Galaxy", "BreakDownPlanetGalaxy", 1, 2},
    {"Sling Pod Galaxy", "CocoonExGalaxy", 1, 2},
    {"Bowser Star Reactor", "KoopaBattleVs1Galaxy", 1, 2},

    // Kitchen (category 3)
    {"Beach Bowl Galaxy", "HeavenlyBeachGalaxy", 6, 3},
    {"Ghostly Galaxy", "PhantomGalaxy", 6, 3},
    {"Bubble Breeze Galaxy", "CubeBubbleExLv1Galaxy", 1, 3},
    {"Buoy Base Galaxy", "OceanFloaterLandGalaxy", 2, 3},
    {"Drip Drop Galaxy", "TearDropGalaxy", 1, 3},
    {"Bowser Jr. Airship", "KoopaJrShipLv1Galaxy", 1, 3},

    // Bedroom (category 4)
    {"Gusty Garden Galaxy", "CosmosGardenGalaxy", 6, 4},
    {"Freezeflame Galaxy", "IceVolcanoGalaxy", 6, 4},
    {"Dusty Dune Galaxy", "SandClockGalaxy", 6, 4},
    {"Honeyclimb Galaxy", "HoneyBeeExGalaxy", 1, 4},
    {"Bowser Dark Matter", "KoopaBattleVs2Galaxy", 1, 4},

    // Engine (category 5)
    {"Gold Leaf Galaxy", "ReverseKingdomGalaxy", 6, 5},
    {"Sea Slide Galaxy", "OceanRingGalaxy", 6, 5},
    {"Toy Time Galaxy", "FactoryGalaxy", 6, 5},
    {"Bonefin Galaxy", "SkullSharkGalaxy", 1, 5},
    {"Bowser Jr. Lava Dome", "FloaterOtaKingGalaxy", 1, 5},

    // Garden (category 6)
    {"Deep Dark Galaxy", "OceanPhantomCaveGalaxy", 6, 6},
    {"Dreadnought Galaxy", "CannonFleetGalaxy", 6, 6},
    {"Melty Molten Galaxy", "HellProminenceGalaxy", 6, 6},
    {"Matter Splatter Galaxy", "DarkRoomGalaxy", 1, 6},
    {"Bowser Galaxy Reactor", "KoopaBattleVs3Galaxy", 1, 6},

    // Special / Trials / Secret (category 7)
    {"Comet Observatory", "AstroGalaxy", 1, 7},
    {"Gateway Galaxy", "HeavensDoorGalaxy", 2, 7},
    {"Grand Finale Galaxy", "PeachCastleGardenGalaxy", 1, 7},
    {"Rolling Gizmo Galaxy", "TamakoroExLv2Galaxy", 1, 7},
    {"Loopdeeswoop Galaxy", "SurfingLv2Galaxy", 1, 7},
    {"Bubble Blast Galaxy", "CubeBubbleExLv2Galaxy", 1, 7},
    {"Boo's Boneyard Galaxy", "TeresaMario2DGalaxy", 1, 7},
    {"Snow Cap Galaxy", "SnowCapsuleGalaxy", 1, 7},
    {"Bigmouth Galaxy", "FishTunnelGalaxy", 1, 7},
};
static const int kNumGalaxies = (int)(sizeof(kGalaxies) / sizeof(kGalaxies[0]));

static const char* kCategoryNames[8] = {
    "All", "Terrace", "Fountain", "Kitchen", "Bedroom", "Engine", "Garden", "Special"
};

enum UiHitKind {
    HIT_NONE = 0,
    HIT_CLOSE,
    HIT_DONATE,
    HIT_TAB,
    HIT_WARP_BACK,
    HIT_WARP_CAT,
    HIT_WARP_SCENARIO,
    HIT_WARP_QUICK,
    HIT_WARP_CARD,
    HIT_WARP_LAUNCH,
    HIT_TOGGLE_SHOW_CONTROLS,
    HIT_TOGGLE_SHOW_DPAD,
    HIT_SEG_DPAD_SIDE,
    HIT_SEG_STICK_MODE,
    HIT_SEG_OPACITY,
    HIT_SEG_BTN_SIZE,
    HIT_LAYOUT_EDIT,
    HIT_LAYOUT_RESET,
    HIT_SEG_RIGHT_STICK_MODE,
    HIT_SLIDER_SENSITIVITY,
    HIT_TOGGLE_FULLSCREEN,
    HIT_TOGGLE_CAMERA_CUTOUT,
    HIT_SEG_RESOLUTION,
    HIT_TOGGLE_HD_UI,
    HIT_TOGGLE_FPS_COUNTER,
    HIT_STEP_FPS_SCALE,
    HIT_FPS_EDIT,
    HIT_FPS_RESET,
    HIT_SLIDER_FPS_LIMIT,
    HIT_TOGGLE_SKIP_CUTSCENES,
    HIT_TOGGLE_INF_HP,
    HIT_TOGGLE_INF_POWERUP,
    HIT_TOGGLE_FLIGHT,
    HIT_POWERUP_MORPH,
    HIT_OPEN_WARP,
    HIT_UNLOCK_SPIN,
    HIT_SEG_CHARACTER,
    HIT_TOGGLE_HAPTICS,
    HIT_SLIDER_LANGUAGE,
    HIT_OPEN_INSTALLER,
    HIT_INSTALL_PLUS,
    HIT_SHARE_LOG,
    HIT_EXPORT_SAVE,
    HIT_IMPORT_SAVE,
    HIT_NET_DISCONNECT,
    HIT_SEG_NET_MODE,
    HIT_NET_HOST_TOGGLE,
    HIT_NET_REFRESH,
    HIT_NET_JOIN_HOST,
    HIT_NET_DIRECT_CONNECT
};

// In-shader vertex for 2D batching
struct UIVertex {
    float x, y;
    float u, v;
    float r, g, b, a;
    float mode;   // 0 = rect, 1 = circle/ring, 2 = font glyph, 3 = cogwheel, 4 = close 'X'
    float param;  // ring: inner ratio; rect: corner radius ratio; cogwheel: rot angle
    float aspect; // rect aspect ratio (w/h or h/w)
    float extra;  // rect: 0 = horiz/square, 1 = vert
};

struct Color {
    float r, g, b, a;
};

inline Color rgba(float r, float g, float b, float a) {
    return {r, g, b, a};
}

inline Color hexColor(uint32_t hex, float a) {
    float r = ((hex >> 16) & 0xFF) / 255.0f;
    float g = ((hex >> 8) & 0xFF) / 255.0f;
    float b = (hex & 0xFF) / 255.0f;
    return {r, g, b, a};
}

int base64Val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

const char* kVS = R"(#version 300 es
precision mediump float;
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aColor;
layout(location = 3) in vec4 aParams;

uniform mat4 uProj;

out vec2 vUv;
out vec4 vColor;
out vec4 vParams;

void main() {
    gl_Position = uProj * vec4(aPos, 0.0, 1.0);
    vUv = aUv;
    vColor = aColor;
    vParams = aParams;
}
)";

const char* kFS = R"(#version 300 es
precision mediump float;

in vec2 vUv;
in vec4 vColor;
in vec4 vParams;

uniform sampler2D uFontTex;

out vec4 FragColor;

void main() {
    float mode = vParams.x;
    float param = vParams.y;
    float alpha = 1.0;

    if (mode > 0.5 && mode < 1.5) {
        // Mode 1: Circle or Ring (vUv in -1..1)
        float d = length(vUv);
        float fw = fwidth(d);
        if (fw < 0.001) fw = 0.005;
        float outer = smoothstep(1.0, 1.0 - fw, d);
        if (param > 0.0) {
            float inner = smoothstep(param - fw, param, d);
            alpha = outer * inner;
        } else {
            alpha = outer;
        }
    } else if (mode > 1.5 && mode < 2.5) {
        // Mode 2: Font glyph texture: sample red channel
        alpha = texture(uFontTex, vUv).r;
    } else if (mode > 2.5 && mode < 3.5) {
        // Mode 3: Iconic Material 3 Settings Cogwheel (⚙)
        float d = length(vUv);
        float fw = fwidth(d);
        if (fw < 0.001) fw = 0.005;
        float angle = atan(vUv.y, vUv.x) + param;
        float teeth = cos(8.0 * angle);
        float rMax = 0.74 + 0.22 * smoothstep(-0.25, 0.25, teeth);
        float outer = smoothstep(rMax, rMax - fw * 1.5, d);
        float inner = smoothstep(0.33 - fw * 1.5, 0.33, d);
        alpha = outer * inner;
    } else if (mode > 3.5 && mode < 4.5) {
        // Mode 4: Material 3 Close 'X' icon
        vec2 p = abs(vUv);
        float lineDist = abs(p.x - p.y) * 0.7071;
        float capDist = max(p.x, p.y);
        float fw = fwidth(lineDist);
        if (fw < 0.001) fw = 0.005;
        float line = smoothstep(0.16, 0.16 - fw, lineDist);
        float box = smoothstep(0.65, 0.65 - fw, capDist);
        alpha = line * box;
    } else if (mode > 4.5 && mode < 5.5) {
        // Mode 5: Spin Cyclone / Galaxy Swirl Glyph (🌀)
        float d = length(vUv);
        float a = atan(vUv.y, vUv.x) + param;
        float aFold = mod(a, 3.14159265);
        float normAngle = clamp((aFold - 0.25) / 2.35, 0.0, 1.0);
        float arcR = mix(0.42, 0.78, normAngle);
        float dArc = abs(d - arcR);
        float thick = mix(0.06, 0.16, normAngle);
        float fw = fwidth(dArc);
        if (fw < 0.001) fw = 0.005;
        float arcAlpha = smoothstep(thick, thick - fw * 1.5, dArc);
        float angleMask = smoothstep(0.12, 0.30, aFold) * smoothstep(2.80, 2.60, aFold);
        alpha = arcAlpha * angleMask;
    } else if (mode > 5.5 && mode < 6.5) {
        // Mode 6: Modern Precision Chevron Arrow (rot in param)
        float ca = cos(param);
        float sa = sin(param);
        vec2 p = vec2(ca * vUv.x - sa * vUv.y, sa * vUv.x + ca * vUv.y);
        vec2 tip = vec2(0.30, 0.0);
        vec2 wing = vec2(-0.30, 0.52);
        vec2 seg = wing - tip;
        vec2 pt = vec2(p.x, abs(p.y)) - tip;
        float t = clamp(dot(pt, seg) / dot(seg, seg), 0.0, 1.0);
        float dLine = length(pt - seg * t);
        float fw = fwidth(dLine);
        if (fw < 0.001) fw = 0.005;
        float strokeW = 0.16;
        alpha = smoothstep(strokeW, strokeW - fw * 1.5, dLine);
    } else if (mode > 6.5 && mode < 7.5) {
        // Mode 7: Modern Minimalist Camera Glyph (📷)
        vec2 p = vUv;
        vec2 bBody = vec2(0.48, 0.32);
        vec2 pBody = p - vec2(0.0, 0.06);
        vec2 dBody = abs(pBody) - bBody;
        float distBody = length(max(dBody, 0.0)) + min(max(dBody.x, dBody.y), 0.0) - 0.10;

        vec2 bTop = vec2(0.18, 0.08);
        vec2 pTop = p - vec2(-0.16, -0.32);
        vec2 dTop = abs(pTop) - bTop;
        float distTop = length(max(dTop, 0.0)) + min(max(dTop.x, dTop.y), 0.0) - 0.05;

        float bodyUnion = min(distBody, distTop);
        float fw = fwidth(length(p));
        if (fw < 0.001) fw = 0.005;
        float fillBody = smoothstep(0.0, -fw * 1.5, bodyUnion);

        float dLens = length(p - vec2(0.0, 0.08));
        float lensHole = smoothstep(0.24 - fw, 0.24, dLens);
        float lensRing = smoothstep(0.24, 0.24 - fw, dLens) * smoothstep(0.15 - fw, 0.15, dLens);
        float lensDot = smoothstep(0.07, 0.07 - fw, dLens);

        float dFlash = length(p - vec2(0.34, -0.12));
        float flashDot = smoothstep(0.05, 0.05 - fw, dFlash);

        alpha = max(fillBody * lensHole * (1.0 - flashDot), max(lensRing, lensDot));
    } else if (mode > 7.5 && mode < 8.5) {
        // Mode 8: 5-Point Super Mario Galaxy Power Star SDF
        float ca = cos(param);
        float sa = sin(param);
        vec2 p = vec2(ca * vUv.x - sa * vUv.y, -(sa * vUv.x + ca * vUv.y));
        const vec2 k1 = vec2(0.809016994375, -0.587785252292);
        const vec2 k2 = vec2(-k1.x, k1.y);
        p.x = abs(p.x);
        p -= 2.0 * max(dot(k1, p), 0.0) * k1;
        p -= 2.0 * max(dot(k2, p), 0.0) * k2;
        p.x = abs(p.x);
        p.y -= 0.84;
        vec2 ba = 0.48 * vec2(-k1.y, k1.x) - vec2(0.0, 0.84);
        float h = clamp(dot(p, ba) / dot(ba, ba), 0.0, 1.0);
        float d = length(p - ba * h) * sign(p.y * ba.x - p.x * ba.y) - 0.04;
        float fw = fwidth(d);
        if (fw < 0.001) fw = 0.005;
        alpha = smoothstep(0.0, -fw, d);
    } else if (mode > 8.5 && mode < 9.5) {
        // Mode 9: 4-Point Cosmic Sparkle / Luma Starburst
        float ca = cos(param);
        float sa = sin(param);
        vec2 p = abs(vec2(ca * vUv.x - sa * vUv.y, sa * vUv.x + ca * vUv.y));
        float crossD = min(p.x, p.y) + 0.16 * (p.x + p.y) - 0.18;
        float d = max(crossD, length(p) - 0.90);
        float fw = fwidth(d);
        if (fw < 0.001) fw = 0.005;
        alpha = smoothstep(0.0, -fw, d);
    } else {
        // Mode 0: Rounded rectangle with exact aspect ratio
        if (param > 0.0) {
            vec2 p = abs(vUv);
            float aspect = vParams.z;
            vec2 b = (vParams.w > 0.5) ? vec2(1.0 - param, aspect - param) : vec2(aspect - param, 1.0 - param);
            vec2 d = p - b;
            float dist = length(max(d, 0.0)) - param;
            float fw = fwidth(dist);
            if (fw < 0.001) fw = 0.005;
            alpha = smoothstep(0.0, -fw, dist);
        }
    }

    if (alpha <= 0.002) discard;
    FragColor = vec4(vColor.rgb, vColor.a * alpha);
}
)";

GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOGE("Shader compile error: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

}  // namespace

void Settings::load(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        float val = 0.0f;
        if (sscanf(line, "%63[^=]=%f", key, &val) == 2) {
            if (!strcmp(key, "show_controls")) showControls = (val > 0.5f);
            else if (!strcmp(key, "fullscreen")) fullscreen = (val > 0.5f);
            else if (!strcmp(key, "opacity")) opacity = std::max(0.1f, std::min(1.0f, val));
            else if (!strcmp(key, "scale")) scale = std::max(0.6f, std::min(1.6f, val));
            else if (!strcmp(key, "dynamic_stick")) dynamicStick = (val > 0.5f);
            else if (!strcmp(key, "stick_offset_x")) stickOffsetX = val;
            else if (!strcmp(key, "stick_offset_y")) stickOffsetY = val;
            else if (!strcmp(key, "button_offset_x")) buttonOffsetX = val;
            else if (!strcmp(key, "button_offset_y")) buttonOffsetY = val;
            else if (!strcmp(key, "show_dpad")) showDpad = (val > 0.5f);
            else if (!strcmp(key, "dpad_on_right")) dpadOnRight = (val > 0.5f);
            else if (!strcmp(key, "dpad_offset_x")) dpadOffsetX = val;
            else if (!strcmp(key, "dpad_offset_y")) dpadOffsetY = val;
            else if (!strcmp(key, "right_stick_pointer")) rightStickPointer = (val > 0.5f);
            else if (!strcmp(key, "right_stick_sensitivity")) rightStickSensitivity = std::max(0.50f, std::min(3.00f, val));
            else if (!strcmp(key, "show_fps_counter")) showFpsCounter = (val > 0.5f);
            else if (!strcmp(key, "fps_offset_x")) fpsOffsetX = val;
            else if (!strcmp(key, "fps_offset_y")) fpsOffsetY = val;
            else if (!strcmp(key, "fps_scale")) fpsScale = std::max(0.60f, std::min(2.20f, val));
            else if (!strcmp(key, "fps_limit")) fpsLimit = std::max(30, std::min(60, (int)(val + 0.5f)));
            else if (!strcmp(key, "mod_skip_cutscenes")) modSkipCutscenes = (val > 0.5f);
            else if (!strcmp(key, "mod_infinite_health")) modInfiniteHealth = (val > 0.5f);
            else if (!strcmp(key, "mod_infinite_powerup")) modInfinitePowerup = (val > 0.5f);
            else if (!strcmp(key, "graphics_preset")) graphicsPreset = std::max(0, std::min(4, (int)(val + 0.5f)));
            else if (!strcmp(key, "hd_ui_textures")) hdUiTextures = (val > 0.5f);
            else if (!strcmp(key, "mod_advanced_haptics")) modAdvancedHaptics = (val > 0.5f);
            else if (!strcmp(key, "mod_flight_everywhere")) modFlightEverywhere = (val > 0.5f);
            else if (!strcmp(key, "show_camera_cutout")) showCameraCutout = (val > 0.5f);
            else if (!strcmp(key, "language_index")) languageIndex = std::max(0, std::min(6, (int)(val + 0.5f)));
            else if (!strcmp(key, "player_character")) playerCharacter = (val > 0.5f) ? 1 : 0;
        }
    }
    fclose(f);
    LOGI("Loaded settings: show=%d, full=%d, op=%.2f, scale=%.2f, dpad=%d, skip=%d, infHp=%d, infPow=%d, hdUi=%d",
         showControls, fullscreen, opacity, scale, showDpad, modSkipCutscenes, modInfiniteHealth, modInfinitePowerup, hdUiTextures);
}

void Settings::save(const std::string& path) const {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return;
    fprintf(f, "show_controls=%d\n", showControls ? 1 : 0);
    fprintf(f, "fullscreen=%d\n", fullscreen ? 1 : 0);
    fprintf(f, "opacity=%.2f\n", opacity);
    fprintf(f, "scale=%.2f\n", scale);
    fprintf(f, "dynamic_stick=%d\n", dynamicStick ? 1 : 0);
    fprintf(f, "stick_offset_x=%.1f\n", stickOffsetX);
    fprintf(f, "stick_offset_y=%.1f\n", stickOffsetY);
    fprintf(f, "button_offset_x=%.1f\n", buttonOffsetX);
    fprintf(f, "button_offset_y=%.1f\n", buttonOffsetY);
    fprintf(f, "show_dpad=%d\n", showDpad ? 1 : 0);
    fprintf(f, "dpad_on_right=%d\n", dpadOnRight ? 1 : 0);
    fprintf(f, "dpad_offset_x=%.1f\n", dpadOffsetX);
    fprintf(f, "dpad_offset_y=%.1f\n", dpadOffsetY);
    fprintf(f, "right_stick_pointer=%d\n", rightStickPointer ? 1 : 0);
    fprintf(f, "right_stick_sensitivity=%.2f\n", rightStickSensitivity);
    fprintf(f, "show_fps_counter=%d\n", showFpsCounter ? 1 : 0);
    fprintf(f, "fps_offset_x=%.1f\n", fpsOffsetX);
    fprintf(f, "fps_offset_y=%.1f\n", fpsOffsetY);
    fprintf(f, "fps_scale=%.2f\n", fpsScale);
    fprintf(f, "fps_limit=%d\n", fpsLimit);
    fprintf(f, "mod_skip_cutscenes=%d\n", modSkipCutscenes ? 1 : 0);
    fprintf(f, "mod_infinite_health=%d\n", modInfiniteHealth ? 1 : 0);
    fprintf(f, "mod_infinite_powerup=%d\n", modInfinitePowerup ? 1 : 0);
    fprintf(f, "graphics_preset=%d\n", graphicsPreset);
    fprintf(f, "hd_ui_textures=%d\n", hdUiTextures ? 1 : 0);
    fprintf(f, "mod_advanced_haptics=%d\n", modAdvancedHaptics ? 1 : 0);
    fprintf(f, "mod_flight_everywhere=%d\n", modFlightEverywhere ? 1 : 0);
    fprintf(f, "show_camera_cutout=%d\n", showCameraCutout ? 1 : 0);
    fprintf(f, "language_index=%d\n", languageIndex);
    fprintf(f, "player_character=%d\n", playerCharacter);
    fclose(f);
    LOGI("Saved settings to %s", path.c_str());
}

UIOverlay::UIOverlay() {
    mPointerX = 0.0f; // Screen center
    mPointerY = 0.0f; // Screen center
    mTabIndicatorY = 0.0f;
    mTabIndicatorH = 0.0f;
    mTabTargetY = 0.0f;
    mTabTargetH = 0.0f;
    mSwAnimShowControls = 1.0f;
    mSwAnimShowDpad = 1.0f;
    mSwAnimFullscreen = 0.0f;
    mSwAnimCameraCutout = 1.0f;
    mSwAnimHdUi = 1.0f;
    mSwAnimSkipCutscenes = 0.0f;
    mSwAnimInfiniteHp = 0.0f;
    mSwAnimInfinitePowerup = 0.0f;
    mSwAnimHaptics = 0.0f;
    mSwAnimFlightEverywhere = 0.0f;
    mSwAnimFpsCounter = 0.0f;
}

UIOverlay& UIOverlay::get() {
    static UIOverlay sInstance;
    return sInstance;
}

void UIOverlay::setSavePath(const std::string& path) {
    mSavePath = path;
    mSettings.load(path);
    if (!mPlusEdition) {
        mSettings.modSkipCutscenes = false;
        mSettings.modInfiniteHealth = false;
        mSettings.modInfinitePowerup = false;
        mSettings.modFlightEverywhere = false;
        mWarpMenuOpen = false;
    }
    mSwAnimShowControls = mSettings.showControls ? 1.0f : 0.0f;
    mSwAnimShowDpad = mSettings.showDpad ? 1.0f : 0.0f;
    mSwAnimFullscreen = mSettings.fullscreen ? 1.0f : 0.0f;
    mSwAnimCameraCutout = mSettings.showCameraCutout ? 1.0f : 0.0f;
    port_set_camera_cutout(mSettings.showCameraCutout ? 1 : 0);
    port_set_game_language(mSettings.languageIndex);
    if (mPlusEdition) {
        port_set_player_character(mSettings.playerCharacter);
    }
    mSwAnimHdUi = mSettings.hdUiTextures ? 1.0f : 0.0f;
    mSwAnimSkipCutscenes = mSettings.modSkipCutscenes ? 1.0f : 0.0f;
    mSwAnimInfiniteHp = mSettings.modInfiniteHealth ? 1.0f : 0.0f;
    mSwAnimInfinitePowerup = mSettings.modInfinitePowerup ? 1.0f : 0.0f;
    mSwAnimHaptics = mSettings.modAdvancedHaptics ? 1.0f : 0.0f;
    mSwAnimFlightEverywhere = mSettings.modFlightEverywhere ? 1.0f : 0.0f;
    mSwAnimFpsCounter = mSettings.showFpsCounter ? 1.0f : 0.0f;
}

bool UIOverlay::needsRedraw() {
    float target = mSettingsOpen ? 1.0f : 0.0f;
    if (fabsf(mDrawerAnim - target) > 0.01f) {
        return true;
    }
    if (fabsf(mDrawerScrollY - mDrawerScrollTarget) > 0.5f) {
        return true;
    }
    if (mWarpMenuOpen && fabsf(mWarpScrollY - mWarpScrollTarget) > 0.5f) {
        return true;
    }
    if (fabsf(mTabIndicatorY - mTabTargetY) > 0.5f || fabsf(mTabIndicatorH - mTabTargetH) > 0.5f) {
        return true;
    }
    auto switchActive = [](float cur, bool target) {
        float tgt = target ? 1.0f : 0.0f;
        return fabsf(cur - tgt) > 0.005f;
    };
    if (switchActive(mSwAnimShowControls, mSettings.showControls) ||
        switchActive(mSwAnimShowDpad, mSettings.showDpad) ||
        switchActive(mSwAnimFullscreen, mSettings.fullscreen) ||
        switchActive(mSwAnimCameraCutout, mSettings.showCameraCutout) ||
        switchActive(mSwAnimHdUi, mSettings.hdUiTextures) ||
        switchActive(mSwAnimSkipCutscenes, mSettings.modSkipCutscenes) ||
        switchActive(mSwAnimInfiniteHp, mSettings.modInfiniteHealth) ||
        switchActive(mSwAnimInfinitePowerup, mSettings.modInfinitePowerup) ||
        switchActive(mSwAnimHaptics, mSettings.modAdvancedHaptics) ||
        switchActive(mSwAnimFlightEverywhere, mSettings.modFlightEverywhere) ||
        switchActive(mSwAnimFpsCounter, mSettings.showFpsCounter)) {
        return true;
    }
    bool d = mDirty;
    mDirty = false;
    return d;
}

void UIOverlay::closeSettings(bool playCloseSe) {
    if (mSettingsOpen) {
        if (playCloseSe) {
            port_play_ui_se(PORT_UI_SE_CLOSE);
        }
        // Guard the top-right + (Pause) button briefly after exiting settings so
        // Resume button taps or quick double-taps cannot accidentally trigger pause.
        mPauseGuardUntilNs = monotonicNowNs() + 400000000LL; // 400 ms
    }
    mSettingsOpen = false;
    mWarpMenuOpen = false;
    mActiveSliderKind = 0;
    mDirty = true;
    if (!mSavePath.empty()) {
        mSettings.save(mSavePath);
    }
}

void UIOverlay::openSettings() {
    if (!mSettingsOpen) {
        port_play_ui_se(PORT_UI_SE_OPEN);
    }
    mSettingsOpen = true;
    mDirty = true;
}

void UIOverlay::setPointer(float x, float y) {
    mPointerX = std::max(-1.0f, std::min(1.0f, x));
    mPointerY = std::max(-1.0f, std::min(1.0f, y));
}

void UIOverlay::setFps(float fps) {
    mCurrentFps = std::max(0.0f, fps);
    int rounded = (int)lroundf(mCurrentFps);
    if (rounded != mDisplayedFps) {
        mDisplayedFps = rounded;
        if (mSettings.showFpsCounter) {
            mDirty = true;
        }
    }
}

void UIOverlay::computeFpsRect(int screenW, int screenH, float& outX, float& outY, float& outW, float& outH, float& outBaseW) const {
    float w = (float)screenW;
    float h = (float)screenH;
    float uiScale = h / 650.0f;
    float fpsScale = std::max(0.60f, std::min(2.20f, mSettings.fpsScale));
    float fs = uiScale * fpsScale;

    const ui_font::Face& face = ui_font::kFaces[0];
    float refW = 0.0f;
    for (const char* p = "888 FPS"; *p; p++) {
        char ch = *p;
        if (ch >= 32 && ch < 127) {
            refW += (face.glyphs[ch - 32].advance * 0.25f) * (0.34f * uiScale);
        }
    }
    outBaseW = std::max(84.0f * uiScale, refW + 33.0f * uiScale);
    outW = outBaseW * fpsScale;
    outH = 28.0f * fs;

    float cutoutInset = mSettings.showCameraCutout ? 0.0f : (42.0f * uiScale);
    float baseX = 70.0f * uiScale + cutoutInset;
    float baseY = 26.0f * uiScale;
    float minX = 6.0f * uiScale;
    float minY = 6.0f * uiScale;
    float maxX = std::max(minX, w - outW - 6.0f * uiScale);
    float maxY = std::max(minY, h - outH - 6.0f * uiScale);
    outX = std::max(minX, std::min(maxX, baseX + mSettings.fpsOffsetX));
    outY = std::max(minY, std::min(maxY, baseY + mSettings.fpsOffsetY));
}

bool UIOverlay::init() {
    if (mInitialized) return true;

    GLuint vs = compileShader(GL_VERTEX_SHADER, kVS);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFS);
    if (!vs || !fs) return false;

    mProg = glCreateProgram();
    glAttachShader(mProg, vs);
    glAttachShader(mProg, fs);
    glLinkProgram(mProg);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(mProg, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(mProg, sizeof(log), nullptr, log);
        LOGE("UI program link error: %s", log);
        return false;
    }

    glGenVertexArrays(1, &mVao);
    glGenBuffers(1, &mVbo);

    glBindVertexArray(mVao);
    glBindBuffer(GL_ARRAY_BUFFER, mVbo);

    // layout 0: pos (vec2)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(UIVertex), (void*)offsetof(UIVertex, x));

    // layout 1: uv (vec2)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(UIVertex), (void*)offsetof(UIVertex, u));

    // layout 2: color (vec4)
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(UIVertex), (void*)offsetof(UIVertex, r));

    // layout 3: params (vec4)
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(UIVertex), (void*)offsetof(UIVertex, mode));

    glBindVertexArray(0);

    // Decode font atlas into GL_R8 texture
    const ui_font::Face& face = ui_font::kFaces[0]; // size 40 Inter Bold
    mAtlasW = face.width;
    mAtlasH = face.height;

    std::vector<uint8_t> packed;
    uint32_t bits = 0;
    int nbits = 0;
    for (const char* p = face.atlas; *p; p++) {
        int v = base64Val(*p);
        if (v < 0) continue;
        bits = (bits << 6) | (uint32_t)v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            packed.push_back((uint8_t)(bits >> nbits));
        }
    }

    std::vector<uint8_t> coverage((size_t)face.width * face.height, 0);
    for (size_t i = 0; i < coverage.size(); i++) {
        uint8_t b = (i / 2 < packed.size()) ? packed[i / 2] : 0;
        coverage[i] = (uint8_t)(((i & 1) ? (b & 15) : (b >> 4)) * 17);
    }

    glGenTextures(1, &mFontTex);
    glBindTexture(GL_TEXTURE_2D, mFontTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, face.width, face.height, 0, GL_RED, GL_UNSIGNED_BYTE, coverage.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    mInitialized = true;
    LOGI("Mobile UI initialized successfully (Atlas %dx%d)", mAtlasW, mAtlasH);
    return true;
}

bool UIOverlay::handleInput(const AInputEvent* event, PortPadState& pad,
                            int screenW, int screenH, int vpX, int vpY, int vpW, int vpH) {
    int32_t eventType = AInputEvent_getType(event);
    if (eventType != AINPUT_EVENT_TYPE_MOTION) {
        return false;
    }

    int32_t actionWithIndex = AMotionEvent_getAction(event);
    int32_t action = actionWithIndex & AMOTION_EVENT_ACTION_MASK;
    int32_t pointerIndex = (actionWithIndex & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    size_t pointerCount = AMotionEvent_getPointerCount(event);

    auto resetLiveTouchState = [&]() {
        mStickPointerId = -1;
        mStickKnobX = mStickBaseX;
        mStickKnobY = mStickBaseY;
        pad.stickX = 0.0f;
        pad.stickY = 0.0f;
        pad.buttons = 0;
        mBtnAPressed = mBtnBPressed = mBtnZPressed = mBtnSpinPressed = mBtnPlusPressed = false;
        mBtnDpadLeft = mBtnDpadRight = mBtnDpadUp = mBtnDpadDown = false;
    };

    // A fresh primary touch down begins a new gesture, clearing prior modal-exit suppression.
    if (action == AMOTION_EVENT_ACTION_DOWN) {
        mSuppressTouchUntilUp = false;
    }
    if (mSuppressTouchUntilUp) {
        if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_CANCEL) {
            mSuppressTouchUntilUp = false;
        }
        resetLiveTouchState();
        mDirty = true;
        return true;
    }

    // Responsive scaling based on screen height (clamped cleanly for narrower panels)
    float baseDpi = (float)screenH / 650.0f;
    float uiScale = std::min(baseDpi, (float)screenW / 1060.0f);
    float scale = baseDpi * mSettings.scale;
    float w = (float)screenW;
    float h = (float)screenH;

    // Settings Trigger Cogwheel (Top-Left Edge)
    float tabW = 56.0f * baseDpi;
    float tabH = 48.0f * baseDpi;
    float tabY = 22.0f * baseDpi;

    // Check Settings Tab tap (when menu is closed)
    if (!mSettingsOpen && mDrawerAnim < 0.15f) {
        if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_POINTER_DOWN) {
            float px = AMotionEvent_getX(event, pointerIndex);
            float py = AMotionEvent_getY(event, pointerIndex);
            if (px >= 0.0f && px <= tabW + 16.0f * baseDpi && py >= tabY - 8.0f * baseDpi && py <= tabY + tabH + 8.0f * baseDpi) {
                mBtnCfgPressed = true;
                openSettings();
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                return true;
            }
        }
    } else if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_CANCEL) {
        mBtnCfgPressed = false;
    }

    // Handle Edit Layout Mode
    if (mEditLayoutMode) {
        if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_POINTER_DOWN) {
            float px = AMotionEvent_getX(event, pointerIndex);
            float py = AMotionEvent_getY(event, pointerIndex);

            // Check Top buttons: Save or Reset
            float saveBtnX = w * 0.5f - 110.0f, saveBtnY = 50.0f;
            if (px >= saveBtnX && px <= saveBtnX + 100.0f && py >= saveBtnY && py <= saveBtnY + 50.0f) {
                mEditLayoutMode = false;
                mSuppressTouchUntilUp = true;
                resetLiveTouchState();
                if (!mSavePath.empty()) mSettings.save(mSavePath);
                mDirty = true;
                return true;
            }
            float rstBtnX = w * 0.5f + 10.0f, rstBtnY = 50.0f;
            if (px >= rstBtnX && px <= rstBtnX + 100.0f && py >= rstBtnY && py <= rstBtnY + 50.0f) {
                mSettings.stickOffsetX = 0.0f;
                mSettings.stickOffsetY = 0.0f;
                mSettings.buttonOffsetX = 0.0f;
                mSettings.buttonOffsetY = 0.0f;
                mSettings.dpadOffsetX = 0.0f;
                mSettings.dpadOffsetY = 0.0f;
                mSettings.fpsOffsetX = 0.0f;
                mSettings.fpsOffsetY = 0.0f;
                mSettings.fpsScale = 1.0f;
                if (!mSavePath.empty()) mSettings.save(mSavePath);
                mDirty = true;
                return true;
            }

            if (mSettings.showFpsCounter) {
                float fpsX, fpsY, fpsW, fpsH, fpsBaseW;
                computeFpsRect(screenW, screenH, fpsX, fpsY, fpsW, fpsH, fpsBaseW);
                float handleX = fpsX + fpsW + 2.0f * uiScale;
                float handleY = fpsY + fpsH + 2.0f * uiScale;
                float dHandle2 = (px - handleX) * (px - handleX) + (py - handleY) * (py - handleY);
                if (dHandle2 <= (28.0f * uiScale) * (28.0f * uiScale)) {
                    mDraggingCluster = 5; // FPS resize handle
                    mDragStartX = px;
                    mDragStartY = py;
                    mInitialFpsScale = mSettings.fpsScale;
                    mFpsBasePillW = fpsBaseW;
                    mDirty = true;
                    return true;
                }
                if (px >= fpsX - 14.0f * uiScale && px <= fpsX + fpsW + 14.0f * uiScale &&
                    py >= fpsY - 14.0f * uiScale && py <= fpsY + fpsH + 14.0f * uiScale) {
                    float cutoutInset = mSettings.showCameraCutout ? 0.0f : (42.0f * uiScale);
                    float baseX = 70.0f * uiScale + cutoutInset;
                    float baseY = 26.0f * uiScale;
                    mDraggingCluster = 4; // FPS position
                    mDragStartX = px;
                    mDragStartY = py;
                    mInitialFpsOfsX = fpsX - baseX;
                    mInitialFpsOfsY = fpsY - baseY;
                    mDirty = true;
                    return true;
                }
            }

            // Start dragging stick cluster, button cluster, or D-Pad cluster
            if (mSettings.showControls) {
                float cutoutInset = mSettings.showCameraCutout ? 0.0f : (42.0f * uiScale);
                float baseDpadX = mSettings.dpadOnRight ? (w - 380.0f * scale - cutoutInset) : (140.0f * scale + cutoutInset);
                float baseDpadY = mSettings.dpadOnRight ? (h - 220.0f * scale) : (h - 390.0f * scale);
                float curDpadX = baseDpadX + mSettings.dpadOffsetX;
                float curDpadY = baseDpadY + mSettings.dpadOffsetY;

                float curStickX = 175.0f * scale + cutoutInset + mSettings.stickOffsetX;
                float curStickY = h - 175.0f * scale + mSettings.stickOffsetY;

                float dDpad2 = (px - curDpadX) * (px - curDpadX) + (py - curDpadY) * (py - curDpadY);
                float dStick2 = (px - curStickX) * (px - curStickX) + (py - curStickY) * (py - curStickY);

                if (mSettings.showDpad && dDpad2 <= (95.0f * scale) * (95.0f * scale)) {
                    mDraggingCluster = 3; // D-Pad
                    mDragStartX = px;
                    mDragStartY = py;
                    mInitialDpadOfsX = mSettings.dpadOffsetX;
                    mInitialDpadOfsY = mSettings.dpadOffsetY;
                } else if (dStick2 <= (140.0f * scale) * (140.0f * scale) || px < w * 0.40f) {
                    mDraggingCluster = 1; // stick
                    mDragStartX = px;
                    mDragStartY = py;
                    mInitialStickOfsX = mSettings.stickOffsetX;
                    mInitialStickOfsY = mSettings.stickOffsetY;
                } else {
                    mDraggingCluster = 2; // button cluster
                    mDragStartX = px;
                    mDragStartY = py;
                    mInitialBtnOfsX = mSettings.buttonOffsetX;
                    mInitialBtnOfsY = mSettings.buttonOffsetY;
                }
            }
            mDirty = true;
            return true;
        } else if (action == AMOTION_EVENT_ACTION_MOVE) {
            float px = AMotionEvent_getX(event, 0);
            float py = AMotionEvent_getY(event, 0);
            if (mDraggingCluster == 1) {
                mSettings.stickOffsetX = mInitialStickOfsX + (px - mDragStartX);
                mSettings.stickOffsetY = mInitialStickOfsY + (py - mDragStartY);
            } else if (mDraggingCluster == 2) {
                mSettings.buttonOffsetX = mInitialBtnOfsX + (px - mDragStartX);
                mSettings.buttonOffsetY = mInitialBtnOfsY + (py - mDragStartY);
            } else if (mDraggingCluster == 3) {
                mSettings.dpadOffsetX = mInitialDpadOfsX + (px - mDragStartX);
                mSettings.dpadOffsetY = mInitialDpadOfsY + (py - mDragStartY);
            } else if (mDraggingCluster == 4) {
                float cutoutInset = mSettings.showCameraCutout ? 0.0f : (42.0f * uiScale);
                float baseX = 70.0f * uiScale + cutoutInset;
                float baseY = 26.0f * uiScale;
                float fpsX, fpsY, fpsW, fpsH, fpsBaseW;
                computeFpsRect(screenW, screenH, fpsX, fpsY, fpsW, fpsH, fpsBaseW);
                float minX = 6.0f * uiScale;
                float minY = 6.0f * uiScale;
                float maxX = std::max(minX, w - fpsW - 6.0f * uiScale);
                float maxY = std::max(minY, h - fpsH - 6.0f * uiScale);
                mSettings.fpsOffsetX = std::max(minX - baseX, std::min(maxX - baseX, mInitialFpsOfsX + (px - mDragStartX)));
                mSettings.fpsOffsetY = std::max(minY - baseY, std::min(maxY - baseY, mInitialFpsOfsY + (py - mDragStartY)));
            } else if (mDraggingCluster == 5) {
                float dx = px - mDragStartX;
                float dy = py - mDragStartY;
                float newScale = mInitialFpsScale + (dx + dy * 0.5f) / std::max(40.0f, mFpsBasePillW);
                mSettings.fpsScale = std::max(0.60f, std::min(2.20f, newScale));
            }
            mDirty = true;
            return true;
        } else if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_CANCEL) {
            mDraggingCluster = 0;
            if (!mSavePath.empty()) mSettings.save(mSavePath);
            mDirty = true;
            return true;
        }
        return true;
    }

    // Handle Full-Screen Observatory Settings Touches (Hitbox-driven)
    if (mSettingsOpen) {
        auto applySliderAtX = [&](int sliderKind, float x, float trackX0, float trackX1) {
            float t = std::max(0.0f, std::min(1.0f, (x - trackX0) / std::max(1.0f, trackX1 - trackX0)));
            if (sliderKind == HIT_SLIDER_SENSITIVITY) {
                float rawSens = 0.50f + t * 2.50f;
                float newSens = std::max(0.50f, std::min(3.00f, roundf(rawSens * 20.0f) * 0.05f));
                if (fabsf(newSens - mSettings.rightStickSensitivity) > 0.001f) {
                    if ((int)lroundf(newSens * 10.0f) != (int)lroundf(mSettings.rightStickSensitivity * 10.0f)) {
                        mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                        port_play_ui_se(PORT_UI_SE_TICK);
                    }
                    mSettings.rightStickSensitivity = newSens;
                }
            } else if (sliderKind == HIT_SLIDER_FPS_LIMIT) {
                int newLimit = std::max(30, std::min(60, (int)lroundf(30.0f + t * 30.0f)));
                if (newLimit != mSettings.fpsLimit) {
                    mSettings.fpsLimit = newLimit;
                    mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                    port_play_ui_se(PORT_UI_SE_TICK);
                }
            } else if (sliderKind == HIT_SLIDER_LANGUAGE) {
                int newLang = std::max(0, std::min(6, (int)(t * 6.0f + 0.5f)));
                if (newLang != mSettings.languageIndex) {
                    mSettings.languageIndex = newLang;
                    port_set_game_language(newLang);
                    mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                    port_play_ui_se(PORT_UI_SE_TICK);
                }
            }
            mDirty = true;
        };

        auto dismissSettings = [&](bool playCloseSe) {
            closeSettings(playCloseSe);
            mSuppressTouchUntilUp = (action != AMOTION_EVENT_ACTION_UP && action != AMOTION_EVENT_ACTION_CANCEL);
            resetLiveTouchState();
        };

        auto executeHit = [&](const UiHitbox& hb) {
            bool saveNow = false;
            switch (hb.kind) {
            case HIT_CLOSE:
                dismissSettings(true);
                return;
            case HIT_DONATE:
                port_open_url("https://ko-fi.com/sebsies");
                port_play_ui_se(PORT_UI_SE_DECIDE);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                return;
            case HIT_TAB:
                if (hb.arg0 != mSettingsTab) {
                    mSettingsTab = hb.arg0;
                    mDrawerScrollY = 0.0f;
                    mDrawerScrollTarget = 0.0f;
                    port_play_ui_se(PORT_UI_SE_TAB);
                    mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                }
                break;
            case HIT_WARP_BACK:
                mWarpMenuOpen = false;
                port_play_ui_se(PORT_UI_SE_CANCEL);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                break;
            case HIT_WARP_CAT: {
                int newCat = hb.arg0;
                if (newCat >= 0 && newCat < 8) {
                    mWarpCategoryIndex = newCat;
                    mWarpScrollY = 0.0f;
                    mWarpScrollTarget = 0.0f;
                    if (newCat != 0 && kGalaxies[mWarpSelectedGalaxy].category != newCat) {
                        for (int i = 0; i < kNumGalaxies; i++) {
                            if (kGalaxies[i].category == newCat) {
                                mWarpSelectedGalaxy = i;
                                mWarpSelectedScenario = 1;
                                break;
                            }
                        }
                    }
                    port_play_ui_se(PORT_UI_SE_TAB);
                    mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                }
                break;
            }
            case HIT_WARP_SCENARIO:
                mWarpSelectedGalaxy = hb.arg0;
                mWarpSelectedScenario = hb.arg1;
                port_play_ui_se(PORT_UI_SE_TICK);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                break;
            case HIT_WARP_QUICK: {
                mWarpSelectedGalaxy = hb.arg0;
                int clampedSc = std::max(1, std::min(mWarpSelectedScenario, kGalaxies[hb.arg0].maxScenarios));
                port_request_warp(kGalaxies[hb.arg0].stageName, clampedSc);
                port_play_ui_se(PORT_UI_SE_WARP);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                dismissSettings(false);
                return;
            }
            case HIT_WARP_CARD:
                mWarpSelectedGalaxy = hb.arg0;
                if (mWarpSelectedScenario > kGalaxies[hb.arg0].maxScenarios) {
                    mWarpSelectedScenario = 1;
                }
                port_play_ui_se(PORT_UI_SE_TICK);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                break;
            case HIT_WARP_LAUNCH:
                if (mWarpSelectedGalaxy >= 0 && mWarpSelectedGalaxy < kNumGalaxies) {
                    int clampedSc = std::max(1, std::min(mWarpSelectedScenario, kGalaxies[mWarpSelectedGalaxy].maxScenarios));
                    port_request_warp(kGalaxies[mWarpSelectedGalaxy].stageName, clampedSc);
                }
                port_play_ui_se(PORT_UI_SE_WARP);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                dismissSettings(false);
                return;
            case HIT_TOGGLE_SHOW_CONTROLS:
                mSettings.showControls = !mSettings.showControls;
                port_play_ui_se(PORT_UI_SE_TICK);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                saveNow = true;
                break;
            case HIT_TOGGLE_SHOW_DPAD:
                mSettings.showDpad = !mSettings.showDpad;
                port_play_ui_se(PORT_UI_SE_TICK);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                saveNow = true;
                break;
            case HIT_SEG_DPAD_SIDE:
                mSettings.dpadOnRight = (hb.arg0 != 0);
                mSettings.dpadOffsetX = 0.0f;
                mSettings.dpadOffsetY = 0.0f;
                port_play_ui_se(PORT_UI_SE_TICK);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                saveNow = true;
                break;
            case HIT_SEG_STICK_MODE:
                mSettings.dynamicStick = (hb.arg0 == 0);
                port_play_ui_se(PORT_UI_SE_TICK);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                saveNow = true;
                break;
            case HIT_SEG_OPACITY: {
                static const float kOps[4] = {0.25f, 0.50f, 0.75f, 1.00f};
                int idx = std::max(0, std::min(3, hb.arg0));
                mSettings.opacity = kOps[idx];
                port_play_ui_se(PORT_UI_SE_TICK);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                saveNow = true;
                break;
            }
            case HIT_SEG_BTN_SIZE: {
                static const float kSizes[3] = {0.85f, 1.00f, 1.25f};
                int idx = std::max(0, std::min(2, hb.arg0));
                mSettings.scale = kSizes[idx];
                port_play_ui_se(PORT_UI_SE_TICK);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                saveNow = true;
                break;
            }
            case HIT_LAYOUT_EDIT:
                mSettingsOpen = false;
                mEditLayoutMode = true;
                port_play_ui_se(PORT_UI_SE_DECIDE);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                saveNow = true;
                break;
            case HIT_LAYOUT_RESET:
                mSettings.stickOffsetX = 0.0f;
                mSettings.stickOffsetY = 0.0f;
                mSettings.buttonOffsetX = 0.0f;
                mSettings.buttonOffsetY = 0.0f;
                mSettings.dpadOffsetX = 0.0f;
                mSettings.dpadOffsetY = 0.0f;
                port_play_ui_se(PORT_UI_SE_CANCEL);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                saveNow = true;
                break;
            case HIT_SEG_RIGHT_STICK_MODE:
                mSettings.rightStickPointer = (hb.arg0 == 0);
                port_play_ui_se(PORT_UI_SE_TICK);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                saveNow = true;
                break;
            case HIT_TOGGLE_FULLSCREEN:
                mSettings.fullscreen = !mSettings.fullscreen;
                port_play_ui_se(PORT_UI_SE_TICK);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                saveNow = true;
                break;
            case HIT_TOGGLE_CAMERA_CUTOUT:
                mSettings.showCameraCutout = !mSettings.showCameraCutout;
                port_set_camera_cutout(mSettings.showCameraCutout ? 1 : 0);
                port_play_ui_se(PORT_UI_SE_TICK);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                saveNow = true;
                break;
            case HIT_SEG_RESOLUTION:
                mSettings.graphicsPreset = std::max(0, std::min(4, hb.arg0));
                port_play_ui_se(PORT_UI_SE_TICK);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                saveNow = true;
                break;
            case HIT_TOGGLE_HD_UI:
                mSettings.hdUiTextures = !mSettings.hdUiTextures;
                port_play_ui_se(PORT_UI_SE_TICK);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                saveNow = true;
                break;
            case HIT_TOGGLE_FPS_COUNTER:
                mSettings.showFpsCounter = !mSettings.showFpsCounter;
                port_play_ui_se(PORT_UI_SE_TICK);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                saveNow = true;
                break;
            case HIT_STEP_FPS_SCALE:
                if (hb.arg0 < 0) {
                    mSettings.fpsScale = std::max(0.60f, mSettings.fpsScale - 0.10f);
                } else if (hb.arg0 > 0) {
                    mSettings.fpsScale = std::min(2.20f, mSettings.fpsScale + 0.10f);
                } else {
                    mSettings.fpsScale = 1.00f;
                }
                port_play_ui_se(PORT_UI_SE_TICK);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                saveNow = true;
                break;
            case HIT_FPS_EDIT:
                mSettings.showFpsCounter = true;
                mSwAnimFpsCounter = 1.0f;
                mSettingsOpen = false;
                mEditLayoutMode = true;
                port_play_ui_se(PORT_UI_SE_DECIDE);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                saveNow = true;
                break;
            case HIT_FPS_RESET:
                mSettings.fpsOffsetX = 0.0f;
                mSettings.fpsOffsetY = 0.0f;
                mSettings.fpsScale = 1.0f;
                port_play_ui_se(PORT_UI_SE_CANCEL);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                saveNow = true;
                break;
            case HIT_TOGGLE_SKIP_CUTSCENES:
                mSettings.modSkipCutscenes = !mSettings.modSkipCutscenes;
                port_play_ui_se(PORT_UI_SE_TICK);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                saveNow = true;
                break;
            case HIT_TOGGLE_INF_HP:
                mSettings.modInfiniteHealth = !mSettings.modInfiniteHealth;
                port_play_ui_se(PORT_UI_SE_TICK);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                saveNow = true;
                break;
            case HIT_TOGGLE_INF_POWERUP:
                mSettings.modInfinitePowerup = !mSettings.modInfinitePowerup;
                port_play_ui_se(PORT_UI_SE_TICK);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                saveNow = true;
                break;
            case HIT_TOGGLE_FLIGHT:
                mSettings.modFlightEverywhere = !mSettings.modFlightEverywhere;
                if (mSettings.modFlightEverywhere) {
                    port_request_powerup(6);
                } else if (port_get_current_powerup() == 7) {
                    port_request_powerup(0);
                }
                port_play_ui_se(PORT_UI_SE_TICK);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                saveNow = true;
                break;
            case HIT_POWERUP_MORPH:
                port_request_powerup(hb.arg0);
                port_play_ui_se(PORT_UI_SE_DECIDE);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                break;
            case HIT_OPEN_WARP:
                mWarpMenuOpen = true;
                mWarpScrollY = 0.0f;
                mWarpScrollTarget = 0.0f;
                port_play_ui_se(PORT_UI_SE_WINDOW_OPEN);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                break;
            case HIT_UNLOCK_SPIN:
                port_request_unlock_spin();
                port_play_ui_se(PORT_UI_SE_DECIDE);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                break;
            case HIT_SEG_CHARACTER:
                if (hb.arg0 != mSettings.playerCharacter) {
                    mSettings.playerCharacter = hb.arg0;
                    port_request_character_change(hb.arg0);
                }
                port_play_ui_se(PORT_UI_SE_DECIDE);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                saveNow = true;
                break;
            case HIT_TOGGLE_HAPTICS:
                mSettings.modAdvancedHaptics = !mSettings.modAdvancedHaptics;
                port_play_ui_se(PORT_UI_SE_TICK);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                saveNow = true;
                break;
            case HIT_OPEN_INSTALLER:
                port_open_installer();
                port_play_ui_se(PORT_UI_SE_DECIDE);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                break;
            case HIT_INSTALL_PLUS:
                port_install_plus();
                port_play_ui_se(PORT_UI_SE_DECIDE);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                break;
            case HIT_SHARE_LOG:
                port_share_log();
                port_play_ui_se(PORT_UI_SE_DECIDE);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                break;
            case HIT_EXPORT_SAVE:
                port_export_save();
                port_play_ui_se(PORT_UI_SE_DECIDE);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                break;
            case HIT_IMPORT_SAVE:
                port_import_save();
                port_play_ui_se(PORT_UI_SE_DECIDE);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                break;
            case HIT_NET_DISCONNECT:
                port_net_disconnect();
                port_play_ui_se(PORT_UI_SE_CANCEL);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                break;
            case HIT_SEG_NET_MODE:
                port_net_set_mode(hb.arg0);
                port_play_ui_se(PORT_UI_SE_TICK);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                break;
            case HIT_NET_HOST_TOGGLE:
                if (port_net_get_role() == 1) {
                    port_net_disconnect();
                    port_play_ui_se(PORT_UI_SE_CANCEL);
                } else {
                    if (mSettings.playerCharacter != 0) {
                        mSettings.playerCharacter = 0;
                        port_request_character_change(0);
                    } else {
                        port_set_player_character(0);
                    }
                    port_net_start_host(24601, port_net_get_mode(), "Galaxy Room");
                    port_play_ui_se(PORT_UI_SE_DECIDE);
                }
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                saveNow = true;
                break;
            case HIT_NET_REFRESH:
                port_net_refresh_discovery();
                port_play_ui_se(PORT_UI_SE_TICK);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                break;
            case HIT_NET_JOIN_HOST: {
                PortLanHostInfo hosts[3];
                int n = port_net_get_lan_hosts(hosts, 3);
                if (hb.arg0 >= 0 && hb.arg0 < n) {
                    if (mSettings.playerCharacter != 1) {
                        mSettings.playerCharacter = 1;
                        port_request_character_change(1);
                    } else {
                        port_set_player_character(1);
                    }
                    port_net_connect(hosts[hb.arg0].ip, hosts[hb.arg0].port);
                    port_play_ui_se(PORT_UI_SE_DECIDE);
                    mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                    saveNow = true;
                }
                break;
            }
            case HIT_NET_DIRECT_CONNECT:
                if (mSettings.playerCharacter != 1) {
                    mSettings.playerCharacter = 1;
                    port_request_character_change(1);
                } else {
                    port_set_player_character(1);
                }
                port_net_connect("192.168.43.1", 24601);
                port_play_ui_se(PORT_UI_SE_DECIDE);
                mobile_haptics::trigger(mobile_haptics::HapticType::Tick);
                saveNow = true;
                break;
            default:
                break;
            }
            if (saveNow && !mSavePath.empty()) {
                mSettings.save(mSavePath);
            }
            mDirty = true;
        };

        auto findHit = [&](float px, float py, bool wantFixed) -> const UiHitbox* {
            for (int i = (int)mSettingsHits.size() - 1; i >= 0; i--) {
                const UiHitbox& hb = mSettingsHits[i];
                if (hb.fixed != wantFixed) continue;
                if (px >= hb.x0 && px <= hb.x1 && py >= hb.y0 && py <= hb.y1) {
                    return &hb;
                }
            }
            return nullptr;
        };

        // 1. Move Gesture: Slider Dragging or Smooth Vertical Deck Scrolling
        if (action == AMOTION_EVENT_ACTION_MOVE) {
            if (mWarpMenuOpen && mWarpScrollPointerId >= 0) {
                for (size_t i = 0; i < pointerCount; i++) {
                    if (AMotionEvent_getPointerId(event, i) == mWarpScrollPointerId) {
                        float py = AMotionEvent_getY(event, i);
                        float deltaY = py - mWarpTouchStartY;
                        if (fabsf(deltaY) > 6.0f * uiScale || mWarpIsScrolling) {
                            mWarpIsScrolling = true;
                            mWarpScrollTarget = std::max(0.0f, std::min(mWarpScrollStartOffset - deltaY, mWarpMaxScroll));
                            mDirty = true;
                        }
                        return true;
                    }
                }
            } else if (!mWarpMenuOpen && mDrawerScrollPointerId >= 0) {
                for (size_t i = 0; i < pointerCount; i++) {
                    if (AMotionEvent_getPointerId(event, i) == mDrawerScrollPointerId) {
                        float px = AMotionEvent_getX(event, i);
                        float py = AMotionEvent_getY(event, i);
                        if (mActiveSliderKind != 0) {
                            applySliderAtX(mActiveSliderKind, px, mActiveSliderX0, mActiveSliderX1);
                            return true;
                        }
                        float deltaY = py - mDrawerTouchStartY;
                        if (fabsf(deltaY) > 6.0f * uiScale || mDrawerIsScrolling) {
                            mDrawerIsScrolling = true;
                            mDrawerScrollTarget = std::max(0.0f, std::min(mDrawerScrollStartOffset - deltaY, mSettingsMaxScroll));
                            mDirty = true;
                        }
                        return true;
                    }
                }
            }
            return true;
        }

        // 2. Up / Pointer Up / Cancel: Finish Drag or Execute Deck Hit
        if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_POINTER_UP || action == AMOTION_EVENT_ACTION_CANCEL) {
            int32_t liftedPid = AMotionEvent_getPointerId(event, pointerIndex);
            if (mWarpMenuOpen && liftedPid == mWarpScrollPointerId) {
                if (!mWarpIsScrolling && action != AMOTION_EVENT_ACTION_CANCEL) {
                    float px = AMotionEvent_getX(event, pointerIndex);
                    float py = AMotionEvent_getY(event, pointerIndex);
                    if (const UiHitbox* hb = findHit(px, py, false)) {
                        executeHit(*hb);
                    }
                }
                mWarpScrollPointerId = -1;
                mWarpIsScrolling = false;
                mDirty = true;
                return true;
            } else if (!mWarpMenuOpen && liftedPid == mDrawerScrollPointerId) {
                if (mActiveSliderKind != 0) {
                    if (!mSavePath.empty()) mSettings.save(mSavePath);
                } else if (!mDrawerIsScrolling && action != AMOTION_EVENT_ACTION_CANCEL) {
                    float px = AMotionEvent_getX(event, pointerIndex);
                    float py = AMotionEvent_getY(event, pointerIndex);
                    if (const UiHitbox* hb = findHit(px, py, false)) {
                        executeHit(*hb);
                    }
                }
                mDrawerScrollPointerId = -1;
                mDrawerIsScrolling = false;
                mActiveSliderKind = 0;
                mDirty = true;
                return true;
            }
            return true;
        }

        // 3. Down / Pointer Down: Instant Fixed Chrome Controls, Slider Grab, or Scroll Acquisition
        if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_POINTER_DOWN) {
            float px = AMotionEvent_getX(event, pointerIndex);
            float py = AMotionEvent_getY(event, pointerIndex);
            int32_t pid = AMotionEvent_getPointerId(event, pointerIndex);

            if (const UiHitbox* fixedHit = findHit(px, py, true)) {
                executeHit(*fixedHit);
                return true;
            }

            if (px >= mDeckLeftX && py >= mDeckTopY && py < mDeckBottomY) {
                if (mWarpMenuOpen) {
                    mWarpScrollPointerId = pid;
                    mWarpTouchStartY = py;
                    mWarpTouchStartX = px;
                    mWarpScrollStartOffset = mWarpScrollY;
                    mWarpIsScrolling = false;
                } else {
                    mDrawerScrollPointerId = pid;
                    mDrawerTouchStartY = py;
                    mDrawerTouchStartX = px;
                    mDrawerScrollStartOffset = mDrawerScrollY;
                    mDrawerIsScrolling = false;
                    mActiveSliderKind = 0;
                    for (int i = (int)mSettingsHits.size() - 1; i >= 0; i--) {
                        const UiHitbox& hb = mSettingsHits[i];
                        if (!hb.fixed &&
                            (hb.kind == HIT_SLIDER_SENSITIVITY || hb.kind == HIT_SLIDER_FPS_LIMIT || hb.kind == HIT_SLIDER_LANGUAGE) &&
                            px >= hb.x0 && px <= hb.x1 && py >= hb.y0 && py <= hb.y1) {
                            mActiveSliderKind = hb.kind;
                            mActiveSliderX0 = hb.p0;
                            mActiveSliderX1 = hb.p1;
                            applySliderAtX(hb.kind, px, hb.p0, hb.p1);
                            break;
                        }
                    }
                }
            }
            return true;
        }
        return true;
    }

    // -------------------------------------------------------------
    // Live Virtual Controls Handling
    // -------------------------------------------------------------
    pad.connected = 1;
    uint32_t touchButtons = 0;

    // Reset button press highlights
    mBtnAPressed = false;
    mBtnBPressed = false;
    mBtnZPressed = false;
    mBtnSpinPressed = false;
    mBtnPlusPressed = false;
    mBtnDpadLeft = false;
    mBtnDpadRight = false;
    mBtnDpadUp = false;
    mBtnDpadDown = false;

    // Button layout positions
    float cutoutInset = mSettings.showCameraCutout ? 0.0f : (42.0f * uiScale);
    float btnBaseX = w - 160.0f * scale - cutoutInset + mSettings.buttonOffsetX;
    float btnBaseY = h - 160.0f * scale + mSettings.buttonOffsetY;

    // A (Jump) - Primary action
    float aX = btnBaseX, aY = btnBaseY, aR = 54.0f * scale;
    // B (Shoot Star Bit) - Lower left of A
    float bX = btnBaseX - 120.0f * scale, bY = btnBaseY + 25.0f * scale, bR = 45.0f * scale;
    // SPIN - Top right of A
    float spinX = btnBaseX + 35.0f * scale, spinY = btnBaseY - 130.0f * scale, spinR = 48.0f * scale;
    // Z (Crouch) - Top left of A
    float zX = btnBaseX - 90.0f * scale, zY = btnBaseY - 105.0f * scale, zR = 44.0f * scale;
    // Plus (+) - Top right
    float plusX = w - 70.0f * scale - cutoutInset, plusY = 70.0f * scale, plusR = 34.0f * scale;

    // Camera D-Pad position
    float baseDpadX = mSettings.dpadOnRight ? (w - 380.0f * scale - cutoutInset) : (140.0f * scale + cutoutInset);
    float baseDpadY = mSettings.dpadOnRight ? (h - 220.0f * scale) : (h - 390.0f * scale);
    float dpadX = baseDpadX + mSettings.dpadOffsetX;
    float dpadY = baseDpadY + mSettings.dpadOffsetY;
    float dpadArmDist = 38.0f * scale;
    float dpadArmR = 24.0f * scale;
    float dpadTotalR = (dpadArmDist + dpadArmR) * 1.15f;

    // Stick default position - Bottom left thumb area
    float defaultStickX = 175.0f * scale + cutoutInset + mSettings.stickOffsetX;
    float defaultStickY = h - 175.0f * scale + mSettings.stickOffsetY;
    float stickBaseR = 95.0f * scale;
    float stickMaxR = 100.0f * scale;

    // Complete release: when all fingers are lifted or gesture canceled
    if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_CANCEL) {
        mStickPointerId = -1;
        mStickKnobX = mStickBaseX;
        mStickKnobY = mStickBaseY;
        pad.stickX = 0.0f;
        pad.stickY = 0.0f;
        touchButtons = 0;
        pad.buttons = 0;
        mBtnAPressed = mBtnBPressed = mBtnZPressed = mBtnSpinPressed = mBtnPlusPressed = false;
        mBtnDpadLeft = mBtnDpadRight = mBtnDpadUp = mBtnDpadDown = false;
        mDirty = true;
        pad.pointerValid = 1;
        pad.pointerX = mPointerX;
        pad.pointerY = mPointerY;
        pad.pointerDist = 2.0f;
        return true;
    }

    // Single finger release during multi-touch
    if (action == AMOTION_EVENT_ACTION_POINTER_UP) {
        int32_t liftedPid = AMotionEvent_getPointerId(event, pointerIndex);
        if (liftedPid == mStickPointerId) {
            mStickPointerId = -1;
            mStickKnobX = mStickBaseX;
            mStickKnobY = mStickBaseY;
            pad.stickX = 0.0f;
            pad.stickY = 0.0f;
            mDirty = true;
        }
    }

    // 1. Virtual Stick Acquisition:
    // Only captures fresh touches in the designated bottom-left thumb zone!
    if (mSettings.showControls && (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_POINTER_DOWN)) {
        float px = AMotionEvent_getX(event, pointerIndex);
        float py = AMotionEvent_getY(event, pointerIndex);
        int32_t pid = AMotionEvent_getPointerId(event, pointerIndex);

        bool stickEligible = false;
        if (mSettings.dynamicStick) {
            // Strictly bottom-left quadrant: left 36% of screen AND bottom 50% of screen
            stickEligible = (px <= w * 0.36f && py >= h * 0.50f);
        } else {
            // Fixed mode: within 1.6x of stick center
            float d2 = (px - defaultStickX) * (px - defaultStickX) + (py - defaultStickY) * (py - defaultStickY);
            stickEligible = (d2 <= (stickBaseR * 1.6f) * (stickBaseR * 1.6f));
        }

        if (stickEligible && mStickPointerId < 0) {
            mStickPointerId = pid;
            if (mSettings.dynamicStick) {
                mStickBaseX = px;
                mStickBaseY = py;
            } else {
                mStickBaseX = defaultStickX;
                mStickBaseY = defaultStickY;
            }
            mStickKnobX = px;
            mStickKnobY = py;
            mDirty = true;
        }
    }

    // 2. Iterate Active Pointers
    bool stickFound = false;
    for (size_t i = 0; i < pointerCount; i++) {
        // Skip pointer that is releasing right now
        if (action == AMOTION_EVENT_ACTION_POINTER_UP && (int32_t)i == pointerIndex) {
            continue;
        }

        int32_t pid = AMotionEvent_getPointerId(event, i);
        float px = AMotionEvent_getX(event, i);
        float py = AMotionEvent_getY(event, i);

        if (pid == mStickPointerId) {
            stickFound = true;
            float dx = px - mStickBaseX;
            float dy = py - mStickBaseY;
            float dist = sqrtf(dx * dx + dy * dy);
            float deadzone = 8.0f * scale;

            if (dist > stickMaxR && dist > 0.001f) {
                mStickKnobX = mStickBaseX + (dx / dist) * stickMaxR;
                mStickKnobY = mStickBaseY + (dy / dist) * stickMaxR;
                pad.stickX = dx / dist;
                pad.stickY = -dy / dist;
            } else if (dist > deadzone) {
                mStickKnobX = px;
                mStickKnobY = py;
                float norm = (dist - deadzone) / (stickMaxR - deadzone);
                pad.stickX = (dx / dist) * norm;
                pad.stickY = -(dy / dist) * norm;
            } else {
                mStickKnobX = px;
                mStickKnobY = py;
                pad.stickX = 0.0f;
                pad.stickY = 0.0f;
            }
            pad.stickX = std::max(-1.0f, std::min(1.0f, pad.stickX));
            pad.stickY = std::max(-1.0f, std::min(1.0f, pad.stickY));
        } else {
            // Button hit testing
            auto inRadius = [](float tx, float ty, float cx, float cy, float r) {
                float d2 = (tx - cx) * (tx - cx) + (ty - cy) * (ty - cy);
                return d2 <= (r * 1.30f) * (r * 1.30f);
            };

            bool hitButton = false;
            if (mSettings.showControls) {
                if (inRadius(px, py, aX, aY, aR)) {
                    touchButtons |= 0x0800; // WPAD_BUTTON_A
                    mBtnAPressed = true;
                    hitButton = true;
                }
                if (inRadius(px, py, bX, bY, bR)) {
                    touchButtons |= 0x0400; // WPAD_BUTTON_B
                    mBtnBPressed = true;
                    hitButton = true;
                }
                if (inRadius(px, py, spinX, spinY, spinR)) {
                    touchButtons |= 0x0020; // WPAD_BUTTON_SPIN
                    mBtnSpinPressed = true;
                    hitButton = true;
                }
                if (inRadius(px, py, zX, zY, zR)) {
                    touchButtons |= 0x2000; // WPAD_BUTTON_Z
                    mBtnZPressed = true;
                    hitButton = true;
                }
                if (inRadius(px, py, plusX, plusY, plusR)) {
                    if (mDrawerAnim <= 0.02f && monotonicNowNs() >= mPauseGuardUntilNs) {
                        touchButtons |= 0x0010; // WPAD_BUTTON_PLUS
                        mBtnPlusPressed = true;
                    }
                    hitButton = true;
                }
                if (mSettings.showDpad) {
                    float dDpadX = px - dpadX;
                    float dDpadY = py - dpadY;
                    float distDpad = sqrtf(dDpadX * dDpadX + dDpadY * dDpadY);
                    if (distDpad <= dpadTotalR) {
                        hitButton = true;
                        float deadzone = 8.0f * scale;
                        if (distDpad > deadzone) {
                            if (dDpadX < -deadzone && fabsf(dDpadX) > fabsf(dDpadY) * 0.40f) {
                                touchButtons |= 0x0001; // WPAD_BUTTON_LEFT (rotate cam left)
                                mBtnDpadLeft = true;
                            } else if (dDpadX > deadzone && fabsf(dDpadX) > fabsf(dDpadY) * 0.40f) {
                                touchButtons |= 0x0002; // WPAD_BUTTON_RIGHT (rotate cam right)
                                mBtnDpadRight = true;
                            }
                            if (dDpadY < -deadzone && fabsf(dDpadY) > fabsf(dDpadX) * 0.40f) {
                                touchButtons |= 0x0008; // WPAD_BUTTON_UP (first-person view / zoom)
                                mBtnDpadUp = true;
                            } else if (dDpadY > deadzone && fabsf(dDpadY) > fabsf(dDpadX) * 0.40f) {
                                touchButtons |= (0x4000 | 0x0004); // WPAD_BUTTON_C | WPAD_BUTTON_DOWN (recenter camera & exit FP)
                                mBtnDpadDown = true;
                            }
                        }
                    }
                }
            }

            // Direct screen touch: exclusively moves the in-game Star Pointer!
            // No phantom buttons behind the HUD: button A is exclusively pressed via the on-screen [A] button!
            if (!hitButton && vpW > 0 && vpH > 0) {
                mPointerX = ((px - vpX) / (float)vpW) * 2.0f - 1.0f;
                mPointerY = ((py - vpY) / (float)vpH) * 2.0f - 1.0f;
                mPointerX = std::max(-1.0f, std::min(1.0f, mPointerX));
                mPointerY = std::max(-1.0f, std::min(1.0f, mPointerY));
            }
        }
    }

    if (!stickFound && mStickPointerId >= 0) {
        mStickPointerId = -1;
        mStickKnobX = mStickBaseX;
        mStickKnobY = mStickBaseY;
        pad.stickX = 0.0f;
        pad.stickY = 0.0f;
        mDirty = true;
    }

    pad.buttons = touchButtons;
    pad.pointerValid = 1;
    pad.pointerX = mPointerX;
    pad.pointerY = mPointerY;
    pad.pointerDist = 2.0f;

    return true;
}

void UIOverlay::render(int screenW, int screenH) {
    mSettingsHits.clear();
    if (!init()) return;

    // Ensure OpenGL viewport always matches physical display window for UI overlay
    glViewport(0, 0, screenW, screenH);

    std::vector<UIVertex> verts;
    verts.reserve(2048);

    float baseDpi = (float)screenH / 650.0f;
    float uiScale = std::min(baseDpi, (float)screenW / 1060.0f);
    float scale = baseDpi * mSettings.scale;
    float op = mSettings.opacity;
    float w = (float)screenW;
    float h = (float)screenH;

    // Animation progress calculation
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
    if (mLastAnimTime == 0) mLastAnimTime = now;
    float dt = (float)(now - mLastAnimTime) / 1e9f;
    mLastAnimTime = now;
    if (dt < 0.0001f) dt = 0.016f;
    if (dt > 0.1f) dt = 0.1f;
    float animPhase = (float)((double)(now % 60000000000ll) / 1e9);

    float target = mSettingsOpen ? 1.0f : 0.0f;
    if (fabsf(mDrawerAnim - target) > 0.01f) {
        float speed = mSettingsOpen ? 16.0f : 34.0f;
        mDrawerAnim += (target - mDrawerAnim) * (1.0f - expf(-speed * dt));
        if (fabsf(mDrawerAnim - target) < 0.02f) {
            mDrawerAnim = target;
        }
        mDirty = true;
    } else if (mDrawerAnim != target) {
        mDrawerAnim = target;
    }
    if (mSettingsOpen || mDrawerAnim > 0.02f) {
        mDirty = true;
    }

    float cogRot = mDrawerAnim * 1.5707963f;

    // Fluid Switch Knob Animations
    auto updateSwAnim = [&](float& anim, bool state) {
        float tgt = state ? 1.0f : 0.0f;
        if (fabsf(anim - tgt) > 0.001f) {
            anim += (tgt - anim) * (1.0f - expf(-26.0f * dt));
            mDirty = true;
        } else {
            anim = tgt;
        }
    };
    updateSwAnim(mSwAnimShowControls, mSettings.showControls);
    updateSwAnim(mSwAnimShowDpad, mSettings.showDpad);
    updateSwAnim(mSwAnimFullscreen, mSettings.fullscreen);
    updateSwAnim(mSwAnimCameraCutout, mSettings.showCameraCutout);
    updateSwAnim(mSwAnimHdUi, mSettings.hdUiTextures);
    updateSwAnim(mSwAnimSkipCutscenes, mSettings.modSkipCutscenes);
    updateSwAnim(mSwAnimInfiniteHp, mSettings.modInfiniteHealth);
    updateSwAnim(mSwAnimInfinitePowerup, mSettings.modInfinitePowerup);
    updateSwAnim(mSwAnimHaptics, mSettings.modAdvancedHaptics);
    updateSwAnim(mSwAnimFlightEverywhere, mSettings.modFlightEverywhere);
    updateSwAnim(mSwAnimFpsCounter, mSettings.showFpsCounter);

    int numTabs = mPlusEdition ? 5 : 3;
    if (mSettingsTab < 0 || mSettingsTab >= numTabs) mSettingsTab = 0;

    // Fluid Vertical Observatory Sidebar Indicator Animation
    float headerH = 68.0f * uiScale;
    float sideTopY = headerH + 14.0f * uiScale;
    if (mWarpMenuOpen) {
        float domeStep = 34.0f * uiScale;
        mTabTargetY = sideTopY + 14.0f * uiScale + mWarpCategoryIndex * domeStep;
        mTabTargetH = 30.0f * uiScale;
    } else {
        float tabStep = 53.0f * uiScale;
        mTabTargetY = sideTopY + 14.0f * uiScale + mSettingsTab * tabStep;
        mTabTargetH = 46.0f * uiScale;
    }
    if (mTabIndicatorH < 1.0f) {
        mTabIndicatorY = mTabTargetY;
        mTabIndicatorH = mTabTargetH;
    } else if (fabsf(mTabIndicatorY - mTabTargetY) > 0.2f || fabsf(mTabIndicatorH - mTabTargetH) > 0.2f) {
        mTabIndicatorY += (mTabTargetY - mTabIndicatorY) * (1.0f - expf(-22.0f * dt));
        mTabIndicatorH += (mTabTargetH - mTabIndicatorH) * (1.0f - expf(-22.0f * dt));
        mDirty = true;
    }

    auto addQuad = [&](float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                       const Color& c, float mode, float param, float aspect = 1.0f, float extra = 0.0f) {
        verts.push_back({x0, y0, u0, v0, c.r, c.g, c.b, c.a, mode, param, aspect, extra});
        verts.push_back({x1, y0, u1, v0, c.r, c.g, c.b, c.a, mode, param, aspect, extra});
        verts.push_back({x1, y1, u1, v1, c.r, c.g, c.b, c.a, mode, param, aspect, extra});

        verts.push_back({x0, y0, u0, v0, c.r, c.g, c.b, c.a, mode, param, aspect, extra});
        verts.push_back({x1, y1, u1, v1, c.r, c.g, c.b, c.a, mode, param, aspect, extra});
        verts.push_back({x0, y1, u0, v1, c.r, c.g, c.b, c.a, mode, param, aspect, extra});
    };

    auto drawCircle = [&](float cx, float cy, float r, const Color& c) {
        addQuad(cx - r, cy - r, cx + r, cy + r, -1.0f, -1.0f, 1.0f, 1.0f, c, 1.0f, 0.0f);
    };

    auto drawRing = [&](float cx, float cy, float r, float innerRatio, const Color& c) {
        addQuad(cx - r, cy - r, cx + r, cy + r, -1.0f, -1.0f, 1.0f, 1.0f, c, 1.0f, innerRatio);
    };

    auto drawRoundRect = [&](float x0, float y0, float x1, float y1, float radius, const Color& c) {
        float rw = x1 - x0;
        float rh = y1 - y0;
        if (rw <= 0.0f || rh <= 0.0f) return;
        if (rw >= rh) {
            float aspect = rw / rh;
            float rNorm = std::min(1.0f, radius / (rh * 0.5f));
            addQuad(x0, y0, x1, y1, -aspect, -1.0f, aspect, 1.0f, c, 0.0f, rNorm, aspect, 0.0f);
        } else {
            float aspect = rh / rw;
            float rNorm = std::min(1.0f, radius / (rw * 0.5f));
            addQuad(x0, y0, x1, y1, -1.0f, -aspect, 1.0f, aspect, c, 0.0f, rNorm, aspect, 1.0f);
        }
    };

    auto addGradRect = [&](float x0, float y0, float x1, float y1, const Color& topCol, const Color& botCol) {
        verts.push_back({x0, y0, 0.0f, 0.0f, topCol.r, topCol.g, topCol.b, topCol.a, 0.0f, 0.0f, 1.0f, 0.0f});
        verts.push_back({x1, y0, 0.0f, 0.0f, topCol.r, topCol.g, topCol.b, topCol.a, 0.0f, 0.0f, 1.0f, 0.0f});
        verts.push_back({x1, y1, 0.0f, 0.0f, botCol.r, botCol.g, botCol.b, botCol.a, 0.0f, 0.0f, 1.0f, 0.0f});

        verts.push_back({x0, y0, 0.0f, 0.0f, topCol.r, topCol.g, topCol.b, topCol.a, 0.0f, 0.0f, 1.0f, 0.0f});
        verts.push_back({x1, y1, 0.0f, 0.0f, botCol.r, botCol.g, botCol.b, botCol.a, 0.0f, 0.0f, 1.0f, 0.0f});
        verts.push_back({x0, y1, 0.0f, 0.0f, botCol.r, botCol.g, botCol.b, botCol.a, 0.0f, 0.0f, 1.0f, 0.0f});
    };

    float uiFade = std::max(0.0f, std::min(1.0f, mDrawerAnim));

    auto drawCard = [&](float x0, float y0, float x1, float y1, float radius, const Color& bg, const Color& border) {
        drawRoundRect(x0, y0, x1, y1, radius, border);
        drawRoundRect(x0 + 1.4f * uiScale, y0 + 1.4f * uiScale, x1 - 1.4f * uiScale, y1 - 1.4f * uiScale, std::max(0.0f, radius - 1.4f * uiScale), bg);
        // Subtle SMG Wii upper glass sheen highlight
        if (x1 - x0 > 24.0f * uiScale && y1 - y0 > 22.0f * uiScale) {
            drawRoundRect(x0 + 10.0f * uiScale, y0 + 2.5f * uiScale, x1 - 10.0f * uiScale, y0 + 14.0f * uiScale, 5.5f * uiScale, rgba(1.0f, 1.0f, 1.0f, 0.048f * uiFade));
        }
    };

    auto drawCogwheel = [&](float cx, float cy, float r, const Color& c, float rot = 0.0f) {
        addQuad(cx - r, cy - r, cx + r, cy + r, -1.0f, -1.0f, 1.0f, 1.0f, c, 3.0f, rot);
    };

    auto drawCloseIcon = [&](float cx, float cy, float r, const Color& c) {
        addQuad(cx - r, cy - r, cx + r, cy + r, -1.0f, -1.0f, 1.0f, 1.0f, c, 4.0f, 0.0f);
    };

    auto drawCyclone = [&](float cx, float cy, float r, const Color& c, float rot = 0.0f) {
        addQuad(cx - r, cy - r, cx + r, cy + r, -1.0f, -1.0f, 1.0f, 1.0f, c, 5.0f, rot);
    };

    auto drawChevron = [&](float cx, float cy, float r, const Color& c, float rotAngle = 0.0f) {
        addQuad(cx - r, cy - r, cx + r, cy + r, -1.0f, -1.0f, 1.0f, 1.0f, c, 6.0f, rotAngle);
    };

    auto drawCameraIcon = [&](float cx, float cy, float r, const Color& c) {
        addQuad(cx - r, cy - r, cx + r, cy + r, -1.0f, -1.0f, 1.0f, 1.0f, c, 7.0f, 0.0f);
    };

    auto drawStar = [&](float cx, float cy, float r, const Color& c, float rot = 0.0f) {
        addQuad(cx - r, cy - r, cx + r, cy + r, -1.0f, -1.0f, 1.0f, 1.0f, c, 8.0f, rot);
    };

    auto drawSparkle = [&](float cx, float cy, float r, const Color& c, float rot = 0.0f) {
        addQuad(cx - r, cy - r, cx + r, cy + r, -1.0f, -1.0f, 1.0f, 1.0f, c, 9.0f, rot);
    };

    auto drawLine = [&](float x0, float y0, float x1, float y1, float thickness, const Color& c) {
        float dx = x1 - x0;
        float dy = y1 - y0;
        float len = sqrtf(dx * dx + dy * dy);
        if (len < 0.001f) return;
        float hThick = thickness * 0.5f;
        float nx = -dy / len * hThick;
        float ny = dx / len * hThick;

        verts.push_back({x0 + nx, y0 + ny, 0.0f, 0.0f, c.r, c.g, c.b, c.a, 0.0f, 0.0f, 1.0f, 0.0f});
        verts.push_back({x1 + nx, y1 + ny, 0.0f, 0.0f, c.r, c.g, c.b, c.a, 0.0f, 0.0f, 1.0f, 0.0f});
        verts.push_back({x1 - nx, y1 - ny, 0.0f, 0.0f, c.r, c.g, c.b, c.a, 0.0f, 0.0f, 1.0f, 0.0f});

        verts.push_back({x0 + nx, y0 + ny, 0.0f, 0.0f, c.r, c.g, c.b, c.a, 0.0f, 0.0f, 1.0f, 0.0f});
        verts.push_back({x1 - nx, y1 - ny, 0.0f, 0.0f, c.r, c.g, c.b, c.a, 0.0f, 0.0f, 1.0f, 0.0f});
        verts.push_back({x0 - nx, y0 - ny, 0.0f, 0.0f, c.r, c.g, c.b, c.a, 0.0f, 0.0f, 1.0f, 0.0f});
    };

    auto drawTriangle = [&](float x0, float y0, float x1, float y1, float x2, float y2, const Color& c) {
        verts.push_back({x0, y0, 0.0f, 0.0f, c.r, c.g, c.b, c.a, 0.0f, 0.0f, 1.0f, 0.0f});
        verts.push_back({x1, y1, 0.0f, 0.0f, c.r, c.g, c.b, c.a, 0.0f, 0.0f, 1.0f, 0.0f});
        verts.push_back({x2, y2, 0.0f, 0.0f, c.r, c.g, c.b, c.a, 0.0f, 0.0f, 1.0f, 0.0f});
    };

    const ui_font::Face& face = ui_font::kFaces[0];
    auto drawText = [&](const char* text, float x, float baseline, float sizeScale, const Color& c, int align = 0) {
        float strW = 0.0f;
        for (const char* p = text; *p; p++) {
            char ch = *p;
            if (ch >= 32 && ch < 127) {
                strW += (face.glyphs[ch - 32].advance * 0.25f) * sizeScale;
            }
        }
        float curX = x;
        if (align == 1) curX -= strW * 0.5f;      // Center
        else if (align == 2) curX -= strW;        // Right

        for (const char* p = text; *p; p++) {
            char ch = *p;
            if (ch < 32 || ch >= 127) continue;
            const ui_font::Glyph& g = face.glyphs[ch - 32];
            float gx = curX + g.left * sizeScale;
            float gy = baseline - g.top * sizeScale;
            float gw = g.w * sizeScale;
            float gh = g.h * sizeScale;
            float u0 = (float)g.x / (float)face.width;
            float v0 = (float)g.y / (float)face.height;
            float u1 = (float)(g.x + g.w) / (float)face.width;
            float v1 = (float)(g.y + g.h) / (float)face.height;

            addQuad(gx, gy, gx + gw, gy + gh, u0, v0, u1, v1, c, 2.0f, 0.0f);
            curX += (g.advance * 0.25f) * sizeScale;
        }
    };

    auto drawStatusChip = [&](float x, float y, const char* label, const Color& chipBg, const Color& textCol, const Color& dotCol) {
        float strW = 0.0f;
        for (const char* p = label; *p; p++) {
            char ch = *p;
            if (ch >= 32 && ch < 127) strW += (face.glyphs[ch - 32].advance * 0.25f) * (0.28f * uiScale);
        }
        float chipW = strW + 28.0f * uiScale;
        float chipH = 22.0f * uiScale;
        drawRoundRect(x, y, x + chipW, y + chipH, 11.0f * uiScale, chipBg);
        drawCircle(x + 9.0f * uiScale, y + chipH * 0.5f, 3.2f * uiScale, dotCol);
        drawText(label, x + 16.0f * uiScale, y + 15.0f * uiScale, 0.28f * uiScale, textCol, 0);
    };

    auto drawM3Switch = [&](float x, float y, float swW, float swH, float anim) {
        float r = swH * 0.5f;
        float t = std::max(0.0f, std::min(1.0f, anim));
        Color trackOffBg = rgba(0.095f, 0.135f, 0.285f, 0.96f * uiFade);
        Color trackOffBorder = rgba(0.36f, 0.52f, 0.80f, 0.65f * uiFade);
        Color trackOnBg = rgba(0.045f, 0.360f, 0.860f, 1.0f * uiFade);
        Color trackOnBorder = rgba(0.220f, 0.780f, 0.990f, 0.98f * uiFade);

        Color curTrackBg = {
            trackOffBg.r + (trackOnBg.r - trackOffBg.r) * t,
            trackOffBg.g + (trackOnBg.g - trackOffBg.g) * t,
            trackOffBg.b + (trackOnBg.b - trackOffBg.b) * t,
            trackOffBg.a + (trackOnBg.a - trackOffBg.a) * t
        };
        Color curTrackBorder = {
            trackOffBorder.r + (trackOnBorder.r - trackOffBorder.r) * t,
            trackOffBorder.g + (trackOnBorder.g - trackOffBorder.g) * t,
            trackOffBorder.b + (trackOnBorder.b - trackOffBorder.b) * t,
            trackOffBorder.a + (trackOnBorder.a - trackOffBorder.a) * t
        };

        drawRoundRect(x, y, x + swW, y + swH, r, curTrackBorder);
        drawRoundRect(x + 1.4f * uiScale, y + 1.4f * uiScale, x + swW - 1.4f * uiScale, y + swH - 1.4f * uiScale, r - 1.4f * uiScale, curTrackBg);

        float thumbMinR = r * 0.54f;
        float thumbMaxR = r * 0.74f;
        float curThumbR = thumbMinR + (thumbMaxR - thumbMinR) * t;

        float thumbMinX = x + r;
        float thumbMaxX = x + swW - r;
        float curThumbX = thumbMinX + (thumbMaxX - thumbMinX) * t;

        if (t > 0.05f) {
            drawCircle(curThumbX, y + r, curThumbR * 1.25f, rgba(0.35f, 0.85f, 1.0f, 0.25f * t * uiFade));
        }

        Color thumbOffCol = rgba(0.68f, 0.78f, 0.94f, uiFade);
        Color thumbOnCol = rgba(1.0f, 0.98f, 0.88f, uiFade);
        Color curThumbCol = {
            thumbOffCol.r + (thumbOnCol.r - thumbOffCol.r) * t,
            thumbOffCol.g + (thumbOnCol.g - thumbOffCol.g) * t,
            thumbOffCol.b + (thumbOnCol.b - thumbOffCol.b) * t,
            uiFade
        };

        drawCircle(curThumbX, y + r, curThumbR, curThumbCol);
    };

    auto drawSegmentedGroup = [&](float x, float y, float grpW, float grpH, const char** labels, int count, int activeIdx) {
        float r = 13.0f * uiScale;
        drawRoundRect(x, y, x + grpW, y + grpH, r, rgba(0.30f, 0.48f, 0.78f, 0.55f * uiFade));
        drawRoundRect(x + 1.2f * uiScale, y + 1.2f * uiScale, x + grpW - 1.2f * uiScale, y + grpH - 1.2f * uiScale, std::max(1.0f, r - 1.2f * uiScale), rgba(0.055f, 0.088f, 0.220f, 0.96f * uiFade));

        float segW = (grpW - 4.0f * uiScale) / (float)count;
        for (int i = 0; i < count; i++) {
            float sx0 = x + 2.0f * uiScale + i * segW;
            float sx1 = sx0 + segW;
            bool active = (i == activeIdx);
            if (active) {
                drawRoundRect(sx0, y + 2.0f * uiScale, sx1, y + grpH - 2.0f * uiScale, std::max(1.0f, r - 2.0f * uiScale), rgba(0.220f, 0.780f, 0.980f, 0.92f * uiFade));
                drawRoundRect(sx0 + 1.2f * uiScale, y + 3.2f * uiScale, sx1 - 1.2f * uiScale, y + grpH - 3.2f * uiScale, std::max(1.0f, r - 3.2f * uiScale), rgba(0.060f, 0.380f, 0.880f, uiFade));
                drawText(labels[i], (sx0 + sx1) * 0.5f, y + grpH * 0.65f, 0.32f * uiScale, rgba(1.0f, 1.0f, 1.0f, uiFade), 1);
            } else {
                drawText(labels[i], (sx0 + sx1) * 0.5f, y + grpH * 0.65f, 0.32f * uiScale, rgba(0.74f, 0.82f, 0.95f, 0.82f * uiFade), 1);
                if (i > 0 && (i - 1) != activeIdx) {
                    drawRoundRect(sx0 - 0.5f, y + 6.0f * uiScale, sx0 + 0.5f, y + grpH - 6.0f * uiScale, 0.0f, rgba(0.32f, 0.45f, 0.70f, 0.40f * uiFade));
                }
            }
        }
    };

    // -------------------------------------------------------------
    // Draw On-Screen Controls (HUD) - Modern Minimalist Frosted Theme
    // -------------------------------------------------------------
    if (mSettings.showControls && mDrawerAnim < 0.98f) {
        float hudOp = op * (1.0f - mDrawerAnim * 0.75f);
        float cutoutInset = mSettings.showCameraCutout ? 0.0f : (42.0f * baseDpi);

        // ---------------------------------------------------------
        // 1. Virtual Joystick - Precision Minimalist Reticle & Laser Tether
        // ---------------------------------------------------------
        float defaultStickX = 175.0f * scale + cutoutInset + mSettings.stickOffsetX;
        float defaultStickY = h - 175.0f * scale + mSettings.stickOffsetY;
        float baseX = (mStickPointerId >= 0) ? mStickBaseX : defaultStickX;
        float baseY = (mStickPointerId >= 0) ? mStickBaseY : defaultStickY;
        float knobX = (mStickPointerId >= 0) ? mStickKnobX : baseX;
        float knobY = (mStickPointerId >= 0) ? mStickKnobY : baseY;
        float baseR = 95.0f * scale;
        float knobR = 38.0f * scale;
        bool stickActive = (mStickPointerId >= 0);

        // Base Plate: Translucent dark glass disc
        drawCircle(baseX, baseY, baseR, rgba(0.05f, 0.07f, 0.11f, hudOp * 0.28f));

        // Outer Reticle: Razor-thin anti-aliased ring
        drawRing(baseX, baseY, baseR, 0.96f, rgba(0.60f, 0.80f, 1.0f, hudOp * 0.45f));

        // Cardinal Precision Orientation Ticks (N, S, E, W)
        float tickIn = baseR * 0.84f;
        float tickOut = baseR * 0.95f;
        float tickW = 2.0f * scale;
        Color tickCol = rgba(0.75f, 0.88f, 1.0f, hudOp * 0.50f);
        drawLine(baseX, baseY - tickOut, baseX, baseY - tickIn, tickW, tickCol); // N
        drawLine(baseX, baseY + tickIn, baseX, baseY + tickOut, tickW, tickCol); // S
        drawLine(baseX - tickOut, baseY, baseX - tickIn, baseY, tickW, tickCol); // W
        drawLine(baseX + tickIn, baseY, baseX + tickOut, baseY, tickW, tickCol); // E

        // Deadzone Center Target: Minimalist ring & center micro-dot
        drawRing(baseX, baseY, 13.0f * scale, 0.86f, rgba(0.50f, 0.70f, 1.0f, hudOp * 0.35f));
        drawCircle(baseX, baseY, 2.5f * scale, rgba(1.0f, 1.0f, 1.0f, hudOp * 0.60f));

        // Dynamic Luminous Laser Tether (Center to Knob deflection vector)
        float sDx = knobX - baseX;
        float sDy = knobY - baseY;
        float sDist = sqrtf(sDx * sDx + sDy * sDy);
        if (sDist > 5.0f * scale) {
            // Soft cyan outer glow tether
            drawLine(baseX, baseY, knobX, knobY, 3.2f * scale, rgba(0.22f, 0.74f, 0.97f, hudOp * 0.65f));
            // Crisp white core laser beam
            drawLine(baseX, baseY, knobX, knobY, 1.4f * scale, rgba(1.0f, 1.0f, 1.0f, hudOp * 0.85f));
        }

        // Floating Frosted Thumb Puck
        float kpR = stickActive ? (knobR * 1.04f) : knobR;
        // Knob base disc
        drawCircle(knobX, knobY, kpR, rgba(0.09f, 0.13f, 0.22f, hudOp * (stickActive ? 0.85f : 0.65f)));
        // Outer glowing rim
        Color knobRim = stickActive ? rgba(0.22f, 0.74f, 0.97f, hudOp * 0.95f) : rgba(0.60f, 0.80f, 1.0f, hudOp * 0.65f);
        drawRing(knobX, knobY, kpR, 0.90f, knobRim);
        // Inner concentric thumb groove ring
        drawRing(knobX, knobY, kpR * 0.68f, 0.93f, rgba(1.0f, 1.0f, 1.0f, hudOp * 0.35f));
        // Center tactile micro-dot
        drawCircle(knobX, knobY, 3.0f * scale, stickActive ? rgba(0.22f, 0.74f, 0.97f, hudOp * 0.95f) : rgba(1.0f, 1.0f, 1.0f, hudOp * 0.50f));

        // ---------------------------------------------------------
        // 2. Action Buttons - Frosted Glass with Signature Micro-Accents
        // ---------------------------------------------------------
        float btnBaseX = w - 160.0f * scale - cutoutInset + mSettings.buttonOffsetX;
        float btnBaseY = h - 160.0f * scale + mSettings.buttonOffsetY;

        auto drawModernButton = [&](float cx, float cy, float r, const char* label, uint32_t accentHex, bool pressed, int specialGlyph = 0) {
            float pr = pressed ? (r * 0.93f) : r;
            float baseAlpha = pressed ? (hudOp * 0.88f) : (hudOp * 0.36f);
            float borderAlpha = pressed ? (hudOp * 0.98f) : (hudOp * 0.65f);
            float ringAlpha = pressed ? (hudOp * 0.85f) : (hudOp * 0.25f);

            // Frosted dark glass base plate
            Color baseBg = pressed ? hexColor(accentHex, baseAlpha) : rgba(0.06f, 0.08f, 0.13f, baseAlpha);
            drawCircle(cx, cy, pr, baseBg);

            // Thin luminous perimeter ring (1.5px)
            Color outerRing = pressed ? rgba(1.0f, 1.0f, 1.0f, borderAlpha) : hexColor(accentHex, borderAlpha);
            drawRing(cx, cy, pr, 0.92f, outerRing);

            // Inner concentric frosted micro-ring
            drawRing(cx, cy, pr * 0.80f, 0.95f, rgba(1.0f, 1.0f, 1.0f, ringAlpha));

            // High-contrast clean glyph / typography
            Color glyphCol = pressed ? rgba(1.0f, 1.0f, 1.0f, hudOp * 1.0f) : rgba(0.92f, 0.95f, 1.0f, hudOp * 0.92f);

            if (specialGlyph == 1) { // SPIN: Procedural Cyclone glyph + clean micro-label
                float iconR = pr * 0.44f;
                drawCyclone(cx, cy - 5.0f * scale, iconR, glyphCol, 0.0f);
                drawText("SPIN", cx, cy + pr * 0.58f, 0.26f * scale, glyphCol, 1);
            } else if (specialGlyph == 2) { // PLUS (+): Modern precision cross
                float armLen = pr * 0.38f;
                float thick = 2.4f * scale;
                drawLine(cx - armLen, cy, cx + armLen, cy, thick, glyphCol);
                drawLine(cx, cy - armLen, cx, cy + armLen, thick, glyphCol);
            } else { // Standard typography (A, B, Z)
                float textSize = (r > 48.0f * scale) ? (0.64f * scale) : (0.54f * scale);
                drawText(label, cx, cy + 11.0f * scale, textSize, glyphCol, 1);
            }
        };

        // Button A (Jump) - Luminous Electric Cyan (#38BDF8)
        drawModernButton(btnBaseX, btnBaseY, 54.0f * scale, "A", 0x38BDF8, mBtnAPressed);

        // Button B (Star Bit Shoot) - Luminous Star Amber (#FBBF24)
        drawModernButton(btnBaseX - 120.0f * scale, btnBaseY + 25.0f * scale, 45.0f * scale, "B", 0xFBBF24, mBtnBPressed);

        // Button SPIN - Luminous Emerald Mint (#34D399) with Cyclone Glyph
        drawModernButton(btnBaseX + 35.0f * scale, btnBaseY - 130.0f * scale, 48.0f * scale, "SPIN", 0x34D399, mBtnSpinPressed, 1);

        // Button Z (Crouch) - Luminous Lavender Violet (#A78BFA)
        drawModernButton(btnBaseX - 90.0f * scale, btnBaseY - 105.0f * scale, 44.0f * scale, "Z", 0xA78BFA, mBtnZPressed);

        // Button + (Pause) - Minimalist Pearl Slate (#E2E8F0) with Vector Cross
        drawModernButton(w - 70.0f * scale - cutoutInset, 70.0f * scale, 34.0f * scale, "+", 0x94A3B8, mBtnPlusPressed, 2);

        // ---------------------------------------------------------
        // 3. Camera D-Pad - Directional Precision Disc with Chevrons & Camera Icon
        // ---------------------------------------------------------
        if (mSettings.showDpad) {
            float baseDpadX = mSettings.dpadOnRight ? (w - 380.0f * scale - cutoutInset) : (140.0f * scale + cutoutInset);
            float baseDpadY = mSettings.dpadOnRight ? (h - 220.0f * scale) : (h - 390.0f * scale);
            float dpadX = baseDpadX + mSettings.dpadOffsetX;
            float dpadY = baseDpadY + mSettings.dpadOffsetY;
            float dialR = 56.0f * scale;
            float armDist = 36.0f * scale;
            float chevronR = 14.0f * scale;

            // Frosted circular dial plate
            drawCircle(dpadX, dpadY, dialR, rgba(0.06f, 0.08f, 0.13f, hudOp * 0.32f));

            // Outer razor-thin precision ring
            drawRing(dpadX, dpadY, dialR, 0.95f, rgba(0.55f, 0.75f, 1.0f, hudOp * 0.42f));

            // Quadrant division hairlines (45 deg diagonals)
            float divInner = 20.0f * scale;
            float divOuter = dialR * 0.88f;
            float diag = 0.7071f;
            Color divCol = rgba(1.0f, 1.0f, 1.0f, hudOp * 0.16f);
            drawLine(dpadX + divInner * diag, dpadY + divInner * diag, dpadX + divOuter * diag, dpadY + divOuter * diag, 1.4f * scale, divCol);
            drawLine(dpadX - divInner * diag, dpadY + divInner * diag, dpadX - divOuter * diag, dpadY + divOuter * diag, 1.4f * scale, divCol);
            drawLine(dpadX + divInner * diag, dpadY - divInner * diag, dpadX + divOuter * diag, dpadY - divOuter * diag, 1.4f * scale, divCol);
            drawLine(dpadX - divInner * diag, dpadY - divInner * diag, dpadX - divOuter * diag, dpadY - divOuter * diag, 1.4f * scale, divCol);

            // 4 Directional Chevron Sectors (◀ ▶ ▲ ▼)
            auto drawDpadSector = [&](float cx, float cy, float rotAngle, bool pressed) {
                if (pressed) {
                    drawCircle(cx, cy, 20.0f * scale, rgba(0.22f, 0.74f, 0.97f, hudOp * 0.80f));
                    drawRing(cx, cy, 20.0f * scale, 0.88f, rgba(1.0f, 1.0f, 1.0f, hudOp * 0.95f));
                }
                Color chevCol = pressed ? rgba(1.0f, 1.0f, 1.0f, hudOp * 1.0f) : rgba(0.85f, 0.92f, 1.0f, hudOp * 0.75f);
                float chevSize = pressed ? (chevronR * 1.15f) : chevronR;
                drawChevron(cx, cy, chevSize, chevCol, rotAngle);
            };

            // Left (rot = PI)
            drawDpadSector(dpadX - armDist, dpadY, 3.14159265f, mBtnDpadLeft);
            // Right (rot = 0)
            drawDpadSector(dpadX + armDist, dpadY, 0.0f, mBtnDpadRight);
            // Up (rot = -PI/2)
            drawDpadSector(dpadX, dpadY - armDist, -1.5707963f, mBtnDpadUp);
            // Down (rot = PI/2)
            drawDpadSector(dpadX, dpadY + armDist, 1.5707963f, mBtnDpadDown);

            // Center Concave Hub with Modern Camera Icon (📷)
            float hubR = 18.0f * scale;
            drawCircle(dpadX, dpadY, hubR, rgba(0.09f, 0.13f, 0.22f, hudOp * 0.80f));
            drawRing(dpadX, dpadY, hubR, 0.88f, rgba(0.55f, 0.75f, 1.0f, hudOp * 0.55f));
            drawCameraIcon(dpadX, dpadY, 11.0f * scale, rgba(0.85f, 0.93f, 1.0f, hudOp * 0.90f));
        }
    }

    auto drawFpsPill = [&]() {
        float fpsX, fpsY, fpsW, fpsH, fpsBaseW;
        computeFpsRect(screenW, screenH, fpsX, fpsY, fpsW, fpsH, fpsBaseW);
        (void)fpsBaseW;
        float fpsScale = std::max(0.60f, std::min(2.20f, mSettings.fpsScale));
        float fs = baseDpi * fpsScale;

        char fpsTxt[32];
        if (mDisplayedFps >= 0) {
            snprintf(fpsTxt, sizeof(fpsTxt), "%d FPS", mDisplayedFps);
        } else {
            snprintf(fpsTxt, sizeof(fpsTxt), "-- FPS");
        }

        Color dotCol = rgba(0.45f, 0.65f, 0.85f, 0.90f);
        if (mDisplayedFps >= 55) {
            dotCol = rgba(0.290f, 0.871f, 0.502f, 1.0f); // Emerald green
        } else if (mDisplayedFps >= 35) {
            dotCol = rgba(0.984f, 0.749f, 0.141f, 1.0f); // Amber
        } else if (mDisplayedFps >= 0) {
            dotCol = rgba(0.960f, 0.360f, 0.360f, 1.0f); // Coral red
        }

        float pillR = fpsH * 0.5f;
        drawRoundRect(fpsX, fpsY, fpsX + fpsW, fpsY + fpsH, pillR, rgba(0.32f, 0.42f, 0.58f, 0.58f));
        float inset = 1.2f * baseDpi;
        drawRoundRect(fpsX + inset, fpsY + inset, fpsX + fpsW - inset, fpsY + fpsH - inset, std::max(1.0f, pillR - inset), rgba(0.06f, 0.08f, 0.12f, 0.84f));
        drawCircle(fpsX + 12.0f * fs, fpsY + fpsH * 0.5f, 4.0f * fs, dotCol);
        drawText(fpsTxt, fpsX + 22.0f * fs, fpsY + 19.2f * fs, 0.34f * fs, rgba(0.96f, 0.97f, 1.0f, 0.98f), 0);
    };

    // -------------------------------------------------------------
    // Performance Overlay (FPS Counter)
    // -------------------------------------------------------------
    if (mSettings.showFpsCounter && !mEditLayoutMode) {
        drawFpsPill();
    }

    // -------------------------------------------------------------
    // Cosmic Observatory Settings Dock (Pinned to Top-Left Edge)
    // -------------------------------------------------------------
    if (mDrawerAnim < 0.95f) {
        float tabW = 56.0f * baseDpi;
        float tabH = 48.0f * baseDpi;
        float tabY = 22.0f * baseDpi;
        float tabPr = mBtnCfgPressed ? 0.92f : 1.0f;

        // Docked cosmic pill container with cyan outer rim
        drawRoundRect(-10.0f, tabY, tabW * tabPr, tabY + tabH, 24.0f * baseDpi, rgba(0.220f, 0.760f, 0.980f, 0.75f));
        drawRoundRect(-10.0f, tabY + 1.4f * baseDpi, tabW * tabPr - 1.4f * baseDpi, tabY + tabH - 1.4f * baseDpi, 22.6f * baseDpi, rgba(0.055f, 0.095f, 0.245f, 0.94f));

        // Rotating Cogwheel icon in starlight cyan
        drawCogwheel(tabW * 0.50f, tabY + tabH * 0.5f, 15.0f * baseDpi, rgba(0.55f, 0.88f, 1.0f, 1.0f), cogRot);
    }

    // -------------------------------------------------------------
    // Draw Edit Layout Overlay
    // -------------------------------------------------------------
    if (mEditLayoutMode) {
        // Scrim
        drawRoundRect(0, 0, w, h, 0.0f, rgba(0.0f, 0.0f, 0.0f, 0.45f));

        // M3 Top bar card
        drawRoundRect(w * 0.5f - 240.0f, 30.0f, w * 0.5f + 240.0f, 95.0f, 20.0f, rgba(0.12f, 0.14f, 0.18f, 0.96f));
        drawText("DRAG TO MOVE • CORNER TO RESIZE FPS", w * 0.5f, 55.0f, 0.40f, rgba(0.90f, 0.92f, 0.96f, 1.0f), 1);

        // Save button (M3 Primary Pill)
        float saveBtnX = w * 0.5f - 110.0f;
        drawRoundRect(saveBtnX, 65.0f, saveBtnX + 100.0f, 90.0f, 12.0f, rgba(0.043f, 0.341f, 0.816f, 0.95f));
        drawText("SAVE", saveBtnX + 50.0f, 83.0f, 0.42f, rgba(1.0f, 1.0f, 1.0f, 1.0f), 1);

        // Reset button (M3 Outline Pill)
        float rstBtnX = w * 0.5f + 10.0f;
        drawRoundRect(rstBtnX, 65.0f, rstBtnX + 100.0f, 90.0f, 12.0f, rgba(0.35f, 0.18f, 0.20f, 0.95f));
        drawText("RESET", rstBtnX + 50.0f, 83.0f, 0.42f, rgba(1.0f, 1.0f, 1.0f, 1.0f), 1);

        if (mSettings.showControls) {
            // Stick cluster bounding outline
            float defaultStickX = 175.0f * scale + mSettings.stickOffsetX;
            float defaultStickY = h - 175.0f * scale + mSettings.stickOffsetY;
            drawRing(defaultStickX, defaultStickY, 120.0f * scale, 0.96f, rgba(0.40f, 0.65f, 0.98f, 0.85f));
            drawText("JOYSTICK", defaultStickX, defaultStickY - 130.0f * scale, 0.45f, rgba(0.40f, 0.65f, 0.98f, 0.95f), 1);

            // Button cluster bounding outline
            float btnBaseX = w - 160.0f * scale + mSettings.buttonOffsetX;
            float btnBaseY = h - 160.0f * scale + mSettings.buttonOffsetY;
            drawRing(btnBaseX - 60.0f * scale, btnBaseY - 60.0f * scale, 160.0f * scale, 0.96f, rgba(0.40f, 0.65f, 0.98f, 0.85f));
            drawText("BUTTONS", btnBaseX - 60.0f * scale, btnBaseY - 230.0f * scale, 0.45f, rgba(0.40f, 0.65f, 0.98f, 0.95f), 1);

            // Camera D-Pad bounding outline
            if (mSettings.showDpad) {
                float baseDpadX = mSettings.dpadOnRight ? (w - 380.0f * scale) : (140.0f * scale);
                float baseDpadY = mSettings.dpadOnRight ? (h - 220.0f * scale) : (h - 390.0f * scale);
                float curDpadX = baseDpadX + mSettings.dpadOffsetX;
                float curDpadY = baseDpadY + mSettings.dpadOffsetY;
                drawRing(curDpadX, curDpadY, 78.0f * scale, 0.96f, rgba(0.40f, 0.65f, 0.98f, 0.85f));
                drawText("CAMERA D-PAD", curDpadX, curDpadY - 88.0f * scale, 0.42f, rgba(0.40f, 0.65f, 0.98f, 0.95f), 1);
            }
        }

        if (mSettings.showFpsCounter) {
            drawFpsPill();
            float fpsX, fpsY, fpsW, fpsH, fpsBaseW;
            computeFpsRect(screenW, screenH, fpsX, fpsY, fpsW, fpsH, fpsBaseW);
            (void)fpsBaseW;

            float pad = 4.0f * baseDpi;
            Color boxCol = rgba(0.220f, 0.741f, 0.969f, 0.95f);
            float x0 = fpsX - pad, y0 = fpsY - pad, x1 = fpsX + fpsW + pad, y1 = fpsY + fpsH + pad;
            float stroke = 2.0f * baseDpi;
            drawLine(x0, y0, x1, y0, stroke, boxCol);
            drawLine(x1, y0, x1, y1, stroke, boxCol);
            drawLine(x1, y1, x0, y1, stroke, boxCol);
            drawLine(x0, y1, x0, y0, stroke, boxCol);

            // Bottom-right circular resize handle
            float hx = fpsX + fpsW + 2.0f * baseDpi;
            float hy = fpsY + fpsH + 2.0f * baseDpi;
            drawCircle(hx, hy, 11.0f * baseDpi, boxCol);
            drawCircle(hx, hy, 5.0f * baseDpi, rgba(1.0f, 1.0f, 1.0f, 1.0f));

            char fpsTag[32];
            int pct = (int)lroundf(mSettings.fpsScale * 100.0f);
            snprintf(fpsTag, sizeof(fpsTag), "FPS (%d%%)", pct);
            float labelY = (fpsY > 36.0f * baseDpi) ? (y0 - 8.0f * baseDpi) : (y1 + 20.0f * baseDpi);
            drawText(fpsTag, fpsX + fpsW * 0.5f, labelY, 0.34f * baseDpi, boxCol, 1);
        }
    }

    // -------------------------------------------------------------
    // Full-Screen Super Mario Galaxy "Comet Observatory" Settings UI
    // -------------------------------------------------------------
    if (mDrawerAnim > 0.02f) {
        float oa = std::max(0.0f, std::min(1.0f, mDrawerAnim));
        float slideY = (1.0f - oa) * 24.0f * uiScale;
        auto sc = [&](Color c) { c.a *= oa; return c; };

        // 1. Deep Cosmic Sky Gradient & Observatory Nebula Backdrop
        addGradRect(0.0f, 0.0f, w, h * 0.56f,
                    rgba(0.016f, 0.028f, 0.102f, 0.96f * oa),
                    rgba(0.043f, 0.086f, 0.258f, 0.96f * oa));
        addGradRect(0.0f, h * 0.56f, w, h,
                    rgba(0.043f, 0.086f, 0.258f, 0.96f * oa),
                    rgba(0.075f, 0.110f, 0.305f, 0.97f * oa));

        // Subtle nebula bloom & Comet Observatory orbital arcs
        float orbCx = w * 0.78f;
        float orbCy = h * 0.26f;
        drawCircle(orbCx, orbCy, h * 0.44f, rgba(0.11f, 0.36f, 0.85f, 0.09f * oa));
        drawCircle(w * 0.34f, h * 0.82f, h * 0.38f, rgba(0.30f, 0.14f, 0.65f, 0.08f * oa));
        drawRing(orbCx, orbCy, h * 0.42f, 0.992f, rgba(0.28f, 0.72f, 0.99f, 0.13f * oa));
        drawRing(orbCx, orbCy, h * 0.62f, 0.994f, rgba(0.98f, 0.82f, 0.28f, 0.08f * oa));

        // Deterministic twinkling starfield & 4-point Luma sparkles
        for (int s = 0; s < 36; s++) {
            float sx = fmodf((float)((s * 173 + 47) % 1009) / 1009.0f * 1.13f, 1.0f) * w;
            float sy = fmodf((float)((s * 293 + 91) % 1013) / 1013.0f * 1.07f, 1.0f) * h;
            float tw = 0.45f + 0.55f * (0.5f + 0.5f * sinf(animPhase * (1.8f + (s % 5) * 0.55f) + (float)s * 1.7f));
            bool goldStar = ((s % 5) == 0);
            bool cyanStar = ((s % 3) == 0);
            Color starCol = goldStar ? rgba(1.0f, 0.88f, 0.36f, 0.65f * tw * oa)
                          : (cyanStar ? rgba(0.55f, 0.88f, 1.0f, 0.60f * tw * oa)
                                      : rgba(0.95f, 0.97f, 1.0f, 0.55f * tw * oa));
            if ((s % 4) == 0) {
                drawSparkle(sx, sy, (4.2f + (s % 3) * 1.6f) * uiScale, starCol, animPhase * 0.4f + s);
            } else {
                drawCircle(sx, sy, (1.1f + (s % 3) * 0.65f) * uiScale, starCol);
            }
        }

        // SMG Palette & Chrome Geometry
        float railX0 = 18.0f * uiScale;
        float railW = 224.0f * uiScale;
        float railX1 = railX0 + railW;
        float railTopY = sideTopY;
        float railBotY = h - 16.0f * uiScale;

        mDeckLeftX = railX1 + 14.0f * uiScale;
        mDeckTopY = headerH + 14.0f * uiScale;
        mDeckBottomY = h - 16.0f * uiScale;
        float deckX0 = mDeckLeftX;
        float deckX1 = w - 20.0f * uiScale;
        float deckW = std::max(280.0f * uiScale, deckX1 - deckX0);
        float colGap = 14.0f * uiScale;
        bool twoCol = (deckW >= 620.0f * uiScale);
        float colW = twoCol ? ((deckW - colGap) * 0.5f) : deckW;
        float visibleDeckH = std::max(1.0f, mDeckBottomY - mDeckTopY);

        Color cCardBg = sc(rgba(0.055f, 0.098f, 0.255f, 0.93f));
        Color cCardBgAlt = sc(rgba(0.072f, 0.122f, 0.305f, 0.94f));
        Color cCardBorder = sc(rgba(0.28f, 0.64f, 0.95f, 0.48f));
        Color cCardBorderGold = sc(rgba(0.98f, 0.80f, 0.26f, 0.60f));
        Color cDivider = sc(rgba(0.32f, 0.52f, 0.85f, 0.30f));
        Color cHeader = sc(rgba(0.30f, 0.84f, 1.0f, 1.0f));
        Color cGold = sc(rgba(0.99f, 0.84f, 0.26f, 1.0f));
        Color cTextPrimary = sc(rgba(0.96f, 0.98f, 1.0f, 1.0f));
        Color cTextSecondary = sc(rgba(0.73f, 0.82f, 0.96f, 0.86f));
        float swW = 50.0f * uiScale, swH = 28.0f * uiScale;

        auto pushFixedHit = [&](float x0, float y0, float x1, float y1, int kind, int arg0 = 0, int arg1 = 0, float p0 = 0.0f, float p1 = 0.0f) {
            if (x1 <= x0 || y1 <= y0) return;
            mSettingsHits.push_back({x0, y0, x1, y1, kind, arg0, arg1, p0, p1, true});
        };
        auto pushDeckHit = [&](float x0, float y0, float x1, float y1, int kind, int arg0 = 0, int arg1 = 0, float p0 = 0.0f, float p1 = 0.0f) {
            float cy0 = std::max(mDeckTopY, y0);
            float cy1 = std::min(mDeckBottomY, y1);
            if (cy1 <= cy0 || x1 <= x0) return;
            mSettingsHits.push_back({x0, cy0, x1, cy1, kind, arg0, arg1, p0, p1, false});
        };

        auto drawSectionTitle = [&](float x0, float y, const char* title, const Color& accent, bool starGlyph = true) {
            if (starGlyph) {
                drawStar(x0 + 17.0f * uiScale, y + 18.0f * uiScale, 6.8f * uiScale, accent, sinf(animPhase * 1.4f) * 0.14f);
            } else {
                drawSparkle(x0 + 17.0f * uiScale, y + 18.0f * uiScale, 6.2f * uiScale, accent, animPhase * 0.6f);
            }
            drawText(title, x0 + 29.0f * uiScale, y + 22.0f * uiScale, 0.31f * uiScale, accent, 0);
        };

        auto drawGlossyBtn = [&](float x0, float y0, float x1, float y1, float r, const Color& rim, const Color& fill, const char* label, const Color& txtCol, float txtScale = 0.33f) {
            drawRoundRect(x0, y0, x1, y1, r, sc(rim));
            float inset = 1.4f * uiScale;
            drawRoundRect(x0 + inset, y0 + inset, x1 - inset, y1 - inset, std::max(1.0f, r - inset), sc(fill));
            if (y1 - y0 > 18.0f * uiScale) {
                drawRoundRect(x0 + 6.0f * uiScale, y0 + 2.2f * uiScale, x1 - 6.0f * uiScale, y0 + (y1 - y0) * 0.44f, std::max(1.0f, r * 0.55f), rgba(1.0f, 1.0f, 1.0f, 0.12f * oa));
            }
            drawText(label, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f + 5.0f * uiScale, txtScale * uiScale, sc(txtCol), 1);
        };

        // ---------------------------------------------------------
        // A. Scrollable Right Observatory Content Deck (Rendered BEFORE Fixed Chrome)
        // ---------------------------------------------------------
        if (mWarpMenuOpen) {
            int matchingIndices[64];
            int matchingCount = 0;
            for (int i = 0; i < kNumGalaxies && matchingCount < 64; i++) {
                if (mWarpCategoryIndex == 0 || kGalaxies[i].category == mWarpCategoryIndex) {
                    matchingIndices[matchingCount++] = i;
                }
            }
            int warpCols = (deckW >= 600.0f * uiScale) ? 2 : 1;
            float cardH = 78.0f * uiScale;
            float rowGap = 10.0f * uiScale;
            float rowStep = cardH + rowGap;
            int rows = (matchingCount + warpCols - 1) / warpCols;
            float totalWarpH = rows * rowStep + 8.0f * uiScale;
            mWarpMaxScroll = std::max(0.0f, totalWarpH - visibleDeckH);

            if (fabsf(mWarpScrollY - mWarpScrollTarget) > 0.5f) {
                mWarpScrollY += (mWarpScrollTarget - mWarpScrollY) * (1.0f - expf(-18.0f * dt));
                mDirty = true;
            } else {
                mWarpScrollY = mWarpScrollTarget;
            }
            mWarpScrollY = std::max(0.0f, std::min(mWarpScrollY, mWarpMaxScroll));
            mWarpScrollTarget = std::max(0.0f, std::min(mWarpScrollTarget, mWarpMaxScroll));

            float wCardW = (warpCols == 2) ? ((deckW - colGap - 10.0f * uiScale) * 0.5f) : (deckW - 10.0f * uiScale);
            for (int m = 0; m < matchingCount; m++) {
                int i = matchingIndices[m];
                int row = m / warpCols;
                int col = m % warpCols;
                float cardX0 = deckX0 + col * (wCardW + colGap);
                float cardX1 = cardX0 + wCardW;
                float cardY = mDeckTopY + slideY + row * rowStep - mWarpScrollY;

                if (cardY + cardH >= mDeckTopY - 24.0f && cardY <= mDeckBottomY + 24.0f) {
                    bool isSel = (mWarpSelectedGalaxy == i);
                    Color bg = isSel ? sc(rgba(0.09f, 0.22f, 0.48f, 0.96f)) : cCardBg;
                    Color brd = isSel ? cGold : cCardBorder;
                    drawCard(cardX0, cardY, cardX1, cardY + cardH, 16.0f * uiScale, bg, brd);
                    pushDeckHit(cardX0, cardY, cardX1, cardY + cardH, HIT_WARP_CARD, i);

                    drawStar(cardX0 + 18.0f * uiScale, cardY + 21.0f * uiScale, 6.2f * uiScale, isSel ? cGold : cHeader, isSel ? animPhase * 0.8f : 0.0f);
                    drawText(kGalaxies[i].displayName, cardX0 + 30.0f * uiScale, cardY + 25.0f * uiScale, 0.35f * uiScale, cTextPrimary, 0);

                    const char* catName = kCategoryNames[kGalaxies[i].category];
                    drawText(catName, cardX1 - 14.0f * uiScale, cardY + 23.0f * uiScale, 0.26f * uiScale, isSel ? cGold : rgba(0.62f, 0.78f, 0.96f, 0.85f * oa), 2);

                    int maxSc = kGalaxies[i].maxScenarios;
                    float pillW = 28.0f * uiScale;
                    float pillGap = 5.0f * uiScale;
                    float pStartX = cardX0 + 14.0f * uiScale;
                    float pY = cardY + 40.0f * uiScale;
                    float pH = 27.0f * uiScale;

                    for (int s = 1; s <= maxSc; s++) {
                        float px0 = pStartX + (s - 1) * (pillW + pillGap);
                        float px1 = px0 + pillW;
                        bool sActive = isSel && (mWarpSelectedScenario == s);
                        Color pRim = sActive ? rgba(1.0f, 0.86f, 0.32f, 0.98f) : rgba(0.30f, 0.50f, 0.82f, 0.45f);
                        Color pBg = sActive ? rgba(0.08f, 0.42f, 0.92f, 1.0f) : rgba(0.09f, 0.14f, 0.32f, 0.92f);
                        char sBuf[8];
                        snprintf(sBuf, sizeof(sBuf), "%d", s);
                        drawGlossyBtn(px0, pY, px1, pY + pH, 9.0f * uiScale, pRim, pBg, sBuf, sActive ? rgba(1.0f, 0.96f, 0.78f, 1.0f) : rgba(0.78f, 0.86f, 0.98f, 0.90f), 0.31f);
                        pushDeckHit(px0, pY, px1, pY + pH, HIT_WARP_SCENARIO, i, s);
                    }

                    float qWarpX0 = cardX1 - 78.0f * uiScale;
                    float qWarpX1 = cardX1 - 12.0f * uiScale;
                    Color qRim = isSel ? rgba(0.45f, 0.96f, 0.65f, 0.95f) : rgba(0.32f, 0.68f, 0.95f, 0.55f);
                    Color qBg = isSel ? rgba(0.11f, 0.52f, 0.28f, 0.98f) : rgba(0.10f, 0.22f, 0.45f, 0.90f);
                    drawGlossyBtn(qWarpX0, pY, qWarpX1, pY + pH, 9.0f * uiScale, qRim, qBg, "WARP", isSel ? rgba(0.92f, 1.0f, 0.94f, 1.0f) : rgba(0.78f, 0.90f, 1.0f, 0.92f), 0.29f);
                    pushDeckHit(qWarpX0, pY, qWarpX1, pY + pH, HIT_WARP_QUICK, i);
                }
            }

            if (mWarpMaxScroll > 1.0f) {
                float thumbH = std::max(28.0f * uiScale, visibleDeckH * (visibleDeckH / totalWarpH));
                float thumbY = mDeckTopY + (mWarpScrollY / mWarpMaxScroll) * (visibleDeckH - thumbH);
                drawRoundRect(deckX1 - 4.0f * uiScale, thumbY, deckX1, thumbY + thumbH, 2.0f * uiScale, rgba(0.45f, 0.85f, 1.0f, 0.48f * oa));
            }
        } else {
            // Smooth deck scroll clamping using latest known max scroll
            mDrawerScrollY = std::max(0.0f, std::min(mDrawerScrollY, mSettingsMaxScroll));
            mDrawerScrollTarget = std::max(0.0f, std::min(mDrawerScrollTarget, mSettingsMaxScroll));
            if (fabsf(mDrawerScrollY - mDrawerScrollTarget) > 0.5f) {
                mDrawerScrollY += (mDrawerScrollTarget - mDrawerScrollY) * (1.0f - expf(-18.0f * dt));
                mDirty = true;
            } else {
                mDrawerScrollY = mDrawerScrollTarget;
            }

            float colY[2] = { 0.0f, 0.0f };
            float cardGap = 12.0f * uiScale;
            float scrollAreaW = deckW - (mSettingsMaxScroll > 1.0f ? 8.0f * uiScale : 0.0f);
            float effColW = twoCol ? ((scrollAreaW - colGap) * 0.5f) : scrollAreaW;

            auto placeCard = [&](int prefCol, float cardH, auto func) {
                int col = twoCol ? prefCol : 0;
                float cX0 = deckX0 + col * (effColW + colGap);
                float cX1 = cX0 + effColW;
                float y = mDeckTopY + slideY + colY[col] - mDrawerScrollY;
                colY[col] += cardH + cardGap;
                if (y + cardH >= mDeckTopY - 24.0f && y <= mDeckBottomY + 24.0f) {
                    func(cX0, cX1, effColW, y, cardH);
                }
            };

            auto drawLangSliderCard = [&](float cX0, float cX1, float cw, float y, float ch) {
                static const char* kLangNames[7] = {
                    "English", "Espanol", "Francais", "Deutsch", "Italiano", "Nederlands", "Japanese"
                };
                static const char* kLangCodes[7] = {
                    "EN", "ES", "FR", "DE", "IT", "NL", "JP"
                };
                drawCard(cX0, y, cX1, y + ch, 18.0f * uiScale, cCardBg, cCardBorder);
                drawSectionTitle(cX0, y, "GAME LANGUAGE", cHeader, false);

                int curLang = std::max(0, std::min(6, mSettings.languageIndex));
                drawText(kLangNames[curLang], cX1 - 14.0f * uiScale, y + 22.0f * uiScale, 0.32f * uiScale, cGold, 2);

                float trackX0 = cX0 + 24.0f * uiScale;
                float trackX1 = cX1 - 24.0f * uiScale;
                float trackY = y + 54.0f * uiScale;
                float frac = (float)curLang / 6.0f;
                float knobX = trackX0 + frac * (trackX1 - trackX0);

                drawRoundRect(trackX0, trackY - 3.5f * uiScale, trackX1, trackY + 3.5f * uiScale, 3.5f * uiScale, sc(rgba(0.10f, 0.16f, 0.36f, 0.95f)));
                if (knobX > trackX0 + 1.0f) {
                    drawRoundRect(trackX0, trackY - 3.5f * uiScale, knobX, trackY + 3.5f * uiScale, 3.5f * uiScale, sc(rgba(0.14f, 0.54f, 0.98f, 1.0f)));
                }
                for (int l = 0; l < 7; l++) {
                    float stepX = trackX0 + ((float)l / 6.0f) * (trackX1 - trackX0);
                    bool isCur = (l == curLang);
                    Color dotCol = (l <= curLang) ? sc(rgba(0.68f, 0.94f, 1.0f, 0.98f)) : sc(rgba(0.36f, 0.48f, 0.72f, 0.75f));
                    drawCircle(stepX, trackY, 3.2f * uiScale, dotCol);
                    drawText(kLangCodes[l], stepX, y + 84.0f * uiScale, (isCur ? 0.29f : 0.25f) * uiScale, isCur ? cTextPrimary : cTextSecondary, 1);
                }
                drawCircle(knobX, trackY, 11.0f * uiScale, sc(rgba(0.22f, 0.80f, 1.0f, 0.98f)));
                drawCircle(knobX, trackY, 6.0f * uiScale, sc(rgba(1.0f, 0.98f, 0.88f, 1.0f)));
                pushDeckHit(cX0 + 8.0f * uiScale, y + 32.0f * uiScale, cX1 - 8.0f * uiScale, y + ch - 4.0f * uiScale, HIT_SLIDER_LANGUAGE, 0, 0, trackX0, trackX1);
                (void)cw;
            };

            auto drawHapticsCard = [&](float cX0, float cX1, float cw, float y, float ch) {
                drawCard(cX0, y, cX1, y + ch, 18.0f * uiScale, cCardBg, cCardBorder);
                drawSectionTitle(cX0, y, "VIBRATION & HAPTICS", cHeader, false);
                float r1Y = y + 34.0f * uiScale;
                drawText("Tactile Haptic Feedback", cX0 + 14.0f * uiScale, r1Y + 18.0f * uiScale, 0.36f * uiScale, cTextPrimary, 0);
                drawText("Vibration pulse on buttons & UI clicks", cX0 + 14.0f * uiScale, r1Y + 34.0f * uiScale, 0.25f * uiScale, cTextSecondary, 0);
                drawM3Switch(cX1 - swW - 14.0f * uiScale, r1Y + 8.0f * uiScale, swW, swH, mSwAnimHaptics);
                pushDeckHit(cX0 + 8.0f * uiScale, r1Y, cX1 - 8.0f * uiScale, y + ch - 4.0f * uiScale, HIT_TOGGLE_HAPTICS);
                (void)cw;
            };

            auto drawDonateAndLogCards = [&](int col) {
                placeCard(col, 126.0f * uiScale, [&](float cX0, float cX1, float cw, float y, float ch) {
                    drawCard(cX0, y, cX1, y + ch, 18.0f * uiScale, cCardBg, cCardBorder);
                    drawSectionTitle(cX0, y, "SAVE DATA BACKUP & TRANSFER", cHeader, true);
                    bool hasSave = (port_has_save_data() != 0);
                    if (hasSave) {
                        drawStatusChip(cX0 + 14.0f * uiScale, y + 34.0f * uiScale, "Save Active (GameData.bin)", sc(rgba(0.10f, 0.34f, 0.22f, 0.90f)), sc(rgba(0.55f, 0.96f, 0.65f, 1.0f)), sc(rgba(0.290f, 0.871f, 0.502f, 1.0f)));
                    } else {
                        drawStatusChip(cX0 + 14.0f * uiScale, y + 34.0f * uiScale, "No Save Found Yet", sc(rgba(0.36f, 0.28f, 0.10f, 0.90f)), sc(rgba(1.0f, 0.88f, 0.42f, 1.0f)), cGold);
                    }
                    drawText("Back up or transfer your Stars across devices (.zip / .bin)", cX0 + 14.0f * uiScale, y + 71.0f * uiScale, 0.24f * uiScale, cTextSecondary, 0);
                    float gap = 10.0f * uiScale;
                    float btnW = (cw - 28.0f * uiScale - gap) * 0.5f;
                    float btnY = y + 81.0f * uiScale;
                    float btnX0 = cX0 + 14.0f * uiScale;
                    float btnX1 = btnX0 + btnW + gap;
                    drawGlossyBtn(btnX0, btnY, btnX0 + btnW, btnY + 35.0f * uiScale, 13.0f * uiScale,
                                  rgba(0.30f, 0.80f, 0.99f, 0.90f), rgba(0.06f, 0.38f, 0.86f, 0.98f),
                                  "EXPORT SAVE", rgba(1.0f, 1.0f, 1.0f, 1.0f), 0.29f);
                    pushDeckHit(btnX0, btnY, btnX0 + btnW, btnY + 35.0f * uiScale, HIT_EXPORT_SAVE);
                    drawGlossyBtn(btnX1, btnY, btnX1 + btnW, btnY + 35.0f * uiScale, 13.0f * uiScale,
                                  rgba(1.0f, 0.86f, 0.30f, 0.92f), rgba(0.76f, 0.48f, 0.04f, 0.98f),
                                  "IMPORT SAVE", rgba(1.0f, 0.99f, 0.90f, 1.0f), 0.29f);
                    pushDeckHit(btnX1, btnY, btnX1 + btnW, btnY + 35.0f * uiScale, HIT_IMPORT_SAVE);
                });

                placeCard(col, 118.0f * uiScale, [&](float cX0, float cX1, float cw, float y, float ch) {
                    drawCard(cX0, y, cX1, y + ch, 18.0f * uiScale, cCardBg, sc(rgba(0.95f, 0.42f, 0.46f, 0.55f)));
                    drawSectionTitle(cX0, y, "SUPPORT & DONATIONS", sc(rgba(0.98f, 0.48f, 0.52f, 1.0f)), true);
                    drawText("Support GalaxyMobile development on Ko-fi!", cX0 + 14.0f * uiScale, y + 40.0f * uiScale, 0.26f * uiScale, cTextSecondary, 0);
                    float btnY = y + 56.0f * uiScale;
                    drawGlossyBtn(cX0 + 14.0f * uiScale, btnY, cX1 - 14.0f * uiScale, btnY + 44.0f * uiScale, 15.0f * uiScale,
                                  rgba(1.0f, 0.62f, 0.66f, 0.85f), rgba(0.84f, 0.22f, 0.28f, 0.96f),
                                  "DONATE ON KO-FI (ko-fi.com/sebsies)", rgba(1.0f, 1.0f, 1.0f, 1.0f), 0.31f);
                    pushDeckHit(cX0 + 14.0f * uiScale, btnY, cX1 - 14.0f * uiScale, btnY + 44.0f * uiScale, HIT_DONATE);
                    (void)cw;
                });

                placeCard(col, 118.0f * uiScale, [&](float cX0, float cX1, float cw, float y, float ch) {
                    drawCard(cX0, y, cX1, y + ch, 18.0f * uiScale, cCardBg, cCardBorder);
                    drawSectionTitle(cX0, y, "DEBUG & DIAGNOSTICS", cHeader, false);
                    drawText("Export engine log & device info for troubleshooting", cX0 + 14.0f * uiScale, y + 40.0f * uiScale, 0.25f * uiScale, cTextSecondary, 0);
                    float btnY = y + 56.0f * uiScale;
                    drawGlossyBtn(cX0 + 14.0f * uiScale, btnY, cX1 - 14.0f * uiScale, btnY + 44.0f * uiScale, 15.0f * uiScale,
                                  rgba(0.30f, 0.78f, 0.98f, 0.85f), rgba(0.06f, 0.38f, 0.86f, 0.96f),
                                  "SHARE DEBUG LOG", rgba(1.0f, 1.0f, 1.0f, 1.0f), 0.32f);
                    pushDeckHit(cX0 + 14.0f * uiScale, btnY, cX1 - 14.0f * uiScale, btnY + 44.0f * uiScale, HIT_SHARE_LOG);
                    (void)cw;
                });
            };

            if (mSettingsTab == 0) {
                // =====================================================
                // Tab 0: Controls
                // =====================================================
                placeCard(0, 244.0f * uiScale, [&](float cX0, float cX1, float cw, float y, float ch) {
                    drawCard(cX0, y, cX1, y + ch, 18.0f * uiScale, cCardBg, cCardBorder);
                    drawSectionTitle(cX0, y, "ON-SCREEN CONTROLS (HUD)", cHeader, true);

                    float r1Y = y + 34.0f * uiScale;
                    drawText("Show Controls", cX0 + 14.0f * uiScale, r1Y + 18.0f * uiScale, 0.36f * uiScale, cTextPrimary, 0);
                    drawText("Display virtual thumbstick & buttons", cX0 + 14.0f * uiScale, r1Y + 34.0f * uiScale, 0.25f * uiScale, cTextSecondary, 0);
                    drawM3Switch(cX1 - swW - 14.0f * uiScale, r1Y + 8.0f * uiScale, swW, swH, mSwAnimShowControls);
                    pushDeckHit(cX0 + 8.0f * uiScale, r1Y, cX1 - 8.0f * uiScale, r1Y + 44.0f * uiScale, HIT_TOGGLE_SHOW_CONTROLS);

                    drawRoundRect(cX0 + 14.0f * uiScale, y + 80.0f * uiScale, cX1 - 14.0f * uiScale, y + 81.0f * uiScale, 0.5f, cDivider);

                    float r2Y = y + 82.0f * uiScale;
                    drawText("Camera D-Pad", cX0 + 14.0f * uiScale, r2Y + 18.0f * uiScale, 0.36f * uiScale, cTextPrimary, 0);
                    drawText("Directional camera rotation dial", cX0 + 14.0f * uiScale, r2Y + 34.0f * uiScale, 0.25f * uiScale, cTextSecondary, 0);
                    drawM3Switch(cX1 - swW - 14.0f * uiScale, r2Y + 8.0f * uiScale, swW, swH, mSwAnimShowDpad);
                    pushDeckHit(cX0 + 8.0f * uiScale, r2Y, cX1 - 8.0f * uiScale, r2Y + 44.0f * uiScale, HIT_TOGGLE_SHOW_DPAD);

                    drawRoundRect(cX0 + 14.0f * uiScale, y + 128.0f * uiScale, cX1 - 14.0f * uiScale, y + 129.0f * uiScale, 0.5f, cDivider);

                    float r3Y = y + 130.0f * uiScale;
                    drawText("D-Pad Placement", cX0 + 14.0f * uiScale, r3Y + 14.0f * uiScale, 0.32f * uiScale, cTextPrimary, 0);
                    const char* dpadSideOpts[2] = {"Left Hand", "Right Hand"};
                    float grpW = cw - 28.0f * uiScale;
                    float segY0 = r3Y + 18.0f * uiScale;
                    drawSegmentedGroup(cX0 + 14.0f * uiScale, segY0, grpW, 30.0f * uiScale, dpadSideOpts, 2, mSettings.dpadOnRight ? 1 : 0);
                    for (int s = 0; s < 2; s++) {
                        float sx0 = cX0 + 14.0f * uiScale + s * (grpW * 0.5f);
                        pushDeckHit(sx0, segY0 - 4.0f * uiScale, sx0 + grpW * 0.5f, segY0 + 34.0f * uiScale, HIT_SEG_DPAD_SIDE, s);
                    }

                    drawRoundRect(cX0 + 14.0f * uiScale, y + 184.0f * uiScale, cX1 - 14.0f * uiScale, y + 185.0f * uiScale, 0.5f, cDivider);

                    float r4Y = y + 186.0f * uiScale;
                    drawText("Joystick Placement", cX0 + 14.0f * uiScale, r4Y + 14.0f * uiScale, 0.32f * uiScale, cTextPrimary, 0);
                    const char* joyOpts[2] = {"Dynamic (Touch)", "Fixed Anchor"};
                    float joyY0 = r4Y + 18.0f * uiScale;
                    drawSegmentedGroup(cX0 + 14.0f * uiScale, joyY0, grpW, 30.0f * uiScale, joyOpts, 2, mSettings.dynamicStick ? 0 : 1);
                    for (int s = 0; s < 2; s++) {
                        float sx0 = cX0 + 14.0f * uiScale + s * (grpW * 0.5f);
                        pushDeckHit(sx0, joyY0 - 4.0f * uiScale, sx0 + grpW * 0.5f, joyY0 + 34.0f * uiScale, HIT_SEG_STICK_MODE, s);
                    }
                });

                placeCard(0, 186.0f * uiScale, [&](float cX0, float cX1, float cw, float y, float ch) {
                    drawCard(cX0, y, cX1, y + ch, 18.0f * uiScale, cCardBg, cCardBorder);
                    drawSectionTitle(cX0, y, "CONTROLLER (GAMEPAD & POINTER)", cHeader, false);

                    float grpW = cw - 28.0f * uiScale;
                    float r1Y = y + 32.0f * uiScale;
                    drawText("Right Joystick Mode", cX0 + 14.0f * uiScale, r1Y + 14.0f * uiScale, 0.32f * uiScale, cTextPrimary, 0);
                    const char* rStickOpts[2] = {"Star Pointer", "Camera"};
                    float rsY0 = r1Y + 18.0f * uiScale;
                    drawSegmentedGroup(cX0 + 14.0f * uiScale, rsY0, grpW, 30.0f * uiScale, rStickOpts, 2, mSettings.rightStickPointer ? 0 : 1);
                    for (int s = 0; s < 2; s++) {
                        float sx0 = cX0 + 14.0f * uiScale + s * (grpW * 0.5f);
                        pushDeckHit(sx0, rsY0 - 4.0f * uiScale, sx0 + grpW * 0.5f, rsY0 + 34.0f * uiScale, HIT_SEG_RIGHT_STICK_MODE, s);
                    }

                    drawRoundRect(cX0 + 14.0f * uiScale, y + 88.0f * uiScale, cX1 - 14.0f * uiScale, y + 89.0f * uiScale, 0.5f, cDivider);

                    float r2Y = y + 92.0f * uiScale;
                    drawText("Pointer Sensitivity", cX0 + 14.0f * uiScale, r2Y + 18.0f * uiScale, 0.33f * uiScale, cTextPrimary, 0);
                    float curSens = std::max(0.50f, std::min(3.00f, mSettings.rightStickSensitivity));
                    char sensTxt[32];
                    snprintf(sensTxt, sizeof(sensTxt), "%.2fx (%d%%)", curSens, (int)lroundf(curSens * 100.0f));
                    drawText(sensTxt, cX1 - 14.0f * uiScale, r2Y + 18.0f * uiScale, 0.32f * uiScale, cGold, 2);

                    float trackX0 = cX0 + 24.0f * uiScale;
                    float trackX1 = cX1 - 24.0f * uiScale;
                    float trackY = r2Y + 46.0f * uiScale;
                    float frac = (curSens - 0.50f) / 2.50f;
                    float knobX = trackX0 + frac * (trackX1 - trackX0);

                    drawRoundRect(trackX0, trackY - 3.5f * uiScale, trackX1, trackY + 3.5f * uiScale, 3.5f * uiScale, sc(rgba(0.10f, 0.16f, 0.36f, 0.95f)));
                    if (knobX > trackX0 + 1.0f) {
                        drawRoundRect(trackX0, trackY - 3.5f * uiScale, knobX, trackY + 3.5f * uiScale, 3.5f * uiScale, sc(rgba(0.14f, 0.54f, 0.98f, 1.0f)));
                    }
                    static const char* kSensTicks[6] = {"0.5x", "1.0x", "1.5x", "2.0x", "2.5x", "3.0x"};
                    for (int s = 0; s < 6; s++) {
                        float stepFrac = (float)s / 5.0f;
                        float stepX = trackX0 + stepFrac * (trackX1 - trackX0);
                        Color dotCol = (stepFrac <= frac + 0.01f) ? sc(rgba(0.68f, 0.94f, 1.0f, 0.98f)) : sc(rgba(0.36f, 0.48f, 0.72f, 0.75f));
                        drawCircle(stepX, trackY, 3.2f * uiScale, dotCol);
                        drawText(kSensTicks[s], stepX, r2Y + 76.0f * uiScale, 0.24f * uiScale, cTextSecondary, 1);
                    }
                    drawCircle(knobX, trackY, 11.0f * uiScale, sc(rgba(0.22f, 0.80f, 1.0f, 0.98f)));
                    drawCircle(knobX, trackY, 6.0f * uiScale, sc(rgba(1.0f, 0.98f, 0.88f, 1.0f)));
                    pushDeckHit(cX0 + 8.0f * uiScale, r2Y + 24.0f * uiScale, cX1 - 8.0f * uiScale, r2Y + 74.0f * uiScale, HIT_SLIDER_SENSITIVITY, 0, 0, trackX0, trackX1);
                });

                placeCard(1, 210.0f * uiScale, [&](float cX0, float cX1, float cw, float y, float ch) {
                    drawCard(cX0, y, cX1, y + ch, 18.0f * uiScale, cCardBg, cCardBorder);
                    drawSectionTitle(cX0, y, "CUSTOMIZE TOUCH CONTROLS", cHeader, true);

                    float grpW = cw - 28.0f * uiScale;
                    float r1Y = y + 34.0f * uiScale;
                    drawText("Opacity Level", cX0 + 14.0f * uiScale, r1Y + 14.0f * uiScale, 0.32f * uiScale, cTextPrimary, 0);
                    const char* opOpts[4] = {"25%", "50%", "75%", "100%"};
                    int opIdx = (fabsf(mSettings.opacity - 0.25f) < 0.12f) ? 0 : (fabsf(mSettings.opacity - 0.50f) < 0.12f) ? 1 : (fabsf(mSettings.opacity - 0.75f) < 0.12f) ? 2 : 3;
                    float opY0 = r1Y + 18.0f * uiScale;
                    drawSegmentedGroup(cX0 + 14.0f * uiScale, opY0, grpW, 30.0f * uiScale, opOpts, 4, opIdx);
                    for (int s = 0; s < 4; s++) {
                        float sx0 = cX0 + 14.0f * uiScale + s * (grpW * 0.25f);
                        pushDeckHit(sx0, opY0 - 4.0f * uiScale, sx0 + grpW * 0.25f, opY0 + 34.0f * uiScale, HIT_SEG_OPACITY, s);
                    }

                    drawRoundRect(cX0 + 14.0f * uiScale, y + 90.0f * uiScale, cX1 - 14.0f * uiScale, y + 91.0f * uiScale, 0.5f, cDivider);

                    float r2Y = y + 94.0f * uiScale;
                    drawText("Button Size", cX0 + 14.0f * uiScale, r2Y + 14.0f * uiScale, 0.32f * uiScale, cTextPrimary, 0);
                    const char* scOpts[3] = {"Small (85%)", "Medium (100%)", "Large (125%)"};
                    int scIdx = (fabsf(mSettings.scale - 0.85f) < 0.08f) ? 0 : (fabsf(mSettings.scale - 1.25f) < 0.08f) ? 2 : 1;
                    float scY0 = r2Y + 18.0f * uiScale;
                    drawSegmentedGroup(cX0 + 14.0f * uiScale, scY0, grpW, 30.0f * uiScale, scOpts, 3, scIdx);
                    for (int s = 0; s < 3; s++) {
                        float sx0 = cX0 + 14.0f * uiScale + s * (grpW / 3.0f);
                        pushDeckHit(sx0, scY0 - 4.0f * uiScale, sx0 + grpW / 3.0f, scY0 + 34.0f * uiScale, HIT_SEG_BTN_SIZE, s);
                    }

                    drawRoundRect(cX0 + 14.0f * uiScale, y + 150.0f * uiScale, cX1 - 14.0f * uiScale, y + 151.0f * uiScale, 0.5f, cDivider);

                    float r3Y = y + 156.0f * uiScale;
                    float b1X0 = cX0 + 14.0f * uiScale;
                    float b1X1 = b1X0 + (cw - 36.0f * uiScale) * 0.62f;
                    float b2X0 = b1X1 + 8.0f * uiScale;
                    float b2X1 = cX1 - 14.0f * uiScale;
                    drawGlossyBtn(b1X0, r3Y, b1X1, r3Y + 38.0f * uiScale, 15.0f * uiScale,
                                  rgba(0.32f, 0.78f, 0.99f, 0.85f), rgba(0.06f, 0.38f, 0.86f, 0.98f),
                                  "Reposition Layout", rgba(1.0f, 1.0f, 1.0f, 1.0f), 0.33f);
                    pushDeckHit(b1X0, r3Y, b1X1, r3Y + 38.0f * uiScale, HIT_LAYOUT_EDIT);

                    drawGlossyBtn(b2X0, r3Y, b2X1, r3Y + 38.0f * uiScale, 15.0f * uiScale,
                                  rgba(0.92f, 0.45f, 0.48f, 0.65f), rgba(0.42f, 0.14f, 0.18f, 0.95f),
                                  "Reset", rgba(1.0f, 0.78f, 0.78f, 1.0f), 0.33f);
                    pushDeckHit(b2X0, r3Y, b2X1, r3Y + 38.0f * uiScale, HIT_LAYOUT_RESET);
                });

                placeCard(1, 118.0f * uiScale, [&](float cX0, float cX1, float cw, float y, float ch) {
                    drawCard(cX0, y, cX1, y + ch, 18.0f * uiScale, cCardBgAlt, cCardBorderGold);
                    drawSectionTitle(cX0, y, "STAR POINTER FLIGHT TIPS", cGold, true);
                    drawText("• Touch anywhere on screen to aim Star Bits manually.", cX0 + 16.0f * uiScale, y + 48.0f * uiScale, 0.26f * uiScale, cTextSecondary, 0);
                    drawText("• Connect a controller to use Right Stick as Star Pointer or Camera.", cX0 + 16.0f * uiScale, y + 70.0f * uiScale, 0.26f * uiScale, cTextSecondary, 0);
                    drawText("• Use Reposition Layout to freely drag sticks, buttons & camera dial.", cX0 + 16.0f * uiScale, y + 92.0f * uiScale, 0.26f * uiScale, cTextSecondary, 0);
                    (void)cw;
                });
            } else if (mSettingsTab == 1) {
                // =====================================================
                // Tab 1: Display & FPS
                // =====================================================
                placeCard(0, 136.0f * uiScale, [&](float cX0, float cX1, float cw, float y, float ch) {
                    drawCard(cX0, y, cX1, y + ch, 18.0f * uiScale, cCardBg, cCardBorder);
                    drawSectionTitle(cX0, y, "DISPLAY & VIEWPORT", cHeader, true);

                    float r1Y = y + 34.0f * uiScale;
                    drawText("Native Ultrawide (Hor+)", cX0 + 14.0f * uiScale, r1Y + 18.0f * uiScale, 0.36f * uiScale, cTextPrimary, 0);
                    drawText("Expand horizontal FOV to fill screen", cX0 + 14.0f * uiScale, r1Y + 34.0f * uiScale, 0.25f * uiScale, cTextSecondary, 0);
                    drawM3Switch(cX1 - swW - 14.0f * uiScale, r1Y + 8.0f * uiScale, swW, swH, mSwAnimFullscreen);
                    pushDeckHit(cX0 + 8.0f * uiScale, r1Y, cX1 - 8.0f * uiScale, r1Y + 44.0f * uiScale, HIT_TOGGLE_FULLSCREEN);

                    drawRoundRect(cX0 + 14.0f * uiScale, y + 80.0f * uiScale, cX1 - 14.0f * uiScale, y + 81.0f * uiScale, 0.5f, cDivider);

                    float r2Y = y + 82.0f * uiScale;
                    drawText("Show Camera Cutout", cX0 + 14.0f * uiScale, r2Y + 18.0f * uiScale, 0.36f * uiScale, cTextPrimary, 0);
                    drawText("Extend display into camera punch-hole", cX0 + 14.0f * uiScale, r2Y + 34.0f * uiScale, 0.25f * uiScale, cTextSecondary, 0);
                    drawM3Switch(cX1 - swW - 14.0f * uiScale, r2Y + 8.0f * uiScale, swW, swH, mSwAnimCameraCutout);
                    pushDeckHit(cX0 + 8.0f * uiScale, r2Y, cX1 - 8.0f * uiScale, r2Y + 46.0f * uiScale, HIT_TOGGLE_CAMERA_CUTOUT);
                    (void)cw;
                });

                placeCard(0, 180.0f * uiScale, [&](float cX0, float cX1, float cw, float y, float ch) {
                    drawCard(cX0, y, cX1, y + ch, 18.0f * uiScale, cCardBg, cCardBorder);
                    drawSectionTitle(cX0, y, "3D RENDER RESOLUTION PRESET", cHeader, false);

                    float r1Y = y + 36.0f * uiScale;
                    float grpW = cw - 28.0f * uiScale;
                    static const char* kPresetNames[5] = { "480p", "720p", "900p", "1080p", "Ultra" };
                    static const int kSegmentByPreset[5] = { 1, 2, 3, 4, 0 };
                    static const int kPresetBySegment[5] = { 4, 0, 1, 2, 3 };
                    int activeSeg = (mSettings.graphicsPreset >= 0 && mSettings.graphicsPreset < 5) ? kSegmentByPreset[mSettings.graphicsPreset] : 3;
                    drawSegmentedGroup(cX0 + 14.0f * uiScale, r1Y, grpW, 34.0f * uiScale, kPresetNames, 5, activeSeg);
                    for (int s = 0; s < 5; s++) {
                        float sx0 = cX0 + 14.0f * uiScale + s * (grpW * 0.20f);
                        pushDeckHit(sx0, r1Y - 4.0f * uiScale, sx0 + grpW * 0.20f, r1Y + 38.0f * uiScale, HIT_SEG_RESOLUTION, kPresetBySegment[s]);
                    }

                    float boxY = y + 78.0f * uiScale;
                    float boxH = 88.0f * uiScale;
                    drawRoundRect(cX0 + 14.0f * uiScale, boxY, cX1 - 14.0f * uiScale, boxY + boxH, 12.0f * uiScale, sc(rgba(0.08f, 0.14f, 0.34f, 0.95f)));

                    const char* resChip = "1080p FHD • Recommended";
                    const char* resSub = "Internal 1920x1080 render target. Optimal balance.";
                    Color badgeBg = sc(rgba(0.10f, 0.28f, 0.44f, 0.90f));
                    Color badgeTxt = sc(rgba(0.45f, 0.88f, 1.0f, 1.0f));
                    Color badgeDot = cHeader;

                    if (mSettings.graphicsPreset == 4) {
                        resChip = "480p SD • Wii Native";
                        resSub = "Internal 854x480 target. Maximum battery & speed.";
                        badgeBg = sc(rgba(0.14f, 0.24f, 0.40f, 0.90f));
                        badgeTxt = sc(rgba(0.72f, 0.88f, 1.0f, 1.0f));
                    } else if (mSettings.graphicsPreset == 0) {
                        resChip = "720p HD • Power Saver";
                        resSub = "Internal 1280x720 target. Cooler battery operation.";
                        badgeBg = sc(rgba(0.10f, 0.34f, 0.22f, 0.90f));
                        badgeTxt = sc(rgba(0.55f, 0.96f, 0.65f, 1.0f));
                        badgeDot = sc(rgba(0.290f, 0.871f, 0.502f, 1.0f));
                    } else if (mSettings.graphicsPreset == 1) {
                        resChip = "900p HD+ • Performance";
                        resSub = "Internal 1600x900 target. High fluidity.";
                        badgeBg = sc(rgba(0.36f, 0.28f, 0.10f, 0.90f));
                        badgeTxt = sc(rgba(1.0f, 0.88f, 0.42f, 1.0f));
                        badgeDot = cGold;
                    } else if (mSettings.graphicsPreset == 3) {
                        resChip = "Native Display • Ultra Fidelity";
                        resSub = "Full QHD+ native device pixels. Maximum crispness.";
                        badgeBg = sc(rgba(0.34f, 0.16f, 0.44f, 0.90f));
                        badgeTxt = sc(rgba(0.92f, 0.68f, 1.0f, 1.0f));
                        badgeDot = sc(rgba(0.85f, 0.45f, 0.98f, 1.0f));
                    }

                    drawStatusChip(cX0 + 24.0f * uiScale, boxY + 12.0f * uiScale, resChip, badgeBg, badgeTxt, badgeDot);
                    drawText(resSub, cX0 + 24.0f * uiScale, boxY + 54.0f * uiScale, 0.26f * uiScale, cTextSecondary, 0);
                    drawText("Scales 3D scene geometry & offscreen EFB framebuffers.", cX0 + 24.0f * uiScale, boxY + 72.0f * uiScale, 0.23f * uiScale, sc(rgba(0.58f, 0.68f, 0.86f, 0.80f)), 0);
                });

                placeCard(0, 92.0f * uiScale, [&](float cX0, float cX1, float cw, float y, float ch) {
                    drawCard(cX0, y, cX1, y + ch, 18.0f * uiScale, cCardBg, cCardBorder);
                    drawSectionTitle(cX0, y, "TEXTURE ENHANCEMENTS", cHeader, false);

                    float r1Y = y + 34.0f * uiScale;
                    drawText("HD UI & HUD (3D All-Stars)", cX0 + 14.0f * uiScale, r1Y + 18.0f * uiScale, 0.36f * uiScale, cTextPrimary, 0);
                    drawText("Crisp upscaled fonts, icons & HUD overlays", cX0 + 14.0f * uiScale, r1Y + 34.0f * uiScale, 0.25f * uiScale, cTextSecondary, 0);
                    drawM3Switch(cX1 - swW - 14.0f * uiScale, r1Y + 8.0f * uiScale, swW, swH, mSwAnimHdUi);
                    pushDeckHit(cX0 + 8.0f * uiScale, r1Y, cX1 - 8.0f * uiScale, y + ch - 4.0f * uiScale, HIT_TOGGLE_HD_UI);
                    (void)cw;
                });

                placeCard(1, 316.0f * uiScale, [&](float cX0, float cX1, float cw, float y, float ch) {
                    drawCard(cX0, y, cX1, y + ch, 18.0f * uiScale, cCardBg, cCardBorderGold);
                    drawSectionTitle(cX0, y, "PERFORMANCE & FPS LIMITER", cGold, true);

                    float r1Y = y + 34.0f * uiScale;
                    drawText("Show FPS Counter", cX0 + 14.0f * uiScale, r1Y + 18.0f * uiScale, 0.36f * uiScale, cTextPrimary, 0);
                    drawText("Display movable & resizable frame rate badge", cX0 + 14.0f * uiScale, r1Y + 34.0f * uiScale, 0.25f * uiScale, cTextSecondary, 0);
                    drawM3Switch(cX1 - swW - 14.0f * uiScale, r1Y + 8.0f * uiScale, swW, swH, mSwAnimFpsCounter);
                    pushDeckHit(cX0 + 8.0f * uiScale, r1Y, cX1 - 8.0f * uiScale, r1Y + 44.0f * uiScale, HIT_TOGGLE_FPS_COUNTER);

                    drawRoundRect(cX0 + 14.0f * uiScale, y + 80.0f * uiScale, cX1 - 14.0f * uiScale, y + 81.0f * uiScale, 0.5f, cDivider);

                    float r2Y = y + 84.0f * uiScale;
                    drawText("Counter Size", cX0 + 14.0f * uiScale, r2Y + 14.0f * uiScale, 0.32f * uiScale, cTextPrimary, 0);
                    char sizePct[24];
                    snprintf(sizePct, sizeof(sizePct), "%d%%", (int)lroundf(mSettings.fpsScale * 100.0f));
                    const char* fpsSizeOpts[3] = {"- Smaller", sizePct, "+ Larger"};
                    float grpW = cw - 28.0f * uiScale;
                    float szY0 = r2Y + 18.0f * uiScale;
                    drawSegmentedGroup(cX0 + 14.0f * uiScale, szY0, grpW, 30.0f * uiScale, fpsSizeOpts, 3, 1);
                    static const int kStepArg[3] = { -1, 0, 1 };
                    for (int s = 0; s < 3; s++) {
                        float sx0 = cX0 + 14.0f * uiScale + s * (grpW / 3.0f);
                        pushDeckHit(sx0, szY0 - 4.0f * uiScale, sx0 + grpW / 3.0f, szY0 + 34.0f * uiScale, HIT_STEP_FPS_SCALE, kStepArg[s]);
                    }

                    drawRoundRect(cX0 + 14.0f * uiScale, y + 142.0f * uiScale, cX1 - 14.0f * uiScale, y + 143.0f * uiScale, 0.5f, cDivider);

                    float r3Y = y + 148.0f * uiScale;
                    float b1X0 = cX0 + 14.0f * uiScale;
                    float b1X1 = b1X0 + (cw - 36.0f * uiScale) * 0.62f;
                    float b2X0 = b1X1 + 8.0f * uiScale;
                    float b2X1 = cX1 - 14.0f * uiScale;
                    drawGlossyBtn(b1X0, r3Y, b1X1, r3Y + 36.0f * uiScale, 14.0f * uiScale,
                                  rgba(0.32f, 0.78f, 0.99f, 0.85f), rgba(0.06f, 0.38f, 0.86f, 0.98f),
                                  "Move & Resize Badge", rgba(1.0f, 1.0f, 1.0f, 1.0f), 0.32f);
                    pushDeckHit(b1X0, r3Y, b1X1, r3Y + 36.0f * uiScale, HIT_FPS_EDIT);

                    drawGlossyBtn(b2X0, r3Y, b2X1, r3Y + 36.0f * uiScale, 14.0f * uiScale,
                                  rgba(0.92f, 0.45f, 0.48f, 0.65f), rgba(0.42f, 0.14f, 0.18f, 0.95f),
                                  "Reset", rgba(1.0f, 0.78f, 0.78f, 1.0f), 0.32f);
                    pushDeckHit(b2X0, r3Y, b2X1, r3Y + 36.0f * uiScale, HIT_FPS_RESET);

                    drawRoundRect(cX0 + 14.0f * uiScale, y + 196.0f * uiScale, cX1 - 14.0f * uiScale, y + 197.0f * uiScale, 0.5f, cDivider);

                    float r4Y = y + 202.0f * uiScale;
                    drawText("FPS Limiter (Physics stays 60 Hz)", cX0 + 14.0f * uiScale, r4Y + 18.0f * uiScale, 0.31f * uiScale, cTextPrimary, 0);
                    int curLimit = std::max(30, std::min(60, mSettings.fpsLimit));
                    char limitTxt[32];
                    if (curLimit >= 60) {
                        snprintf(limitTxt, sizeof(limitTxt), "60 FPS (Full)");
                    } else if (curLimit == 30) {
                        snprintf(limitTxt, sizeof(limitTxt), "30 FPS (Saver)");
                    } else {
                        snprintf(limitTxt, sizeof(limitTxt), "%d FPS", curLimit);
                    }
                    drawText(limitTxt, cX1 - 14.0f * uiScale, r4Y + 18.0f * uiScale, 0.31f * uiScale, cGold, 2);

                    float trackX0 = cX0 + 24.0f * uiScale;
                    float trackX1 = cX1 - 24.0f * uiScale;
                    float trackY = r4Y + 46.0f * uiScale;
                    float frac = (float)(curLimit - 30) / 30.0f;
                    float knobX = trackX0 + frac * (trackX1 - trackX0);

                    drawRoundRect(trackX0, trackY - 3.5f * uiScale, trackX1, trackY + 3.5f * uiScale, 3.5f * uiScale, sc(rgba(0.10f, 0.16f, 0.36f, 0.95f)));
                    if (knobX > trackX0 + 1.0f) {
                        drawRoundRect(trackX0, trackY - 3.5f * uiScale, knobX, trackY + 3.5f * uiScale, 3.5f * uiScale, sc(rgba(0.14f, 0.54f, 0.98f, 1.0f)));
                    }
                    static const char* kFpsTicks[7] = {"30", "35", "40", "45", "50", "55", "60"};
                    for (int s = 0; s < 7; s++) {
                        float stepFrac = (float)s / 6.0f;
                        float stepX = trackX0 + stepFrac * (trackX1 - trackX0);
                        Color dotCol = (stepFrac <= frac + 0.01f) ? sc(rgba(0.68f, 0.94f, 1.0f, 0.98f)) : sc(rgba(0.36f, 0.48f, 0.72f, 0.75f));
                        drawCircle(stepX, trackY, 3.2f * uiScale, dotCol);
                        drawText(kFpsTicks[s], stepX, r4Y + 76.0f * uiScale, 0.24f * uiScale, cTextSecondary, 1);
                    }
                    drawCircle(knobX, trackY, 11.0f * uiScale, sc(rgba(0.22f, 0.80f, 1.0f, 0.98f)));
                    drawCircle(knobX, trackY, 6.0f * uiScale, sc(rgba(1.0f, 0.98f, 0.88f, 1.0f)));
                    drawText("Frame cap skips redundant renders while preserving full speed.", cX0 + 14.0f * uiScale, r4Y + 98.0f * uiScale, 0.23f * uiScale, cTextSecondary, 0);
                    pushDeckHit(cX0 + 8.0f * uiScale, r4Y + 24.0f * uiScale, cX1 - 8.0f * uiScale, r4Y + 78.0f * uiScale, HIT_SLIDER_FPS_LIMIT, 0, 0, trackX0, trackX1);
                });

                placeCard(1, 96.0f * uiScale, [&](float cX0, float cX1, float cw, float y, float ch) {
                    drawCard(cX0, y, cX1, y + ch, 18.0f * uiScale, cCardBgAlt, cCardBorder);
                    drawSectionTitle(cX0, y, "ENGINE & GRAPHICS PIPELINE", cHeader, false);
                    drawText("GX-to-GLES3 hardware shader translation runs live on GPU.", cX0 + 16.0f * uiScale, y + 48.0f * uiScale, 0.26f * uiScale, cTextSecondary, 0);
                    drawText("Lower resolution or FPS cap to cool device during long play.", cX0 + 16.0f * uiScale, y + 70.0f * uiScale, 0.26f * uiScale, cTextSecondary, 0);
                    (void)cw;
                });
            } else if (mPlusEdition && mSettingsTab == 2) {
                // =====================================================
                // Tab 2 (Plus): Cosmic Mods, Power-Ups, Warp & Bros
                // =====================================================
                placeCard(0, 240.0f * uiScale, [&](float cX0, float cX1, float cw, float y, float ch) {
                    drawCard(cX0, y, cX1, y + ch, 18.0f * uiScale, cCardBg, cCardBorderGold);
                    drawSectionTitle(cX0, y, "COSMIC CHEATS & MODS", cGold, true);

                    float r1Y = y + 34.0f * uiScale;
                    drawText("Skip Cutscenes", cX0 + 14.0f * uiScale, r1Y + 18.0f * uiScale, 0.36f * uiScale, cTextPrimary, 0);
                    drawText("Bypass intro & story event cinematics", cX0 + 14.0f * uiScale, r1Y + 34.0f * uiScale, 0.25f * uiScale, cTextSecondary, 0);
                    drawM3Switch(cX1 - swW - 14.0f * uiScale, r1Y + 8.0f * uiScale, swW, swH, mSwAnimSkipCutscenes);
                    pushDeckHit(cX0 + 8.0f * uiScale, r1Y, cX1 - 8.0f * uiScale, r1Y + 46.0f * uiScale, HIT_TOGGLE_SKIP_CUTSCENES);

                    drawRoundRect(cX0 + 14.0f * uiScale, y + 82.0f * uiScale, cX1 - 14.0f * uiScale, y + 83.0f * uiScale, 0.5f, cDivider);

                    float r2Y = y + 84.0f * uiScale;
                    drawText("Infinite Health", cX0 + 14.0f * uiScale, r2Y + 18.0f * uiScale, 0.36f * uiScale, cTextPrimary, 0);
                    drawText("Invulnerable Hero locked at 6 HP max", cX0 + 14.0f * uiScale, r2Y + 34.0f * uiScale, 0.25f * uiScale, cTextSecondary, 0);
                    drawM3Switch(cX1 - swW - 14.0f * uiScale, r2Y + 8.0f * uiScale, swW, swH, mSwAnimInfiniteHp);
                    pushDeckHit(cX0 + 8.0f * uiScale, r2Y, cX1 - 8.0f * uiScale, r2Y + 46.0f * uiScale, HIT_TOGGLE_INF_HP);

                    drawRoundRect(cX0 + 14.0f * uiScale, y + 132.0f * uiScale, cX1 - 14.0f * uiScale, y + 133.0f * uiScale, 0.5f, cDivider);

                    float r3Y = y + 134.0f * uiScale;
                    drawText("Infinite Power-Up Duration", cX0 + 14.0f * uiScale, r3Y + 18.0f * uiScale, 0.36f * uiScale, cTextPrimary, 0);
                    drawText("Never lose Fire, Bee, Boo or Ice timer", cX0 + 14.0f * uiScale, r3Y + 34.0f * uiScale, 0.25f * uiScale, cTextSecondary, 0);
                    drawM3Switch(cX1 - swW - 14.0f * uiScale, r3Y + 8.0f * uiScale, swW, swH, mSwAnimInfinitePowerup);
                    pushDeckHit(cX0 + 8.0f * uiScale, r3Y, cX1 - 8.0f * uiScale, r3Y + 46.0f * uiScale, HIT_TOGGLE_INF_POWERUP);

                    drawRoundRect(cX0 + 14.0f * uiScale, y + 182.0f * uiScale, cX1 - 14.0f * uiScale, y + 183.0f * uiScale, 0.5f, cDivider);

                    float r4Y = y + 184.0f * uiScale;
                    drawText("Flight Everywhere (Red Star)", cX0 + 14.0f * uiScale, r4Y + 18.0f * uiScale, 0.36f * uiScale, cTextPrimary, 0);
                    drawText("Soar in any galaxy using Red Star suit", cX0 + 14.0f * uiScale, r4Y + 34.0f * uiScale, 0.25f * uiScale, cTextSecondary, 0);
                    drawM3Switch(cX1 - swW - 14.0f * uiScale, r4Y + 8.0f * uiScale, swW, swH, mSwAnimFlightEverywhere);
                    pushDeckHit(cX0 + 8.0f * uiScale, r4Y, cX1 - 8.0f * uiScale, y + ch - 4.0f * uiScale, HIT_TOGGLE_FLIGHT);
                    (void)cw;
                });

                placeCard(0, 110.0f * uiScale, [&](float cX0, float cX1, float cw, float y, float ch) {
                    drawCard(cX0, y, cX1, y + ch, 18.0f * uiScale, cCardBg, cCardBorder);
                    drawSectionTitle(cX0, y, "PLAYER CHARACTER (MARIO / LUIGI)", cHeader, true);
                    drawText("Switch hero anytime in solo or co-op multiplayer", cX0 + 14.0f * uiScale, y + 42.0f * uiScale, 0.25f * uiScale, cTextSecondary, 0);

                    float mBtnY = y + 54.0f * uiScale;
                    float mBtnW = (cw - 36.0f * uiScale) * 0.5f;
                    bool isLuigi = (mSettings.playerCharacter != 0);

                    drawGlossyBtn(cX0 + 14.0f * uiScale, mBtnY, cX0 + 14.0f * uiScale + mBtnW, mBtnY + 40.0f * uiScale, 14.0f * uiScale,
                                  !isLuigi ? rgba(1.0f, 0.52f, 0.52f, 0.90f) : rgba(0.32f, 0.45f, 0.72f, 0.45f),
                                  !isLuigi ? rgba(0.78f, 0.16f, 0.18f, 0.98f) : rgba(0.09f, 0.14f, 0.32f, 0.92f),
                                  "MARIO", !isLuigi ? rgba(1.0f, 1.0f, 1.0f, 1.0f) : rgba(0.72f, 0.80f, 0.94f, 0.85f), 0.34f);
                    pushDeckHit(cX0 + 14.0f * uiScale, mBtnY, cX0 + 14.0f * uiScale + mBtnW, mBtnY + 40.0f * uiScale, HIT_SEG_CHARACTER, 0);

                    drawGlossyBtn(cX1 - 14.0f * uiScale - mBtnW, mBtnY, cX1 - 14.0f * uiScale, mBtnY + 40.0f * uiScale, 14.0f * uiScale,
                                  isLuigi ? rgba(0.45f, 0.95f, 0.62f, 0.90f) : rgba(0.32f, 0.45f, 0.72f, 0.45f),
                                  isLuigi ? rgba(0.12f, 0.56f, 0.24f, 0.98f) : rgba(0.09f, 0.14f, 0.32f, 0.92f),
                                  "LUIGI", isLuigi ? rgba(1.0f, 1.0f, 1.0f, 1.0f) : rgba(0.72f, 0.80f, 0.94f, 0.85f), 0.34f);
                    pushDeckHit(cX1 - 14.0f * uiScale - mBtnW, mBtnY, cX1 - 14.0f * uiScale, mBtnY + 40.0f * uiScale, HIT_SEG_CHARACTER, 1);
                });

                placeCard(1, 174.0f * uiScale, [&](float cX0, float cX1, float cw, float y, float ch) {
                    drawCard(cX0, y, cX1, y + ch, 18.0f * uiScale, cCardBg, cCardBorder);
                    drawSectionTitle(cX0, y, "LIVE POWER-UP MORPH", cHeader, true);
                    drawText("Tap any form to transform Mario/Luigi instantly", cX0 + 14.0f * uiScale, y + 40.0f * uiScale, 0.25f * uiScale, cTextSecondary, 0);

                    int curMode = port_get_current_powerup();
                    struct PowerupItem { const char* label; int type; int playerMode; };
                    static const PowerupItem kPowerupGrid[3][3] = {
                        { {"Normal", 0, 0}, {"Fire", 5, 2}, {"Ice", 4, 3} },
                        { {"Bee", 2, 4}, {"Flying", 6, 7}, {"Rainbow", 7, 1} },
                        { {"Boo", 3, 6}, {"Spring", 1, 5}, {"+Life 6HP", 10, -1} }
                    };

                    float pStartX = cX0 + 14.0f * uiScale;
                    float pillW = (cw - 28.0f * uiScale - 12.0f * uiScale) / 3.0f;
                    float pillH = 34.0f * uiScale;
                    float pillGapY = 6.0f * uiScale;

                    for (int r = 0; r < 3; r++) {
                        for (int c = 0; c < 3; c++) {
                            const auto& item = kPowerupGrid[r][c];
                            float px0 = pStartX + c * (pillW + 6.0f * uiScale);
                            float px1 = px0 + pillW;
                            float py0 = y + 50.0f * uiScale + r * (pillH + pillGapY);
                            float py1 = py0 + pillH;
                            bool isActive = (item.playerMode >= 0 && curMode == item.playerMode);
                            Color rim = isActive ? rgba(1.0f, 0.86f, 0.32f, 0.98f) : rgba(0.32f, 0.56f, 0.88f, 0.50f);
                            Color fill = isActive ? rgba(0.08f, 0.42f, 0.92f, 1.0f) : rgba(0.09f, 0.15f, 0.34f, 0.92f);
                            drawGlossyBtn(px0, py0, px1, py1, 10.0f * uiScale, rim, fill, item.label, isActive ? rgba(1.0f, 0.98f, 0.80f, 1.0f) : rgba(0.84f, 0.90f, 1.0f, 0.92f), 0.30f);
                            pushDeckHit(px0, py0, px1, py1, HIT_POWERUP_MORPH, item.type);
                        }
                    }
                });

                placeCard(1, 144.0f * uiScale, [&](float cX0, float cX1, float cw, float y, float ch) {
                    drawCard(cX0, y, cX1, y + ch, 18.0f * uiScale, cCardBg, cCardBorderGold);
                    drawSectionTitle(cX0, y, "WARP OBSERVATORY & ABILITIES", cGold, true);

                    float btnW = 104.0f * uiScale;
                    float btnH = 34.0f * uiScale;
                    float r1Y = y + 36.0f * uiScale;
                    drawText("Galaxy Warp Menu", cX0 + 14.0f * uiScale, r1Y + 18.0f * uiScale, 0.35f * uiScale, cTextPrimary, 0);
                    drawText("Teleport to any galaxy & scenario star", cX0 + 14.0f * uiScale, r1Y + 34.0f * uiScale, 0.25f * uiScale, cTextSecondary, 0);
                    drawGlossyBtn(cX1 - btnW - 14.0f * uiScale, r1Y + 5.0f * uiScale, cX1 - 14.0f * uiScale, r1Y + 5.0f * uiScale + btnH, 14.0f * uiScale,
                                  rgba(1.0f, 0.86f, 0.32f, 0.95f), rgba(0.14f, 0.42f, 0.90f, 0.98f),
                                  "Open Warp", rgba(1.0f, 0.98f, 0.84f, 1.0f), 0.31f);
                    pushDeckHit(cX1 - btnW - 14.0f * uiScale, r1Y + 2.0f * uiScale, cX1 - 14.0f * uiScale, r1Y + 8.0f * uiScale + btnH, HIT_OPEN_WARP);

                    drawRoundRect(cX0 + 14.0f * uiScale, y + 86.0f * uiScale, cX1 - 14.0f * uiScale, y + 87.0f * uiScale, 0.5f, cDivider);

                    float r2Y = y + 90.0f * uiScale;
                    drawText("Early Spin Ability", cX0 + 14.0f * uiScale, r2Y + 18.0f * uiScale, 0.35f * uiScale, cTextPrimary, 0);
                    drawText("Unlock Spin & Launch Stars early", cX0 + 14.0f * uiScale, r2Y + 34.0f * uiScale, 0.25f * uiScale, cTextSecondary, 0);
                    bool isSpinUnlocked = (port_is_spin_unlocked() != 0);
                    Color sRim = isSpinUnlocked ? rgba(0.45f, 0.95f, 0.65f, 0.90f) : rgba(0.32f, 0.78f, 0.99f, 0.85f);
                    Color sBg = isSpinUnlocked ? rgba(0.12f, 0.52f, 0.28f, 0.98f) : rgba(0.06f, 0.38f, 0.86f, 0.98f);
                    drawGlossyBtn(cX1 - btnW - 14.0f * uiScale, r2Y + 5.0f * uiScale, cX1 - 14.0f * uiScale, r2Y + 5.0f * uiScale + btnH, 14.0f * uiScale,
                                  sRim, sBg, isSpinUnlocked ? "Unlocked" : "Unlock Spin", rgba(1.0f, 1.0f, 1.0f, 1.0f), 0.31f);
                    pushDeckHit(cX1 - btnW - 14.0f * uiScale, r2Y + 2.0f * uiScale, cX1 - 14.0f * uiScale, r2Y + 8.0f * uiScale + btnH, HIT_UNLOCK_SPIN);
                    (void)cw;
                });
            } else if (mPlusEdition && mSettingsTab == 3) {
                // =====================================================
                // Tab 3 (Plus): Netplay (Wi-Fi LAN & Online Multiplayer)
                // =====================================================
                bool isConn = (port_net_is_connected() != 0);
                int netRole = port_net_get_role();
                int netMode = port_net_get_mode();
                int ping = port_net_get_ping_ms();
                const char* peerIp = port_net_get_peer_ip();
                const char* localIp = port_net_get_local_ip();

                placeCard(0, 114.0f * uiScale, [&](float cX0, float cX1, float cw, float y, float ch) {
                    drawCard(cX0, y, cX1, y + ch, 18.0f * uiScale, cCardBg, isConn ? sc(rgba(0.35f, 0.92f, 0.56f, 0.65f)) : cCardBorder);
                    drawSectionTitle(cX0, y, "MULTIPLAYER STATUS", isConn ? sc(rgba(0.35f, 0.92f, 0.56f, 1.0f)) : cHeader, true);

                    if (isConn) {
                        char statusBuf[64];
                        snprintf(statusBuf, sizeof(statusBuf), "Connected (%s)", (netRole == 1) ? "Host" : "Client");
                        drawStatusChip(cX0 + 14.0f * uiScale, y + 36.0f * uiScale, statusBuf,
                                       sc(rgba(0.12f, 0.36f, 0.22f, 0.90f)), sc(rgba(0.55f, 0.98f, 0.65f, 1.0f)), sc(rgba(0.290f, 0.871f, 0.502f, 1.0f)));
                        char pingBuf[32];
                        snprintf(pingBuf, sizeof(pingBuf), "%d ms", ping);
                        drawStatusChip(cX0 + 178.0f * uiScale, y + 36.0f * uiScale, pingBuf,
                                       sc(rgba(0.14f, 0.26f, 0.42f, 0.90f)), sc(rgba(0.65f, 0.88f, 1.0f, 1.0f)), cHeader);

                        char peerBuf[64];
                        snprintf(peerBuf, sizeof(peerBuf), "Peer: %s", (peerIp && *peerIp) ? peerIp : "Unknown");
                        drawText(peerBuf, cX0 + 14.0f * uiScale, y + 84.0f * uiScale, 0.26f * uiScale, cTextSecondary, 0);

                        float dcBtnY = y + 70.0f * uiScale;
                        drawGlossyBtn(cX1 - 112.0f * uiScale, dcBtnY, cX1 - 14.0f * uiScale, dcBtnY + 32.0f * uiScale, 13.0f * uiScale,
                                      rgba(0.95f, 0.48f, 0.48f, 0.85f), rgba(0.62f, 0.15f, 0.18f, 0.96f),
                                      "DISCONNECT", rgba(1.0f, 1.0f, 1.0f, 1.0f), 0.27f);
                        pushDeckHit(cX1 - 112.0f * uiScale, dcBtnY, cX1 - 14.0f * uiScale, dcBtnY + 32.0f * uiScale, HIT_NET_DISCONNECT);
                    } else if (netRole == 1) {
                        drawStatusChip(cX0 + 14.0f * uiScale, y + 36.0f * uiScale, "Hosting • Port 24601",
                                       sc(rgba(0.38f, 0.28f, 0.08f, 0.95f)), sc(rgba(1.0f, 0.90f, 0.42f, 1.0f)), cGold);
                        char localBuf[64];
                        snprintf(localBuf, sizeof(localBuf), "Local IP: %s (Beacon active)", (localIp && *localIp) ? localIp : "127.0.0.1");
                        drawText(localBuf, cX0 + 14.0f * uiScale, y + 84.0f * uiScale, 0.25f * uiScale, cTextSecondary, 0);

                        float stopBtnY = y + 70.0f * uiScale;
                        drawGlossyBtn(cX1 - 96.0f * uiScale, stopBtnY, cX1 - 14.0f * uiScale, stopBtnY + 32.0f * uiScale, 13.0f * uiScale,
                                      rgba(0.95f, 0.48f, 0.48f, 0.85f), rgba(0.62f, 0.15f, 0.18f, 0.96f),
                                      "STOP", rgba(1.0f, 1.0f, 1.0f, 1.0f), 0.28f);
                        pushDeckHit(cX1 - 96.0f * uiScale, stopBtnY, cX1 - 14.0f * uiScale, stopBtnY + 32.0f * uiScale, HIT_NET_DISCONNECT);
                    } else {
                        drawStatusChip(cX0 + 14.0f * uiScale, y + 36.0f * uiScale, "Offline / Ready",
                                       sc(rgba(0.14f, 0.20f, 0.36f, 0.90f)), sc(rgba(0.75f, 0.82f, 0.95f, 1.0f)), rgba(0.55f, 0.65f, 0.85f, 1.0f * oa));
                        char localBuf[64];
                        snprintf(localBuf, sizeof(localBuf), "Local IP: %s", (localIp && *localIp) ? localIp : "127.0.0.1");
                        drawText(localBuf, cX0 + 14.0f * uiScale, y + 76.0f * uiScale, 0.26f * uiScale, cTextSecondary, 0);
                        drawText("Host a room or join a friend on Wi-Fi or Hotspot.", cX0 + 14.0f * uiScale, y + 96.0f * uiScale, 0.24f * uiScale, cTextSecondary, 0);
                    }
                    (void)cw;
                });

                placeCard(0, 136.0f * uiScale, [&](float cX0, float cX1, float cw, float y, float ch) {
                    drawCard(cX0, y, cX1, y + ch, 18.0f * uiScale, cCardBg, cCardBorder);
                    drawSectionTitle(cX0, y, "GAMEPLAY MODE & HERO", cHeader, false);

                    float mBtnY = y + 34.0f * uiScale;
                    float mBtnW = (cw - 36.0f * uiScale) * 0.5f;
                    bool isCoop = (netMode == 1);
                    bool isCostar = (netMode == 2);

                    drawGlossyBtn(cX0 + 14.0f * uiScale, mBtnY, cX0 + 14.0f * uiScale + mBtnW, mBtnY + 36.0f * uiScale, 14.0f * uiScale,
                                  isCoop ? rgba(0.32f, 0.78f, 0.99f, 0.92f) : rgba(0.32f, 0.45f, 0.72f, 0.45f),
                                  isCoop ? rgba(0.06f, 0.38f, 0.86f, 0.98f) : rgba(0.09f, 0.14f, 0.32f, 0.92f),
                                  "Mario & Luigi Co-op", isCoop ? rgba(1.0f, 1.0f, 1.0f, 1.0f) : rgba(0.72f, 0.80f, 0.94f, 0.85f), 0.28f);
                    pushDeckHit(cX0 + 14.0f * uiScale, mBtnY, cX0 + 14.0f * uiScale + mBtnW, mBtnY + 36.0f * uiScale, HIT_SEG_NET_MODE, 1);

                    drawGlossyBtn(cX1 - 14.0f * uiScale - mBtnW, mBtnY, cX1 - 14.0f * uiScale, mBtnY + 36.0f * uiScale, 14.0f * uiScale,
                                  isCostar ? rgba(0.78f, 0.52f, 1.0f, 0.92f) : rgba(0.32f, 0.45f, 0.72f, 0.45f),
                                  isCostar ? rgba(0.42f, 0.20f, 0.74f, 0.98f) : rgba(0.09f, 0.14f, 0.32f, 0.92f),
                                  "Co-Star Assist Mode", isCostar ? rgba(1.0f, 1.0f, 1.0f, 1.0f) : rgba(0.72f, 0.80f, 0.94f, 0.85f), 0.28f);
                    pushDeckHit(cX1 - 14.0f * uiScale - mBtnW, mBtnY, cX1 - 14.0f * uiScale, mBtnY + 36.0f * uiScale, HIT_SEG_NET_MODE, 2);

                    drawRoundRect(cX0 + 14.0f * uiScale, y + 78.0f * uiScale, cX1 - 14.0f * uiScale, y + 79.0f * uiScale, 0.5f, cDivider);

                    float cBtnY = y + 86.0f * uiScale;
                    bool isLuigi = (mSettings.playerCharacter != 0);
                    drawGlossyBtn(cX0 + 14.0f * uiScale, cBtnY, cX0 + 14.0f * uiScale + mBtnW, cBtnY + 36.0f * uiScale, 14.0f * uiScale,
                                  !isLuigi ? rgba(1.0f, 0.52f, 0.52f, 0.90f) : rgba(0.32f, 0.45f, 0.72f, 0.45f),
                                  !isLuigi ? rgba(0.78f, 0.16f, 0.18f, 0.98f) : rgba(0.09f, 0.14f, 0.32f, 0.92f),
                                  "MARIO (Host)", !isLuigi ? rgba(1.0f, 1.0f, 1.0f, 1.0f) : rgba(0.72f, 0.80f, 0.94f, 0.85f), 0.29f);
                    pushDeckHit(cX0 + 14.0f * uiScale, cBtnY, cX0 + 14.0f * uiScale + mBtnW, cBtnY + 36.0f * uiScale, HIT_SEG_CHARACTER, 0);

                    drawGlossyBtn(cX1 - 14.0f * uiScale - mBtnW, cBtnY, cX1 - 14.0f * uiScale, cBtnY + 36.0f * uiScale, 14.0f * uiScale,
                                  isLuigi ? rgba(0.45f, 0.95f, 0.62f, 0.90f) : rgba(0.32f, 0.45f, 0.72f, 0.45f),
                                  isLuigi ? rgba(0.12f, 0.56f, 0.24f, 0.98f) : rgba(0.09f, 0.14f, 0.32f, 0.92f),
                                  "LUIGI (Joiner)", isLuigi ? rgba(1.0f, 1.0f, 1.0f, 1.0f) : rgba(0.72f, 0.80f, 0.94f, 0.85f), 0.29f);
                    pushDeckHit(cX1 - 14.0f * uiScale - mBtnW, cBtnY, cX1 - 14.0f * uiScale, cBtnY + 36.0f * uiScale, HIT_SEG_CHARACTER, 1);
                });

                placeCard(0, 104.0f * uiScale, [&](float cX0, float cX1, float cw, float y, float ch) {
                    drawCard(cX0, y, cX1, y + ch, 18.0f * uiScale, cCardBg, cCardBorderGold);
                    drawSectionTitle(cX0, y, "HOST A GAME ROOM", cGold, true);
                    drawText("Broadcasts room on Wi-Fi / Hotspot (UDP 24601)", cX0 + 14.0f * uiScale, y + 44.0f * uiScale, 0.25f * uiScale, cTextSecondary, 0);
                    float hBtnY = y + 56.0f * uiScale;
                    bool hosting = (netRole == 1);
                    drawGlossyBtn(cX0 + 14.0f * uiScale, hBtnY, cX1 - 14.0f * uiScale, hBtnY + 36.0f * uiScale, 14.0f * uiScale,
                                  hosting ? rgba(0.95f, 0.48f, 0.48f, 0.85f) : rgba(0.32f, 0.78f, 0.99f, 0.90f),
                                  hosting ? rgba(0.55f, 0.16f, 0.18f, 0.96f) : rgba(0.06f, 0.38f, 0.86f, 0.98f),
                                  hosting ? "STOP HOSTING ROOM" : "START HOSTING ROOM (PORT 24601)", rgba(1.0f, 1.0f, 1.0f, 1.0f), 0.31f);
                    pushDeckHit(cX0 + 14.0f * uiScale, hBtnY, cX1 - 14.0f * uiScale, hBtnY + 36.0f * uiScale, HIT_NET_HOST_TOGGLE);
                    (void)cw;
                });

                placeCard(1, 196.0f * uiScale, [&](float cX0, float cX1, float cw, float y, float ch) {
                    drawCard(cX0, y, cX1, y + ch, 18.0f * uiScale, cCardBg, cCardBorder);
                    drawSectionTitle(cX0, y, "DISCOVERED LAN ROOMS", cHeader, false);

                    float rBtnW = 78.0f * uiScale;
                    float rBtnH = 26.0f * uiScale;
                    float rBtnX1 = cX1 - 14.0f * uiScale;
                    float rBtnX0 = rBtnX1 - rBtnW;
                    float rBtnY0 = y + 8.0f * uiScale;
                    drawGlossyBtn(rBtnX0, rBtnY0, rBtnX1, rBtnY0 + rBtnH, 11.0f * uiScale,
                                  rgba(0.35f, 0.78f, 0.98f, 0.75f), rgba(0.14f, 0.28f, 0.56f, 0.96f),
                                  "RELOAD", rgba(0.88f, 0.95f, 1.0f, 1.0f), 0.26f);
                    pushDeckHit(rBtnX0, rBtnY0, rBtnX1, rBtnY0 + rBtnH, HIT_NET_REFRESH);

                    PortLanHostInfo hosts[3];
                    int numHosts = port_net_get_lan_hosts(hosts, 3);
                    if (numHosts == 0) {
                        drawStatusChip(cX0 + 14.0f * uiScale, y + 40.0f * uiScale, "Searching LAN for active rooms...",
                                       sc(rgba(0.14f, 0.20f, 0.36f, 0.90f)), sc(rgba(0.75f, 0.82f, 0.95f, 1.0f)), cHeader);
                        drawText("Both devices must be on the same Wi-Fi or Hotspot.", cX0 + 14.0f * uiScale, y + 82.0f * uiScale, 0.25f * uiScale, cTextSecondary, 0);
                        drawText("Host must tap 'Start Hosting Room' on the left.", cX0 + 14.0f * uiScale, y + 102.0f * uiScale, 0.25f * uiScale, cTextSecondary, 0);
                    } else {
                        for (int hIdx = 0; hIdx < numHosts; hIdx++) {
                            float rowY = y + 40.0f * uiScale + hIdx * 48.0f * uiScale;
                            drawRoundRect(cX0 + 14.0f * uiScale, rowY, cX1 - 14.0f * uiScale, rowY + 42.0f * uiScale, 12.0f * uiScale, sc(rgba(0.08f, 0.15f, 0.36f, 0.96f)));
                            drawText(hosts[hIdx].name, cX0 + 24.0f * uiScale, rowY + 18.0f * uiScale, 0.30f * uiScale, cTextPrimary, 0);
                            char infoStr[64];
                            snprintf(infoStr, sizeof(infoStr), "%s:%d • %s", hosts[hIdx].ip, hosts[hIdx].port, (hosts[hIdx].mode == 1) ? "Co-op" : "Co-Star");
                            drawText(infoStr, cX0 + 24.0f * uiScale, rowY + 34.0f * uiScale, 0.24f * uiScale, cTextSecondary, 0);

                            float jBtnX1 = cX1 - 22.0f * uiScale;
                            float jBtnX0 = jBtnX1 - 72.0f * uiScale;
                            float jBtnY = rowY + 7.0f * uiScale;
                            drawGlossyBtn(jBtnX0, jBtnY, jBtnX1, jBtnY + 28.0f * uiScale, 11.0f * uiScale,
                                          rgba(0.45f, 0.95f, 0.65f, 0.90f), rgba(0.12f, 0.52f, 0.28f, 0.96f),
                                          "JOIN", rgba(1.0f, 1.0f, 1.0f, 1.0f), 0.28f);
                            pushDeckHit(jBtnX0, jBtnY, jBtnX1, jBtnY + 28.0f * uiScale, HIT_NET_JOIN_HOST, hIdx);
                        }
                    }
                    (void)cw;
                });

                placeCard(1, 112.0f * uiScale, [&](float cX0, float cX1, float cw, float y, float ch) {
                    drawCard(cX0, y, cX1, y + ch, 18.0f * uiScale, cCardBg, cCardBorder);
                    drawSectionTitle(cX0, y, "DIRECT IP CONNECT (HOTSPOT)", cHeader, false);
                    drawText("Connect directly to Hotspot gateway host", cX0 + 14.0f * uiScale, y + 44.0f * uiScale, 0.25f * uiScale, cTextSecondary, 0);
                    float cBtnY = y + 58.0f * uiScale;
                    drawGlossyBtn(cX0 + 14.0f * uiScale, cBtnY, cX1 - 14.0f * uiScale, cBtnY + 38.0f * uiScale, 14.0f * uiScale,
                                  rgba(0.32f, 0.78f, 0.99f, 0.75f), rgba(0.14f, 0.30f, 0.62f, 0.96f),
                                  "CONNECT TO GATEWAY (192.168.43.1)", rgba(1.0f, 1.0f, 1.0f, 1.0f), 0.29f);
                    pushDeckHit(cX0 + 14.0f * uiScale, cBtnY, cX1 - 14.0f * uiScale, cBtnY + 38.0f * uiScale, HIT_NET_DIRECT_CONNECT);
                    (void)cw;
                });

                placeCard(1, 86.0f * uiScale, [&](float cX0, float cX1, float cw, float y, float ch) {
                    drawCard(cX0, y, cX1, y + ch, 18.0f * uiScale, cCardBgAlt, cCardBorderGold);
                    drawStatusChip(cX0 + 16.0f * uiScale, y + 14.0f * uiScale, "Galaxy Netplay Core v1.0",
                                   sc(rgba(0.38f, 0.28f, 0.08f, 0.95f)), sc(rgba(1.0f, 0.90f, 0.42f, 1.0f)), cGold);
                    drawText("Ultra-low latency UDP protocol • Auto LAN discovery", cX0 + 16.0f * uiScale, y + 54.0f * uiScale, 0.25f * uiScale, cTextSecondary, 0);
                    drawText("Both heroes enjoy synchronized 60 FPS cosmic gameplay.", cX0 + 16.0f * uiScale, y + 70.0f * uiScale, 0.24f * uiScale, sc(rgba(0.60f, 0.72f, 0.90f, 0.85f)), 0);
                    (void)cw;
                });
            } else {
                // =====================================================
                // System Tab: Plus (Tab 4) & Vanilla (Tab 2)
                // =====================================================
                if (!mPlusEdition) {
                    placeCard(0, 130.0f * uiScale, [&](float cX0, float cX1, float cw, float y, float ch) {
                        drawCard(cX0, y, cX1, y + ch, 18.0f * uiScale, cCardBg, cCardBorder);
                        drawSectionTitle(cX0, y, "GAME STORAGE & DUMP SETUP", cHeader, true);
                        bool hasData = (port_has_game_data() != 0);
                        if (hasData) {
                            drawStatusChip(cX0 + 14.0f * uiScale, y + 36.0f * uiScale, "Ready (/sdcard/GalaxyData)", sc(rgba(0.10f, 0.34f, 0.22f, 0.90f)), sc(rgba(0.55f, 0.96f, 0.65f, 1.0f)), sc(rgba(0.290f, 0.871f, 0.502f, 1.0f)));
                        } else {
                            drawStatusChip(cX0 + 14.0f * uiScale, y + 36.0f * uiScale, "Missing - Setup Required", sc(rgba(0.36f, 0.28f, 0.10f, 0.90f)), sc(rgba(1.0f, 0.88f, 0.42f, 1.0f)), cGold);
                        }
                        drawText("Select .rvz, .wbfs, .iso or .zip to install or re-cook.", cX0 + 14.0f * uiScale, y + 74.0f * uiScale, 0.25f * uiScale, cTextSecondary, 0);
                        float btnY = y + 84.0f * uiScale;
                        drawGlossyBtn(cX0 + 14.0f * uiScale, btnY, cX1 - 14.0f * uiScale, btnY + 36.0f * uiScale, 14.0f * uiScale,
                                      rgba(0.32f, 0.78f, 0.99f, 0.85f), rgba(0.06f, 0.38f, 0.86f, 0.98f),
                                      "OPEN GAME INSTALLER & COOKER", rgba(1.0f, 1.0f, 1.0f, 1.0f), 0.31f);
                        pushDeckHit(cX0 + 14.0f * uiScale, btnY, cX1 - 14.0f * uiScale, btnY + 36.0f * uiScale, HIT_OPEN_INSTALLER);
                        (void)cw;
                    });

                    placeCard(0, 130.0f * uiScale, [&](float cX0, float cX1, float cw, float y, float ch) {
                        drawCard(cX0, y, cX1, y + ch, 18.0f * uiScale, cCardBg, cCardBorderGold);
                        drawSectionTitle(cX0, y, "GALAXYMOBILE+ ALL-STAR EDITION", cGold, true);
                        bool plusInstalled = (port_is_plus_installed() != 0);
                        if (plusInstalled) {
                            drawStatusChip(cX0 + 14.0f * uiScale, y + 36.0f * uiScale, "Installed on Device", sc(rgba(0.10f, 0.34f, 0.22f, 0.90f)), sc(rgba(0.55f, 0.96f, 0.65f, 1.0f)), sc(rgba(0.290f, 0.871f, 0.502f, 1.0f)));
                        } else {
                            drawStatusChip(cX0 + 14.0f * uiScale, y + 36.0f * uiScale, "Bundled APK Ready (v0.7.8)", sc(rgba(0.36f, 0.28f, 0.10f, 0.90f)), sc(rgba(1.0f, 0.88f, 0.42f, 1.0f)), cGold);
                        }
                        drawText("Includes Cheats, Netplay Co-op, Power-Ups & Galaxy Warp.", cX0 + 14.0f * uiScale, y + 74.0f * uiScale, 0.25f * uiScale, cTextSecondary, 0);
                        float btnY = y + 84.0f * uiScale;
                        drawGlossyBtn(cX0 + 14.0f * uiScale, btnY, cX1 - 14.0f * uiScale, btnY + 36.0f * uiScale, 14.0f * uiScale,
                                      plusInstalled ? rgba(0.45f, 0.95f, 0.65f, 0.90f) : rgba(0.82f, 0.58f, 1.0f, 0.90f),
                                      plusInstalled ? rgba(0.12f, 0.52f, 0.28f, 0.98f) : rgba(0.40f, 0.20f, 0.72f, 0.98f),
                                      plusInstalled ? "OPEN GALAXYMOBILE+" : "INSTALL GALAXYMOBILE+", rgba(1.0f, 1.0f, 1.0f, 1.0f), 0.32f);
                        pushDeckHit(cX0 + 14.0f * uiScale, btnY, cX1 - 14.0f * uiScale, btnY + 36.0f * uiScale, HIT_INSTALL_PLUS);
                        (void)cw;
                    });

                    placeCard(0, 92.0f * uiScale, drawHapticsCard);
                    placeCard(1, 102.0f * uiScale, drawLangSliderCard);

                    placeCard(1, 88.0f * uiScale, [&](float cX0, float cX1, float cw, float y, float ch) {
                        drawCard(cX0, y, cX1, y + ch, 18.0f * uiScale, cCardBgAlt, cCardBorder);
                        drawStatusChip(cX0 + 16.0f * uiScale, y + 14.0f * uiScale, "GalaxyMobile v0.7.8", sc(rgba(0.10f, 0.28f, 0.46f, 0.92f)), sc(rgba(0.45f, 0.88f, 1.0f, 1.0f)), cHeader);
                        drawText("Vanilla Edition • Native ARM64 • GLES3 / Vulkan", cX0 + 16.0f * uiScale, y + 54.0f * uiScale, 0.26f * uiScale, cTextSecondary, 0);
                        drawText("Tuned for Android 11+ Snapdragon & Dimensity devices.", cX0 + 16.0f * uiScale, y + 72.0f * uiScale, 0.24f * uiScale, sc(rgba(0.60f, 0.72f, 0.90f, 0.85f)), 0);
                        (void)cw;
                    });

                    drawDonateAndLogCards(1);
                } else {
                    placeCard(0, 92.0f * uiScale, drawHapticsCard);
                    placeCard(0, 102.0f * uiScale, drawLangSliderCard);

                    placeCard(0, 96.0f * uiScale, [&](float cX0, float cX1, float cw, float y, float ch) {
                        drawCard(cX0, y, cX1, y + ch, 18.0f * uiScale, cCardBgAlt, cCardBorderGold);
                        drawStatusChip(cX0 + 16.0f * uiScale, y + 14.0f * uiScale, "GalaxyMobile+ v0.7.8", sc(rgba(0.38f, 0.28f, 0.08f, 0.95f)), sc(rgba(1.0f, 0.90f, 0.42f, 1.0f)), cGold);
                        drawText("All-Star Edition • Native ARM64 • GLES3 / Vulkan", cX0 + 16.0f * uiScale, y + 54.0f * uiScale, 0.26f * uiScale, cTextSecondary, 0);
                        drawText("Mods, Power-Ups, Warp & Netplay run live in-engine.", cX0 + 16.0f * uiScale, y + 72.0f * uiScale, 0.24f * uiScale, sc(rgba(0.60f, 0.72f, 0.90f, 0.85f)), 0);
                        (void)cw;
                    });

                    drawDonateAndLogCards(1);
                }
            }

            float totalContentH = (twoCol ? std::max(colY[0], colY[1]) : colY[0]) + 8.0f * uiScale;
            mSettingsMaxScroll = std::max(0.0f, totalContentH - visibleDeckH);
            mDrawerScrollY = std::max(0.0f, std::min(mDrawerScrollY, mSettingsMaxScroll));
            mDrawerScrollTarget = std::max(0.0f, std::min(mDrawerScrollTarget, mSettingsMaxScroll));

            if (mSettingsMaxScroll > 1.0f) {
                float thumbH = std::max(28.0f * uiScale, visibleDeckH * (visibleDeckH / totalContentH));
                float thumbY = mDeckTopY + (mDrawerScrollY / mSettingsMaxScroll) * (visibleDeckH - thumbH);
                drawRoundRect(deckX1 - 4.0f * uiScale, thumbY, deckX1, thumbY + thumbH, 2.0f * uiScale, rgba(0.45f, 0.85f, 1.0f, 0.48f * oa));
            }
        }

        // ---------------------------------------------------------
        // B. Fixed Header Backdrop & Bottom Frame Mask (Clips scrolled cards cleanly)
        // ---------------------------------------------------------
        addGradRect(0.0f, 0.0f, w, headerH,
                    rgba(0.022f, 0.042f, 0.155f, 0.99f * oa),
                    rgba(0.045f, 0.088f, 0.255f, 0.98f * oa));
        drawRoundRect(0.0f, headerH - 2.0f * uiScale, w, headerH, 0.0f, rgba(0.28f, 0.78f, 0.99f, 0.55f * oa));
        drawRoundRect(0.0f, headerH - 0.8f * uiScale, w, headerH, 0.0f, rgba(0.98f, 0.84f, 0.30f, 0.45f * oa));
        drawRoundRect(0.0f, mDeckBottomY, w, h, 0.0f, rgba(0.024f, 0.045f, 0.145f, 0.96f * oa));

        // ---------------------------------------------------------
        // C. Top Header Bar Controls (fixed == true)
        // ---------------------------------------------------------
        float doneW = 116.0f * uiScale;
        float doneX1 = w - 20.0f * uiScale;
        float doneX0 = doneX1 - doneW;
        drawGlossyBtn(doneX0, 15.0f * uiScale, doneX1, headerH - 15.0f * uiScale, 18.0f * uiScale,
                      rgba(0.42f, 0.86f, 1.0f, 0.95f), rgba(0.06f, 0.40f, 0.90f, 0.98f),
                      "Resume", rgba(1.0f, 1.0f, 1.0f, 1.0f), 0.36f);
        pushFixedHit(doneX0, 8.0f * uiScale, doneX1, headerH - 8.0f * uiScale, HIT_CLOSE);

        if (mWarpMenuOpen) {
            float backX0 = 18.0f * uiScale;
            float backX1 = backX0 + 104.0f * uiScale;
            drawGlossyBtn(backX0, 15.0f * uiScale, backX1, headerH - 15.0f * uiScale, 18.0f * uiScale,
                          rgba(0.40f, 0.78f, 0.98f, 0.75f), rgba(0.10f, 0.22f, 0.48f, 0.96f),
                          "< Back", rgba(0.92f, 0.96f, 1.0f, 1.0f), 0.33f);
            pushFixedHit(backX0, 8.0f * uiScale, backX1, headerH - 8.0f * uiScale, HIT_WARP_BACK);

            drawStar(backX1 + 26.0f * uiScale, headerH * 0.5f, 13.0f * uiScale, cGold, animPhase * 0.9f);
            drawText("Galaxy Warp", backX1 + 46.0f * uiScale, headerH * 0.5f + 7.0f * uiScale, 0.48f * uiScale, cTextPrimary, 0);

            // Top-right Launch shortcut in addition to Sidebar Launch Beacon
            float lW = 134.0f * uiScale;
            float lX1 = doneX0 - 12.0f * uiScale;
            float lX0 = lX1 - lW;
            drawGlossyBtn(lX0, 15.0f * uiScale, lX1, headerH - 15.0f * uiScale, 18.0f * uiScale,
                          rgba(1.0f, 0.88f, 0.32f, 0.96f), rgba(0.14f, 0.44f, 0.92f, 0.98f),
                          "Launch Warp", rgba(1.0f, 0.98f, 0.84f, 1.0f), 0.33f);
            pushFixedHit(lX0, 8.0f * uiScale, lX1, headerH - 8.0f * uiScale, HIT_WARP_LAUNCH);
        } else {
            // Spinning Power Star emblem + Simple GalaxyMobile / GalaxyMobile+ Title + v0.7.8 underneath
            float emblemX = 38.0f * uiScale;
            float emblemY = headerH * 0.5f;
            drawCircle(emblemX, emblemY, 17.0f * uiScale, rgba(1.0f, 0.84f, 0.24f, 0.16f * oa));
            drawStar(emblemX, emblemY, 15.0f * uiScale, cGold, sinf(animPhase * 1.2f) * 0.16f);
            const char* mainTitle = mPlusEdition ? "GalaxyMobile+" : "GalaxyMobile";
            drawText(mainTitle, emblemX + 26.0f * uiScale, headerH * 0.5f - 1.0f * uiScale, 0.50f * uiScale, cTextPrimary, 0);
            drawText("v0.7.8", emblemX + 27.0f * uiScale, headerH * 0.5f + 17.0f * uiScale, 0.29f * uiScale, mPlusEdition ? cGold : cHeader, 0);

            // Support Ko-fi pill button on top right
            float donateW = 92.0f * uiScale;
            float donateX1 = doneX0 - 12.0f * uiScale;
            float donateX0 = donateX1 - donateW;
            drawGlossyBtn(donateX0, 15.0f * uiScale, donateX1, headerH - 15.0f * uiScale, 18.0f * uiScale,
                          rgba(1.0f, 0.62f, 0.66f, 0.90f), rgba(0.84f, 0.22f, 0.28f, 0.96f),
                          "Ko-fi", rgba(1.0f, 1.0f, 1.0f, 1.0f), 0.34f);
            pushFixedHit(donateX0, 8.0f * uiScale, donateX1, headerH - 8.0f * uiScale, HIT_DONATE);
        }

        // ---------------------------------------------------------
        // D. Fixed Left Observatory Navigation Rail (fixed == true)
        // ---------------------------------------------------------
        drawCard(railX0, railTopY, railX1, railBotY, 20.0f * uiScale, sc(rgba(0.042f, 0.075f, 0.215f, 0.96f)), cCardBorder);

        float navX0 = railX0 + 10.0f * uiScale;
        float navX1 = railX1 - 10.0f * uiScale;

        // Animated SMG vertical pill indicator
        drawRoundRect(navX0, mTabIndicatorY, navX1, mTabIndicatorY + mTabIndicatorH, 13.0f * uiScale, sc(rgba(0.32f, 0.84f, 1.0f, 0.95f)));
        drawRoundRect(navX0 + 1.5f * uiScale, mTabIndicatorY + 1.5f * uiScale, navX1 - 1.5f * uiScale, mTabIndicatorY + mTabIndicatorH - 1.5f * uiScale, 11.5f * uiScale, sc(rgba(0.06f, 0.38f, 0.88f, 0.98f)));

        if (mWarpMenuOpen) {
            for (int c = 0; c < 8; c++) {
                float ty = railTopY + 14.0f * uiScale + c * (34.0f * uiScale);
                float th = 30.0f * uiScale;
                bool isSel = (mWarpCategoryIndex == c);
                if (isSel) {
                    drawStar(navX0 + 18.0f * uiScale, ty + th * 0.5f, 6.2f * uiScale, cGold, animPhase * 0.6f);
                } else {
                    drawSparkle(navX0 + 18.0f * uiScale, ty + th * 0.5f, 4.8f * uiScale, sc(rgba(0.45f, 0.70f, 0.95f, 0.65f)), 0.0f);
                }
                drawText(kCategoryNames[c], navX0 + 32.0f * uiScale, ty + 20.0f * uiScale, 0.32f * uiScale, isSel ? cTextPrimary : cTextSecondary, 0);
                pushFixedHit(navX0, ty, navX1, ty + th, HIT_WARP_CAT, c);
            }

            // Selected Galaxy & Launch Star Beacon Card pinned at bottom of Left Rail
            float launchTop = railBotY - 96.0f * uiScale;
            if (launchTop > railTopY + 14.0f * uiScale + 8 * 34.0f * uiScale + 8.0f * uiScale) {
                drawRoundRect(navX0, launchTop, navX1, railBotY - 12.0f * uiScale, 14.0f * uiScale, sc(rgba(0.06f, 0.12f, 0.34f, 0.94f)));
                drawText("DESTINATION", navX0 + 10.0f * uiScale, launchTop + 17.0f * uiScale, 0.22f * uiScale, cGold, 0);
                drawText(kGalaxies[mWarpSelectedGalaxy].displayName, navX0 + 10.0f * uiScale, launchTop + 33.0f * uiScale, 0.27f * uiScale, cTextPrimary, 0);
                char btnLbl[32];
                snprintf(btnLbl, sizeof(btnLbl), "LAUNCH STAR %d", mWarpSelectedScenario);
                drawGlossyBtn(navX0 + 6.0f * uiScale, launchTop + 42.0f * uiScale, navX1 - 6.0f * uiScale, railBotY - 18.0f * uiScale, 12.0f * uiScale,
                              rgba(1.0f, 0.88f, 0.32f, 0.96f), rgba(0.12f, 0.44f, 0.92f, 0.98f),
                              btnLbl, rgba(1.0f, 0.98f, 0.84f, 1.0f), 0.30f);
                pushFixedHit(navX0 + 6.0f * uiScale, launchTop + 42.0f * uiScale, navX1 - 6.0f * uiScale, railBotY - 18.0f * uiScale, HIT_WARP_LAUNCH);
            }
        } else {
            const char* tabTitles[5];
            const char* tabSubs[5];
            if (mPlusEdition) {
                tabTitles[0] = "Controls";       tabSubs[0] = "HUD, Sticks & Pointer";
                tabTitles[1] = "Display & FPS";  tabSubs[1] = "Resolution & Limiter";
                tabTitles[2] = "Cosmic Mods";    tabSubs[2] = "Cheats, Suits & Warp";
                tabTitles[3] = "Netplay";        tabSubs[3] = "Co-op & Co-Star Room";
                tabTitles[4] = "System";         tabSubs[4] = "Language, Haptics & Log";
            } else {
                tabTitles[0] = "Controls";       tabSubs[0] = "HUD, Sticks & Pointer";
                tabTitles[1] = "Display & FPS";  tabSubs[1] = "Resolution & Limiter";
                tabTitles[2] = "System";         tabSubs[2] = "Dump, Plus+, Lang & Log";
            }

            for (int t = 0; t < numTabs; t++) {
                float ty = railTopY + 14.0f * uiScale + t * (53.0f * uiScale);
                float th = 46.0f * uiScale;
                bool isSel = (mSettingsTab == t);
                if (isSel) {
                    drawStar(navX0 + 20.0f * uiScale, ty + th * 0.5f, 8.2f * uiScale, cGold, sinf(animPhase * 1.6f) * 0.14f);
                } else {
                    drawSparkle(navX0 + 20.0f * uiScale, ty + th * 0.5f, 6.2f * uiScale, sc(rgba(0.45f, 0.72f, 0.98f, 0.70f)), 0.0f);
                }
                drawText(tabTitles[t], navX0 + 36.0f * uiScale, ty + 21.0f * uiScale, 0.34f * uiScale, isSel ? cTextPrimary : sc(rgba(0.82f, 0.88f, 0.98f, 0.88f)), 0);
                drawText(tabSubs[t], navX0 + 36.0f * uiScale, ty + 36.0f * uiScale, 0.22f * uiScale, isSel ? rgba(0.84f, 0.94f, 1.0f, 0.95f * oa) : cTextSecondary, 0);
                pushFixedHit(navX0, ty, navX1, ty + th, HIT_TAB, t);
            }

            // Live Observatory Telemetry Summary Box at bottom of Left Sidebar
            float tabsBottom = railTopY + 14.0f * uiScale + numTabs * (53.0f * uiScale);
            float teleTop = railBotY - 122.0f * uiScale;
            if (teleTop > tabsBottom + 10.0f * uiScale) {
                drawRoundRect(navX0, teleTop, navX1, railBotY - 12.0f * uiScale, 14.0f * uiScale, sc(rgba(0.06f, 0.11f, 0.30f, 0.90f)));
                drawText("OBSERVATORY TELEMETRY", navX0 + 10.0f * uiScale, teleTop + 18.0f * uiScale, 0.22f * uiScale, cHeader, 0);

                char fpsLine[40];
                if (mDisplayedFps >= 0) snprintf(fpsLine, sizeof(fpsLine), "Live FPS: %d / %d", mDisplayedFps, std::max(30, std::min(60, mSettings.fpsLimit)));
                else snprintf(fpsLine, sizeof(fpsLine), "FPS Cap: %d Hz", std::max(30, std::min(60, mSettings.fpsLimit)));
                drawText(fpsLine, navX0 + 10.0f * uiScale, teleTop + 40.0f * uiScale, 0.25f * uiScale, cTextPrimary, 0);

                static const char* kShortRes[5] = { "720p HD", "900p HD+", "1080p FHD", "Ultra Native", "480p Wii" };
                int gp = std::max(0, std::min(4, mSettings.graphicsPreset));
                char resLine[40];
                snprintf(resLine, sizeof(resLine), "3D Target: %s", kShortRes[gp]);
                drawText(resLine, navX0 + 10.0f * uiScale, teleTop + 60.0f * uiScale, 0.25f * uiScale, cTextSecondary, 0);

                const char* heroLine = mPlusEdition ? ((mSettings.playerCharacter != 0) ? "Hero: Luigi" : "Hero: Mario")
                                                    : (mSettings.hdUiTextures ? "HUD: 3D All-Stars HD" : "HUD: Wii Classic");
                drawText(heroLine, navX0 + 10.0f * uiScale, teleTop + 80.0f * uiScale, 0.25f * uiScale, cGold, 0);

                const char* netLine = (port_net_is_connected() != 0) ? "Netplay: Connected"
                                    : (port_net_get_role() == 1 ? "Netplay: Hosting" : (mSettings.fullscreen ? "View: Ultrawide Hor+" : "View: 16:9 Framed"));
                drawText(netLine, navX0 + 10.0f * uiScale, teleTop + 98.0f * uiScale, 0.24f * uiScale, cTextSecondary, 0);
            }
        }
    }

    if (verts.empty()) return;

    // -------------------------------------------------------------
    // Single GLES Draw Call
    // -------------------------------------------------------------
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glUseProgram(mProg);

    float proj[16] = {
        2.0f / w, 0.0f,      0.0f, 0.0f,
        0.0f,     -2.0f / h, 0.0f, 0.0f,
        0.0f,     0.0f,     -1.0f, 0.0f,
        -1.0f,    1.0f,      0.0f, 1.0f
    };
    GLint uProjLoc = glGetUniformLocation(mProg, "uProj");
    glUniformMatrix4fv(uProjLoc, 1, GL_FALSE, proj);

    GLint uFontLoc = glGetUniformLocation(mProg, "uFontTex");
    glUniform1i(uFontLoc, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindSampler(0, 0);
    glBindTexture(GL_TEXTURE_2D, mFontTex);

    glBindVertexArray(mVao);
    glBindBuffer(GL_ARRAY_BUFFER, mVbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(UIVertex), verts.data(), GL_STREAM_DRAW);

    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)verts.size());

    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

}  // namespace mobile_ui
