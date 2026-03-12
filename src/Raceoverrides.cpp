// =====================================================================
//  RaceOverrides.cpp — INI-driven race multipliers & keyword resolution
//
//  Keyword resolution:
//    INI string "zad_DeviousBelt,zad_DeviousPlugVaginal"
//      → split by ','  → trim  → LookupByEditorID<BGSKeyword>
//      → nullptr means mod not loaded, silently skip
//
//  Race multiplier workflow:
//    First launch → enumerate all TESRace in DataHandler
//      → classify via ActorTypeCreature / ActorTypeAnimal keywords
//      → dump sorted list to INI with default 1.0, 1.0
//    Subsequent launches → parse existing INI
//      → resolve EditorID → FormID at init time
//      → runtime lookups are pure integer hash, zero string ops
// =====================================================================

#include "RaceOverrides.h"
#include "ConfigManager.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <sstream>
#include <string>

namespace Fertility
{

static constexpr std::string_view kIniPath = "Data/SKSE/Plugins/CycleVolume_Advanced.ini";

// =====================================================================
//  helpers
// =====================================================================

static std::string Trim(std::string_view sv)
{
  auto start = sv.find_first_not_of(" \t\r\n");
  if (start == std::string_view::npos)
    return {};
  auto end = sv.find_last_not_of(" \t\r\n");
  return std::string(sv.substr(start, end - start + 1));
}

static std::vector<std::string> SplitCSV(const std::string& input)
{
  std::vector<std::string> result;
  std::istringstream ss(input);
  std::string token;
  while (std::getline(ss, token, ',')) {
    auto t = Trim(token);
    if (!t.empty())
      result.push_back(std::move(t));
  }
  return result;
}

/// parse "1.5" or "1.5, 2.0" → {spermMult, volumeMult}
/// single value means both use the same multiplier
static std::pair<float, float> ParseMultPair(const std::string& value)
{
  auto parts = SplitCSV(value);
  float sm   = 1.0f;
  float vm   = 1.0f;

  if (!parts.empty()) {
    std::from_chars(parts[0].data(), parts[0].data() + parts[0].size(), sm);
  }
  if (parts.size() >= 2) {
    std::from_chars(parts[1].data(), parts[1].data() + parts[1].size(), vm);
  } else {
    vm = sm;  // single value → same for both
  }

  return {sm, vm};
}

// =====================================================================
//  race classification — ActorTypeCreature / ActorTypeAnimal
// =====================================================================

static bool IsCreatureRace(RE::TESRace* race)
{
  // cache these; they won't change after DataLoaded
  static auto* kwCreature = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("ActorTypeCreature");
  static auto* kwAnimal   = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("ActorTypeAnimal");

  if (kwCreature && race->HasKeyword(kwCreature))
    return true;
  if (kwAnimal && race->HasKeyword(kwAnimal))
    return true;
  return false;
}

/// filter out child races, internal template races, etc.
static bool IsUsableRace(RE::TESRace* race)
{
  if (!race)
    return false;
  const char* eid = race->GetFormEditorID();
  if (!eid || eid[0] == '\0')
    return false;

  // skip stuff like DefaultRace, __intRace, test races
  std::string_view sv{eid};
  if (sv.starts_with("__") || sv.starts_with("Default") || sv.find("Child") != std::string_view::npos)
    return false;

  return true;
}

// =====================================================================
//  keyword EditorID → BGSKeyword* runtime resolution
// =====================================================================

std::vector<RE::BGSKeyword*> RaceOverrides::ResolveKeywords(const std::string& commaSeparated, const char* debugLabel)
{
  std::vector<RE::BGSKeyword*> resolved;

  auto editorIDs = SplitCSV(commaSeparated);
  if (editorIDs.empty())
    return resolved;

  resolved.reserve(editorIDs.size());

  for (auto& eid : editorIDs) {
    auto* form = RE::TESForm::LookupByEditorID(eid);
    if (!form) {
      logger::info("[Fertility] {}: '{}' → not found (mod not installed?)", debugLabel, eid);
      continue;
    }

    auto* kw = form->As<RE::BGSKeyword>();
    if (!kw) {
      logger::warn("[Fertility] {}: '{}' → found {:08X} but not a Keyword", debugLabel, eid, form->GetFormID());
      continue;
    }

    resolved.push_back(kw);
    logger::info("[Fertility] {}: '{}' → {:08X} resolved", debugLabel, eid, kw->GetFormID());
  }

  return resolved;
}

// =====================================================================
//  init
// =====================================================================

void RaceOverrides::Initialize()
{
  bool freshGenerate = !std::filesystem::exists(kIniPath);

  if (freshGenerate)
    GenerateRaceIni();

  LoadFromDisk();

  // ── resolve EditorID → FormID for runtime lookups ──
  ResolveOverrides();

  // ── keywords ──
  _blockingKeywords = ResolveKeywords(_rawBlocking, "BlockingKeywords");
  _noStripKeywords  = ResolveKeywords(_rawNoStrip, "NoStripKeywords");

  if (_stripSlots.empty()) {
    _stripSlots.insert(32);  // Body
    _stripSlots.insert(52);  // Pelvis (mod slot)
  }

  if (_noStripKeywords.empty() && _rawNoStrip.empty()) {
    _noStripKeywords = ResolveKeywords("SexLabNoStrip", "NoStripKeywords(default)");
  }

  logger::info("[Fertility] RaceOverrides ready: {} resolved races, {} strip slots, "
               "{} blocking kw, {} nostrip kw",
               _resolvedOverrides.size(), _stripSlots.size(), _blockingKeywords.size(), _noStripKeywords.size());
}

// =====================================================================
//  resolve: EditorID map → FormID map
//  one-time cost at startup, after that all queries are integer keyed
// =====================================================================

void RaceOverrides::ResolveOverrides()
{
  if (_iniOverrides.empty())
    return;

  auto& allRaces = RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESRace>();

  // build name→Race* lookup (avoid O(N*M) for each INI entry)
  std::unordered_map<std::string_view, RE::TESRace*> byEID;
  for (auto* race : allRaces) {
    if (!race)
      continue;
    const char* eid = race->GetFormEditorID();
    if (eid && eid[0] != '\0')
      byEID[eid] = race;
  }

  for (auto& [eid, mult] : _iniOverrides) {
    auto it = byEID.find(eid);
    if (it == byEID.end()) {
      logger::warn("[Fertility] RaceOverride: '{}' not found in loaded races — typo or missing mod?", eid);
      continue;
    }

    _resolvedOverrides[it->second->GetFormID()] = mult;

    if (verboseMode)
      logger::info("[Fertility] RaceOverride: {} ({:08X}) → sperm x{:.2f}, vol x{:.2f}", eid, it->second->GetFormID(), mult.spermMultiplier,
                   mult.volumeMultiplier);
  }

  // done with string map
  _iniOverrides.clear();
}

// =====================================================================
//  queries — race overrides (integer keyed, hot path safe)
// =====================================================================

std::optional<RaceSpermOverride> RaceOverrides::GetOverride(RE::FormID raceFormID) const
{
  auto it = _resolvedOverrides.find(raceFormID);
  return it != _resolvedOverrides.end() ? std::optional{it->second} : std::nullopt;
}

std::optional<RaceSpermOverride> RaceOverrides::GetOverride(RE::TESRace* race) const
{
  return race ? GetOverride(race->GetFormID()) : std::nullopt;
}

float RaceOverrides::GetEffectiveSperm(RE::TESRace* race, float globalBase) const
{
  auto ovr = GetOverride(race);
  return globalBase * (ovr ? ovr->spermMultiplier : 1.0f);
}

float RaceOverrides::GetEffectiveVolume(RE::TESRace* race, float globalBase) const
{
  auto ovr = GetOverride(race);
  return globalBase * (ovr ? ovr->volumeMultiplier : 1.0f);
}

// =====================================================================
//  queries — slot stripping
// =====================================================================

bool RaceOverrides::ShouldStrip(RE::TESObjectARMO* armor) const
{
  if (!armor || _stripSlots.empty())
    return false;

  auto slotMask = armor->GetSlotMask();
  for (auto slot : _stripSlots) {
    if (slot < 30 || slot > 61)
      continue;
    auto flag = static_cast<RE::BIPED_MODEL::BipedObjectSlot>(1u << (slot - 30));
    if (slotMask.any(flag))
      return true;
  }
  return false;
}

// =====================================================================
//  queries — DD blocking
// =====================================================================

bool RaceOverrides::IsExpulsionBlocked(RE::Actor* actor) const
{
  if (!actor || _blockingKeywords.empty())
    return false;

  auto* changes = actor->GetInventoryChanges();
  if (!changes || !changes->entryList)
    return false;

  for (auto* entry : *changes->entryList) {
    if (!entry || !entry->object)
      continue;

    auto* armor = entry->object->As<RE::TESObjectARMO>();
    if (!armor)
      continue;

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

    for (auto* kw : _blockingKeywords) {
      if (armor->HasKeyword(kw)) {
        if (verboseMode)
          logger::info("[Fertility] Expulsion blocked by '{}' on {:08X} ({})", kw->GetFormEditorID(), armor->GetFormID(), armor->GetName());
        return true;
      }
    }
  }

  return false;
}

// =====================================================================
//  queries — NoStrip exemption
// =====================================================================

bool RaceOverrides::HasNoStripKeyword(RE::TESObjectARMO* armor) const
{
  if (!armor || _noStripKeywords.empty())
    return false;

  return std::ranges::any_of(_noStripKeywords, [armor](RE::BGSKeyword* kw) {
    return armor->HasKeyword(kw);
  });
}

// =====================================================================
//  INI parsing
// =====================================================================

void RaceOverrides::LoadFromDisk()
{
  std::ifstream ifs(kIniPath.data());
  if (!ifs.is_open()) {
    logger::warn("[Fertility] RaceOverrides: Failed to open {}", kIniPath);
    return;
  }

  enum class Section
  {
    None,
    Expulsion,
    HumanoidRaces,
    CreatureRaces
  };
  Section cur = Section::None;

  std::string line;
  while (std::getline(ifs, line)) {
    auto trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#')
      continue;

    if (trimmed.front() == '[' && trimmed.back() == ']') {
      auto name = Trim(trimmed.substr(1, trimmed.size() - 2));
      if (name == "Expulsion")
        cur = Section::Expulsion;
      else if (name == "HumanoidRaces")
        cur = Section::HumanoidRaces;
      else if (name == "CreatureRaces")
        cur = Section::CreatureRaces;
      else
        cur = Section::None;
      continue;
    }

    auto eqPos = trimmed.find('=');
    if (eqPos == std::string::npos)
      continue;

    auto key   = Trim(trimmed.substr(0, eqPos));
    auto value = Trim(trimmed.substr(eqPos + 1));
    if (key.empty() || value.empty())
      continue;

    switch (cur) {
    case Section::Expulsion: {
      if (key == "StripSlots") {
        _stripSlots.clear();
        for (auto& token : SplitCSV(value)) {
          int slot       = 0;
          auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), slot);
          if (ec == std::errc{} && slot >= 30 && slot <= 61)
            _stripSlots.insert(static_cast<std::uint32_t>(slot));
        }
      } else if (key == "BlockingKeywords") {
        _rawBlocking = value;
      } else if (key == "NoStripKeywords") {
        _rawNoStrip = value;
      }
      break;
    }

    case Section::HumanoidRaces:
    case Section::CreatureRaces: {
      auto [sm, vm]      = ParseMultPair(value);
      _iniOverrides[key] = RaceSpermOverride{sm, vm};
      break;
    }

    default:
      break;
    }
  }

  logger::info("[Fertility] RaceOverrides: Loaded {} race entries from {}", _iniOverrides.size(), kIniPath);
}

// =====================================================================
//  INI generation — enumerate all loaded races on first run
// =====================================================================

void RaceOverrides::GenerateRaceIni()
{
  std::filesystem::create_directories(std::filesystem::path(kIniPath).parent_path());

  auto& allRaces = RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESRace>();

  // partition into humanoid vs creature, sorted by EditorID for readability
  auto usable = allRaces | std::views::filter(IsUsableRace) | std::ranges::to<std::vector<RE::TESRace*>>();

  auto [creaturesEnd, humanoidsEnd] = std::ranges::partition(usable, IsCreatureRace);

  // usable is now [creatures... | humanoids...] thanks to partition
  // but we actually want two sorted sub-ranges
  auto creatures = std::ranges::subrange(usable.begin(), creaturesEnd);
  auto humanoids = std::ranges::subrange(creaturesEnd, usable.end());

  auto byEditorID = [](RE::TESRace* a, RE::TESRace* b) {
    return _stricmp(a->GetFormEditorID(), b->GetFormEditorID()) < 0;
  };
  std::ranges::sort(creatures, byEditorID);
  std::ranges::sort(humanoids, byEditorID);

  std::ofstream ofs(kIniPath.data());
  if (!ofs.is_open()) {
    logger::warn("[Fertility] RaceOverrides: Failed to create {}", kIniPath);
    return;
  }

  ofs << "; =====================================================================\n"
      << ";  CycleAndVolume — Advanced Configuration\n"
      << ";\n"
      << ";  Auto-generated on first run. Delete this file to regenerate.\n"
      << ";  Lines starting with ; or # are comments.\n"
      << ";\n"
      << ";  Race values are MULTIPLIERS on the global base, not absolute amounts.\n"
      << ";    1.0 = default   2.0 = double   0.5 = half   0.0 = sterile\n"
      << ";  Format: RaceEditorID = SpermMult, VolumeMult\n"
      << ";    (single value means both use the same multiplier)\n"
      << ";\n"
      << ";  Keywords are resolved by EditorID at runtime.\n"
      << ";  If the source mod is not installed, the keyword is silently skipped.\n"
      << "; =====================================================================\n\n";

  // ── Expulsion section (same as before) ──

  ofs << "[Expulsion]\n\n"
      << "; Armor slots to strip during expulsion (BipedObject 30~61)\n"
      << "StripSlots = 32,52\n\n"
      << "; DD keywords that block expulsion entirely\n"
      << "BlockingKeywords = zad_DeviousBelt,zad_DeviousPlugVaginal,zad_DeviousPlugAnal\n\n"
      << "; Keywords marking items as non-strippable (prevents DD render desync)\n"
      << "NoStripKeywords = SexLabNoStrip\n\n";

  // ── Humanoid races ──

  ofs << "[HumanoidRaces]\n"
      << "; Race = SpermMult, VolumeMult\n";
  for (auto* race : humanoids)
    ofs << race->GetFormEditorID() << " = 1.0, 1.0\n";

  // ── Creature races ──

  ofs << "\n[CreatureRaces]\n"
      << "; Race = SpermMult, VolumeMult\n";
  for (auto* race : creatures)
    ofs << race->GetFormEditorID() << " = 1.0, 1.0\n";

  ofs.flush();
  logger::info("[Fertility] RaceOverrides: Generated template with {} humanoid + {} creature races", std::ranges::distance(humanoids),
               std::ranges::distance(creatures));
}

}  // namespace Fertility
