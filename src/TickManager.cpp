#include "TickManager.h"
#include "MorphManager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <random>
#include <ranges>

namespace Fertility
{

static thread_local std::mt19937 tls_rng{std::random_device{}()};

static int RollD100()
{
  return std::uniform_int_distribution<int>(1, 100)(tls_rng);
}

static float RollRange(float lo, float hi)
{
  if (lo >= hi)
    return lo;
  return std::uniform_real_distribution<float>(lo, hi)(tls_rng);
}

float TickManager::Now()
{
  auto* cal = RE::Calendar::GetSingleton();
  return cal ? cal->GetCurrentGameTime() : 0.0f;
}

void TickManager::Initialize()
{
  _faction = RE::TESForm::LookupByID<RE::TESFaction>(0xFE000800);
  if (_faction)
    logger::info("[Fertility] Faction {:08X} cached", _faction->GetFormID());
  else
    logger::warn("[Fertility] Faction FE000800 not found");
  _lastTickTime = Now();
}

// ═══════════════════════════════════════════════════════════════
//  Tick — called from PlayerCharacter::Update hook
// ═══════════════════════════════════════════════════════════════

void TickManager::Tick()
{
  auto* ui = RE::UI::GetSingleton();
  if (!ui || ui->GameIsPaused())
    return;

  float now = Now();
  if (now <= 0.0f)
    return;

  // always advance labor timers
  UpdateLaborTimers();

  // check tick interval for recipient processing
  float elH = (now - _lastTickTime) * 24.0f;
  if (elH < tickIntervalHours)
    return;
  _lastTickTime = now;

  if (_cursor >= _queue.size())
    RebuildQueue();

  std::size_t remaining = _queue.size() - _cursor;
  std::size_t n         = std::min(static_cast<std::size_t>(batchSize), remaining);

  for (std::size_t i = 0; i < n; ++i)
    TickRecipient(_queue[_cursor + i], now);

  _cursor += n;
}

void TickManager::RebuildQueue()
{
  _queue.clear();
  _cursor = 0;
  Storage::GetSingleton()->ForEachRecipient([&](RE::FormID id, RecipientData&) {
    _queue.push_back(id);
  });
}

std::size_t TickManager::GetTrackedCount() const
{
  return Storage::GetSingleton()->RecipientCount();
}

// ═══════════════════════════════════════════════════════════════
//  Per-recipient tick
//
//  All RecipientData mutations inside WithRecipientWrite.
//  Game API calls after the lock is released.
// ═══════════════════════════════════════════════════════════════

void TickManager::TickRecipient(RE::FormID id, float now)
{
  auto* storage = Storage::GetSingleton();

  // results collected inside lock, consumed outside
  int factionRank     = FRank::kNone;
  bool isPregnant     = false;
  bool wasPregnant    = false;  // detect miscarriage
  bool inRecovery     = false;
  float progress      = 0.0f;
  int trimester       = 0;
  CyclePhase phase    = CyclePhase::Follicular;
  float spermInflNorm = 0.0f;
  bool clearMorphs    = false;
  bool shouldLabor    = false;
  float laborProgress = 0.0f;
  bool conceived      = false;
  bool miscarriage    = false;
  bool nearLabor      = false;
  bool spermExpired   = false;
  std::string fatherName;

  bool found = storage->WithRecipientWrite(id, [&](RecipientData& r) {
    if (IsInLabor(id)) {
      factionRank = FRank::kLabor;
      isPregnant  = r.IsPregnant();
      return;
    }

    r.UpdateInflation(now);

    if (r.InRecovery(now, recoveryDays)) {
      inRecovery       = true;
      factionRank      = CalcFactionRank(r, now);
      r.lastUpdateTime = now;
      return;
    }

    auto* player  = RE::PlayerCharacter::GetSingleton();
    bool isPlayer = player && id == player->GetFormID();

    wasPregnant = r.IsPregnant();

    if (!wasPregnant)
      TickNonPregnant(r, id, isPlayer, now);

    isPregnant = r.IsPregnant();

    if (isPregnant && !wasPregnant) {
      conceived = true;
      if (r.pregnancy)
        fatherName = r.pregnancy->fatherName;
    }

    if (isPregnant) {
      TickPregnant(r, id, isPlayer, now);
      // re-check — miscarriage may have cleared it
      if (!r.IsPregnant() && wasPregnant) {
        miscarriage = true;
        isPregnant  = false;
        clearMorphs = true;
      }
    }

    if (isPregnant && r.pregnancy) {
      float gd   = std::max(1.0f, gestationDays);
      float days = r.pregnancy->DaysSinceConception(now);
      progress   = std::clamp(days / gd, 0.0f, 1.0f);
      int triLen = std::max(1, static_cast<int>(std::ceil(gd / 3.0f)));
      int dayInt = static_cast<int>(days);
      trimester  = (dayInt < triLen) ? 1 : (dayInt < triLen * 2) ? 2 : 3;

      if (dayInt >= static_cast<int>(gd)) {
        shouldLabor   = true;
        laborProgress = progress;
      }
      if (dayInt >= static_cast<int>(gd) - 1)
        nearLabor = true;

      if (!r.sperm.empty())
        r.sperm.clear();
    }

    if (!isPregnant && !clearMorphs) {
      float vol     = r.GetCurrentVolume(now);
      float fullVol = spermFullVolume > 0.0f ? spermFullVolume : 100.0f;
      spermInflNorm = std::clamp(vol / fullVol * 1.5f, 0.0f, 1.5f);

      if (!r.sperm.empty() && r.TotalViableSperm(now) <= 0.01f) {
        r.sperm.clear();
        spermExpired = true;
      }
    }

    phase            = r.phase;
    factionRank      = CalcFactionRank(r, now);
    r.lastUpdateTime = now;
  });

  if (!found)
    return;

  // ── post-lock: game API calls ──

  auto* actor   = RE::TESForm::LookupByID<RE::Actor>(id);
  auto* player  = RE::PlayerCharacter::GetSingleton();
  bool isPlayer = player && id == player->GetFormID();

  ApplyFactionRank(actor, factionRank);

  if (shouldLabor) {
    if (actor && actor->Is3DLoaded() && !IsActorBusy(actor))
      BeginLabor(actor, laborProgress);
    else if (actor) {
      storage->GiveBirth(id);
      MorphManager::GetSingleton()->ClearMorphs(id);
      GiveSoulGem(actor, laborProgress);
    } else {
      storage->GiveBirth(id);
      MorphManager::GetSingleton()->ClearMorphs(id);
    }
  }

  auto* morphMgr = MorphManager::GetSingleton();
  if (clearMorphs)
    morphMgr->ClearMorphs(id);
  else if (isPregnant && !shouldLabor) {
    morphMgr->UpdatePregnancyMorph(id, progress);
    morphMgr->UpdateSpermInflation(id, 0.0f);
  } else if (!isPregnant)
    morphMgr->UpdateSpermInflation(id, spermInflNorm);

  if (miscarriage && actor)
    GiveSoulGem(actor, progress);

  if (actor && actor->Is3DLoaded()) {
    // re-read for graph vars under lock (brief read)
    storage->WithRecipientRead(id, [&](const RecipientData& r) {
      SyncGraphVariables(actor, r, now);
    });
  }

  if (isPlayer) {
    if (conceived)
      AddPlayerFlag(PFlag::kConceived);
    if (nearLabor)
      AddPlayerFlag(PFlag::kNearLabor);
    if (miscarriage)
      AddPlayerFlag(PFlag::kMiscarriage);
    if (spermExpired)
      AddPlayerFlag(PFlag::kAllSpermExpired);
  }

  if (eventMessages && actor) {
    if (conceived && !fatherName.empty())
      RE::SendHUDMessage::ShowHUDMessage(std::format("{} got {} pregnant!", fatherName, actor->GetName()).c_str());
    if (miscarriage)
      RE::SendHUDMessage::ShowHUDMessage(std::format("{} had a miscarriage", actor->GetName()).c_str());
  }
}

// ═══════════════════════════════════════════════════════════════
//  Non-pregnant logic — inside WithRecipientWrite
// ═══════════════════════════════════════════════════════════════

void TickManager::TickNonPregnant(RecipientData& r, RE::FormID, bool isPlayer, float now)
{
  if (r.phaseStartTime <= 0.0f) {
    r.phaseStartTime    = now;
    r.phaseDurationDays = RollPhaseDuration(r.phase);
  }

  for (int i = 0; i < 8; ++i) {
    if (now - r.phaseStartTime < r.phaseDurationDays)
      break;
    AdvanceCyclePhase(r);
  }

  r.PurgeExpiredSperm(now);

  if (!r.sperm.empty()) {
    if (TryConception(r, now)) {
      if (isPlayer) {
        auto* storage = Storage::GetSingleton();
        storage->SetPlayerBabyHealth(100.0f);
        storage->SetPlayerLastSleepTime(now);
      }
    }
  }
}

// ═══════════════════════════════════════════════════════════════
//  Pregnant logic — inside WithRecipientWrite
// ═══════════════════════════════════════════════════════════════

void TickManager::TickPregnant(RecipientData& r, RE::FormID, bool isPlayer, float now)
{
  if (!r.IsPregnant() || !isPlayer || !miscarriageEnabled)
    return;

  auto* storage   = Storage::GetSingleton();
  float sleepTime = 0.0f;
  float health    = 0.0f;
  storage->WithPlayerStateRead([&](const PlayerState& ps) {
    sleepTime = ps.lastSleepTime;
    health    = ps.babyHealth;
  });

  if (static_cast<int>(now - sleepTime) <= 0)
    return;

  health -= 1.0f;
  if (health <= 0.0f) {
    r.ClearFertilityData();
    r.pregnancy.reset();
    storage->SetPlayerBabyHealth(100.0f);
  } else {
    storage->SetPlayerBabyHealth(health);
  }
}

// ═══════════════════════════════════════════════════════════════
//  Cycle
// ═══════════════════════════════════════════════════════════════

float TickManager::RollPhaseDuration(CyclePhase phase)
{
  switch (phase) {
  case CyclePhase::Menstruation:
    return RollRange(menstruationDaysMin, menstruationDaysMax);
  case CyclePhase::Follicular:
    return RollRange(follicularDaysMin, follicularDaysMax);
  case CyclePhase::Ovulation:
    return RollRange(ovulationDaysMin, ovulationDaysMax);
  case CyclePhase::Luteal:
    return RollRange(lutealDaysMin, lutealDaysMax);
  default:
    return 5.0f;
  }
}

void TickManager::AdvanceCyclePhase(RecipientData& r)
{
  auto old = r.phase;
  r.phase  = NextPhase(r.phase);
  r.phaseStartTime += r.phaseDurationDays;
  r.phaseDurationDays = RollPhaseDuration(r.phase);

  if (verboseMode)
    logger::info("[Fertility] {:08X} phase {} → {} (dur={:.1f}d)", r.formID, static_cast<int>(old), static_cast<int>(r.phase), r.phaseDurationDays);
}

// ═══════════════════════════════════════════════════════════════
//  Conception — phase-modified probability
// ═══════════════════════════════════════════════════════════════

static float GetPhaseMultiplier(CyclePhase phase)
{
  switch (phase) {
  case CyclePhase::Menstruation:
    return phaseMultMenstruation;
  case CyclePhase::Follicular:
    return phaseMultFollicular;
  case CyclePhase::Ovulation:
    return phaseMultOvulation;
  case CyclePhase::Luteal:
    return phaseMultLuteal;
  default:
    return 0.5f;
  }
}

bool TickManager::TryConception(RecipientData& r, float now)
{
  float phaseMult = GetPhaseMultiplier(r.phase);
  if (phaseMult <= 0.0f)
    return false;

  struct Cand
  {
    std::size_t idx;
    float eff;
  };
  std::vector<Cand> cands;
  cands.reserve(r.sperm.size());

  for (std::size_t i = 0; i < r.sperm.size(); ++i) {
    auto& d = r.sperm[i];
    if (!d.IsViable(now))
      continue;
    if (d.type != InseminationType::Natural && d.type != InseminationType::Anal && d.type != InseminationType::Magical)
      continue;
    float eff = d.EffectiveAmount(now);
    if (eff > 0.01f)
      cands.push_back({i, eff});
  }
  if (cands.empty())
    return false;

  std::ranges::sort(cands, std::ranges::greater{}, &Cand::eff);

  for (auto& [idx, eff] : cands) {
    float amtF = 1.0f - std::exp(-eff / amountHalfLife);
    float prob = amtF * baseFertility * phaseMult;
    int thr    = std::clamp(static_cast<int>(prob * 100.0f), 1, 95);

    if (RollD100() <= thr) {
      auto& dep = r.sperm[idx];
      PregnancyState preg;
      preg.conceptionTime = now;
      preg.father         = dep.donor;
      preg.fatherName     = dep.donorName;
      preg.fatherRaceID   = dep.donorRaceID;
      preg.creatureFather = dep.isCreature;
      r.pregnancy         = std::move(preg);
      r.sperm.clear();

      if (verboseMode)
        logger::info("[Fertility] {:08X} conceived by {} (eff={:.1f} phase={} prob={:.1f}%)", r.formID, dep.donorName, eff, static_cast<int>(r.phase),
                     prob * 100.0f);
      return true;
    }
  }
  return false;
}

// ═══════════════════════════════════════════════════════════════
//  Faction rank
// ═══════════════════════════════════════════════════════════════

int TickManager::CalcFactionRank(const RecipientData& r, float now) const
{
  {
    std::lock_guard lk(_laborMtx);
    if (_laboring.contains(r.formID))
      return FRank::kLabor;
  }

  if (r.InRecovery(now, recoveryDays)) {
    if (r.pregnancy.has_value() && r.pregnancy->birthTime > 0.0f) {
      float recDays = now - r.pregnancy->birthTime;
      float recMax  = recoveryDays > 0.0f ? recoveryDays : 3.0f;
      return FRank::kRecoveryBase + std::clamp(static_cast<int>(recDays / recMax * 14.0f), 0, 13);
    }
    return FRank::kRecoveryBase;
  }

  if (r.IsPregnant()) {
    float gd   = std::max(1.0f, gestationDays);
    float prog = std::clamp(r.pregnancy->DaysSinceConception(now) / gd, 0.0f, 1.0f);
    return std::clamp(static_cast<int>(prog * 100.0f), FRank::kPregMin, FRank::kPregMax);
  }

  return FRank::kNone;
}

void TickManager::ApplyFactionRank(RE::Actor* actor, int rank)
{
  if (!_faction || !actor)
    return;
  std::int8_t er = (rank > 0) ? static_cast<std::int8_t>(std::min(rank, 127)) : -2;
  actor->AddToFaction(_faction, er);
}

// ═══════════════════════════════════════════════════════════════
//  Graph variables
// ═══════════════════════════════════════════════════════════════

void TickManager::SyncGraphVariables(RE::Actor* actor, const RecipientData& r, float now)
{
  if (!actor || !actor->Is3DLoaded())
    return;

  bool pregnant = r.IsPregnant();
  float prog    = 0.0f;
  int tri       = 0;
  if (pregnant && r.pregnancy) {
    float gd   = std::max(1.0f, gestationDays);
    float days = r.pregnancy->DaysSinceConception(now);
    prog       = std::clamp(days / gd, 0.0f, 1.0f);
    int triLen = std::max(1, static_cast<int>(std::ceil(gd / 3.0f)));
    int dayInt = static_cast<int>(days);
    tri        = (dayInt < triLen) ? 1 : (dayInt < triLen * 2) ? 2 : 3;
  }

  actor->SetGraphVariableBool(GVar::kPregnant, pregnant);
  actor->SetGraphVariableFloat(GVar::kProgress, prog);
  actor->SetGraphVariableInt(GVar::kTrimester, tri);

  LaborPhase lp = LaborPhase::None;
  {
    std::lock_guard lk(_laborMtx);
    if (auto it = _laboring.find(r.formID); it != _laboring.end())
      lp = it->second.phase;
  }

  actor->SetGraphVariableBool(GVar::kInLabor, lp != LaborPhase::None);
  actor->SetGraphVariableInt(GVar::kLaborPhase, static_cast<int>(lp));
  actor->SetGraphVariableBool(GVar::kRecovery, r.InRecovery(now, recoveryDays));
  actor->SetGraphVariableInt(GVar::kCyclePhase, static_cast<int>(r.phase));
}

// ═══════════════════════════════════════════════════════════════
//  Player flags
// ═══════════════════════════════════════════════════════════════

void TickManager::AddPlayerFlag(int flag)
{
  std::lock_guard lk(_flagsMtx);
  _playerFlags |= flag;
}

int TickManager::ConsumePlayerFlags()
{
  std::lock_guard lk(_flagsMtx);
  int f        = _playerFlags;
  _playerFlags = 0;
  return f;
}

int TickManager::PeekPlayerFlags() const
{
  std::lock_guard lk(_flagsMtx);
  return _playerFlags;
}

// ═══════════════════════════════════════════════════════════════
//  Soul gem
// ═══════════════════════════════════════════════════════════════

void TickManager::GiveSoulGem(RE::Actor* actor, float progress)
{
  if (!actor)
    return;
  struct Tier
  {
    float threshold;
    RE::FormID formID;
    const char* name;
  };
  static constexpr std::array<Tier, 5> tiers{{
      {0.8f, VanillaSoulGem::kGrand, "Grand"},
      {0.6f, VanillaSoulGem::kGreater, "Greater"},
      {0.4f, VanillaSoulGem::kCommon, "Common"},
      {0.2f, VanillaSoulGem::kLesser, "Lesser"},
      {0.0f, VanillaSoulGem::kPetty, "Petty"},
  }};
  auto it          = std::ranges::find_if(tiers, [progress](const Tier& t) {
    return progress >= t.threshold;
  });
  const auto& tier = (it != tiers.end()) ? *it : tiers.back();
  auto* gem        = RE::TESForm::LookupByID<RE::TESBoundObject>(tier.formID);
  if (!gem)
    return;
  actor->AddObjectToContainer(gem, nullptr, 1, nullptr);
  if (eventMessages)
    RE::SendHUDMessage::ShowHUDMessage(std::format("{} received a {} Soul Gem", actor->GetName(), tier.name).c_str());
}

// ═══════════════════════════════════════════════════════════════
//  Labor
// ═══════════════════════════════════════════════════════════════

void TickManager::BeginLabor(RE::Actor* actor, float pregProgress)
{
  if (!actor)
    return;
  RE::FormID id      = actor->GetFormID();
  bool shouldControl = actor->Is3DLoaded();

  {
    std::lock_guard lk(_laborMtx);
    if (_laboring.contains(id))
      return;
    LaborState ls;
    ls.phase        = LaborPhase::Contractions;
    ls.phaseStart   = LaborState::Clock::now();
    ls.pregProgress = pregProgress;
    ls.controlled   = shouldControl;
    _laboring[id]   = ls;
  }

  ApplyFactionRank(actor, FRank::kLabor);
  if (shouldControl) {
    if (actor->IsPlayerRef())
      DisablePlayerInput();
    else
      FreezeActor(actor);
  }
  actor->SetGraphVariableBool(GVar::kInLabor, true);
  actor->SetGraphVariableInt(GVar::kLaborPhase, static_cast<int>(LaborPhase::Contractions));
  if (eventMessages)
    RE::SendHUDMessage::ShowHUDMessage(std::format("{} has gone into labor!", actor->GetName()).c_str());
}

void TickManager::AdvanceLaborPhase(RE::FormID, LaborState& ls)
{
  switch (ls.phase) {
  case LaborPhase::Contractions:
    ls.phase      = LaborPhase::Delivery;
    ls.phaseStart = LaborState::Clock::now();
    break;
  case LaborPhase::Delivery:
    ls.phase      = LaborPhase::Done;
    ls.phaseStart = LaborState::Clock::now();
    break;
  default:
    break;
  }
}

void TickManager::UpdateLaborTimers()
{
  auto clock_now = LaborState::Clock::now();
  std::vector<RE::FormID> toComplete;
  struct PhaseUpdate
  {
    RE::FormID id;
    LaborPhase newPhase;
  };
  std::vector<PhaseUpdate> updates;

  {
    std::lock_guard lk(_laborMtx);
    for (auto& [id, ls] : _laboring) {
      float elapsed = std::chrono::duration<float>(clock_now - ls.phaseStart).count();
      if (elapsed < laborPhaseSec)
        continue;
      if (ls.phase == LaborPhase::Done)
        toComplete.push_back(id);
      else {
        AdvanceLaborPhase(id, ls);
        updates.push_back({id, ls.phase});
      }
    }
  }

  for (auto& [id, newPhase] : updates) {
    if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(id); actor && actor->Is3DLoaded())
      actor->SetGraphVariableInt(GVar::kLaborPhase, static_cast<int>(newPhase));
  }
  for (auto id : toComplete)
    CompleteBirth(id);
}

void TickManager::CompleteBirth(RE::FormID id)
{
  float pregProgress = 1.0f;
  bool wasControlled = false;
  {
    std::lock_guard lk(_laborMtx);
    auto it = _laboring.find(id);
    if (it == _laboring.end())
      return;
    pregProgress  = it->second.pregProgress;
    wasControlled = it->second.controlled;
    _laboring.erase(it);
  }

  Storage::GetSingleton()->GiveBirth(id);
  MorphManager::GetSingleton()->ClearMorphs(id);

  auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
  if (actor)
    GiveSoulGem(actor, pregProgress);

  if (actor && wasControlled) {
    if (actor->IsPlayerRef())
      EnablePlayerInput();
    else
      UnfreezeActor(actor);
  }
  if (actor && actor->Is3DLoaded()) {
    actor->SetGraphVariableBool(GVar::kInLabor, false);
    actor->SetGraphVariableInt(GVar::kLaborPhase, 0);
    actor->SetGraphVariableBool(GVar::kRecovery, true);
    actor->SetGraphVariableBool(GVar::kPregnant, false);
    actor->SetGraphVariableFloat(GVar::kProgress, 0.0f);
    actor->SetGraphVariableInt(GVar::kTrimester, 0);
  }
  if (actor)
    ApplyFactionRank(actor, FRank::kRecoveryBase);
  if (actor && actor->IsPlayerRef())
    AddPlayerFlag(PFlag::kLaborCompleted);
  if (verboseMode)
    logger::info("[Fertility] Birth complete {:08X}, gem={:.0f}%", id, pregProgress * 100.0f);
}

bool TickManager::IsInLabor(RE::FormID id) const
{
  std::lock_guard lk(_laborMtx);
  return _laboring.contains(id);
}

// ═══════════════════════════════════════════════════════════════
//  Input / AI
// ═══════════════════════════════════════════════════════════════

bool TickManager::IsActorBusy(RE::Actor* actor)
{
  if (!actor)
    return true;
  if (actor->IsPlayerRef()) {
    auto* ui = RE::UI::GetSingleton();
    if (ui) {
      if (ui->GameIsPaused())
        return true;
      static constexpr std::array kBlockingMenus = {
          RE::DialogueMenu::MENU_NAME, RE::BarterMenu::MENU_NAME,      RE::ContainerMenu::MENU_NAME,
          RE::CraftingMenu::MENU_NAME, RE::LockpickingMenu::MENU_NAME, RE::BookMenu::MENU_NAME,
      };
      if (std::ranges::any_of(kBlockingMenus, [ui](auto name) {
            return ui->IsMenuOpen(name);
          }))
        return true;
    }
  }
  if (actor->IsBleedingOut() || actor->IsOnMount() || actor->IsInKillMove())
    return true;
  if (actor->GetOccupiedFurniture().get())
    return true;
  return false;
}

void TickManager::DisablePlayerInput()
{
  if (auto* c = RE::ControlMap::GetSingleton())
    c->ToggleControls(RE::ControlMap::UEFlag::kAll, false, true);
}

void TickManager::EnablePlayerInput()
{
  if (auto* c = RE::ControlMap::GetSingleton())
    c->ToggleControls(RE::ControlMap::UEFlag::kAll, true, true);
}

void TickManager::FreezeActor(RE::Actor* a)
{
  if (a)
    a->SetActorValue(RE::ActorValue::kParalysis, 1.0f);
}
void TickManager::UnfreezeActor(RE::Actor* a)
{
  if (a)
    a->SetActorValue(RE::ActorValue::kParalysis, 0.0f);
}

}  // namespace Fertility
