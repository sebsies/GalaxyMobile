#include "Game/Scene/SceneDataInitializer.hpp"
#include "Game/NameObj/NameObjArchiveListCollector.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Scene/StageDataHolder.hpp"
#include "Game/Scene/StageFileLoader.hpp"
#include "Game/Screen/LuigiLetter.hpp"
#include "Game/Util/EventUtil.hpp"
#include "Game/Util/FileUtil.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "port/port.h"

SceneDataInitializer::SceneDataInitializer() : NameObj("SceneDataInitializer") {
    mFileLoader = nullptr;
    mDataHolder = nullptr;
}

void SceneDataInitializer::startStageFileLoad() {
    mFileLoader = new StageFileLoader(MR::getCurrentStageName());
    mFileLoader->startLoadingStageFile();

    if (!MR::isStageDisablePauseMenu()) {
        MR::mountAsyncArchive("/LayoutData/PauseMenu.arc", MR::getAproposHeapForSceneArchive(0.03f));
    }
}

void SceneDataInitializer::startStageFileLoadAfterScenarioSelected() {
    NameObjArchiveListCollector collector;

    LuigiLetter::makeArchiveListForMenu(&collector, JMapInfoIter());

#ifdef TARGET_PC
    if (port_is_plus_edition()) {
        if (MR::isPlayerLuigi()) {
            collector.addArchive("BeeLuigi");
            collector.addArchive("HopperLuigi");
            collector.addArchive("IceLuigi");
            collector.addArchive("TeresaLuigi");
            collector.addArchive("InvincibleLuigi");
        } else {
            collector.addArchive("BeeMario");
            collector.addArchive("HopperMario");
            collector.addArchive("IceMario");
            collector.addArchive("TeresaMario");
            collector.addArchive("InvincibleMario");
        }
        collector.addArchive("BeeMarioWing");
        collector.addArchive("IceMarioHandL");
        collector.addArchive("IceMarioHandR");
        collector.addArchive("InvincibleMarioHandL");
        collector.addArchive("InvincibleMarioHandR");
    }
#endif

    for (int i = 0; i < collector.getArchiveNum(); i++) {
        MR::mountAsyncArchiveByObjectOrLayoutName(collector.getArchive(i), nullptr);
    }

#ifdef TARGET_PC
    if (port_is_plus_edition()) {
        MR::receiveAllRequestedFile();
    }
#endif
}

void SceneDataInitializer::waitDoneStageFileLoad() {
    mFileLoader->waitLoadedStageFile();
    mDataHolder = static_cast< StageDataHolder* >(MR::createSceneObj(SceneObj_StageDataHolder));
}

void SceneDataInitializer::startActorFileLoadCommon() {
    mDataHolder->requestFileLoadCommon();
}

void SceneDataInitializer::startActorFileLoadScenario() {
    mDataHolder->requestFileLoadScenario();
}

void SceneDataInitializer::startActorPlacement() {
    mDataHolder->initPlacement();
}

void SceneDataInitializer::initAfterScenarioSelected() {
    mDataHolder->initAfterScenarioSelected();
}

SceneDataInitializer::~SceneDataInitializer() {
}
