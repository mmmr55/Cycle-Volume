#pragma once

#include "FertilityStorage.h"
#include "Fertilitytypes.h"
#include <RaceOverrides.h>

#include <mutex>

namespace Fertility
{

class SceneProcessor
{
public:
  static SceneProcessor* GetSingleton()
  {
    static SceneProcessor inst;
    return &inst;
  }

  static bool CanDonate(int sex) { return sex == 0 || sex == 2 || sex == 3; }
  static bool CanReceive(int sex) { return sex == 1 || sex == 2 || sex == 4; }
  static bool IsCreature(int sex) { return sex == 3 || sex == 4; }
  static bool IsFemale(int sex) { return sex == 1 || sex == 4; }
  static bool IsFuta(int sex) { return sex == 2; }

  static constexpr int CTYPE_Vaginal  = 1;
  static constexpr int CTYPE_Anal     = 2;
  static constexpr int CTYPE_Oral     = 3;
  static constexpr int CTYPE_Grinding = 4;

  static InseminationType MapInteraction(int ctype, bool isFF);

  int ProcessSceneBatch(RE::Actor** actors, const int* sexes, int actorCount, const int* pairs, int pairCount);
  int ProcessPair(RE::Actor* recipient, RE::Actor* donor, int recipientSex, int donorSex, InseminationType type);

  int ConsumeLastConceptionCount();

  static bool RegisterNativeFunctions(RE::BSScript::IVirtualMachine* vm);

private:
  SceneProcessor() = default;
  mutable std::mutex _batchMtx;
  int _lastConceptions = 0;
};

}  // namespace Fertility
