#include "Game/Player/RemotePlayerActor.hpp"
#include "port/net.h"
#include "port/port.h"

#include "Game/Player/MarioActor.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/EventUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/ActorShadowUtil.hpp"
#include "Game/Util/ActorMovementUtil.hpp"
#include "Game/Util/ModelUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/SoundUtil.hpp"
#include "Game/Util/EffectUtil.hpp"

RemotePlayerActor* RemotePlayerActor::sInstance = nullptr;

RemotePlayerActor::RemotePlayerActor(const char* pName)
    : LiveActor(pName), mIsLuigi(false), mTargetPos(0.0f, 0.0f, 0.0f),
      mTargetRot(0.0f, 0.0f, 0.0f), mCurrentAnim(nullptr), mSpinSoundCooldown(0),
      mCurrentPowerupMode(-1), mWasSpinning(false) {
}

RemotePlayerActor::~RemotePlayerActor() {
    if (sInstance == this) {
        sInstance = nullptr;
    }
}

RemotePlayerActor* RemotePlayerActor::getInstance() {
    return sInstance;
}

void RemotePlayerActor::tryCreate() {
#ifdef TARGET_PC
    if (!port_is_plus_edition()) return;

    // In a new stage, reset any previous pointer and allocate freshly
    sInstance = new RemotePlayerActor("RemotePlayer");
    sInstance->initWithoutIter();
#endif
}

void RemotePlayerActor::tryDestroy() {
    sInstance = nullptr;
}

void RemotePlayerActor::init(const JMapInfoIter& rIter) {
    // If local player is Mario, remote partner is Luigi (and vice versa)
    mIsLuigi = !MR::isPlayerLuigi();

    // Use whatever player archive is already loaded and stationed in memory
    // (Mario.arc when playing as Mario, Luigi.arc when playing as Luigi).
    // This avoids synchronous storage I/O and heap reallocation during gameplay.
    const char* modelName = MR::isPlayerLuigi() ? "Luigi" : "Mario";

    initModelManagerWithAnm(modelName, "MarioAnime", true);

    MR::connectToScene(this, MR::MovementType_Player, MR::CalcAnimType_Player,
                       MR::DrawBufferType_PlayerDecoration, MR::DrawType_None);

    // Audio, particle effects & scene lighting
    initSound(4, false);
    initEffectKeeper(16, "Mario", false);
    MR::initLightCtrl(this);

    MR::initShadowVolumeSphere(this, 50.0f);
    MR::validateShadow(this, nullptr);
    MR::calcGravity(this);
    MR::onCalcShadow(this, nullptr);
    MR::onCalcShadowDropGravity(this, nullptr);
    MR::invalidateClipping(this);

    // Apply distinct visual silhouette for Player 2
    if (mIsLuigi) {
        // Luigi: taller and slimmer
        mScale.set(0.95f, 1.05f, 0.95f);
    } else {
        // Mario: stockier
        mScale.set(1.05f, 0.95f, 1.05f);
    }
    MR::getJ3DModel(this)->setBaseScale(mScale);

    // Initial normal texture pattern
    if (MR::startBtpIfExist(this, "ColorChange")) {
        MR::setBtpFrameAndStop(this, 0.0f);
    }

    mCurrentAnim = "Wait";
    MR::tryStartBck(this, "Wait", nullptr);

    // Start dormant (invisible & paused) until a peer connects
    makeActorDead();
}

void RemotePlayerActor::updatePowerup(int powerupMode) {
    // Clean up any active continuous powerup looping effects
    if (mEffectKeeper) {
        MR::deleteEffect(this, "\x96\xb3\x93\x47\x92\x86"); // Invincible ("無敵中")
        MR::deleteEffect(this, "\x83\x41\x83\x43\x83\x58\x92\x86"); // Ice ("アイス中")
    }

    switch (powerupMode) {
    case PlayerMode_Foo: // Flying / Red Star Mario
        if (MR::startBtpIfExist(this, "ColorChange")) {
            MR::setBtpFrameAndStop(this, 1.0f);
        }
        break;

    case PlayerMode_2: // Fire Mario
        if (MR::startBtpIfExist(this, "ColorChange")) {
            MR::setBtpFrameAndStop(this, 2.0f);
        }
        break;

    case PlayerMode_Invincible: // Rainbow Star
        if (MR::startBtpIfExist(this, "ColorChange")) {
            MR::setBtpFrameAndStop(this, 0.0f);
        }
        if (mEffectKeeper) {
            MR::emitEffect(this, "\x96\xb3\x93\x47\x92\x86");
        }
        break;

    case PlayerMode_Ice: // Ice Mario
        if (MR::startBtpIfExist(this, "ColorChange")) {
            MR::setBtpFrameAndStop(this, 0.0f);
        }
        if (mEffectKeeper) {
            MR::emitEffect(this, "\x83\x41\x83\x43\x83\x58\x92\x86");
        }
        break;

    default: // Normal (0) and other modes
        if (MR::startBtpIfExist(this, "ColorChange")) {
            MR::setBtpFrameAndStop(this, 0.0f);
        }
        break;
    }
}

void RemotePlayerActor::control() {
#ifdef TARGET_PC
    PortRemotePlayerState rState;
    if (!port_net_is_connected() || port_net_get_mode() != PORT_NET_MODE_COOP ||
        !port_net_get_remote_player_state(&rState) || !rState.valid) {
        if (!MR::isDead(this)) {
            makeActorDead();
        }
        return;
    }

    if (MR::isDead(this)) {
        makeActorAppeared();
    }

    // Smooth position interpolation (LERP with snap distance threshold)
    TVec3f target(rState.posX, rState.posY, rState.posZ);
    TVec3f delta = target - mPosition;
    float dist = delta.length();
    if (dist > 500.0f || dist < 0.1f) {
        mPosition = target;
    } else {
        mPosition += delta * 0.40f;
    }

    // Direct orientation match
    mRotation.set(rState.rotX, rState.rotY, rState.rotZ);

    // Power-up skin synchronization
    if (rState.powerupMode != mCurrentPowerupMode) {
        mCurrentPowerupMode = rState.powerupMode;
        updatePowerup(mCurrentPowerupMode);
    }

    // Dynamic animation selection based on remote player action flags
    const char* nextAnim = "Wait";
    bool isSpinning = (rState.stateFlags & PORT_NET_FLAG_SPINNING) != 0;

    if (isSpinning) {
        nextAnim = "Spin";
        if (mSpinSoundCooldown <= 0 || !mWasSpinning) {
            MR::startSound(this, "SE_PV_MARIO_SPIN");
            mSpinSoundCooldown = 25;
            if (mEffectKeeper) {
                // Iconic star burst ring around player
                MR::emitEffect(this, "\x83\x58\x83\x73\x83\x93\x83\x8a\x83\x93\x83\x4f"); // SpinRing

                // Star sparkle / elemental spin trail
                if (mCurrentPowerupMode == PlayerMode_2) {
                    MR::emitEffect(this, "\x83\x74\x83\x40\x83\x43\x83\x41\x83\x58\x83\x73\x83\x93"); // FireMarioSpinLight
                } else if (mCurrentPowerupMode == PlayerMode_Ice) {
                    MR::emitEffect(this, "\x83\x41\x83\x43\x83\x58\x83\x58\x83\x73\x83\x93"); // IceMarioSpinLight
                } else {
                    MR::emitEffect(this, "\x83\x58\x83\x73\x83\x93\x83\x89\x83\x43\x83\x67"); // SpinLight
                }
            }
        }
    } else {
        if (mWasSpinning && mEffectKeeper) {
            MR::deleteEffect(this, "\x83\x58\x83\x73\x83\x93\x83\x89\x83\x43\x83\x67");
            MR::deleteEffect(this, "\x83\x74\x83\x40\x83\x43\x83\x41\x83\x58\x83\x73\x83\x93");
            MR::deleteEffect(this, "\x83\x41\x83\x43\x83\x58\x83\x58\x83\x73\x83\x93");
        }
    }
    mWasSpinning = isSpinning;

    if (!isSpinning) {
        if (rState.stateFlags & PORT_NET_FLAG_FLYING) {
            nextAnim = "\x83\x74\x81\x5b\x83\x74\x83\x40\x83\x43\x83\x5e\x81\x5b\x94\xf2\x8d\x73"; // Flight
        } else if (rState.stateFlags & PORT_NET_FLAG_AIR) {
            nextAnim = "Jump";
        } else if (rState.stateFlags & PORT_NET_FLAG_SWIMMING) {
            nextAnim = "Swim";
        } else if (rState.stateFlags & PORT_NET_FLAG_SLIDING) {
            nextAnim = "Slide";
        } else if (rState.stateFlags & PORT_NET_FLAG_SQUATTING) {
            nextAnim = "Squat";
        } else {
            TVec3f vel(rState.velX, rState.velY, rState.velZ);
            if (vel.length() > 2.0f) {
                nextAnim = "Run";
            }
        }
    }

    if (nextAnim != mCurrentAnim) {
        if (!MR::tryStartBck(this, nextAnim, nullptr)) {
            // Fallback for animations if not found
            if (MR::tryStartBck(this, "Jump", nullptr)) {
                nextAnim = "Jump";
            }
        }
        mCurrentAnim = nextAnim;
    }

    if (mSpinSoundCooldown > 0) {
        mSpinSoundCooldown--;
    }
#endif
}

void RemotePlayerActor::calcAndSetBaseMtx() {
    LiveActor::calcAndSetBaseMtx();
}
