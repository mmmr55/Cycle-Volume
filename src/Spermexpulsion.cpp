// =====================================================================
//  SpermExpulsion.cpp — 按键长按排出系统
//
//  DD 兼容逻辑全部委托给 RaceOverrides:
//    RaceOverrides::IsExpulsionBlocked(actor) — DD 类别拒绝排出
//    RaceOverrides::HasNoStripKeyword(armor)  — 脱衣时跳过 DD 渲染件
//    RaceOverrides::ShouldStrip(armor)        — 槽位匹配
//
//  本文件只负责: 输入检测 → 状态机 → 脱衣/排出/穿回 → 动画/通知
// =====================================================================

#include "SpermExpulsion.h"
#include "ConfigManager.h"
#include "MorphManager.h"

#include <algorithm>
#include <format>

namespace Fertility
{

static constexpr const char* kGVarExpelling = "FertilityExpelling";

// =====================================================================
//  帧调度
// =====================================================================

static void ScheduleExpulsionFrame()
{
  auto* taskIntfc = SKSE::GetTaskInterface();
  if (!taskIntfc)
    return;
  taskIntfc->AddTask([]() {
    if (Fertility::enabled && Fertility::expulsionEnabled)
      SpermExpulsion::GetSingleton()->Update();
    ScheduleExpulsionFrame();
  });
}

void SpermExpulsion::ScheduleFrame()
{
  ScheduleExpulsionFrame();
}

// =====================================================================
//  初始化
// =====================================================================

void SpermExpulsion::Initialize()
{
  if (auto* inputMgr = RE::BSInputDeviceManager::GetSingleton())
    inputMgr->AddEventSink(this);

  ScheduleFrame();

  logger::info("[Fertility] SpermExpulsion initialized (key={})", expulsionKey);
}

// =====================================================================
//  输入事件
// =====================================================================

RE::BSEventNotifyControl SpermExpulsion::ProcessEvent(RE::InputEvent* const* events, RE::BSTEventSource<RE::InputEvent*>*)
{
  if (!events || !expulsionEnabled)
    return RE::BSEventNotifyControl::kContinue;

  for (auto* evt = *events; evt; evt = evt->next) {
    auto* btn = evt->AsButtonEvent();
    if (!btn)
      continue;
    if (btn->GetIDCode() != static_cast<std::uint32_t>(expulsionKey))
      continue;
    if (btn->GetDevice() != RE::INPUT_DEVICE::kKeyboard)
      continue;

    if (btn->IsDown() && !_keyHeld) {
      _keyHeld   = true;
      _holdStart = Clock::now();
    } else if (btn->IsUp() && _keyHeld) {
      _keyHeld = false;
      if (_expelling) {
        SKSE::GetTaskInterface()->AddTask([this]() {
          if (auto* player = RE::PlayerCharacter::GetSingleton())
            EndExpulsion(player);
        });
      }
    }
  }

  return RE::BSEventNotifyControl::kContinue;
}

// =====================================================================
//  每帧更新
// =====================================================================

void SpermExpulsion::Update()
{
  if (!expulsionEnabled)
    return;

  auto* player = RE::PlayerCharacter::GetSingleton();
  if (!player)
    return;

  if (auto* ui = RE::UI::GetSingleton(); ui && ui->GameIsPaused())
    return;

  auto now = Clock::now();

  // ── 长按检测 ──
  if (_keyHeld && !_expelling) {
    float held = std::chrono::duration<float>(now - _holdStart).count();
    if (held >= expulsionHoldSec) {

      auto id = player->GetFormID();
      auto r  = Storage::GetSingleton()->GetRecipient(id);
      if (!r || r->IsPregnant() || r->sperm.empty())
        return;

      // ★ Layer 1: DD 类别阻止
      if (RaceOverrides::GetSingleton()->IsExpulsionBlocked(player)) {
        if (eventMessages)
          RE::SendHUDMessage::ShowHUDMessage("Cannot expel while wearing a restricting device.");
        _keyHeld = false;
        return;
      }

      BeginExpulsion(player);
    }
  }

  // ── 排出 tick ──
  if (_expelling && _keyHeld) {
    float elapsed = std::chrono::duration<float>(now - _lastExpelTick).count();
    if (elapsed >= 1.0f) {
      _lastExpelTick = now;
      TickExpulsion(player);
    }
  }
}

// =====================================================================
//  开始排出
// =====================================================================

void SpermExpulsion::BeginExpulsion(RE::Actor* player)
{
  _expelling     = true;
  _lastExpelTick = Clock::now();
  _initialTotal  = GetTotalSperm(player->GetFormID());

  if (_initialTotal <= 0.01f) {
    _expelling = false;
    return;
  }

  StripArmor(player);

  if (expulsionAnimation) {
    player->SetGraphVariableBool(kGVarExpelling, true);
    player->NotifyAnimationGraph("IdleForceDefaultState");
  }

  if (auto* ctrl = RE::ControlMap::GetSingleton())
    ctrl->ToggleControls(RE::ControlMap::UEFlag::kMovement, false, true);

  if (eventMessages)
    RE::SendHUDMessage::ShowHUDMessage("Expelling...");

  if (verboseMode)
    logger::info("[Fertility] Expulsion started: total={:.0f}", _initialTotal);
}

// =====================================================================
//  排出 tick (每秒)
// =====================================================================

void SpermExpulsion::TickExpulsion(RE::Actor* player)
{
  auto id       = player->GetFormID();
  auto* storage = Storage::GetSingleton();

  auto r = storage->GetRecipient(id);
  if (!r || r->sperm.empty()) {
    EndExpulsion(player);
    return;
  }

  float currentTotal = GetTotalSperm(id);
  if (currentTotal <= 0.01f) {
    EndExpulsion(player);
    return;
  }

  // 绝对量 = initialTotal × rate, 转为当前量的比例
  float expelAbsolute = _initialTotal * expulsionRatePerSec;
  float fraction      = std::clamp(expelAbsolute / currentTotal, 0.0f, 1.0f);

  float expelled = storage->ExpelPortion(id, fraction);

  // 更新 morph
  auto* cal     = RE::Calendar::GetSingleton();
  float now     = cal ? cal->GetCurrentGameTime() : 0.0f;
  float vol     = r->GetCurrentVolume(now);
  float fullVol = spermFullVolume > 0.0f ? spermFullVolume : 100.0f;
  float norm    = std::clamp(vol / fullVol * 1.5f, 0.0f, 1.5f);
  MorphManager::GetSingleton()->UpdateSpermInflation(id, norm);

  if (verboseMode)
    logger::info("[Fertility] Expulsion tick: {:.1f}mL expelled, {:.0f} remaining", expelled, GetTotalSperm(id));

  if (GetTotalSperm(id) <= 0.01f)
    EndExpulsion(player);
}

// =====================================================================
//  结束排出
// =====================================================================

void SpermExpulsion::EndExpulsion(RE::Actor* player)
{
  if (!_expelling)
    return;

  _expelling = false;

  if (expulsionAnimation)
    player->SetGraphVariableBool(kGVarExpelling, false);

  if (auto* ctrl = RE::ControlMap::GetSingleton())
    ctrl->ToggleControls(RE::ControlMap::UEFlag::kMovement, true, true);

  if (expulsionReequip)
    ReequipArmor(player);

  // 最终 morph
  auto id = player->GetFormID();
  auto r  = Storage::GetSingleton()->GetRecipient(id);
  if (r) {
    auto* cal     = RE::Calendar::GetSingleton();
    float now     = cal ? cal->GetCurrentGameTime() : 0.0f;
    float vol     = r->GetCurrentVolume(now);
    float fullVol = spermFullVolume > 0.0f ? spermFullVolume : 100.0f;
    float norm    = std::clamp(vol / fullVol * 1.5f, 0.0f, 1.5f);
    MorphManager::GetSingleton()->UpdateSpermInflation(id, norm);
  }

  if (eventMessages)
    RE::SendHUDMessage::ShowHUDMessage("Expulsion complete.");

  if (verboseMode)
    logger::info("[Fertility] Expulsion ended: remaining={:.0f}", GetTotalSperm(player->GetFormID()));
}

// =====================================================================
//  脱衣
//    - ShouldStrip(armor)       → 槽位匹配
//    - HasNoStripKeyword(armor) → DD 渲染件保护
// =====================================================================

void SpermExpulsion::StripArmor(RE::Actor* player)
{
  _strippedItems.clear();
  auto* overrides = RaceOverrides::GetSingleton();
  auto* equipMgr  = RE::ActorEquipManager::GetSingleton();
  if (!equipMgr)
    return;

  auto* changes = player->GetInventoryChanges();
  if (!changes || !changes->entryList)
    return;

  for (auto* entry : *changes->entryList) {
    if (!entry || !entry->object)
      continue;

    auto* armor = entry->object->As<RE::TESObjectARMO>();
    if (!armor)
      continue;

    // 已装备?
    bool equipped = false;
    if (entry->extraLists) {
      for (auto* xl : *entry->extraLists) {
        if (xl && xl->HasType(RE::ExtraDataType::kWorn)) {
          equipped = true;
          break;
        }
      }
    }
    if (!equipped)
      continue;

    // 槽位匹配?
    if (!overrides->ShouldStrip(armor))
      continue;

    // ★ Layer 2: NoStrip 关键词保护 (DD 渲染件)
    if (overrides->HasNoStripKeyword(armor)) {
      if (verboseMode)
        logger::info("[Fertility] Strip skipped (NoStrip): {} ({:08X})", armor->GetName(), armor->GetFormID());
      continue;
    }

    equipMgr->UnequipObject(player, armor);
    _strippedItems.push_back({armor});

    if (verboseMode)
      logger::info("[Fertility] Stripped: {} ({:08X})", armor->GetName(), armor->GetFormID());
  }
}

void SpermExpulsion::ReequipArmor(RE::Actor* player)
{
  auto* equipMgr = RE::ActorEquipManager::GetSingleton();
  if (!equipMgr)
    return;

  for (auto& item : _strippedItems)
    if (item.armor)
      equipMgr->EquipObject(player, item.armor);

  if (verboseMode && !_strippedItems.empty())
    logger::info("[Fertility] Re-equipped {} items", _strippedItems.size());

  _strippedItems.clear();
}

// =====================================================================
//  辅助
// =====================================================================

float SpermExpulsion::GetTotalSperm(RE::FormID id) const
{
  auto r = Storage::GetSingleton()->GetRecipient(id);
  if (!r)
    return 0.0f;
  float total = 0.0f;
  for (auto& sp : r->sperm)
    total += sp.amount;
  return total;
}

}  // namespace Fertility
