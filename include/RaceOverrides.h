#pragma once
#include <Fertilitytypes.h>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace Fertility
{

struct RaceSpermOverride
{
  float spermMultiplier  = 1.0f;
  float volumeMultiplier = 1.0f;

  // convenience: apply to a global base
  float baseSperm  = 0.0f;  // 0 = use global
  float baseVolume = 0.0f;  // 0 = use global
};

class RaceOverrides
{
public:
  static RaceOverrides* GetSingleton()
  {
    static RaceOverrides instance;
    return &instance;
  }

  RaceOverrides(const RaceOverrides&)            = delete;
  RaceOverrides& operator=(const RaceOverrides&) = delete;

  void Initialize();

  // runtime queries — all integer-keyed after init
  std::optional<RaceSpermOverride> GetOverride(RE::FormID raceFormID) const;
  std::optional<RaceSpermOverride> GetOverride(RE::TESRace* race) const;

  // convenience: globalBase * multiplier (returns globalBase if no override)
  float GetEffectiveSperm(RE::TESRace* race, float globalBase) const;
  float GetEffectiveVolume(RE::TESRace* race, float globalBase) const;

  // slot / keyword queries
  bool ShouldStrip(RE::TESObjectARMO* armor) const;
  bool IsExpulsionBlocked(RE::Actor* actor) const;
  bool HasNoStripKeyword(RE::TESObjectARMO* armor) const;

  // keyword resolution (public for reuse)
  static std::vector<RE::BGSKeyword*> ResolveKeywords(const std::string& commaSeparated, const char* debugLabel);

private:
  RaceOverrides()  = default;
  ~RaceOverrides() = default;

  void LoadFromDisk();
  void GenerateRaceIni();
  void ResolveOverrides();

  // INI stage: EditorID -> override (cleared after resolve)
  std::unordered_map<std::string, RaceSpermOverride> _iniOverrides;

  // runtime stage: FormID -> override
  std::unordered_map<RE::FormID, RaceSpermOverride> _resolvedOverrides;

  // expulsion config
  std::set<std::uint32_t> _stripSlots;
  std::string _rawBlocking;
  std::string _rawNoStrip;
  std::vector<RE::BGSKeyword*> _blockingKeywords;
  std::vector<RE::BGSKeyword*> _noStripKeywords;
};

}  // namespace Fertility
