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

    static RemotePlayerActor* getInstance();
    static void tryCreate();
    static void tryDestroy();

private:
    bool mIsLuigi;
    TVec3f mTargetPos;
    TVec3f mTargetRot;
    const char* mCurrentAnim;
    int mSpinSoundCooldown;

    static RemotePlayerActor* sInstance;
};
