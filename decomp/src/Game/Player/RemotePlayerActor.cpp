#include "Game/Player/RemotePlayerActor.hpp"
#include "port/net.h"
#include "port/port.h"

#include "Game/Player/MarioActor.hpp"
#include "Game/Player/MarioAccess.hpp"
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
    if (sInstance != nullptr) return;

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
    const char* modelName = MR::isPlayerLuigi() ? "Luigi" : "Mario";

    initModelManagerWithAnm(modelName, "MarioAnime", true);

    MR::connectToScene(this, MR::MovementType_Player, MR::CalcAnimType_Player,
                       MR::DrawBufferType_PlayerDecoration, MR::DrawType_None);

    initSound(4, false);

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

    // Enable dynamic differed display lists and fog so stage shading and lighting apply
    MR::initDLMakerFog(this, true);
    MR::newDifferedDLBuffer(this);

    mCurrentAnim = "Wait";
    MR::tryStartBck(this, "Wait", nullptr);

    // Start dormant until active peer packets are processed in control()
    makeActorDead();
}

void RemotePlayerActor::updatePowerup(int powerupMode) {
    float frame = 0.0f;
    if (powerupMode == PlayerMode_Foo) {
        frame = 1.0f; // Flying / Red Star Mario
    } else if (powerupMode == PlayerMode_2) {
        frame = 2.0f; // Fire Mario
    }

    if (mModelManager && MR::startBtpIfExist(this, "ColorChange")) {
        MR::setBtpFrameAndStop(this, frame);
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

            LiveActor* player = MarioAccess::getPlayerActor();
            if (player && MR::isExistEffectKeeper(player)) {
                const char* spinEffects[] = {
                    "\x83\x58\x83\x73\x83\x93\x83\x8a\x83\x93\x83\x4f", // SpinRing
                    "\x83\x58\x83\x73\x83\x93\x83\x89\x83\x43\x83\x67\x8f\xc1\x8b\x8e", // SpinLightBurst
                    "SpinRing",
                    "SpinLightBurst"
                };
                for (int i = 0; i < 4; i++) {
                    if (MR::isRegisteredEffect(player, spinEffects[i])) {
                        MR::emitEffectHit(player, mPosition, spinEffects[i]);
                    }
                }
            }
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
