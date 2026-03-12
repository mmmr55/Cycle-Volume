#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>


namespace Fertility
{

struct DonorKey
{
  enum class Kind : std::uint8_t
  {
    kFormID   = 0,
    kEditorID = 1
  };

  std::variant<RE::FormID, std::string> id;

  static DonorKey FromFormID(RE::FormID fid) { return {fid}; }
  static DonorKey FromEditorID(const std::string& eid) { return {eid}; }

  static DonorKey From(RE::Actor* actor)
  {
    if (!actor)
      return {RE::FormID(0)};
    if (actor->IsPlayerRef())
      return {actor->GetFormID()};
    auto* base = actor->GetActorBase();
    if (!base)
      return {actor->GetFormID()};
    if (base->IsUnique())
      return {actor->GetFormID()};
    if (const char* eid = base->GetFormEditorID(); eid && eid[0] != '\0')
      return {std::string(eid)};
    return {base->GetFormID()};
  }

  Kind GetKind() const { return std::holds_alternative<RE::FormID>(id) ? Kind::kFormID : Kind::kEditorID; }
  bool IsFormID() const { return GetKind() == Kind::kFormID; }
  bool IsEditorID() const { return GetKind() == Kind::kEditorID; }
  RE::FormID AsFormID() const { return std::get<RE::FormID>(id); }
  const std::string& AsEditorID() const { return std::get<std::string>(id); }

  bool operator==(const DonorKey& o) const = default;

  struct Hash
  {
    std::size_t operator()(const DonorKey& k) const noexcept
    {
      if (k.IsFormID())
        return std::hash<RE::FormID>{}(k.AsFormID());
      return std::hash<std::string>{}(k.AsEditorID());
    }
  };
};

enum class InseminationType : std::uint8_t
{
  Natural    = 0,
  Oral       = 1,
  Anal       = 2,
  Magical    = 3,
  Artificial = 4,
  Fill       = 5,
};

struct DonorProfile
{
  DonorKey key;
  RE::FormID raceID          = 0;
  float baseCount            = 100.0f;
  float survivalHours        = 72.0f;
  float fertilityMult        = 1.0f;
  bool cooldownLocked        = false;
  float lastInseminationTime = 0.0f;
  bool isCreature            = false;
};

struct SpermDeposit
{
  DonorKey donor;
  std::string donorName;
  RE::FormID donorRaceID = 0;
  InseminationType type  = InseminationType::Natural;
  float depositTime      = 0.0f;
  float amount           = 0.0f;
  float volumeML         = 0.0f;
  float survivalHours    = 72.0f;
  bool isCreature        = false;

  float EffectiveAmount(float now) const
  {
    float elapsedH = (now - depositTime) * 24.0f;
    if (elapsedH <= 0.0f)
      return amount;
    if (elapsedH >= survivalHours)
      return 0.0f;
    float t = 1.0f - (elapsedH / survivalHours);
    return amount * t * t;
  }

  bool IsViable(float now) const { return EffectiveAmount(now) > 0.01f; }
};

// per-recipient state machine driven by TickManager
enum class CyclePhase : std::uint8_t
{
  Menstruation = 0,
  Follicular   = 1,
  Ovulation    = 2,
  Luteal       = 3,
};

inline CyclePhase NextPhase(CyclePhase p)
{
  switch (p) {
  case CyclePhase::Menstruation:
    return CyclePhase::Follicular;
  case CyclePhase::Follicular:
    return CyclePhase::Ovulation;
  case CyclePhase::Ovulation:
    return CyclePhase::Luteal;
  case CyclePhase::Luteal:
    return CyclePhase::Menstruation;
  default:
    return CyclePhase::Follicular;
  }
}

enum class RaceInheritance : int
{
  Mother = 0,
  Father = 1,
  Random = 2,
};

enum class TrainingStatus : int
{
  None     = 0,
  Training = 1,
  Complete = 2,
};

struct PregnancyState
{
  float conceptionTime  = 0.0f;
  float birthTime       = 0.0f;
  float recoveryEndTime = 0.0f;
  float babyEquipTime   = 0.0f;
  DonorKey father;
  std::string fatherName;
  RE::FormID fatherRaceID = 0;
  bool creatureFather     = false;

  bool IsActive() const { return conceptionTime > 0.0f && birthTime <= 0.0f; }
  float DaysSinceConception(float now) const { return now - conceptionTime; }
};

struct InseminationRecord
{
  DonorKey donor;
  std::string donorName;
  InseminationType type = InseminationType::Natural;
  float time            = 0.0f;
  float amount          = 0.0f;
  float volumeML        = 0.0f;
  bool isCreature       = false;
};

struct RecipientData
{
  RE::FormID formID = 0;

  CyclePhase phase        = CyclePhase::Follicular;
  float phaseStartTime    = 0.0f;
  float phaseDurationDays = 3.0f;

  std::vector<SpermDeposit> sperm;
  std::optional<PregnancyState> pregnancy;
  std::vector<InseminationRecord> history;

  float inflationVolume     = 0.0f;
  float volumeLastUpdate    = 0.0f;
  float volumeDecayHalfLife = 8.0f;

  float lastUpdateTime = 0.0f;
  std::string lastLocation;
  float locationLeftTime = 0.0f;
  bool isDead            = false;
  int factionRank        = 0;

  void PurgeExpiredSperm(float now)
  {
    std::erase_if(sperm, [now](const SpermDeposit& s) {
      return !s.IsViable(now);
    });
  }

  float TotalViableSperm(float now) const
  {
    float t = 0.0f;
    for (auto& s : sperm)
      t += s.EffectiveAmount(now);
    return t;
  }

  float ViableSpermFrom(const DonorKey& dk, float now) const
  {
    float t = 0.0f;
    for (auto& s : sperm)
      if (s.donor == dk)
        t += s.EffectiveAmount(now);
    return t;
  }

  void UpdateInflation(float now)
  {
    if (inflationVolume <= 0.01f) {
      inflationVolume  = 0.0f;
      volumeLastUpdate = now;
      return;
    }
    float elapsedH = (now - volumeLastUpdate) * 24.0f;
    if (elapsedH <= 0.0f)
      return;
    inflationVolume *= std::pow(0.5f, elapsedH / volumeDecayHalfLife);
    if (inflationVolume < 0.01f)
      inflationVolume = 0.0f;
    volumeLastUpdate = now;
  }

  float AddVolume(float amount, float now)
  {
    UpdateInflation(now);
    inflationVolume += amount;
    volumeLastUpdate = now;
    return inflationVolume;
  }

  float GetCurrentVolume(float now) const
  {
    if (inflationVolume <= 0.01f)
      return 0.0f;
    float elapsedH = (now - volumeLastUpdate) * 24.0f;
    if (elapsedH <= 0.0f)
      return inflationVolume;
    return inflationVolume * std::pow(0.5f, elapsedH / volumeDecayHalfLife);
  }

  float GetTotalInflation(float now, float gestDays, float spermFullVolume = 1000.0f) const
  {
    float spermBelly = std::clamp(GetCurrentVolume(now) / spermFullVolume, 0.0f, 3.0f);
    float pregBelly  = 0.0f;
    if (IsPregnant() && gestDays > 0.0f)
      pregBelly = std::clamp(pregnancy->DaysSinceConception(now) / gestDays, 0.0f, 1.0f);
    return spermBelly + pregBelly;
  }

  bool IsPregnant() const { return pregnancy.has_value() && pregnancy->IsActive(); }

  bool InRecovery(float now, float recoveryDays) const
  {
    return pregnancy.has_value() && pregnancy->birthTime > 0.0f && (now - pregnancy->birthTime) <= recoveryDays;
  }

  bool HasActiveData(float now, float recoveryDays, float babyDuration) const
  {
    if (TotalViableSperm(now) > 0.0f)
      return true;
    if (IsPregnant())
      return true;
    if (pregnancy.has_value()) {
      auto& p = *pregnancy;
      if (p.birthTime > 0.0f && (now - p.birthTime) <= recoveryDays)
        return true;
      if (p.babyEquipTime > 0.0f && (now - p.babyEquipTime) <= babyDuration)
        return true;
    }
    return false;
  }

  bool PhaseExpired(float now) const { return (now - phaseStartTime) >= phaseDurationDays; }

  void ClearFertilityData()
  {
    sperm.clear();
    phase = CyclePhase::Follicular;
  }
};

struct ChildRecord
{
  RE::FormID actorID  = 0;
  RE::FormID motherID = 0;
  DonorKey father;
  std::string name;
  float birthTime   = 0.0f;
  RE::FormID raceID = 0;
};

struct FollowerSlot
{
  RE::FormID actorID = 0;
  int childIndex     = -1;
};

struct PlayerState
{
  float lastSleepTime      = 0.0f;
  float staminaDelta       = 0.0f;
  float magickaDelta       = 0.0f;
  float babyHealth         = 100.0f;
  float lastBabyDamage     = 0.0f;
  float lastBabyDamageTime = 0.0f;
};

}  // namespace Fertility
