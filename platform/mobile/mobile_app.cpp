// Flat-screen Android Mobile application layer (AOSP / Phones).
// Replaces the OpenXR bootstrap with ANativeWindow + EGL surface rendering
// and standard monoscopic presentation.

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/sensor.h>
#include <android_native_app_glue.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "gx/gl_renderer.h"
#include "mobile_ui.h"
#include "mobile_haptics.h"
#include "port/input.h"
#include "port/port.h"
#include "port/net.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "PetariVR", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "PetariVR", __VA_ARGS__)

extern "C" void port_mem_set_reserved_window(uintptr_t base, size_t size);
extern "C" void port_boot(const char* dataRoot, const char* saveRoot);
extern std::atomic<uint64_t> gPresentedFrames;

namespace {

struct MobileApp {
    android_app* android = nullptr;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    EGLConfig config = nullptr;

    bool windowReady = false;
    bool eglReady = false;
    int screenWidth = 0;
    int screenHeight = 0;

    gpu::EfbTarget efbTarget;
    bool targetReady = false;

    bool gameBooted = false;
    std::string dataRoot;
    std::string saveRoot;

    // Viewport calculation for 16:9 pillarbox/letterbox
    int vpX = 0, vpY = 0, vpW = 0, vpH = 0;

    // Input state
    PortPadState pad{};
    PortPadState touchPad{};
    uint32_t gamepadKeyButtons = 0;
    uint32_t gamepadAxisButtons = 0;
    float gamepadStickX = 0.0f;
    float gamepadStickY = 0.0f;
    float gamepadRightStickX = 0.0f;
    float gamepadRightStickY = 0.0f;
    uint32_t gamepadCameraPulsedButtons = 0;
    int camRepeatLeft = 0;
    int camRepeatRight = 0;
    int32_t lastGamepadDeviceId = -1;
    bool gamepadHasRxRy = false;
    bool gamepadZIsTrigger = false;
    bool gamepadSawCenterZ = false;
    int64_t lastPointerTickNs = 0;
    bool touchActive = false;
    float touchX = 0.0f, touchY = 0.0f;

    // Virtual touch controls state
    int stickPointerId = -1;
    float stickOriginX = 0.0f;
    float stickOriginY = 0.0f;

    // Tilt / gyro sensor state (only enabled in gyro-required Galaxies)
    ASensorManager* sensorManager = nullptr;
    const ASensor* tiltSensor = nullptr;
    ASensorEventQueue* sensorQueue = nullptr;
    bool tiltSensorUsingRawAccel = false;
    bool tiltSensorEnabled = false;
    int displayRotation = 1;  // Surface.ROTATION_90 default for landscape
    bool hasGravity = false;
    float filtDownX = 0.0f;
    float filtDownY = 0.0f;
    float filtDownZ = 0.0f;
    bool neutralCalibrated = false;
    int calSamples = 0;
    float calSumY = 0.0f;
    float calSumZ = 0.0f;
    float neutralCos = 1.0f;
    float neutralSin = 0.0f;
    float outDownX = 0.0f;
    float outDownY = -1.0f;
    float outDownZ = 0.0f;
};

MobileApp gMobile;

bool isDir(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool checkGameFolder(const std::string& dir, bool* ready) {
    struct stat st;
    if (stat((dir + "/sys/fst.bin").c_str(), &st) != 0 || !S_ISREG(st.st_mode) || !isDir(dir + "/files")) {
        return false;
    }
    char magic[4] = {};
    FILE* f = fopen((dir + "/files/ObjectData/Coin.arc").c_str(), "rb");
    bool converted = f && fread(magic, 1, 4, f) == 4 && memcmp(magic, "CRAR", 4) == 0;
    if (f) fclose(f);

    bool dolData = true;
    for (const char* name : {"ErrorMessageArchive.arc", "StoryEvent.bcsv", "GalaxyID.bcsv"}) {
        dolData = dolData && stat((dir + "/sys/" + name).c_str(), &st) == 0 && S_ISREG(st.st_mode);
    }
    bool known = port_dvd_identify(dir.c_str()) != nullptr;
    if (ready) *ready = converted && dolData && known;
    return true;
}

std::string findGameData(const std::string& extDir) {
    std::vector<std::string> candidates = {
        "/sdcard/GalaxyData",
        "/sdcard/GalaxyData/game",
        "/storage/emulated/0/GalaxyData",
        "/storage/emulated/0/GalaxyData/game",
        extDir + "/game",
        "/sdcard/Android/data/com.galaxy.mobile.plus/files/game",
        "/sdcard/Android/data/com.galaxy.mobile/files/game",
        "/sdcard/Android/data/com.galaxy.quest/files/game",
        "/sdcard/Galaxy/game",
        "/sdcard/GalaxyQuest",
        "/sdcard/GalaxyQuest/game",
        "/sdcard/Download/game",
        "/sdcard/Download/cooked",
        "/sdcard/Download/GalaxyQuest",
        "/sdcard/Documents/game",
        "/sdcard/Documents/cooked",
        "/sdcard/game",
        "/sdcard/cooked",
        "/data/local/tmp/petari/game"
    };

    if (const char* envRoot = getenv("PETARI_GAME_ROOT")) {
        candidates.insert(candidates.begin(), envRoot);
    }

    for (const auto& path : candidates) {
        bool ready = false;
        if (checkGameFolder(path, &ready) && ready) {
            LOGI("Found valid game data at: %s", path.c_str());
            return path;
        }
    }
    return "";
}

void promptStoragePermissionIfMissing(ANativeActivity* activity) {
    if (!activity || !activity->vm || !activity->clazz) return;
    JNIEnv* env = nullptr;
    if (activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return;

    jclass versionClass = env->FindClass("android/os/Build$VERSION");
    if (!versionClass) return;
    jfieldID sdkIntField = env->GetStaticFieldID(versionClass, "SDK_INT", "I");
    jint sdkInt = env->GetStaticIntField(versionClass, sdkIntField);

    if (sdkInt >= 30) {
        jclass envClass = env->FindClass("android/os/Environment");
        if (envClass) {
            jmethodID isManagerMethod = env->GetStaticMethodID(envClass, "isExternalStorageManager", "()Z");
            if (isManagerMethod) {
                jboolean isManager = env->CallStaticBooleanMethod(envClass, isManagerMethod);
                if (!isManager) {
                    LOGI("MANAGE_EXTERNAL_STORAGE permission missing, launching settings prompt...");
                    jclass intentClass = env->FindClass("android/content/Intent");
                    jmethodID intentCtor = env->GetMethodID(intentClass, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V");

                    jclass uriClass = env->FindClass("android/net/Uri");
                    jmethodID parseMethod = env->GetStaticMethodID(uriClass, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");

                    jclass actClass = env->GetObjectClass(activity->clazz);
                    jmethodID getPackageNameMethod = env->GetMethodID(actClass, "getPackageName", "()Ljava/lang/String;");
                    jstring pkgName = (jstring)env->CallObjectMethod(activity->clazz, getPackageNameMethod);

                    const char* pkgChars = env->GetStringUTFChars(pkgName, nullptr);
                    std::string uriStr = std::string("package:") + (pkgChars ? pkgChars : "");
                    if (pkgChars) env->ReleaseStringUTFChars(pkgName, pkgChars);

                    jstring jUriStr = env->NewStringUTF(uriStr.c_str());
                    jobject jUri = env->CallStaticObjectMethod(uriClass, parseMethod, jUriStr);

                    jstring actionStr = env->NewStringUTF("android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION");
                    jobject intent = env->NewObject(intentClass, intentCtor, actionStr, jUri);

                    jmethodID startActivityMethod = env->GetMethodID(actClass, "startActivity", "(Landroid/content/Intent;)V");
                    env->CallVoidMethod(activity->clazz, startActivityMethod, intent);

                    env->DeleteLocalRef(actionStr);
                    env->DeleteLocalRef(jUriStr);
                    env->DeleteLocalRef(jUri);
                    env->DeleteLocalRef(intent);
                }
            }
        }
    }
}

bool initEgl(MobileApp& a) {
    if (a.eglReady) return true;

    a.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (a.display == EGL_NO_DISPLAY || !eglInitialize(a.display, nullptr, nullptr)) {
        LOGE("eglInitialize failed");
        return false;
    }

    const EGLint cfgAttr[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };

    EGLint count = 0;
    if (!eglChooseConfig(a.display, cfgAttr, &a.config, 1, &count) || count == 0) {
        LOGE("eglChooseConfig failed");
        return false;
    }

    const EGLint ctxAttr[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 2,
        EGL_NONE
    };

    a.context = eglCreateContext(a.display, a.config, EGL_NO_CONTEXT, ctxAttr);
    if (a.context == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed (0x%x)", eglGetError());
        return false;
    }

    a.eglReady = true;
    return true;
}

int getDesiredTargetHeight(const MobileApp& a) {
    if (!mobile_ui::UIOverlay::get().isPlusEdition()) {
        return (a.screenHeight > 1080) ? std::min(a.screenHeight, 1440) : 1080;
    }
    int preset = mobile_ui::UIOverlay::get().settings().graphicsPreset;
    switch (preset) {
        case 0: return 720;                                                   // Low: 720p HD
        case 1: return 900;                                                   // Medium: 900p HD+
        case 3: return std::max(1080, a.screenHeight);                        // Ultra: Native (QHD+/1440p+)
        case 2:                                                               // High: 1080p / 1440p
        default:
            return (a.screenHeight > 1080) ? std::min(a.screenHeight, 1440) : 1080;
    }
}

void updateViewport(MobileApp& a);

static void presentGameImage(MobileApp& a) {
    updateViewport(a);
    // Direct high-quality blit
    glBindFramebuffer(GL_READ_FRAMEBUFFER, a.efbTarget.fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, a.efbTarget.width, a.efbTarget.height,
                      a.vpX, a.vpY, a.vpX + a.vpW, a.vpY + a.vpH,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
}

void updateViewport(MobileApp& a) {
    if (mobile_ui::get().settings().fullscreen) {
        a.vpX = 0;
        a.vpY = 0;
        a.vpW = a.screenWidth;
        a.vpH = a.screenHeight;
    } else {
        float targetAspect = 16.0f / 9.0f;
        float screenAspect = (float)a.screenWidth / (float)a.screenHeight;
        if (screenAspect > targetAspect) {
            a.vpH = a.screenHeight;
            a.vpW = (int)lroundf(a.screenHeight * targetAspect);
            a.vpX = (a.screenWidth - a.vpW) / 2;
            a.vpY = 0;
        } else {
            a.vpW = a.screenWidth;
            a.vpH = (int)lroundf(a.screenWidth / targetAspect);
            a.vpX = 0;
            a.vpY = (a.screenHeight - a.vpH) / 2;
        }
    }
}

bool createSurface(MobileApp& a) {
    if (!a.android->window || !a.eglReady) return false;

    EGLint format = 0;
    eglGetConfigAttrib(a.display, a.config, EGL_NATIVE_VISUAL_ID, &format);
    ANativeWindow_setBuffersGeometry(a.android->window, 0, 0, format);

    a.surface = eglCreateWindowSurface(a.display, a.config, a.android->window, nullptr);
    if (a.surface == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface failed (0x%x)", eglGetError());
        return false;
    }

    if (!eglMakeCurrent(a.display, a.surface, a.surface, a.context)) {
        LOGE("eglMakeCurrent failed (0x%x)", eglGetError());
        return false;
    }

    eglQuerySurface(a.display, a.surface, EGL_WIDTH, &a.screenWidth);
    eglQuerySurface(a.display, a.surface, EGL_HEIGHT, &a.screenHeight);
    eglSwapInterval(a.display, 1);

    LOGI("Window surface ready: %dx%d (GL: %s)", a.screenWidth, a.screenHeight, (const char*)glGetString(GL_RENDERER));

    updateViewport(a);
    LOGI("Viewport: x=%d, y=%d, w=%d, h=%d (fullscreen=%d)", a.vpX, a.vpY, a.vpW, a.vpH, mobile_ui::get().settings().fullscreen ? 1 : 0);

    // Initialize renderer and render target
    gpu::Renderer& r = gpu::renderer();
    if (!r.init()) {
        LOGE("gpu::renderer().init() failed");
        return false;
    }

    // Allocate offscreen target (preset & aspect-matched when in fullscreen mode)
    int targetH = getDesiredTargetHeight(a);
    int targetW;
    if (mobile_ui::get().settings().fullscreen) {
        float nativeAspect = (a.vpH > 0) ? ((float)a.vpW / (float)a.vpH) : ((float)a.screenWidth / (float)a.screenHeight);
        targetW = (int)lroundf(targetH * nativeAspect);
    } else {
        targetW = (targetH * 16) / 9;
    }
    if (!a.targetReady || a.efbTarget.width != targetW || a.efbTarget.height != targetH) {
        if (a.targetReady) {
            r.destroyTarget(a.efbTarget);
        }
        a.efbTarget = r.createTarget(targetW, targetH);
        a.targetReady = true;
        LOGI("Created offscreen EfbTarget: %dx%d (fullscreen=%d)", targetW, targetH, mobile_ui::get().settings().fullscreen ? 1 : 0);
    }

    a.windowReady = true;
    return true;
}

void destroySurface(MobileApp& a) {
    if (a.display != EGL_NO_DISPLAY) {
        eglMakeCurrent(a.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (a.surface != EGL_NO_SURFACE) {
            eglDestroySurface(a.display, a.surface);
            a.surface = EGL_NO_SURFACE;
        }
    }
    a.windowReady = false;
}

int queryDisplayRotation(android_app* app) {
    if (!app || !app->activity || !app->activity->vm || !app->activity->clazz) {
        return 1;
    }
    JNIEnv* env = nullptr;
    bool attached = false;
    if (app->activity->vm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (app->activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) {
            return 1;
        }
        attached = true;
    }
    int rot = 1;
    if (env) {
        jclass actCls = env->GetObjectClass(app->activity->clazz);
        if (actCls) {
            jmethodID getWm = env->GetMethodID(actCls, "getWindowManager", "()Landroid/view/WindowManager;");
            if (getWm) {
                jobject wm = env->CallObjectMethod(app->activity->clazz, getWm);
                if (wm) {
                    jclass wmCls = env->GetObjectClass(wm);
                    jmethodID getDisp = wmCls ? env->GetMethodID(wmCls, "getDefaultDisplay", "()Landroid/view/Display;") : nullptr;
                    if (getDisp) {
                        jobject disp = env->CallObjectMethod(wm, getDisp);
                        if (disp) {
                            jclass dispCls = env->GetObjectClass(disp);
                            jmethodID getRot = dispCls ? env->GetMethodID(dispCls, "getRotation", "()I") : nullptr;
                            if (getRot) {
                                rot = (int)env->CallIntMethod(disp, getRot);
                            }
                            if (dispCls) env->DeleteLocalRef(dispCls);
                            env->DeleteLocalRef(disp);
                        }
                    }
                    if (wmCls) env->DeleteLocalRef(wmCls);
                    env->DeleteLocalRef(wm);
                }
            }
            env->DeleteLocalRef(actCls);
        }
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
    }
    if (attached) {
        app->activity->vm->DetachCurrentThread();
    }
    return (rot >= 0 && rot <= 3) ? rot : 1;
}

void initTiltSensor(MobileApp& a) {
    if (a.sensorManager || !a.android || !a.android->looper) {
        return;
    }
    const char* pkg = mobile_ui::get().isPlusEdition() ? "com.galaxy.mobile.plus" : "com.galaxy.mobile";
    a.sensorManager = ASensorManager_getInstanceForPackage(pkg);
    if (!a.sensorManager) {
        return;
    }
    a.tiltSensor = ASensorManager_getDefaultSensor(a.sensorManager, ASENSOR_TYPE_GRAVITY);
    a.tiltSensorUsingRawAccel = false;
    if (!a.tiltSensor) {
        a.tiltSensor = ASensorManager_getDefaultSensor(a.sensorManager, ASENSOR_TYPE_ACCELEROMETER);
        a.tiltSensorUsingRawAccel = true;
    }
    if (!a.tiltSensor) {
        LOGI("Tilt sensor: no gravity or accelerometer sensor available");
        return;
    }
    a.sensorQueue = ASensorManager_createEventQueue(a.sensorManager, a.android->looper, LOOPER_ID_USER, nullptr, nullptr);
    if (!a.sensorQueue) {
        a.tiltSensor = nullptr;
        LOGE("Tilt sensor: failed to create ASensorEventQueue");
    } else {
        LOGI("Tilt sensor initialized (%s)", a.tiltSensorUsingRawAccel ? "accelerometer" : "fused gravity");
    }
}

void setTiltSensorEnabled(MobileApp& a, bool enable) {
    if (enable) {
        initTiltSensor(a);
        if (!a.sensorQueue || !a.tiltSensor || a.tiltSensorEnabled) {
            return;
        }
        a.displayRotation = queryDisplayRotation(a.android);
        if (ASensorEventQueue_enableSensor(a.sensorQueue, a.tiltSensor) == 0) {
            ASensorEventQueue_setEventRate(a.sensorQueue, a.tiltSensor, 16666);  // ~60 Hz
            a.tiltSensorEnabled = true;
            a.hasGravity = false;
            a.neutralCalibrated = false;
            a.calSamples = 0;
            a.calSumY = 0.0f;
            a.calSumZ = 0.0f;
            a.neutralCos = 1.0f;
            a.neutralSin = 0.0f;
            a.outDownX = 0.0f;
            a.outDownY = -1.0f;
            a.outDownZ = 0.0f;
            LOGI("Tilt sensor enabled (rotation=%d)", a.displayRotation);
        }
    } else if (a.tiltSensorEnabled) {
        if (a.sensorQueue && a.tiltSensor) {
            ASensorEventQueue_disableSensor(a.sensorQueue, a.tiltSensor);
        }
        a.tiltSensorEnabled = false;
        a.hasGravity = false;
        a.neutralCalibrated = false;
        LOGI("Tilt sensor disabled");
    }
}

void destroyTiltSensor(MobileApp& a) {
    setTiltSensorEnabled(a, false);
    if (a.sensorManager && a.sensorQueue) {
        ASensorManager_destroyEventQueue(a.sensorManager, a.sensorQueue);
    }
    a.sensorQueue = nullptr;
    a.tiltSensor = nullptr;
    a.sensorManager = nullptr;
}

void drainTiltSensorEvents(MobileApp& a) {
    if (!a.sensorQueue) {
        return;
    }
    ASensorEvent events[16];
    ssize_t count = 0;
    const float alpha = a.tiltSensorUsingRawAccel ? 0.22f : 0.45f;
    while ((count = ASensorEventQueue_getEvents(a.sensorQueue, events, 16)) > 0) {
        if (!a.tiltSensorEnabled) {
            continue;
        }
        for (ssize_t i = 0; i < count; i++) {
            // Android sensors measure upward reaction force (-gravity); negate for world down.
            float dx = -events[i].vector.x;
            float dy = -events[i].vector.y;
            float dz = -events[i].vector.z;
            float mag = sqrtf(dx * dx + dy * dy + dz * dz);
            if (mag < 0.5f) {
                continue;
            }
            dx /= mag;
            dy /= mag;
            dz /= mag;
            if (!a.hasGravity) {
                a.filtDownX = dx;
                a.filtDownY = dy;
                a.filtDownZ = dz;
                a.hasGravity = true;
            } else {
                a.filtDownX += alpha * (dx - a.filtDownX);
                a.filtDownY += alpha * (dy - a.filtDownY);
                a.filtDownZ += alpha * (dz - a.filtDownZ);
                float fmag = sqrtf(a.filtDownX * a.filtDownX + a.filtDownY * a.filtDownY + a.filtDownZ * a.filtDownZ);
                if (fmag > 0.001f) {
                    a.filtDownX /= fmag;
                    a.filtDownY /= fmag;
                    a.filtDownZ /= fmag;
                }
            }
        }
    }
}

void updateTiltSensor(MobileApp& a) {
    bool active = (port_input_is_tilt_active() != 0);
    if (active != a.tiltSensorEnabled) {
        setTiltSensorEnabled(a, active);
    }
    drainTiltSensorEvents(a);
    if (!active || !a.tiltSensorEnabled || !a.sensorQueue || !a.hasGravity) {
        a.pad.tilted = 0;
        a.pad.downX = 0.0f;
        a.pad.downY = -1.0f;
        a.pad.downZ = 0.0f;
        return;
    }

    // Remap device gravity vector to screen coordinates (Right, Up, Out toward face).
    float downR = 0.0f, downU = 0.0f, downO = a.filtDownZ;
    switch (a.displayRotation) {
    case 0:  // ROTATION_0
        downR = a.filtDownX;
        downU = a.filtDownY;
        break;
    case 2:  // ROTATION_180
        downR = -a.filtDownX;
        downU = -a.filtDownY;
        break;
    case 3:  // ROTATION_270 (reverse landscape)
        downR = a.filtDownY;
        downU = -a.filtDownX;
        break;
    case 1:  // ROTATION_90 (standard landscape)
    default:
        downR = -a.filtDownY;
        downU = a.filtDownX;
        break;
    }

    // Map screen (Right, Out, Up) to PortPadState controller down (right, up, forward).
    float cx = downR;
    float cy = downO;
    float cz = downU;

    // Capture the user's comfortable holding pitch on entry to the gyro Galaxy so
    // their resting hand angle becomes neutral (0, -1, 0) without affecting roll steering.
    if (!a.neutralCalibrated) {
        a.calSumY += cy;
        a.calSumZ += cz;
        a.calSamples++;
        if (a.calSamples >= 3) {
            float avgY = a.calSumY / (float)a.calSamples;
            float avgZ = a.calSumZ / (float)a.calSamples;
            float yzLen = sqrtf(avgY * avgY + avgZ * avgZ);
            if (yzLen > 0.25f && avgY <= 0.1f) {
                avgY /= yzLen;
                avgZ /= yzLen;
                a.neutralCos = -avgY;
                a.neutralSin = avgZ;
            } else {
                a.neutralCos = 1.0f;
                a.neutralSin = 0.0f;
            }
            a.neutralCalibrated = true;
        }
    }

    a.outDownX = cx;
    a.outDownY = cy * a.neutralCos - cz * a.neutralSin;
    a.outDownZ = cz * a.neutralCos + cy * a.neutralSin;

    a.pad.tilted = 1;
    a.pad.downX = a.outDownX;
    a.pad.downY = a.outDownY;
    a.pad.downZ = a.outDownZ;
}

void syncMergedPad(MobileApp& a) {
    a.pad.connected = 1;
    if (fabsf(a.gamepadStickX) > fabsf(a.touchPad.stickX) ||
        fabsf(a.gamepadStickY) > fabsf(a.touchPad.stickY)) {
        a.pad.stickX = a.gamepadStickX;
        a.pad.stickY = a.gamepadStickY;
    } else {
        a.pad.stickX = a.touchPad.stickX;
        a.pad.stickY = a.touchPad.stickY;
    }

    uint32_t rawButtons = a.touchPad.buttons | a.gamepadKeyButtons | a.gamepadAxisButtons;
    uint32_t nonCamButtons = rawButtons & ~(0x0001u | 0x0002u);

    // Right Stick Vertical: Up for First-Person, Down for Center Camera & Exit FP (Camera mode only)
    if (!mobile_ui::get().settings().rightStickPointer) {
        if (a.gamepadRightStickY < -0.45f) {
            nonCamButtons |= 0x0008; // WPAD_BUTTON_UP
        } else if (a.gamepadRightStickY > 0.45f) {
            nonCamButtons |= (0x4000 | 0x0004); // WPAD_BUTTON_C | WPAD_BUTTON_DOWN
        }
    }

    a.pad.buttons = nonCamButtons | a.gamepadCameraPulsedButtons;

    a.pad.pointerX = a.touchPad.pointerX;
    a.pad.pointerY = a.touchPad.pointerY;
    a.pad.pointerValid = a.touchPad.pointerValid;
    a.pad.pointerDist = a.touchPad.pointerDist;
}

void updateRightStickPointer(MobileApp& a, int64_t nowNs) {
    if (a.lastPointerTickNs == 0) {
        a.lastPointerTickNs = nowNs;
        return;
    }
    float dt = (float)(nowNs - a.lastPointerTickNs) * 1e-9f;
    a.lastPointerTickNs = nowNs;
    if (dt <= 0.0f) {
        return;
    }
    if (dt > 0.05f) {
        dt = 0.05f;
    }
    if (!mobile_ui::get().settings().rightStickPointer || mobile_ui::get().isSettingsOpen()) {
        return;
    }

    float rx = a.gamepadRightStickX;
    float ry = a.gamepadRightStickY;
    float mag = sqrtf(rx * rx + ry * ry);
    constexpr float kDeadzone = 0.15f;
    if (mag <= kDeadzone) {
        return;
    }

    float clampedMag = std::min(1.0f, mag);
    float norm = (clampedMag - kDeadzone) / (1.0f - kDeadzone);
    float curved = norm * (0.45f + 0.55f * norm);
    float dirX = rx / mag;
    float dirY = ry / mag;

    // Normalized [-1, 1] pointer space is 16:9, so scale vertical speed for uniform visual motion.
    constexpr float kSpeedX = 2.50f;
    constexpr float kSpeedY = 2.50f * (16.0f / 9.0f);
    a.touchPad.pointerX = std::max(-1.0f, std::min(1.0f, a.touchPad.pointerX + dirX * curved * kSpeedX * dt));
    a.touchPad.pointerY = std::max(-1.0f, std::min(1.0f, a.touchPad.pointerY + dirY * curved * kSpeedY * dt));
    a.touchPad.pointerValid = 1;
    a.touchPad.pointerDist = 2.0f;
    mobile_ui::get().setPointer(a.touchPad.pointerX, a.touchPad.pointerY);
}

int32_t onInput(android_app* app, AInputEvent* event) {
    MobileApp* a = (MobileApp*)app->userData;
    if (!a) return 0;

    int32_t eventType = AInputEvent_getType(event);
    if (eventType == AINPUT_EVENT_TYPE_MOTION) {
        int32_t source = AInputEvent_getSource(event);
        bool isPointer = (source & AINPUT_SOURCE_CLASS_POINTER) != 0;

        // Physical gamepad analog sticks, D-Pad hat axes, and analog triggers
        if (!isPointer) {
            float stickX = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_X, 0);
            float stickY = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, 0);
            if (fabsf(stickX) > 0.15f || fabsf(stickY) > 0.15f) {
                a->gamepadStickX = stickX;
                a->gamepadStickY = -stickY;
            } else {
                a->gamepadStickX = 0.0f;
                a->gamepadStickY = 0.0f;
            }

            uint32_t axisBtns = 0;

            // D-Pad Hat Axes (standard on Xbox, DualShock/DualSense, Switch Pro, 8BitDo, Razer Kishi)
            float hatX = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_X, 0);
            float hatY = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_Y, 0);
            if (hatX < -0.45f) axisBtns |= 0x0001; // D-Pad Left (Camera Orbit Left)
            if (hatX >  0.45f) axisBtns |= 0x0002; // D-Pad Right (Camera Orbit Right)
            if (hatY < -0.45f) axisBtns |= 0x0008; // D-Pad Up (First-Person Camera)
            if (hatY >  0.45f) axisBtns |= (0x4000 | 0x0004); // D-Pad Down (Center Camera & Exit FP)

            int32_t devId = AInputEvent_getDeviceId(event);
            if (devId != a->lastGamepadDeviceId) {
                a->lastGamepadDeviceId = devId;
                a->gamepadHasRxRy = false;
                a->gamepadZIsTrigger = false;
                a->gamepadSawCenterZ = false;
            }

            // Right Stick Control (Star Pointer or Camera)
            float valZ  = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Z, 0);
            float valRz = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RZ, 0);
            float valRx = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RX, 0);
            float valRy = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RY, 0);
            float rawLTrig = std::max(
                AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_LTRIGGER, 0),
                AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_BRAKE, 0));
            float rawRTrig = std::max(
                AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RTRIGGER, 0),
                AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_GAS, 0));

            bool rxMirrorsTrigger = (rawLTrig > 0.05f && fabsf(valRx - rawLTrig) < 0.02f) ||
                                    (rawRTrig > 0.05f && fabsf(valRy - rawRTrig) < 0.02f);
            if (!rxMirrorsTrigger && (fabsf(valRx) > 0.12f || fabsf(valRy) > 0.12f)) {
                a->gamepadHasRxRy = true;
            }
            if (rawLTrig > 0.02f || rawRTrig > 0.02f || (fabsf(valZ) < 0.35f && fabsf(valRz) < 0.35f)) {
                a->gamepadSawCenterZ = true;
            }
            if (!a->gamepadSawCenterZ && (valZ <= -0.90f && valRz <= -0.90f)) {
                a->gamepadZIsTrigger = true;
            }

            // Legacy HID controllers map Right Stick to RX/RY and rest L2/R2 on Z/RZ at -1.0f.
            bool zIsTrigger = a->gamepadZIsTrigger || (a->gamepadHasRxRy && (valZ < -0.5f || valRz < -0.5f));

            float rightX = 0.0f;
            float rightY = 0.0f;

            if (!zIsTrigger && (fabsf(valZ) > 0.10f || fabsf(valRz) > 0.10f)) {
                // AXIS_Z / AXIS_RZ is active right stick (standard AOSP)
                rightX = valZ;
                rightY = valRz;
            } else if (!rxMirrorsTrigger && (fabsf(valRx) > 0.10f || fabsf(valRy) > 0.10f)) {
                // AXIS_RX / AXIS_RY is active right stick
                rightX = valRx;
                rightY = valRy;
            }

            a->gamepadRightStickX = rightX;
            a->gamepadRightStickY = rightY;

            // Analog Triggers (L2/LT = Crouch/Ground Pound, R2/RT = Spin)
            float lTrig = std::max(
                rawLTrig,
                (zIsTrigger && valZ > -0.9f) ? ((valZ + 1.0f) * 0.5f) : 0.0f
            );
            float rTrig = std::max(
                rawRTrig,
                (zIsTrigger && valRz > -0.9f) ? ((valRz + 1.0f) * 0.5f) : 0.0f
            );

            if (lTrig > 0.30f) {
                axisBtns |= 0x2000;
            }
            if (rTrig > 0.30f) {
                axisBtns |= 0x0020;  // WPAD_BUTTON_SPIN
            }

            a->gamepadAxisButtons = axisBtns;

            // Immediate responsive pulse for newly pressed camera directions
            uint32_t rawCam = a->touchPad.buttons | a->gamepadKeyButtons | a->gamepadAxisButtons;
            bool rightStickCam = !mobile_ui::get().settings().rightStickPointer;
            bool wantLeft = (rawCam & 0x0001u) != 0 || (rightStickCam && a->gamepadRightStickX < -0.35f);
            bool wantRight = (rawCam & 0x0002u) != 0 || (rightStickCam && a->gamepadRightStickX > 0.35f);
            if (wantLeft && a->camRepeatLeft == 0) a->gamepadCameraPulsedButtons |= 0x0001;
            else if (!wantLeft) { a->gamepadCameraPulsedButtons &= ~0x0001; a->camRepeatLeft = 0; }
            if (wantRight && a->camRepeatRight == 0) a->gamepadCameraPulsedButtons |= 0x0002;
            else if (!wantRight) { a->gamepadCameraPulsedButtons &= ~0x0002; a->camRepeatRight = 0; }

            syncMergedPad(*a);
            port_input_set(0, &a->pad);
            return 1;
        }

        updateViewport(*a);
        int ptrX = a->vpX, ptrY = a->vpY, ptrW = a->vpW, ptrH = a->vpH;
        if (mobile_ui::get().settings().fullscreen) {
            ptrW = (int)lroundf((float)a->screenHeight * (16.0f / 9.0f));
            ptrX = (a->screenWidth - ptrW) / 2;
        }
        if (mobile_ui::get().handleInput(event, a->touchPad,
                                         a->screenWidth, a->screenHeight,
                                         ptrX, ptrY, ptrW, ptrH)) {
            syncMergedPad(*a);
            port_input_set(0, &a->pad);
            return 1;
        }
        return 0;
    } else if (eventType == AINPUT_EVENT_TYPE_KEY) {
        int32_t keyCode = AKeyEvent_getKeyCode(event);
        // Do not intercept system volume keys: let Android OS adjust system volume normally
        if (keyCode == AKEYCODE_VOLUME_UP || keyCode == AKEYCODE_VOLUME_DOWN || keyCode == AKEYCODE_VOLUME_MUTE) {
            return 0;
        }

        int32_t keyAction = AKeyEvent_getAction(event);
        bool down = (keyAction == AKEY_EVENT_ACTION_DOWN);

        switch (keyCode) {
        case AKEYCODE_BUTTON_A:
        case AKEYCODE_DPAD_CENTER:
            if (down) a->gamepadKeyButtons |= 0x0800;  // A (Jump / Confirm)
            else a->gamepadKeyButtons &= ~0x0800;
            break;
        case AKEYCODE_BUTTON_B:
            if (down) a->gamepadKeyButtons |= 0x0400;  // B (Shoot Star Bit / Cancel)
            else a->gamepadKeyButtons &= ~0x0400;
            break;
        case AKEYCODE_BUTTON_X:
            if (down) a->gamepadKeyButtons |= 0x0020;  // Spin button
            else a->gamepadKeyButtons &= ~0x0020;
            break;
        case AKEYCODE_BUTTON_Y:
            if (down) a->gamepadKeyButtons |= (0x0020 | 0x1000);  // Spin + Minus
            else a->gamepadKeyButtons &= ~(0x0020 | 0x1000);
            break;
        case AKEYCODE_BUTTON_L1:
        case AKEYCODE_BUTTON_L2:
            if (down) a->gamepadKeyButtons |= 0x2000;  // Z (crouch / ground pound)
            else a->gamepadKeyButtons &= ~0x2000;
            break;
        case AKEYCODE_BUTTON_THUMBL:
            if (down) a->gamepadKeyButtons |= (0x4000 | 0x0004);  // L3: Center Camera behind player & Exit FP
            else a->gamepadKeyButtons &= ~(0x4000 | 0x0004);
            break;
        case AKEYCODE_BUTTON_R1:
        case AKEYCODE_BUTTON_R2:
        case AKEYCODE_BUTTON_THUMBR:
            if (down) a->gamepadKeyButtons |= 0x0020;  // Spin button
            else a->gamepadKeyButtons &= ~0x0020;
            break;
        case AKEYCODE_BUTTON_START:
        case AKEYCODE_MENU:
            if (down) a->gamepadKeyButtons |= 0x0010;  // Plus / Pause
            else a->gamepadKeyButtons &= ~0x0010;
            break;
        case AKEYCODE_BUTTON_SELECT:
        case AKEYCODE_BUTTON_MODE:
            if (down) a->gamepadKeyButtons |= 0x1000;  // Minus / C
            else a->gamepadKeyButtons &= ~0x1000;
            break;
        case AKEYCODE_DPAD_UP:
            if (down) a->gamepadKeyButtons |= 0x0008;
            else a->gamepadKeyButtons &= ~0x0008;
            break;
        case AKEYCODE_DPAD_DOWN:
            if (down) a->gamepadKeyButtons |= (0x4000 | 0x0004); // D-Pad Down: Center camera & Exit FP
            else a->gamepadKeyButtons &= ~(0x4000 | 0x0004);
            break;
        case AKEYCODE_DPAD_LEFT:
            if (down) a->gamepadKeyButtons |= 0x0001;
            else a->gamepadKeyButtons &= ~0x0001;
            break;
        case AKEYCODE_DPAD_RIGHT:
            if (down) a->gamepadKeyButtons |= 0x0002;
            else a->gamepadKeyButtons &= ~0x0002;
            break;
        case AKEYCODE_BACK:
            if (mobile_ui::get().isSettingsOpen()) {
                if (down) {
                    mobile_ui::get().closeSettings();
                }
                return 1;
            }
            return 0;
        default:
            return 0;
        }

        // Immediate responsive pulse for newly pressed camera directions
        uint32_t rawCam = a->touchPad.buttons | a->gamepadKeyButtons | a->gamepadAxisButtons;
        bool wantLeft = (rawCam & 0x0001u) != 0 || (a->gamepadRightStickX < -0.35f);
        bool wantRight = (rawCam & 0x0002u) != 0 || (a->gamepadRightStickX > 0.35f);
        if (wantLeft && a->camRepeatLeft == 0) a->gamepadCameraPulsedButtons |= 0x0001;
        else if (!wantLeft) { a->gamepadCameraPulsedButtons &= ~0x0001; a->camRepeatLeft = 0; }
        if (wantRight && a->camRepeatRight == 0) a->gamepadCameraPulsedButtons |= 0x0002;
        else if (!wantRight) { a->gamepadCameraPulsedButtons &= ~0x0002; a->camRepeatRight = 0; }

        syncMergedPad(*a);
        port_input_set(0, &a->pad);
        return 1;
    }

    return 0;
}

void onAppCmd(android_app* app, int32_t cmd) {
    MobileApp* a = (MobileApp*)app->userData;
    if (!a) return;

    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        LOGI("APP_CMD_INIT_WINDOW");
        port_set_camera_cutout(mobile_ui::get().settings().showCameraCutout ? 1 : 0);
        initEgl(*a);
        createSurface(*a);
        break;
    case APP_CMD_TERM_WINDOW:
        LOGI("APP_CMD_TERM_WINDOW");
        setTiltSensorEnabled(*a, false);
        destroySurface(*a);
        break;
    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONFIG_CHANGED:
        LOGI("APP_CMD_WINDOW_RESIZED / CONFIG_CHANGED");
        if (a->tiltSensorEnabled) {
            a->displayRotation = queryDisplayRotation(a->android);
        }
        if (a->surface != EGL_NO_SURFACE) {
            destroySurface(*a);
            createSurface(*a);
        }
        break;
    case APP_CMD_RESUME:
        LOGI("APP_CMD_RESUME");
        port_set_paused(0);
        if (a->tiltSensorEnabled) {
            a->displayRotation = queryDisplayRotation(a->android);
        }
        port_set_camera_cutout(mobile_ui::get().settings().showCameraCutout ? 1 : 0);
        if (!a->gameBooted && a->dataRoot.empty()) {
            std::string ext = (a->android && a->android->activity && a->android->activity->externalDataPath) ? a->android->activity->externalDataPath : "/sdcard/Android/data/com.galaxy.mobile/files";
            a->dataRoot = findGameData(ext);
        }
        break;
    case APP_CMD_PAUSE:
        LOGI("APP_CMD_PAUSE");
        setTiltSensorEnabled(*a, false);
        port_set_paused(1);
        break;
    case APP_CMD_DESTROY:
        LOGI("APP_CMD_DESTROY");
        destroyTiltSensor(*a);
        destroySurface(*a);
        break;
    default:
        break;
    }
}

}  // namespace

extern "C" __attribute__((visibility("default"))) void port_android_main(struct android_app* app, uintptr_t windowBase, size_t windowSize) {
    LOGI("==================================================");
    LOGI("GalaxyMobile: Native flat-screen mobile initialized");
    LOGI("==================================================");

    MobileApp& a = gMobile;
    a.android = app;
    app->userData = &a;
    app->onAppCmd = onAppCmd;
    app->onInputEvent = onInput;

    port_mem_set_reserved_window(windowBase, windowSize);

    a.touchPad.connected = 1;
    a.touchPad.pointerValid = 1;
    a.touchPad.pointerDist = 2.0f;
    a.pad.connected = 1;
    a.pad.pointerValid = 1;
    a.pad.pointerDist = 2.0f;
    port_input_set(0, &a.pad);

    std::string internalPath = (app && app->activity && app->activity->internalDataPath) ? app->activity->internalDataPath : "";
    std::string externalPath = (app && app->activity && app->activity->externalDataPath) ? app->activity->externalDataPath : "";
    bool isPlus = (internalPath.find("plus") != std::string::npos) || (externalPath.find("plus") != std::string::npos);

    if (!isPlus && app && app->activity && app->activity->vm && app->activity->clazz) {
        JNIEnv* env = nullptr;
        if (app->activity->vm->AttachCurrentThread(&env, nullptr) == JNI_OK && env) {
            jclass actClass = env->GetObjectClass(app->activity->clazz);
            if (actClass) {
                jmethodID getPkgName = env->GetMethodID(actClass, "getPackageName", "()Ljava/lang/String;");
                if (getPkgName) {
                    jstring pkgName = (jstring)env->CallObjectMethod(app->activity->clazz, getPkgName);
                    if (pkgName) {
                        const char* str = env->GetStringUTFChars(pkgName, nullptr);
                        if (str) {
                            if (strstr(str, "plus") != nullptr) {
                                isPlus = true;
                            }
                            env->ReleaseStringUTFChars(pkgName, str);
                        }
                        env->DeleteLocalRef(pkgName);
                    }
                }
                env->DeleteLocalRef(actClass);
            }
        }
    }

    mobile_ui::get().setPlusEdition(isPlus);
    mobile_haptics::init(app->activity->vm, app->activity->clazz);
    port_net_init();
    port_skip_set_hold_seconds(isPlus ? 1.0f : 0.0f);
    LOGI("GalaxyMobile edition detected: %s (internalDataPath: %s, externalPath: %s)", isPlus ? "GalaxyMobile+" : "GalaxyMobile (Standard)", internalPath.c_str(), externalPath.c_str());

    std::string ext = app->activity->externalDataPath ? app->activity->externalDataPath : (isPlus ? "/sdcard/Android/data/com.galaxy.mobile.plus/files" : "/sdcard/Android/data/com.galaxy.mobile/files");
    a.saveRoot = internalPath + "/nand";
    mkdir(a.saveRoot.c_str(), 0770);
    mobile_ui::get().setSavePath(a.saveRoot + (isPlus ? "/touch_settings_plus.ini" : "/touch_settings.ini"));
    port_set_game_language(mobile_ui::get().settings().languageIndex);
    port_set_camera_cutout(mobile_ui::get().settings().showCameraCutout ? 1 : 0);
    if (isPlus) {
        port_set_player_character(mobile_ui::get().settings().playerCharacter);
    }

    mkdir(ext.c_str(), 0770);
    std::string logPath = ext + "/petari_mobile_log.txt";
    FILE* testLogF = fopen(logPath.c_str(), "a");
    if (testLogF) {
        fclose(testLogF);
        port_log_file(logPath.c_str());
    } else {
        port_log_file((internalPath + "/petari_mobile_log.txt").c_str());
    }
    gpu::setShaderCachePath((std::string(app->activity->internalDataPath) + "/shaders.bin").c_str());

    // Search for game data files
    a.dataRoot = findGameData(ext);
    if (a.dataRoot.empty()) {
        promptStoragePermissionIfMissing(app->activity);
        mobile_ui::get().openSettings();
    }

    bool running = true;
    while (running && !app->destroyRequested) {
        int ident;
        int events;
        int sensorDrains = 0;
        android_poll_source* source = nullptr;
        int timeout = (a.windowReady) ? 0 : 50;

        while ((ident = ALooper_pollOnce(timeout, nullptr, &events, (void**)&source)) >= 0) {
            if (source) {
                source->process(app, source);
            } else if (ident == LOOPER_ID_USER) {
                drainTiltSensorEvents(a);
                if (++sensorDrains >= 8) {
                    break;
                }
            }
            if (app->destroyRequested) {
                running = false;
                break;
            }
            timeout = 0;
        }

        if (!a.windowReady) {
            continue;
        }

        if (a.android && a.android->window && a.surface != EGL_NO_SURFACE) {
            int winW = ANativeWindow_getWidth(a.android->window);
            int winH = ANativeWindow_getHeight(a.android->window);
            if (winW > 0 && winH > 0 && (winW != a.screenWidth || winH != a.screenHeight)) {
                LOGI("Window dimensions changed (%dx%d -> %dx%d), recreating EGL surface", a.screenWidth, a.screenHeight, winW, winH);
                destroySurface(a);
                createSurface(a);
                if (!a.windowReady) continue;
            }
        }

        // Boot game once window and data are available
        if (!a.gameBooted) {
            if (a.dataRoot.empty()) {
                a.dataRoot = findGameData(ext);
            }
            if (!a.dataRoot.empty()) {
                LOGI("Booting game from: %s", a.dataRoot.c_str());
                a.gameBooted = true;
                mobile_ui::get().closeSettings();
                port_boot(a.dataRoot.c_str(), a.saveRoot.c_str());
            } else {
                static int64_t lastLog = 0;
                int64_t now = port_host_time_ns();
                if (now - lastLog > 3000000000ll) {  // Every 3 seconds
                    lastLog = now;
                    LOGI("Waiting for game files in /sdcard/GalaxyData or %s/game", ext.c_str());
                    // Re-scan in case player pushed files
                    a.dataRoot = findGameData(ext);
                }
            }
        }

        // Camera Auto-Repeat Manager (Runs once per frame at 60 FPS)
        uint32_t rawButtons = a.touchPad.buttons | a.gamepadKeyButtons | a.gamepadAxisButtons;
        bool rightStickCam = !mobile_ui::get().settings().rightStickPointer;
        bool wantCamLeft = (rawButtons & 0x0001u) != 0 || (rightStickCam && a.gamepadRightStickX < -0.35f);
        bool wantCamRight = (rawButtons & 0x0002u) != 0 || (rightStickCam && a.gamepadRightStickX > 0.35f);

        uint32_t pulsed = 0;
        if (wantCamLeft) {
            int interval = (fabsf(a.gamepadRightStickX) > 0.70f) ? 9 : 12;
            if (a.camRepeatLeft == 0) {
                pulsed |= 0x0001;
            } else if (a.camRepeatLeft >= 14) {
                int cycle = (a.camRepeatLeft - 14) % interval;
                if (cycle != 0) {
                    pulsed |= 0x0001;
                }
            } else {
                pulsed |= 0x0001;
            }
            a.camRepeatLeft++;
        } else {
            a.camRepeatLeft = 0;
        }

        if (wantCamRight) {
            int interval = (fabsf(a.gamepadRightStickX) > 0.70f) ? 9 : 12;
            if (a.camRepeatRight == 0) {
                pulsed |= 0x0002;
            } else if (a.camRepeatRight >= 14) {
                int cycle = (a.camRepeatRight - 14) % interval;
                if (cycle != 0) {
                    pulsed |= 0x0002;
                }
            } else {
                pulsed |= 0x0002;
            }
            a.camRepeatRight++;
        } else {
            a.camRepeatRight = 0;
        }
        a.gamepadCameraPulsedButtons = pulsed;

        // Move Star Pointer smoothly from Right Joystick when enabled
        updateRightStickPointer(a, port_host_time_ns());

        // Merge touch, gamepad, and tilt/gyro inputs
        syncMergedPad(a);
        updateTiltSensor(a);
        a.pad.accX = 0.0f;
        a.pad.accY = 0.0f;
        port_input_set(0, &a.pad);
        if (port_is_plus_edition() && port_net_is_connected() &&
            port_net_get_mode() == PORT_NET_MODE_COSTAR &&
            port_net_get_role() == PORT_NET_ROLE_CLIENT) {
            port_net_send_costar_state(&a.pad);
        }
        port_net_update();
        mobile_haptics::updateRumble(port_input_rumble(0));


        // Presentation & Render Loop
        gpu::Renderer& r = gpu::renderer();
        bool newFrame = r.update();

        if (newFrame) {
            gPresentedFrames.store(r.frameNumber());

            // Check if offscreen target dimensions need to change (e.g. toggled fullscreen or graphics preset)
            int desiredTargetH = getDesiredTargetHeight(a);
            int desiredTargetW;
            if (mobile_ui::get().settings().fullscreen) {
                updateViewport(a);
                float nativeAspect = (a.vpH > 0) ? ((float)a.vpW / (float)a.vpH) : ((float)a.screenWidth / (float)a.screenHeight);
                desiredTargetW = (int)lroundf(desiredTargetH * nativeAspect);
            } else {
                desiredTargetW = (desiredTargetH * 16) / 9;
            }
            if (a.targetReady && (a.efbTarget.width != desiredTargetW || a.efbTarget.height != desiredTargetH)) {
                r.destroyTarget(a.efbTarget);
                a.efbTarget = r.createTarget(desiredTargetW, desiredTargetH);
                updateViewport(a);
                LOGI("Resized offscreen EfbTarget: %dx%d (fullscreen=%d, preset=%d)", desiredTargetW, desiredTargetH, mobile_ui::get().settings().fullscreen ? 1 : 0, mobile_ui::get().settings().graphicsPreset);
            }

            // Monoscopic rendering into offscreen target
            r.render(a.efbTarget, nullptr, nullptr, gpu::HudMode::Inline);

            // Clean GL states before clearing backbuffer and blitting
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, a.screenWidth, a.screenHeight);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glDisable(GL_BLEND);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDepthMask(GL_TRUE);

            // Clear black pillarbox borders
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            // Present game image to viewport (blitted or Ultra post-processing shader)
            presentGameImage(a);

            // Ensure viewport is full window for mobile UI overlay
            glViewport(0, 0, a.screenWidth, a.screenHeight);
            mobile_ui::get().render(a.screenWidth, a.screenHeight);

            eglSwapBuffers(a.display, a.surface);
        } else {
            if (!r.hasFrame()) {
                // Idle screen while waiting for the game to boot or submit frames
                (void)mobile_ui::get().needsRedraw();
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glViewport(0, 0, a.screenWidth, a.screenHeight);
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_CULL_FACE);
                glDisable(GL_BLEND);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                glDepthMask(GL_TRUE);
                glClearColor(0.04f, 0.05f, 0.10f, 1.0f);  // Galaxy deep blue
                glClear(GL_COLOR_BUFFER_BIT);
                mobile_ui::get().render(a.screenWidth, a.screenHeight);
                eglSwapBuffers(a.display, a.surface);
                usleep(16000);
            } else if (mobile_ui::get().isSettingsOpen() && mobile_ui::get().needsRedraw()) {
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glViewport(0, 0, a.screenWidth, a.screenHeight);
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_CULL_FACE);
                glDisable(GL_BLEND);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                glDepthMask(GL_TRUE);

                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);

                if (a.targetReady) {
                    presentGameImage(a);
                }
                glViewport(0, 0, a.screenWidth, a.screenHeight);
                mobile_ui::get().render(a.screenWidth, a.screenHeight);
                eglSwapBuffers(a.display, a.surface);
            } else {
                usleep(1000);
            }
        }
    }

    LOGI("GalaxyMobile exiting cleanly");
    port_net_shutdown();
    mobile_haptics::shutdown();
    destroySurface(a);
    exit(0);
}

extern "C" {
float port_get_display_aspect(void) {
    if (gMobile.vpH > 0 && gMobile.vpW > 0) {
        return (float)gMobile.vpW / (float)gMobile.vpH;
    }
    if (gMobile.screenHeight > 0) {
        return (float)gMobile.screenWidth / (float)gMobile.screenHeight;
    }
    return 16.0f / 9.0f;
}

int port_is_fullscreen_aspect(void) {
    return mobile_ui::UIOverlay::get().settings().fullscreen ? 1 : 0;
}

int port_is_plus_edition(void) {
    return mobile_ui::UIOverlay::get().isPlusEdition() ? 1 : 0;
}

int port_is_early_luigi_unlocked(void) {
    return mobile_ui::UIOverlay::get().isPlusEdition() ? 1 : 0;
}

int port_is_modern_boo_flight(void) {
    return mobile_ui::UIOverlay::get().isPlusEdition() ? 1 : 0;
}

int port_is_skip_cutscenes(void) {
    if (!mobile_ui::UIOverlay::get().isPlusEdition()) return 0;
    return mobile_ui::UIOverlay::get().settings().modSkipCutscenes ? 1 : 0;
}

int port_is_infinite_health(void) {
    if (!mobile_ui::UIOverlay::get().isPlusEdition()) return 0;
    return mobile_ui::UIOverlay::get().settings().modInfiniteHealth ? 1 : 0;
}

static std::mutex sWarpMutex;
static std::string sPendingWarpStage;
static int sPendingWarpScenario = 1;
static bool sHasPendingWarp = false;

void port_request_warp_ex(const char* stage, int scenario, int broadcast) {
    if (!mobile_ui::UIOverlay::get().isPlusEdition()) return;
    {
        std::lock_guard<std::mutex> lk(sWarpMutex);
        sPendingWarpStage = stage ? stage : "";
        sPendingWarpScenario = scenario;
        sHasPendingWarp = true;
    }
    LOGI("Queued warp request to stage=%s scenario=%d (broadcast=%d)", sPendingWarpStage.c_str(), scenario, broadcast);
    if (broadcast && port_net_is_connected()) {
        port_net_send_warp_event(stage ? stage : "", scenario);
    }
}

void port_request_warp(const char* stage, int scenario) {
    port_request_warp_ex(stage, scenario, 1);
}

int port_take_warp_request(char* outStage, size_t maxLen, int* outScenario) {
    if (!mobile_ui::UIOverlay::get().isPlusEdition()) return 0;
    std::lock_guard<std::mutex> lk(sWarpMutex);
    if (!sHasPendingWarp) return 0;
    sHasPendingWarp = false;
    if (outStage && maxLen > 0) {
        snprintf(outStage, maxLen, "%s", sPendingWarpStage.c_str());
    }
    if (outScenario) {
        *outScenario = sPendingWarpScenario;
    }
    return 1;
}

int port_is_infinite_powerup(void) {
    if (!mobile_ui::UIOverlay::get().isPlusEdition()) return 0;
    return mobile_ui::UIOverlay::get().settings().modInfinitePowerup ? 1 : 0;
}

int port_is_flight_everywhere(void) {
    if (!mobile_ui::UIOverlay::get().isPlusEdition()) return 0;
    return mobile_ui::UIOverlay::get().settings().modFlightEverywhere ? 1 : 0;
}

int port_is_hd_ui_enabled(void) {
    if (!mobile_ui::UIOverlay::get().isPlusEdition()) return 0;
    return mobile_ui::UIOverlay::get().settings().hdUiTextures ? 1 : 0;
}

static std::mutex sPowerupMutex;
static int sPendingPowerup = -1;
static bool sHasPendingPowerup = false;
static std::atomic<int> sCurrentPowerup{0};

void port_request_powerup(int type) {
    if (!mobile_ui::UIOverlay::get().isPlusEdition()) return;
    std::lock_guard<std::mutex> lk(sPowerupMutex);
    sPendingPowerup = type;
    sHasPendingPowerup = true;
    LOGI("Queued power-up request type=%d", type);
}

int port_take_powerup_request(void) {
    if (!mobile_ui::UIOverlay::get().isPlusEdition()) return -1;
    std::lock_guard<std::mutex> lk(sPowerupMutex);
    if (!sHasPendingPowerup) return -1;
    sHasPendingPowerup = false;
    return sPendingPowerup;
}

void port_set_current_powerup(int mode) {
    sCurrentPowerup.store(mode, std::memory_order_relaxed);
}

int port_get_current_powerup(void) {
    return sCurrentPowerup.load(std::memory_order_relaxed);
}

static std::atomic<bool> sPendingUnlockSpin{false};
static std::atomic<int> sSpinUnlocked{0};

void port_request_unlock_spin(void) {
    if (!mobile_ui::UIOverlay::get().isPlusEdition()) return;
    int nextState = (sSpinUnlocked.load(std::memory_order_relaxed) != 0) ? 0 : 1;
    sSpinUnlocked.store(nextState, std::memory_order_relaxed);
    sPendingUnlockSpin.store(true, std::memory_order_relaxed);
    LOGI("Queued early spin unlock request (state=%d)", nextState);
}

int port_take_unlock_spin_request(void) {
    if (!mobile_ui::UIOverlay::get().isPlusEdition()) return 0;
    return sPendingUnlockSpin.exchange(false, std::memory_order_relaxed) ? 1 : 0;
}

void port_set_spin_unlocked(int unlocked) {
    sSpinUnlocked.store(unlocked, std::memory_order_relaxed);
}

int port_is_spin_unlocked(void) {
    if (!mobile_ui::UIOverlay::get().isPlusEdition()) return 0;
    return sSpinUnlocked.load(std::memory_order_relaxed);
}

int port_has_game_data(void) {
    if (!gMobile.dataRoot.empty()) {
        return 1;
    }
    std::string ext = "";
    if (gMobile.android && gMobile.android->activity && gMobile.android->activity->externalDataPath) {
        ext = gMobile.android->activity->externalDataPath;
    }
    std::string found = findGameData(ext);
    if (!found.empty()) {
        gMobile.dataRoot = found;
        return 1;
    }
    return 0;
}

void port_open_installer(void) {
    if (!gMobile.android || !gMobile.android->activity) {
        LOGE("port_open_installer: Android activity not initialized");
        return;
    }
    ANativeActivity* activity = gMobile.android->activity;
    if (!activity->vm || !activity->clazz) {
        LOGE("port_open_installer: VM or activity clazz is null");
        return;
    }
    JNIEnv* env = nullptr;
    bool attached = false;
    if (activity->vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        if (activity->vm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
            attached = true;
        }
    }
    if (!env) {
        LOGE("port_open_installer: failed to get JNIEnv");
        return;
    }

    jclass actClass = env->GetObjectClass(activity->clazz);
    jmethodID getPackageNameMethod = env->GetMethodID(actClass, "getPackageName", "()Ljava/lang/String;");
    jstring pkgName = (jstring)env->CallObjectMethod(activity->clazz, getPackageNameMethod);

    jclass intentClass = env->FindClass("android/content/Intent");
    jmethodID intentCtor = env->GetMethodID(intentClass, "<init>", "()V");
    jobject intent = env->NewObject(intentClass, intentCtor);

    jmethodID setClassNameMethod = env->GetMethodID(intentClass, "setClassName", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;");
    jstring clsName = env->NewStringUTF("com.galaxy.installer.InstallerActivity");
    env->CallObjectMethod(intent, setClassNameMethod, pkgName, clsName);

    jmethodID startActivityMethod = env->GetMethodID(actClass, "startActivity", "(Landroid/content/Intent;)V");
    env->CallVoidMethod(activity->clazz, startActivityMethod, intent);

    if (env->ExceptionCheck()) {
        LOGE("port_open_installer: Exception occurred while starting InstallerActivity");
        env->ExceptionDescribe();
        env->ExceptionClear();
    } else {
        LOGI("port_open_installer: successfully launched InstallerActivity");
    }

    env->DeleteLocalRef(clsName);
    env->DeleteLocalRef(pkgName);
    env->DeleteLocalRef(intent);
    env->DeleteLocalRef(intentClass);
    env->DeleteLocalRef(actClass);

    if (attached) {
        activity->vm->DetachCurrentThread();
    }
}

void port_open_url(const char* url) {
    if (!url || !gMobile.android || !gMobile.android->activity) return;
    ANativeActivity* activity = gMobile.android->activity;
    if (!activity->vm || !activity->clazz) return;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (activity->vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        if (activity->vm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
            attached = true;
        }
    }
    if (!env) return;

    jclass actClass = env->GetObjectClass(activity->clazz);
    jmethodID getClassLoaderMethod = env->GetMethodID(actClass, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject classLoader = env->CallObjectMethod(activity->clazz, getClassLoaderMethod);

    jclass clClass = env->FindClass("java/lang/ClassLoader");
    jmethodID loadClassMethod = env->GetMethodID(clClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring clsName = env->NewStringUTF("com.galaxy.installer.AppInstaller");
    jclass appInstallerClass = (jclass)env->CallObjectMethod(classLoader, loadClassMethod, clsName);

    if (env->ExceptionCheck() || !appInstallerClass) {
        env->ExceptionClear();
        jclass intentClass = env->FindClass("android/content/Intent");
        jclass uriClass = env->FindClass("android/net/Uri");
        if (intentClass && uriClass) {
            jstring actionStr = env->NewStringUTF("android.intent.action.VIEW");
            jstring urlStr = env->NewStringUTF(url);
            jmethodID parseMethod = env->GetStaticMethodID(uriClass, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
            jobject uriObj = env->CallStaticObjectMethod(uriClass, parseMethod, urlStr);
            jmethodID intentCtor = env->GetMethodID(intentClass, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V");
            jobject intentObj = env->NewObject(intentClass, intentCtor, actionStr, uriObj);
            jmethodID addFlagsMethod = env->GetMethodID(intentClass, "addFlags", "(I)Landroid/content/Intent;");
            env->CallObjectMethod(intentObj, addFlagsMethod, 0x10000000);
            jmethodID startActivityMethod = env->GetMethodID(actClass, "startActivity", "(Landroid/content/Intent;)V");
            env->CallVoidMethod(activity->clazz, startActivityMethod, intentObj);
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(intentObj);
            env->DeleteLocalRef(uriObj);
            env->DeleteLocalRef(urlStr);
            env->DeleteLocalRef(actionStr);
            env->DeleteLocalRef(uriClass);
            env->DeleteLocalRef(intentClass);
        }
    } else {
        jmethodID openUrlMethod = env->GetStaticMethodID(appInstallerClass, "openUrl", "(Landroid/app/Activity;Ljava/lang/String;)V");
        if (openUrlMethod) {
            jstring jUrl = env->NewStringUTF(url);
            env->CallStaticVoidMethod(appInstallerClass, openUrlMethod, activity->clazz, jUrl);
            env->DeleteLocalRef(jUrl);
        }
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        env->DeleteLocalRef(appInstallerClass);
    }

    env->DeleteLocalRef(clsName);
    env->DeleteLocalRef(clClass);
    env->DeleteLocalRef(classLoader);
    env->DeleteLocalRef(actClass);

    if (attached) {
        activity->vm->DetachCurrentThread();
    }
}

int port_is_plus_installed(void) {
    if (!gMobile.android || !gMobile.android->activity) return 0;
    ANativeActivity* activity = gMobile.android->activity;
    if (!activity->vm || !activity->clazz) return 0;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (activity->vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        if (activity->vm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
            attached = true;
        }
    }
    if (!env) return 0;

    jclass actClass = env->GetObjectClass(activity->clazz);
    jmethodID getPackageManagerMethod = env->GetMethodID(actClass, "getPackageManager", "()Landroid/content/pm/PackageManager;");
    jobject pm = env->CallObjectMethod(activity->clazz, getPackageManagerMethod);

    jclass pmClass = env->GetObjectClass(pm);
    jmethodID getPackageInfoMethod = env->GetMethodID(pmClass, "getPackageInfo", "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;");
    jstring pkgName = env->NewStringUTF("com.galaxy.mobile.plus");

    jobject pkgInfo = env->CallObjectMethod(pm, getPackageInfoMethod, pkgName, 0);
    int installed = 0;
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        installed = 0;
    } else if (pkgInfo != nullptr) {
        installed = 1;
        env->DeleteLocalRef(pkgInfo);
    }

    env->DeleteLocalRef(pkgName);
    env->DeleteLocalRef(pmClass);
    env->DeleteLocalRef(pm);
    env->DeleteLocalRef(actClass);

    if (attached) {
        activity->vm->DetachCurrentThread();
    }
    return installed;
}

void port_install_plus(void) {
    if (!gMobile.android || !gMobile.android->activity) return;
    ANativeActivity* activity = gMobile.android->activity;
    if (!activity->vm || !activity->clazz) return;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (activity->vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        if (activity->vm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
            attached = true;
        }
    }
    if (!env) return;

    jclass actClass = env->GetObjectClass(activity->clazz);
    jmethodID getClassLoaderMethod = env->GetMethodID(actClass, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject classLoader = env->CallObjectMethod(activity->clazz, getClassLoaderMethod);

    jclass clClass = env->FindClass("java/lang/ClassLoader");
    jmethodID loadClassMethod = env->GetMethodID(clClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring clsName = env->NewStringUTF("com.galaxy.installer.AppInstaller");
    jclass appInstallerClass = (jclass)env->CallObjectMethod(classLoader, loadClassMethod, clsName);

    if (env->ExceptionCheck() || !appInstallerClass) {
        LOGE("port_install_plus: failed to load AppInstaller class");
        env->ExceptionClear();
    } else {
        jmethodID installMethod = env->GetStaticMethodID(appInstallerClass, "installOrOpenPlus", "(Landroid/app/Activity;)V");
        if (installMethod) {
            env->CallStaticVoidMethod(appInstallerClass, installMethod, activity->clazz);
            LOGI("port_install_plus: called AppInstaller.installOrOpenPlus");
        }
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        env->DeleteLocalRef(appInstallerClass);
    }

    env->DeleteLocalRef(clsName);
    env->DeleteLocalRef(clClass);
    env->DeleteLocalRef(classLoader);
    env->DeleteLocalRef(actClass);

    if (attached) {
        activity->vm->DetachCurrentThread();
    }
}

extern "C" void port_share_log(void) {
    if (!gMobile.android || !gMobile.android->activity) return;
    ANativeActivity* activity = gMobile.android->activity;
    if (!activity->vm || !activity->clazz) return;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (activity->vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        if (activity->vm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
            attached = true;
        }
    }
    if (!env) return;

    jclass actClass = env->GetObjectClass(activity->clazz);
    jmethodID getClassLoaderMethod = env->GetMethodID(actClass, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject classLoader = env->CallObjectMethod(activity->clazz, getClassLoaderMethod);

    jclass clClass = env->FindClass("java/lang/ClassLoader");
    jmethodID loadClassMethod = env->GetMethodID(clClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring clsName = env->NewStringUTF("com.galaxy.installer.AppInstaller");
    jclass appInstallerClass = (jclass)env->CallObjectMethod(classLoader, loadClassMethod, clsName);

    if (env->ExceptionCheck() || !appInstallerClass) {
        LOGE("port_share_log: failed to load AppInstaller class");
        env->ExceptionClear();
    } else {
        jmethodID shareMethod = env->GetStaticMethodID(appInstallerClass, "shareLogFile", "(Landroid/app/Activity;)V");
        if (shareMethod) {
            env->CallStaticVoidMethod(appInstallerClass, shareMethod, activity->clazz);
            LOGI("port_share_log: called AppInstaller.shareLogFile");
        }
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        env->DeleteLocalRef(appInstallerClass);
    }

    env->DeleteLocalRef(clsName);
    env->DeleteLocalRef(clClass);
    env->DeleteLocalRef(classLoader);
    env->DeleteLocalRef(actClass);

    if (attached) {
        activity->vm->DetachCurrentThread();
    }
}

static std::atomic<int> sShowCameraCutout{1};
void port_set_camera_cutout(int show) {
    sShowCameraCutout.store(show ? 1 : 0, std::memory_order_relaxed);
    if (!gMobile.android || !gMobile.android->activity) return;
    ANativeActivity* activity = gMobile.android->activity;
    if (!activity->vm || !activity->clazz) return;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (activity->vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        if (activity->vm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
            attached = true;
        }
    }
    if (!env) return;

    jclass actClass = env->GetObjectClass(activity->clazz);
    jmethodID getClassLoaderMethod = env->GetMethodID(actClass, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject classLoader = env->CallObjectMethod(activity->clazz, getClassLoaderMethod);

    jclass clClass = env->FindClass("java/lang/ClassLoader");
    jmethodID loadClassMethod = env->GetMethodID(clClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring clsName = env->NewStringUTF("com.galaxy.installer.AppInstaller");
    jclass appInstallerClass = (jclass)env->CallObjectMethod(classLoader, loadClassMethod, clsName);

    if (env->ExceptionCheck() || !appInstallerClass) {
        env->ExceptionClear();
    } else {
        jmethodID cutoutMethod = env->GetStaticMethodID(appInstallerClass, "setDisplayCutoutMode", "(Landroid/app/Activity;Z)V");
        if (cutoutMethod) {
            env->CallStaticVoidMethod(appInstallerClass, cutoutMethod, activity->clazz, show ? JNI_TRUE : JNI_FALSE);
        }
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        env->DeleteLocalRef(appInstallerClass);
    }

    env->DeleteLocalRef(clsName);
    env->DeleteLocalRef(clClass);
    env->DeleteLocalRef(classLoader);
    env->DeleteLocalRef(actClass);

    if (attached) {
        activity->vm->DetachCurrentThread();
    }
}
int port_get_camera_cutout(void) {
    return sShowCameraCutout.load(std::memory_order_relaxed);
}

static std::atomic<int> sPlayerCharacter{0};
static std::atomic<bool> sPendingCharacterChange{false};
void port_set_player_character(int isLuigi) {
    sPlayerCharacter.store(isLuigi ? 1 : 0, std::memory_order_relaxed);
}
int port_get_player_character(void) {
    if (!mobile_ui::UIOverlay::get().isPlusEdition()) return 0;
    return sPlayerCharacter.load(std::memory_order_relaxed);
}
void port_request_character_change(int isLuigi) {
    if (!mobile_ui::UIOverlay::get().isPlusEdition()) return;
    sPlayerCharacter.store(isLuigi ? 1 : 0, std::memory_order_relaxed);
    sPendingCharacterChange.store(true, std::memory_order_relaxed);
    LOGI("Queued character change request: isLuigi=%d", isLuigi ? 1 : 0);
}
int port_take_character_change_request(void) {
    if (!mobile_ui::UIOverlay::get().isPlusEdition()) return 0;
    return sPendingCharacterChange.exchange(false, std::memory_order_relaxed) ? 1 : 0;
}
}


