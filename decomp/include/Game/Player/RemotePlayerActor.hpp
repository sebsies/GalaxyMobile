#pragma once

#include "Game/LiveActor/LiveActor.hpp"
#include <JSystem/JGeometry/TVec.hpp>

class RemotePlayerActor : public LiveActor {
public:
    RemotePlayerActor(const char* pName = "RemotePlayer");
    virtual ~RemotePlayerActor();

    virtual void init(const JMapInfoIter& rIter) override;
    virtual void control() override;
    virtual void calcAndSetBaseMtx() override;
    virtual void draw() const override;

    static RemotePlayerActor* getInstance();
    static void tryCreate();
    static void tryDestroy();

private:
    void updatePowerup(int powerupMode);

    bool mIsLuigi;
    TVec3f mTargetPos;
    TVec3f mTargetRot;
    const char* mCurrentAnim;
    int mSpinSoundCooldown;
    int mCurrentPowerupMode;
    bool mWasSpinning;

    static RemotePlayerActor* sInstance;
};
