#include "ConfigManager.h"
#include "EventHandlers.h"
#include "FertilityStorage.h"
#include "Menu.h"
#include "MorphManager.h"
#include "MorphQueue.h"
#include "RaceOverrides.h"
#include "SceneProcessor.h"
#include "SpermExpulsion.h"
#include "TickManager.h"

using namespace Fertility;

static void ScheduleTickFrame()
{
  auto* taskIntfc = SKSE::GetTaskInterface();
  if (!taskIntfc)
    return;
  taskIntfc->AddTask([]() {
    if (Fertility::enabled)
      TickManager::GetSingleton()->Tick();
    ScheduleTickFrame();
  });
}

void MessageHandler(SKSE::MessagingInterface::Message* msg)
{
  switch (msg->type) {
  case SKSE::MessagingInterface::kDataLoaded:
    ConfigManager::GetSingleton()->Reload();
    Storage::GetSingleton()->Initialize();
    RaceOverrides::GetSingleton()->Initialize();
    MorphManager::GetSingleton()->Initialize();
    MorphQueue::GetSingleton()->Initialize();
    TickManager::GetSingleton()->Initialize();
    SpermExpulsion::GetSingleton()->Initialize();
    Events::Register();
    Menu::GetSingleton();
    ScheduleTickFrame();
    logger::info("[Fertility] Data loaded, all systems initialized");
    break;
  case SKSE::MessagingInterface::kNewGame:
  case SKSE::MessagingInterface::kPostLoadGame:
    Storage::GetSingleton()->AutoCleanup();
    break;
  default:
    break;
  }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
  SKSE::Init(skse, true);
  logger::info("=== Fertility SKSE Plugin ===");

  auto* messaging = SKSE::GetMessagingInterface();
  if (!messaging)
    return false;
  messaging->RegisterListener(MessageHandler);

  auto* papyrus = SKSE::GetPapyrusInterface();
  if (!papyrus)
    return false;
  papyrus->Register(SceneProcessor::RegisterNativeFunctions);

  auto* serial = SKSE::GetSerializationInterface();
  if (!serial)
    return false;
  serial->SetUniqueID('FERT');
  serial->SetSaveCallback(Storage::OnSave);
  serial->SetLoadCallback(Storage::OnLoad);
  serial->SetRevertCallback(Storage::OnRevert);

  logger::info("[Fertility] Plugin loaded successfully");
  return true;
}
