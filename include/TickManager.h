#pragma once

#include "FertilityStorage.h"
#include "Fertilitytypes.h"

#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <RaceOverrides.h>

namespace Fertility
{

namespace FRank
{
  constexpr int kNone         = 0;
  constexpr int kPregMin      = 1;
  constexpr int kPregMax      = 100;
  constexpr int kLabor        = 101;
  constexpr int kRecoveryBase = 102;
  constexpr int kRecoveryMax  = 115;
}  // namespace FRank

enum class LaborPhase : int
{
  None         = 0,
  Contractions = 1,
  Delivery     = 2,
  Done         = 3,
};

namespace GVar
{
  constexpr const char* kPregnant   = "FertilityPregnant";
  constexpr const char* kProgress   = "FertilityProgress";
  constexpr const char* kTrimester  = "FertilityTrimester";
  constexpr const char* kInLabor    = "FertilityInLabor";
  constexpr const char* kLaborPhase = "FertilityLaborPhase";
  constexpr const char* kRecovery   = "FertilityRecovery";
  constexpr const char* kCyclePhase = "FertilityCyclePhase";
}  // namespace GVar

namespace VanillaSoulGem
{
  constexpr RE::FormID kPetty   = 0x0002E4E2;
  constexpr RE::FormID kLesser  = 0x0002E4E4;
  constexpr RE::FormID kCommon  = 0x0002E4E6;
  constexpr RE::FormID kGreater = 0x0002E4FC;
  constexpr RE::FormID kGrand   = 0x0002E4FF;
}  // namespace VanillaSoulGem

namespace PFlag
{
  constexpr int kConceived       = 0x001;
  constexpr int kLaborTriggered  = 0x002;
  constexpr int kMiscarriage     = 0x004;
  constexpr int kAllSpermExpired = 0x010;
  constexpr int kNearLabor       = 0x400;
  constexpr int kLaborCompleted  = 0x800;
}  // namespace PFlag

struct LaborState
{
  using Clock      = std::chrono::steady_clock;
  LaborPhase phase = LaborPhase::None;
  Clock::time_point phaseStart;
  float pregProgress = 1.0f;
  bool controlled    = false;
};

// main-thread frame-batched tick manager
// call Tick() from a PlayerCharacter::Update hook
class TickManager
{
public:
  static TickManager* GetSingleton()
  {
    static TickManager instance;
    return &instance;
  }
  TickManager(const TickManager&)            = delete;
  TickManager& operator=(const TickManager&) = delete;

  void Initialize();
  void Tick();

  static float Now();

  // Papyrus-callable (VM thread, hence locks)
  bool IsInLabor(RE::FormID id) const;
  int ConsumePlayerFlags();
  int PeekPlayerFlags() const;

  // main-thread only
  void BeginLabor(RE::Actor* actor, float pregProgress);
  void GiveSoulGem(RE::Actor* actor, float progress);
  std::size_t GetTrackedCount() const;

  static float RollPhaseDuration(CyclePhase phase);

  int batchSize           = 10;
  float tickIntervalHours = 2.0f;
  float laborPhaseSec     = 15.0f;
  float baseFertility     = 0.30f;
  float amountHalfLife    = 50.0f;

private:
  TickManager() = default;

  void TickRecipient(RE::FormID id, float now);
  void TickNonPregnant(RecipientData& r, RE::FormID id, bool isPlayer, float now);
  void TickPregnant(RecipientData& r, RE::FormID id, bool isPlayer, float now);

  void AdvanceCyclePhase(RecipientData& r);
  bool TryConception(RecipientData& r, float now);

  int CalcFactionRank(const RecipientData& r, float now) const;
  void ApplyFactionRank(RE::Actor* actor, int rank);

  void UpdateLaborTimers();
  void AdvanceLaborPhase(RE::FormID id, LaborState& ls);
  void CompleteBirth(RE::FormID id);

  void SyncGraphVariables(RE::Actor* actor, const RecipientData& r, float now);

  static bool IsActorBusy(RE::Actor* actor);
  static void DisablePlayerInput();
  static void EnablePlayerInput();
  static void FreezeActor(RE::Actor* actor);
  static void UnfreezeActor(RE::Actor* actor);

  void AddPlayerFlag(int flag);

  // Papyrus reads these from VM thread
  mutable std::mutex _flagsMtx;
  int _playerFlags = 0;

  mutable std::mutex _laborMtx;
  std::unordered_map<RE::FormID, LaborState> _laboring;

  // main-thread only, no lock
  std::vector<RE::FormID> _queue;
  std::size_t _cursor = 0;
  void RebuildQueue();

  float _lastTickTime      = 0.0f;
  RE::TESFaction* _faction = nullptr;
};

}  // namespace Fertility
