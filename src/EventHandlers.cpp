#include "EventHandlers.h"
#include "ConfigManager.h"
#include "FertilityStorage.h"

namespace Fertility::Events
{

// =================================================================
//  Death Event Sink
//
//  数据生命周期关键节点:
//  TickManager 不检测 actor 死亡, 必须由此 Sink 触发清理
//  调用 Storage::OnActorDeath → 标记 isDead + 清除 recipient + 移除 donor
// =================================================================
class DeathEventSink : public RE::BSTEventSink<RE::TESDeathEvent>
{
public:
  static DeathEventSink* GetSingleton()
  {
    static DeathEventSink instance;
    return &instance;
  }

  RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent* event, RE::BSTEventSource<RE::TESDeathEvent>*) override
  {
    if (!event || !event->actorDying)
      return RE::BSEventNotifyControl::kContinue;

    auto* dead = event->actorDying->As<RE::Actor>();
    if (!dead)
      return RE::BSEventNotifyControl::kContinue;

    auto* storage = Storage::GetSingleton();
    auto id       = dead->GetFormID();

    // 只处理被追踪的 actor (recipient 或 donor)
    if (storage->IsRecipientTracked(id) || storage->FindDonor(DonorKey::FromFormID(id))) {
      storage->OnActorDeath(dead);

      if (Fertility::verboseMode)
        logger::info("[Fertility] Death event: {} ({:08X})", dead->GetName(), id);
    }

    return RE::BSEventNotifyControl::kContinue;
  }

private:
  DeathEventSink() = default;
};

// =================================================================
//  Location Change Sink
//
//  数据生命周期关键节点:
//  驱动 Storage::OnLocationChange → 更新 locationLeftTime → 触发 AutoCleanup
//  没有此 Sink, 位置清理完全失效
// =================================================================
class LocationEventSink : public RE::BSTEventSink<RE::TESActorLocationChangeEvent>
{
public:
  static LocationEventSink* GetSingleton()
  {
    static LocationEventSink instance;
    return &instance;
  }

  RE::BSEventNotifyControl ProcessEvent(const RE::TESActorLocationChangeEvent* event, RE::BSTEventSource<RE::TESActorLocationChangeEvent>*) override
  {
    if (!event || !event->actor)
      return RE::BSEventNotifyControl::kContinue;

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player)
      return RE::BSEventNotifyControl::kContinue;

    // 仅关心玩家位置变化
    if (event->actor->GetFormID() != player->GetFormID())
      return RE::BSEventNotifyControl::kContinue;

    std::string oldLocName;
    if (event->oldLoc)
      oldLocName = event->oldLoc->GetName();

    std::string newLocName;
    if (event->newLoc)
      newLocName = event->newLoc->GetName();

    Storage::GetSingleton()->OnLocationChange(oldLocName, newLocName);

    return RE::BSEventNotifyControl::kContinue;
  }

private:
  LocationEventSink() = default;
};

// =================================================================
//  Object Loaded Sink (NPC 3D 加载/卸载)
//
//  辅助节点 (非关键, 但推荐保留):
//  NPC 加载时立即刷新 lastLocation + lastUpdateTime
//  TickManager 下次 tick 也会更新, 但此 Sink 减少延迟
//  对 AutoCleanup 的 stale 判定精度有帮助
// =================================================================
class ObjectLoadedSink : public RE::BSTEventSink<RE::TESObjectLoadedEvent>
{
public:
  static ObjectLoadedSink* GetSingleton()
  {
    static ObjectLoadedSink instance;
    return &instance;
  }

  RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent* event, RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override
  {
    if (!event || !event->loaded)
      return RE::BSEventNotifyControl::kContinue;

    auto* form = RE::TESForm::LookupByID(event->formID);
    if (!form)
      return RE::BSEventNotifyControl::kContinue;

    auto* actor = form->As<RE::Actor>();
    if (!actor)
      return RE::BSEventNotifyControl::kContinue;

    auto* storage = Storage::GetSingleton();
    auto id       = actor->GetFormID();

    auto r = storage->GetRecipient(id);
    if (r) {
      if (auto* loc = actor->GetCurrentLocation())
        r->lastLocation = loc->GetName();

      if (auto* cal = RE::Calendar::GetSingleton())
        r->lastUpdateTime = cal->GetCurrentGameTime();

      // 恢复 locationLeftTime (回到之前离开的位置)
      if (r->locationLeftTime > 0.0f)
        r->locationLeftTime = 0.0f;
    }

    return RE::BSEventNotifyControl::kContinue;
  }

private:
  ObjectLoadedSink() = default;
};

// =================================================================
//  注册
//
//  在 main.cpp kDataLoaded 时调用
//  三个 Sink 的职责:
//    Death       — 清除死亡 actor 数据 (关键)
//    Location    — 驱动位置清理 (关键)
//    ObjectLoaded — 刷新 NPC 位置/时间 (辅助)
// =================================================================
void Register()
{
  auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
  if (!holder) {
    logger::error("[Fertility] ScriptEventSourceHolder not available");
    return;
  }

  holder->AddEventSink<RE::TESDeathEvent>(DeathEventSink::GetSingleton());
  holder->AddEventSink<RE::TESActorLocationChangeEvent>(LocationEventSink::GetSingleton());
  holder->AddEventSink<RE::TESObjectLoadedEvent>(ObjectLoadedSink::GetSingleton());

  logger::info("[Fertility] Event sinks registered (Death, Location, ObjectLoaded)");
}

}  // namespace Fertility::Events
