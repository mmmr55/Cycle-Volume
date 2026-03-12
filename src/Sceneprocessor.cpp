#include "SceneProcessor.h"
#include "MorphManager.h"
#include "TickManager.h"

#include <format>

namespace Fertility
{

InseminationType SceneProcessor::MapInteraction(int ctype, bool isFF)
{
  if (isFF)
    return InseminationType::Magical;
  switch (ctype) {
  case CTYPE_Vaginal:
    return InseminationType::Natural;
  case CTYPE_Anal:
    return InseminationType::Anal;
  case CTYPE_Oral:
    return InseminationType::Oral;
  case CTYPE_Grinding:
    return InseminationType::Magical;
  default:
    return InseminationType::Natural;
  }
}

int SceneProcessor::ProcessPair(RE::Actor* recipient, RE::Actor* donor, int recipientSex, int donorSex, InseminationType type)
{
  if (!recipient || !donor || recipient == donor)
    return 0;

  auto* storage = Storage::GetSingleton();
  auto rID      = recipient->GetFormID();

  auto rd = storage->GetRecipient(rID);
  if (!rd) {
    rd = storage->RegisterRecipient(recipient);
    if (!rd)
      return 0;
  }

  if (!storage->Inseminate(recipient, donor, type, IsCreature(donorSex)))
    return 0;

  // try conception — phase-probability based, no window
  bool alreadyPregnant = false;
  storage->WithRecipientRead(rID, [&](const RecipientData& r) {
    alreadyPregnant = r.IsPregnant();
  });

  if (!alreadyPregnant) {
    if (storage->Conceive(rID))
      return 2;
  }

  return 1;
}

int SceneProcessor::ProcessSceneBatch(RE::Actor** actors, const int* sexes, int actorCount, const int* pairs, int pairCount)
{
  if (!actors || !sexes || !pairs || actorCount < 2 || pairCount < 3)
    return 0;

  if (pairCount % 3 != 0)
    logger::warn("[Fertility] ProcessSceneBatch: pairCount {} not multiple of 3", pairCount);

  int totalInsem = 0;
  int totalConc  = 0;
  int numPairs   = pairCount / 3;

  for (int p = 0; p < numPairs; ++p) {
    int recvIdx  = pairs[p * 3 + 0];
    int donorIdx = pairs[p * 3 + 1];
    int ctype    = pairs[p * 3 + 2];

    if (recvIdx < 0 || recvIdx >= actorCount)
      continue;
    if (donorIdx < 0 || donorIdx >= actorCount)
      continue;

    RE::Actor* recipient = actors[recvIdx];
    RE::Actor* donor     = actors[donorIdx];
    if (!recipient || !donor || recipient == donor)
      continue;

    int recvSex  = sexes[recvIdx];
    int donorSex = sexes[donorIdx];
    bool isFF    = IsFemale(recvSex) && IsFemale(donorSex);

    if (!isFF && !CanDonate(donorSex))
      continue;
    if (!CanReceive(recvSex))
      continue;

    InseminationType type = MapInteraction(ctype, isFF);

    int res = ProcessPair(recipient, donor, recvSex, donorSex, type);
    if (res >= 1)
      totalInsem++;
    if (res >= 2)
      totalConc++;

    if (isFF && CanReceive(donorSex)) {
      int res2 = ProcessPair(donor, recipient, donorSex, recvSex, type);
      if (res2 >= 1)
        totalInsem++;
      if (res2 >= 2)
        totalConc++;
    }
  }

  {
    std::lock_guard lk(_batchMtx);
    _lastConceptions = totalConc;
  }
  return totalInsem;
}

int SceneProcessor::ConsumeLastConceptionCount()
{
  std::lock_guard lk(_batchMtx);
  int val          = _lastConceptions;
  _lastConceptions = 0;
  return val;
}

// ── Papyrus ──

namespace
{
  std::int32_t Papyrus_ProcessSceneBatch(RE::StaticFunctionTag*, RE::reference_array<RE::Actor*> actors, std::vector<std::int32_t> sexes,
                                         std::vector<std::int32_t> pairs)
  {
    int actorCount = static_cast<int>(actors.size());
    if (actorCount < 2 || actorCount != static_cast<int>(sexes.size()))
      return 0;
    std::vector<RE::Actor*> ptrs(actorCount);
    for (int i = 0; i < actorCount; ++i)
      ptrs[i] = actors[i];
    return SceneProcessor::GetSingleton()->ProcessSceneBatch(ptrs.data(), sexes.data(), actorCount, pairs.data(), static_cast<int>(pairs.size()));
  }

  std::int32_t Papyrus_ProcessPair(RE::StaticFunctionTag*, RE::Actor* recipient, RE::Actor* donor, std::int32_t recipientSex, std::int32_t donorSex,
                                   std::int32_t ctype)
  {
    if (!recipient || !donor)
      return 0;
    bool isFF = SceneProcessor::IsFemale(recipientSex) && SceneProcessor::IsFemale(donorSex);
    auto type = SceneProcessor::MapInteraction(ctype, isFF);
    return SceneProcessor::GetSingleton()->ProcessPair(recipient, donor, recipientSex, donorSex, type);
  }

  std::int32_t Papyrus_GetLastConceptionCount(RE::StaticFunctionTag*)
  {
    return SceneProcessor::GetSingleton()->ConsumeLastConceptionCount();
  }

  bool Papyrus_IsPregnant(RE::StaticFunctionTag*, RE::Actor* a)
  {
    return a ? Storage::GetSingleton()->IsPregnant(a->GetFormID()) : false;
  }

  float Papyrus_GetPregnancyProgress(RE::StaticFunctionTag*, RE::Actor* a)
  {
    return a ? Storage::GetSingleton()->PregnancyProgress(a->GetFormID()) : 0.0f;
  }

  float Papyrus_GetInflation(RE::StaticFunctionTag*, RE::Actor* a)
  {
    return a ? Storage::GetSingleton()->GetInflation(a->GetFormID()) : 0.0f;
  }

  float Papyrus_GetSpermVolume(RE::StaticFunctionTag*, RE::Actor* a)
  {
    return a ? Storage::GetSingleton()->GetSpermVolume(a->GetFormID()) : 0.0f;
  }

  bool Papyrus_IsEligible(RE::StaticFunctionTag*, RE::Actor* a)
  {
    return a ? Storage::GetSingleton()->IsEligible(a) : false;
  }

  //   [3:0] phase, [4] pregnant, [5] labor, [6] recovery,
  //   [7] viable sperm, [23:16] progress%
  std::int32_t Papyrus_GetStatus(RE::StaticFunctionTag*, RE::Actor* actor)
  {
    if (!actor)
      return 0;
    auto* storage = Storage::GetSingleton();
    auto id       = actor->GetFormID();
    int status    = 0;
    bool found    = storage->WithRecipientRead(id, [&](const RecipientData& r) {
      float now = TickManager::Now();
      status |= (static_cast<int>(r.phase) & 0xF);
      if (r.IsPregnant())
        status |= (1 << 4);
      if (TickManager::GetSingleton()->IsInLabor(id))
        status |= (1 << 5);
      if (r.InRecovery(now, recoveryDays))
        status |= (1 << 6);
      if (r.TotalViableSperm(now) > 0.01f)
        status |= (1 << 7);
      int progPct = static_cast<int>(storage->PregnancyProgress(id) * 100.0f);
      status |= ((progPct & 0xFF) << 16);
    });
    return found ? status : -1;
  }

  std::int32_t Papyrus_ForcePregnancy(RE::StaticFunctionTag*, RE::Actor* recipient, RE::Actor* father)
  {
    if (!recipient)
      return 0;
    auto* storage = Storage::GetSingleton();
    auto rID      = recipient->GetFormID();
    auto r        = storage->GetRecipient(rID);
    if (!r) {
      r = storage->RegisterRecipient(recipient);
      if (!r)
        return 0;
    }

    float now = TickManager::Now();
    PregnancyState preg;
    preg.conceptionTime = now;
    if (father) {
      preg.father       = DonorKey::From(father);
      preg.fatherName   = father->GetName();
      auto* race        = father->GetRace();
      preg.fatherRaceID = race ? race->GetFormID() : 0;
    } else {
      preg.fatherName = "Unknown";
    }

    bool applied = false;
    storage->WithRecipientWrite(rID, [&](RecipientData& rd) {
      if (rd.IsPregnant())
        return;
      rd.pregnancy = std::move(preg);
      rd.sperm.clear();
      applied = true;
    });
    if (!applied)
      return 0;
    if (eventMessages)
      RE::SendHUDMessage::ShowHUDMessage(std::format("{} is now pregnant!", recipient->GetName()).c_str());
    return 1;
  }

  bool Papyrus_ForceOvulation(RE::StaticFunctionTag*, RE::Actor* actor)
  {
    if (!actor)
      return false;
    auto* storage = Storage::GetSingleton();
    auto id       = actor->GetFormID();
    auto r        = storage->GetRecipient(id);
    if (!r) {
      r = storage->RegisterRecipient(actor);
      if (!r)
        return false;
    }

    float now    = TickManager::Now();
    float dur    = TickManager::RollPhaseDuration(CyclePhase::Ovulation);
    bool applied = false;
    storage->WithRecipientWrite(id, [&](RecipientData& rd) {
      if (rd.IsPregnant())
        return;
      rd.phase             = CyclePhase::Ovulation;
      rd.phaseStartTime    = now;
      rd.phaseDurationDays = dur;
      applied              = true;
    });
    if (!applied)
      return false;
    if (eventMessages)
      RE::SendHUDMessage::ShowHUDMessage(std::format("{} is now in peak fertility.", actor->GetName()).c_str());
    return true;
  }

  bool Papyrus_Abort(RE::StaticFunctionTag*, RE::Actor* actor)
  {
    if (!actor)
      return false;
    auto* storage = Storage::GetSingleton();
    auto id       = actor->GetFormID();
    auto r        = storage->GetRecipient(id);
    if (!r || !r->IsPregnant())
      return false;

    float progress = storage->PregnancyProgress(id);
    TickManager::GetSingleton()->GiveSoulGem(actor, progress);
    storage->WithRecipientWrite(id, [&](RecipientData& rd) {
      rd.ClearFertilityData();
      rd.pregnancy.reset();
    });
    MorphManager::GetSingleton()->ClearMorphs(id);
    if (actor->IsPlayerRef())
      storage->SetPlayerBabyHealth(100.0f);
    if (eventMessages)
      RE::SendHUDMessage::ShowHUDMessage(std::format("{} has lost the pregnancy.", actor->GetName()).c_str());
    Storage::FireEvent("FertilityAbort", actor, progress);
    return true;
  }

  bool Papyrus_ForceLabor(RE::StaticFunctionTag*, RE::Actor* actor)
  {
    if (!actor)
      return false;
    auto* storage = Storage::GetSingleton();
    auto id       = actor->GetFormID();
    auto r        = storage->GetRecipient(id);
    if (!r || !r->IsPregnant())
      return false;
    float progress = storage->PregnancyProgress(id);
    if (actor->Is3DLoaded()) {
      TickManager::GetSingleton()->BeginLabor(actor, progress);
    } else {
      storage->GiveBirth(id);
      MorphManager::GetSingleton()->ClearMorphs(id);
      TickManager::GetSingleton()->GiveSoulGem(actor, progress);
    }
    return true;
  }

  bool Papyrus_SetPhase(RE::StaticFunctionTag*, RE::Actor* actor, std::int32_t phaseInt)
  {
    if (!actor || phaseInt < 0 || phaseInt > 3)
      return false;
    auto* storage = Storage::GetSingleton();
    auto id       = actor->GetFormID();
    auto r        = storage->GetRecipient(id);
    if (!r) {
      r = storage->RegisterRecipient(actor);
      if (!r)
        return false;
    }

    float now     = TickManager::Now();
    auto newPhase = static_cast<CyclePhase>(phaseInt);
    float dur     = TickManager::RollPhaseDuration(newPhase);
    bool applied  = false;
    storage->WithRecipientWrite(id, [&](RecipientData& rd) {
      if (rd.IsPregnant())
        return;
      rd.phase             = newPhase;
      rd.phaseStartTime    = now;
      rd.phaseDurationDays = dur;
      applied              = true;
    });
    return applied;
  }

  bool Papyrus_AdvancePregnancy(RE::StaticFunctionTag*, RE::Actor* actor, float days)
  {
    if (!actor || days <= 0.0f)
      return false;
    auto* storage = Storage::GetSingleton();
    auto id       = actor->GetFormID();
    return storage->WithRecipientWrite(id, [&](RecipientData& rd) {
      if (rd.pregnancy)
        rd.pregnancy->conceptionTime -= days;
    });
  }

  bool Papyrus_ClearAll(RE::StaticFunctionTag*, RE::Actor* actor)
  {
    if (!actor)
      return false;
    auto* storage = Storage::GetSingleton();
    auto id       = actor->GetFormID();
    if (!storage->GetRecipient(id))
      return false;

    float now = TickManager::Now();
    float dur = TickManager::RollPhaseDuration(CyclePhase::Follicular);
    storage->WithRecipientWrite(id, [&](RecipientData& rd) {
      rd.sperm.clear();
      rd.history.clear();
      rd.pregnancy.reset();
      rd.phase             = CyclePhase::Follicular;
      rd.phaseStartTime    = now;
      rd.phaseDurationDays = dur;
      rd.inflationVolume   = 0.0f;
      rd.isDead            = false;
    });
    MorphManager::GetSingleton()->ClearMorphs(id);
    if (actor->IsPlayerRef())
      storage->ResetPlayerState();
    if (eventMessages)
      RE::SendHUDMessage::ShowHUDMessage(std::format("{} fertility data cleared.", actor->GetName()).c_str());
    return true;
  }

  bool Papyrus_AbortionDrug(RE::StaticFunctionTag* tag, RE::Actor* actor)
  {
    return Papyrus_Abort(tag, actor);
  }

  void Papyrus_AllowActor(RE::StaticFunctionTag*, RE::Actor* a)
  {
    if (a)
      Storage::GetSingleton()->AllowActor(a->GetFormID());
  }

  void Papyrus_ExcludeActor(RE::StaticFunctionTag*, RE::Actor* a)
  {
    if (a)
      Storage::GetSingleton()->ExcludeActor(a->GetFormID());
  }

  void Papyrus_ReloadConfig(RE::StaticFunctionTag*)
  {
    ConfigManager::GetSingleton()->Reload();
  }

}  // namespace

bool SceneProcessor::RegisterNativeFunctions(RE::BSScript::IVirtualMachine* vm)
{
  constexpr auto script = "FertilityNative";

  vm->RegisterFunction("ProcessSceneBatch", script, Papyrus_ProcessSceneBatch);
  vm->RegisterFunction("ProcessPair", script, Papyrus_ProcessPair);
  vm->RegisterFunction("GetLastConceptionCount", script, Papyrus_GetLastConceptionCount);

  vm->RegisterFunction("IsPregnant", script, Papyrus_IsPregnant);
  vm->RegisterFunction("GetPregnancyProgress", script, Papyrus_GetPregnancyProgress);
  vm->RegisterFunction("GetInflation", script, Papyrus_GetInflation);
  vm->RegisterFunction("GetSpermVolume", script, Papyrus_GetSpermVolume);
  vm->RegisterFunction("IsEligible", script, Papyrus_IsEligible);
  vm->RegisterFunction("GetStatus", script, Papyrus_GetStatus);

  vm->RegisterFunction("ForcePregnancy", script, Papyrus_ForcePregnancy);
  vm->RegisterFunction("ForceOvulation", script, Papyrus_ForceOvulation);
  vm->RegisterFunction("Abort", script, Papyrus_Abort);
  vm->RegisterFunction("ForceLabor", script, Papyrus_ForceLabor);
  vm->RegisterFunction("SetPhase", script, Papyrus_SetPhase);
  vm->RegisterFunction("AdvancePregnancy", script, Papyrus_AdvancePregnancy);
  vm->RegisterFunction("ClearAll", script, Papyrus_ClearAll);
  vm->RegisterFunction("AbortionDrug", script, Papyrus_AbortionDrug);

  vm->RegisterFunction("AllowActor", script, Papyrus_AllowActor);
  vm->RegisterFunction("ExcludeActor", script, Papyrus_ExcludeActor);
  vm->RegisterFunction("ReloadConfig", script, Papyrus_ReloadConfig);

  logger::info("[Fertility] Registered {} Papyrus native functions", 20);
  return true;
}

}  // namespace Fertility
