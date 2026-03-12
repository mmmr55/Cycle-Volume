#include "FertilityStorage.h"
#include "RaceOverrides.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <numeric>
#include <random>
#include <ranges>
#include <unordered_set>

namespace Fertility
{

float Storage::Now() const
{
  if (auto* cal = RE::Calendar::GetSingleton())
    return cal->GetCurrentGameTime();
  return 0.0f;
}

void Storage::WriteStr(SKSE::SerializationInterface* i, const std::string& s)
{
  auto len = static_cast<std::uint32_t>(s.size());
  i->WriteRecordData(len);
  if (len)
    i->WriteRecordData(s.data(), len);
}

std::string Storage::ReadStr(SKSE::SerializationInterface* i)
{
  std::uint32_t len = 0;
  i->ReadRecordData(len);
  if (!len)
    return {};
  constexpr std::uint32_t kMaxLen = 1u << 20;
  if (len > kMaxLen) {
    logger::error("[Fertility] ReadStr: len {} exceeds limit, co-save likely corrupt", len);
    return {};
  }
  std::string s(len, '\0');
  i->ReadRecordData(s.data(), len);
  return s;
}

void Storage::WriteDonorKey(SKSE::SerializationInterface* i, const DonorKey& k)
{
  auto kind = static_cast<std::uint8_t>(k.GetKind());
  i->WriteRecordData(kind);
  if (k.IsFormID())
    i->WriteRecordData(k.AsFormID());
  else
    WriteStr(i, k.AsEditorID());
}

DonorKey Storage::ReadDonorKey(SKSE::SerializationInterface* i, bool resolve)
{
  std::uint8_t kind = 0;
  i->ReadRecordData(kind);
  if (kind == static_cast<std::uint8_t>(DonorKey::Kind::kFormID)) {
    RE::FormID raw = 0;
    i->ReadRecordData(raw);
    if (resolve) {
      RE::FormID resolved = 0;
      if (i->ResolveFormID(raw, resolved))
        return DonorKey::FromFormID(resolved);
    }
    return DonorKey::FromFormID(raw);
  }
  return DonorKey::FromEditorID(ReadStr(i));
}

void Storage::Initialize()
{
  logger::info("[Fertility] Storage initialized (v{})", kVersion);
}

// ── Race eligibility ──

void Storage::ResolveVanillaKeywords() const
{
  if (_kwResolved.load(std::memory_order_acquire))
    return;
  std::lock_guard lk(_kwInitMtx);
  if (_kwResolved.load(std::memory_order_relaxed))
    return;

  if (auto* form = RE::TESForm::LookupByID(0x13794); form && form->Is(RE::FormType::Keyword))
    _kwActorTypeNPC = static_cast<RE::BGSKeyword*>(form);

  _kwResolved.store(true, std::memory_order_release);
  logger::info("[Fertility] ActorTypeNPC keyword: {:08X}", _kwActorTypeNPC ? _kwActorTypeNPC->GetFormID() : 0);
}

static bool ContainsCaseInsensitive(std::string_view haystack, std::string_view needle)
{
  if (needle.empty())
    return true;
  if (haystack.size() < needle.size())
    return false;
  auto toLower = [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  };
  return !std::ranges::search(haystack, needle, {}, toLower, toLower).empty();
}

bool Storage::RaceHeuristic(RE::TESRace* race) const
{
  if (!race)
    return false;
  int score = 0;
  if (race->data.flags.any(RE::RACE_DATA::Flag::kPlayable))
    ++score;
  bool hasSkeleton = std::ranges::any_of(race->skeletonModels, [](auto& model) {
    auto path = model.GetModel();
    return path && path[0] != '\0' && ContainsCaseInsensitive(path, "character");
  });
  if (hasSkeleton)
    ++score;
  if (race->bodyPartData)
    ++score;
  return score >= 2;
}

bool Storage::IsRaceEligible(RE::TESRace* race) const
{
  if (!race)
    return false;
  auto raceID = race->GetFormID();

  {
    std::shared_lock lk(_raceCacheMtx);
    if (auto it = _raceEligCache.find(raceID); it != _raceEligCache.end())
      return it->second;
  }

  ResolveVanillaKeywords();
  bool result = (_kwActorTypeNPC && race->HasKeyword(_kwActorTypeNPC)) ? true : RaceHeuristic(race);

  {
    std::unique_lock lk(_raceCacheMtx);
    _raceEligCache[raceID] = result;
  }

  if (verboseMode) {
    const char* eid = race->GetFormEditorID();
    logger::info("[Fertility] Race {:08X} '{}' → {}", raceID, eid ? eid : "?", result ? "eligible" : "rejected");
  }
  return result;
}

bool Storage::IsEligible(RE::Actor* actor) const
{
  if (!actor)
    return false;
  return DiagnoseEligibility(actor).eligible;
}

Storage::EligibilityResult Storage::DiagnoseEligibility(RE::Actor* actor) const
{
  using R = EligibilityResult::Reason;
  if (!actor)
    return {false, R::NullActor};
  auto id = actor->GetFormID();
  {
    std::shared_lock lk(_eligMtx);
    if (_excludedActors.contains(id))
      return {false, R::ManualExclude};
    if (_allowedActors.contains(id))
      return {true, R::ManualAllow};
  }

  auto* race = actor->GetRace();
  if (IsRaceEligible(race)) {
    ResolveVanillaKeywords();
    if (_kwActorTypeNPC && race && race->HasKeyword(_kwActorTypeNPC))
      return {true, R::KeywordNPC};
    return {true, R::HeuristicPass};
  }
  return {false, R::HeuristicFail};
}

void Storage::AllowActor(RE::FormID id)
{
  std::unique_lock lk(_eligMtx);
  _allowedActors.insert(id);
  _excludedActors.erase(id);
}
void Storage::RevokeAllow(RE::FormID id)
{
  std::unique_lock lk(_eligMtx);
  _allowedActors.erase(id);
}
bool Storage::IsManuallyAllowed(RE::FormID id) const
{
  std::shared_lock lk(_eligMtx);
  return _allowedActors.contains(id);
}
void Storage::ExcludeActor(RE::FormID id)
{
  std::unique_lock lk(_eligMtx);
  _excludedActors.insert(id);
  _allowedActors.erase(id);
}
void Storage::RevokeExclude(RE::FormID id)
{
  std::unique_lock lk(_eligMtx);
  _excludedActors.erase(id);
}
bool Storage::IsManuallyExcluded(RE::FormID id) const
{
  std::shared_lock lk(_eligMtx);
  return _excludedActors.contains(id);
}

// ── Donors ──

std::shared_ptr<DonorProfile> Storage::GetOrCreateDonor(RE::Actor* actor)
{
  if (!actor)
    return nullptr;
  auto key = DonorKey::From(actor);
  std::unique_lock lk(_donorMtx);
  if (auto it = _donors.find(key); it != _donors.end())
    return it->second;
  auto prof           = std::make_shared<DonorProfile>();
  prof->key           = key;
  prof->raceID        = actor->GetRace() ? actor->GetRace()->GetFormID() : 0;
  prof->survivalHours = defaultSpermLifeHours;
  auto [ins, _]       = _donors.emplace(key, std::move(prof));
  return ins->second;
}

std::shared_ptr<DonorProfile> Storage::FindDonor(const DonorKey& key)
{
  std::shared_lock lk(_donorMtx);
  auto it = _donors.find(key);
  return it != _donors.end() ? it->second : nullptr;
}

void Storage::RemoveDonor(const DonorKey& key)
{
  std::unique_lock lk(_donorMtx);
  _donors.erase(key);
}
std::size_t Storage::DonorCount() const
{
  std::shared_lock lk(_donorMtx);
  return _donors.size();
}

void Storage::ForEachDonor(const std::function<void(const DonorKey&, DonorProfile&)>& fn)
{
  std::vector<std::pair<DonorKey, std::shared_ptr<DonorProfile>>> snap;
  {
    std::shared_lock lk(_donorMtx);
    snap.reserve(_donors.size());
    for (auto& [k, v] : _donors)
      snap.emplace_back(k, v);
  }
  for (auto& [k, v] : snap)
    fn(k, *v);
}

// ── Recipients ──

std::shared_ptr<RecipientData> Storage::RegisterRecipient(RE::Actor* actor)
{
  if (!actor || !IsEligible(actor))
    return nullptr;
  auto id = actor->GetFormID();
  std::unique_lock lk(_recipientMtx);
  if (auto it = _recipients.find(id); it != _recipients.end())
    return it->second;

  float now         = Now();
  auto r            = std::make_shared<RecipientData>();
  r->formID         = id;
  r->lastUpdateTime = now;

  thread_local std::mt19937 tls_rng{std::random_device{}()};
  std::uniform_int_distribution<int> phaseDist(0, 3);
  auto startPhase = static_cast<CyclePhase>(phaseDist(tls_rng));
  float dur       = TickManager::GetSingleton()->RollPhaseDuration(startPhase);
  std::uniform_real_distribution<float> offsetDist(0.0f, dur);

  r->phase             = startPhase;
  r->phaseDurationDays = dur;
  r->phaseStartTime    = now - offsetDist(tls_rng);

  if (auto* loc = actor->GetCurrentLocation())
    r->lastLocation = loc->GetName();

  auto [ins, _] = _recipients.emplace(id, std::move(r));
  return ins->second;
}

std::shared_ptr<RecipientData> Storage::GetRecipient(RE::FormID id)
{
  std::shared_lock lk(_recipientMtx);
  auto it = _recipients.find(id);
  return it != _recipients.end() ? it->second : nullptr;
}

bool Storage::UnregisterRecipient(RE::FormID id, const char* reason)
{
  std::unique_lock lk(_recipientMtx);
  auto it = _recipients.find(id);
  if (it == _recipients.end())
    return false;
  if (!it->second->isDead && HasActiveData(*it->second)) {
    if (verboseMode)
      logger::info("[Fertility] Unregister blocked: {:08X} — {}", id, reason);
    return false;
  }
  if (verboseMode)
    logger::info("[Fertility] Unregistered {:08X} — {}", id, reason);
  _recipients.erase(it);
  return true;
}

bool Storage::IsRecipientTracked(RE::FormID id) const
{
  std::shared_lock lk(_recipientMtx);
  return _recipients.contains(id);
}
std::size_t Storage::RecipientCount() const
{
  std::shared_lock lk(_recipientMtx);
  return _recipients.size();
}
void Storage::ClearRecipients()
{
  std::unique_lock lk(_recipientMtx);
  _recipients.clear();
}

void Storage::ForEachRecipient(const std::function<void(RE::FormID, RecipientData&)>& fn)
{
  std::vector<std::pair<RE::FormID, std::shared_ptr<RecipientData>>> snap;
  {
    std::shared_lock lk(_recipientMtx);
    snap.reserve(_recipients.size());
    for (auto& [id, sp] : _recipients)
      snap.emplace_back(id, sp);
  }
  for (auto& [id, sp] : snap)
    fn(id, *sp);
}

// ── Insemination ──

static std::mt19937& RNG()
{
  static thread_local std::mt19937 gen{std::random_device{}()};
  return gen;
}

bool Storage::Inseminate(RE::Actor* recipient, RE::Actor* donor, InseminationType type, bool creatureDonor)
{
  if (!recipient || !donor)
    return false;
  auto rID = recipient->GetFormID();
  auto dp  = GetOrCreateDonor(donor);
  if (!dp)
    return false;
  float now = Now();

  // cooldown under _donorMtx
  {
    std::unique_lock lk(_donorMtx);
    if (dp->cooldownLocked) {
      float hoursSince = (now - dp->lastInseminationTime) * 24.0f;
      if (hoursSince < donorCooldownHours)
        return false;
      dp->cooldownLocked = false;
    }
  }

  float sMin = baseSpermMin, sMax = baseSpermMax;
  float vMin = baseVolumeMin, vMax = baseVolumeMax;
  if (auto* race = donor->GetRace()) {
    if (auto ov = RaceOverrides::GetSingleton()->GetOverride(race)) {
      if (ov->baseSperm > 0.0f)
        sMin = sMax = ov->baseSperm;
      if (ov->baseVolume > 0.0f)
        vMin = vMax = ov->baseVolume;
    }
  }

  std::uniform_real_distribution<float> spermDist(sMin, std::max(sMin, sMax));
  std::uniform_real_distribution<float> volDist(vMin, std::max(vMin, vMax));
  float finalAmount = spermDist(RNG()) * dp->fertilityMult;
  float finalVolume = volDist(RNG());

  switch (type) {
  case InseminationType::Natural:
  case InseminationType::Magical:
  case InseminationType::Fill:
    break;
  case InseminationType::Anal:
    finalAmount *= 0.15f;
    break;
  case InseminationType::Oral:
    finalAmount = 0.0f;
    finalVolume *= 0.3f;
    break;
  case InseminationType::Artificial:
    finalVolume = 0.0f;
    break;
  }

  SpermDeposit dep;
  dep.donor         = dp->key;
  dep.donorName     = donor->GetName();
  dep.donorRaceID   = dp->raceID;
  dep.type          = type;
  dep.depositTime   = now;
  dep.amount        = finalAmount;
  dep.volumeML      = finalVolume;
  dep.survivalHours = dp->survivalHours;
  dep.isCreature    = creatureDonor || dp->isCreature;

  {
    std::unique_lock lk(_recipientMtx);
    auto it = _recipients.find(rID);
    if (it == _recipients.end())
      return false;
    auto& rd = it->second;
    if (rd->IsPregnant())
      return false;
    if (dep.amount > 0.0f)
      rd->sperm.push_back(dep);
    if (dep.volumeML > 0.0f)
      rd->AddVolume(dep.volumeML, now);
    InseminationRecord rec;
    rec.donor      = dp->key;
    rec.donorName  = dep.donorName;
    rec.type       = type;
    rec.time       = now;
    rec.amount     = dep.amount;
    rec.volumeML   = dep.volumeML;
    rec.isCreature = dep.isCreature;
    rd->history.push_back(std::move(rec));
  }

  // mark cooldown under _donorMtx
  {
    std::unique_lock lk(_donorMtx);
    dp->cooldownLocked       = true;
    dp->lastInseminationTime = now;
  }

  if (verboseMode)
    logger::info("[Fertility] {} -> {}: type={}, sperm={:.0f}, vol={:.1f}mL", donor->GetName(), recipient->GetName(), static_cast<int>(type),
                 dep.amount, dep.volumeML);
  return true;
}

bool Storage::InseminateRaw(RE::FormID recipientID, SpermDeposit deposit)
{
  std::unique_lock lk(_recipientMtx);
  auto it = _recipients.find(recipientID);
  if (it == _recipients.end())
    return false;
  auto& r = it->second;
  if (r->IsPregnant())
    return false;
  float now = Now();
  InseminationRecord rec;
  rec.donor      = deposit.donor;
  rec.donorName  = deposit.donorName;
  rec.type       = deposit.type;
  rec.time       = deposit.depositTime;
  rec.amount     = deposit.amount;
  rec.volumeML   = deposit.volumeML;
  rec.isCreature = deposit.isCreature;
  r->history.push_back(std::move(rec));
  if (deposit.volumeML > 0.0f)
    r->AddVolume(deposit.volumeML, now);
  if (deposit.amount > 0.0f)
    r->sperm.push_back(std::move(deposit));
  return true;
}

void Storage::PurgeExpiredSperm(RE::FormID id)
{
  std::unique_lock lk(_recipientMtx);
  if (auto it = _recipients.find(id); it != _recipients.end())
    it->second->PurgeExpiredSperm(Now());
}

void Storage::ClearSperm(RE::FormID id)
{
  std::unique_lock lk(_recipientMtx);
  if (auto it = _recipients.find(id); it != _recipients.end())
    it->second->sperm.clear();
}

float Storage::GetViableSpermFrom(RE::FormID id, const DonorKey& donor) const
{
  std::shared_lock lk(_recipientMtx);
  auto it = _recipients.find(id);
  if (it == _recipients.end())
    return 0.0f;
  return it->second->ViableSpermFrom(donor, Now());
}

float Storage::ExpelPortion(RE::FormID recipientID, float fraction)
{
  fraction = std::clamp(fraction, 0.0f, 1.0f);
  std::unique_lock lk(_recipientMtx);
  auto it = _recipients.find(recipientID);
  if (it == _recipients.end())
    return 0.0f;
  auto& r = *it->second;
  if (r.IsPregnant())
    return 0.0f;
  float now         = Now();
  float expelledVol = 0.0f;
  for (auto& sp : r.sperm) {
    sp.amount -= sp.amount * fraction;
    float dv = sp.volumeML * fraction;
    sp.volumeML -= dv;
    expelledVol += dv;
  }
  std::erase_if(r.sperm, [](const SpermDeposit& sp) {
    return sp.amount <= 0.01f && sp.volumeML <= 0.01f;
  });
  if (expelledVol > 0.0f) {
    r.inflationVolume  = std::max(0.0f, r.GetCurrentVolume(now) - expelledVol);
    r.volumeLastUpdate = now;
  }
  return expelledVol;
}

// ── Conception ──

bool Storage::Conceive(RE::FormID recipientID)
{
  std::unique_lock lk(_recipientMtx);
  auto it = _recipients.find(recipientID);
  if (it == _recipients.end())
    return false;
  auto& r = it->second;
  if (r->IsPregnant() || r->sperm.empty())
    return false;

  float now         = Now();
  auto viable       = r->sperm | std::views::filter([now](auto& sp) {
                  return sp.IsViable(now);
                });
  float totalWeight = std::ranges::fold_left(viable, 0.0f, [now](float acc, const SpermDeposit& sp) {
    return acc + sp.EffectiveAmount(now);
  });
  if (totalWeight <= 0.0f)
    return false;

  std::uniform_real_distribution<float> dist(0.0f, totalWeight);
  float roll                 = dist(RNG());
  float cum                  = 0.0f;
  const SpermDeposit* winner = nullptr;
  for (auto& sp : r->sperm) {
    if (!sp.IsViable(now))
      continue;
    cum += sp.EffectiveAmount(now);
    if (cum >= roll) {
      winner = &sp;
      break;
    }
  }
  if (!winner) {
    for (auto i = r->sperm.rbegin(); i != r->sperm.rend(); ++i)
      if (i->IsViable(now)) {
        winner = &*i;
        break;
      }
  }
  if (!winner)
    return false;

  PregnancyState ps;
  ps.conceptionTime = now;
  ps.father         = winner->donor;
  ps.fatherName     = winner->donorName;
  ps.fatherRaceID   = winner->donorRaceID;
  ps.creatureFather = winner->isCreature;
  r->pregnancy      = std::move(ps);
  r->sperm.clear();

  if (eventMessages) {
    auto& name = r->pregnancy->fatherName;
    RE::SendHUDMessage::ShowHUDMessage(
        std::format("Conception — father: {}{}", name.empty() ? "Unknown" : name, r->pregnancy->creatureFather ? " (creature)" : "").c_str());
  }
  return true;
}

bool Storage::GiveBirth(RE::FormID recipientID)
{
  std::unique_lock lk(_recipientMtx);
  auto it = _recipients.find(recipientID);
  if (it == _recipients.end())
    return false;
  auto& r = it->second;
  if (!r->IsPregnant())
    return false;
  float now                     = Now();
  r->pregnancy->birthTime       = now;
  r->pregnancy->recoveryEndTime = now + recoveryDays;
  r->ClearFertilityData();
  return true;
}

bool Storage::IsPregnant(RE::FormID id) const
{
  std::shared_lock lk(_recipientMtx);
  auto it = _recipients.find(id);
  return it != _recipients.end() && it->second->IsPregnant();
}

float Storage::PregnancyProgress(RE::FormID id) const
{
  std::shared_lock lk(_recipientMtx);
  auto it = _recipients.find(id);
  if (it == _recipients.end() || !it->second->IsPregnant())
    return 0.0f;
  float gest = gestationDays > 0.0f ? gestationDays : 28.0f;
  return std::clamp(it->second->pregnancy->DaysSinceConception(Now()) / gest, 0.0f, 1.0f);
}

std::optional<PregnancyState> Storage::GetPregnancy(RE::FormID id) const
{
  std::shared_lock lk(_recipientMtx);
  auto it = _recipients.find(id);
  if (it != _recipients.end())
    return it->second->pregnancy;
  return std::nullopt;
}

// ── Inflation ──

float Storage::GetInflation(RE::FormID id) const
{
  std::shared_lock lk(_recipientMtx);
  auto it = _recipients.find(id);
  if (it == _recipients.end())
    return 0.0f;
  return it->second->GetTotalInflation(Now(), gestationDays, spermFullVolume);
}

float Storage::GetSpermVolume(RE::FormID id) const
{
  std::shared_lock lk(_recipientMtx);
  auto it = _recipients.find(id);
  if (it == _recipients.end())
    return 0.0f;
  return it->second->GetCurrentVolume(Now());
}

int Storage::GetUniqueDonorCount(RE::FormID id) const
{
  std::shared_lock lk(_recipientMtx);
  auto it = _recipients.find(id);
  if (it == _recipients.end())
    return 0;
  float now      = Now();
  auto donorKeys = it->second->sperm | std::views::filter([now](const SpermDeposit& sp) {
                     return sp.IsViable(now);
                   }) |
                   std::views::transform(&SpermDeposit::donor);
  std::unordered_set<DonorKey, DonorKey::Hash> unique(std::ranges::begin(donorKeys), std::ranges::end(donorKeys));
  return static_cast<int>(unique.size());
}

std::string Storage::GetCompetitionSummary(RE::FormID id) const
{
  std::shared_lock lk(_recipientMtx);
  auto it = _recipients.find(id);
  if (it == _recipients.end())
    return "No data";
  auto& r = it->second;
  if (r->IsPregnant())
    return std::format("Conceived — Father: {}{}", r->pregnancy->fatherName, r->pregnancy->creatureFather ? " (creature)" : "");
  float now = Now();
  struct Entry
  {
    std::string name;
    float weight = 0.0f;
  };
  std::unordered_map<DonorKey, Entry, DonorKey::Hash> donors;
  for (auto& sp : r->sperm) {
    if (!sp.IsViable(now))
      continue;
    auto& e = donors[sp.donor];
    if (e.name.empty())
      e.name = sp.donorName;
    e.weight += sp.EffectiveAmount(now);
  }
  float total = std::ranges::fold_left(donors | std::views::values | std::views::transform(&Entry::weight), 0.0f, std::plus<>{});
  if (total <= 0.0f)
    return "No viable sperm";
  std::string result;
  for (auto& [k, e] : donors) {
    if (!result.empty())
      result += " | ";
    result += std::format("{}: {:.1f}%", e.name, (e.weight / total) * 100.0f);
  }
  return result;
}

// ── Cycle ──



// ── Faction Rank ──

void Storage::UpdateFactionRank(RE::FormID id, RE::TESFaction* faction)
{
  if (!faction)
    return;
  auto r = GetRecipient(id);
  if (!r)
    return;
  auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
  if (!actor)
    return;

  int rank  = 0;
  float now = Now();
  WithRecipientRead(id, [&](const RecipientData& rd) {
    if (rd.IsPregnant()) {
      float gest = gestationDays > 0.0f ? gestationDays : 28.0f;
      float prog = std::clamp(rd.pregnancy->DaysSinceConception(now) / gest, 0.0f, 1.0f);
      rank       = std::clamp(static_cast<int>(prog * 100.0f), 1, 100);
    } else if (rd.InRecovery(now, recoveryDays)) {
      float rec     = recoveryDays > 0.0f ? recoveryDays : 3.0f;
      float recProg = (now - rd.pregnancy->birthTime) / rec;
      rank          = 101 + std::clamp(static_cast<int>(recProg * 14.0f), 0, 14);
    } else {
      rank = 116 + static_cast<int>(rd.phase);
      if (rank > 119)
        rank = 0;
    }
  });

  WithRecipientWrite(id, [&](RecipientData& rd) {
    rd.factionRank = rank;
  });
  actor->AddToFaction(faction, rank > 0 ? static_cast<std::int8_t>(rank) : -1);
}

// ── Children ──

int Storage::AddChild(const ChildRecord& child)
{
  std::unique_lock lk(_childMtx);
  _children.push_back(child);
  return static_cast<int>(_children.size()) - 1;
}

bool Storage::RemoveChild(int index)
{
  std::unique_lock lk(_childMtx);
  if (index < 0 || index >= static_cast<int>(_children.size()))
    return false;
  _children.erase(_children.begin() + index);
  for (auto& f : _followers) {
    if (f.childIndex == index) {
      f.childIndex = -1;
      f.actorID    = 0;
    } else if (f.childIndex > index)
      --f.childIndex;
  }
  std::erase_if(_followers, [](const FollowerSlot& f) {
    return f.childIndex < 0;
  });
  return true;
}

std::optional<ChildRecord> Storage::GetChild(int index) const
{
  std::shared_lock lk(_childMtx);
  if (index < 0 || index >= static_cast<int>(_children.size()))
    return std::nullopt;
  return _children[index];
}

int Storage::ChildCount() const
{
  std::shared_lock lk(_childMtx);
  return static_cast<int>(_children.size());
}

bool Storage::AddFollower(RE::FormID actorID, int childIdx)
{
  std::unique_lock lk(_childMtx);
  if (std::ranges::any_of(_followers, [childIdx](const FollowerSlot& f) {
        return f.childIndex == childIdx;
      }))
    return false;
  int active = static_cast<int>(std::ranges::count_if(_followers, [](const FollowerSlot& f) {
    return f.actorID != 0;
  }));
  if (active >= maxActiveFollowers)
    return false;
  _followers.push_back({actorID, childIdx});
  return true;
}

bool Storage::RemoveFollower(int childIdx)
{
  std::unique_lock lk(_childMtx);
  auto it = std::ranges::find_if(_followers, [childIdx](const FollowerSlot& f) {
    return f.childIndex == childIdx;
  });
  if (it == _followers.end())
    return false;
  if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(it->actorID)) {
    actor->GetActorRuntimeData().boolBits.reset(RE::Actor::BOOL_BITS::kPlayerTeammate);
    actor->EvaluatePackage();
    actor->Disable();
  }
  _followers.erase(it);
  return true;
}

void Storage::DismissAll()
{
  std::unique_lock lk(_childMtx);
  for (auto& f : _followers) {
    if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(f.actorID)) {
      actor->GetActorRuntimeData().boolBits.reset(RE::Actor::BOOL_BITS::kPlayerTeammate);
      actor->EvaluatePackage();
      actor->Disable();
    }
  }
  _followers.clear();
}

bool Storage::IsFollowing(int childIdx) const
{
  std::shared_lock lk(_childMtx);
  return std::ranges::any_of(_followers, [childIdx](const FollowerSlot& f) {
    return f.childIndex == childIdx;
  });
}

int Storage::FollowerCount() const
{
  std::shared_lock lk(_childMtx);
  return static_cast<int>(std::ranges::count_if(_followers, [](const FollowerSlot& f) {
    return f.actorID != 0;
  }));
}

// ── Death ──

void Storage::OnActorDeath(RE::Actor* dead)
{
  if (!dead)
    return;
  auto id = dead->GetFormID();
  {
    std::unique_lock lk(_recipientMtx);
    auto it = _recipients.find(id);
    if (it != _recipients.end()) {
      bool wasPregnant = it->second->IsPregnant();
      std::string name = dead->GetName();
      if (eventMessages) {
        if (wasPregnant)
          RE::SendHUDMessage::ShowHUDMessage(std::format("{} and their unborn child have perished.", name).c_str());
        else
          RE::SendHUDMessage::ShowHUDMessage(std::format("{} has perished.", name).c_str());
      }
      FireEvent("RecipientDeath", dead, wasPregnant ? 1.0f : 0.0f);
      _recipients.erase(it);
    }
  }
  RemoveDonor(DonorKey::FromFormID(id));
}

// ── Cleanup ──

bool Storage::HasActiveData(const RecipientData& r) const
{
  return r.HasActiveData(Now(), recoveryDays, babyDuration);
}

void Storage::AutoCleanupLocked(float now)
{
  std::erase_if(_recipients, [&](auto& pair) {
    auto& [id, rp] = pair;
    auto& r        = *rp;
    if (r.isDead || HasActiveData(r))
      return false;
    if (r.locationLeftTime > 0.0f && (now - r.locationLeftTime) >= autoCleanupDays)
      return true;
    if (r.lastUpdateTime > 0.0f && (now - r.lastUpdateTime) > autoCleanupDays)
      return true;
    return false;
  });
}

void Storage::AutoCleanup()
{
  float now = Now();
  std::unique_lock lk(_recipientMtx);
  AutoCleanupLocked(now);
}

void Storage::OnLocationChange(const std::string& oldLoc, const std::string& newLoc)
{
  if (oldLoc == newLoc || newLoc.empty())
    return;
  float now = Now();
  std::unique_lock lk(_recipientMtx);
  for (auto& [id, rp] : _recipients) {
    auto& r = *rp;
    if (!oldLoc.empty() && r.lastLocation == oldLoc)
      r.locationLeftTime = now;
    if (r.lastLocation == newLoc && r.locationLeftTime > 0.0f)
      r.locationLeftTime = 0.0f;
    if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(id))
      if (actor->Is3DLoaded())
        r.lastUpdateTime = now;
  }
  AutoCleanupLocked(now);
}

// ── Events ──

void Storage::FireEvent(const char* name, RE::Actor* actor, float n1, float n2)
{
  auto* source = SKSE::GetModCallbackEventSource();
  if (!source) {
    logger::warn("[Fertility] ModCallbackEventSource unavailable for '{}'", name);
    return;
  }
  std::string strArg;
  if (n2 != 0.0f)
    strArg = std::format("{:.0f}", n2);
  SKSE::ModCallbackEvent evt{RE::BSFixedString(name), RE::BSFixedString(strArg), n1, actor ? static_cast<RE::TESForm*>(actor) : nullptr};
  source->SendEvent(&evt);
}

// ── History ──

int Storage::GetInseminationCount(RE::FormID id, int typeFilter) const
{
  std::shared_lock lk(_recipientMtx);
  auto it = _recipients.find(id);
  if (it == _recipients.end())
    return 0;
  auto& hist = it->second->history;
  if (typeFilter < 0)
    return static_cast<int>(hist.size());
  auto target = static_cast<InseminationType>(typeFilter);
  return static_cast<int>(std::ranges::count_if(hist, [target](const InseminationRecord& h) {
    return h.type == target;
  }));
}

// ── Co-Save ──

void Storage::DoRevert()
{
  {
    std::unique_lock lk(_donorMtx);
    _donors.clear();
  }
  {
    std::unique_lock lk(_recipientMtx);
    _recipients.clear();
  }
  {
    std::unique_lock lk(_childMtx);
    _children.clear();
    _followers.clear();
  }
  {
    std::unique_lock lk(_eligMtx);
    _allowedActors.clear();
    _excludedActors.clear();
  }
  {
    std::lock_guard lk(_playerMtx);
    _player = {};
  }
  _kwActorTypeNPC = nullptr;
  _kwResolved.store(false, std::memory_order_release);
  {
    std::unique_lock lk(_raceCacheMtx);
    _raceEligCache.clear();
  }
  logger::info("[Fertility] Storage reverted");
}

void Storage::DoSave(SKSE::SerializationInterface* intfc) const
{
  if (intfc->OpenRecord(kRecDonors, kVersion)) {
    std::shared_lock lk(_donorMtx);
    intfc->WriteRecordData(static_cast<std::uint32_t>(_donors.size()));
    for (auto& [key, dp] : _donors) {
      WriteDonorKey(intfc, key);
      intfc->WriteRecordData(dp->raceID);
      intfc->WriteRecordData(dp->baseCount);
      intfc->WriteRecordData(dp->survivalHours);
      intfc->WriteRecordData(dp->fertilityMult);
      intfc->WriteRecordData(static_cast<std::uint8_t>(dp->cooldownLocked ? 1 : 0));
      intfc->WriteRecordData(dp->lastInseminationTime);
      intfc->WriteRecordData(static_cast<std::uint8_t>(dp->isCreature ? 1 : 0));
    }
  }

  if (intfc->OpenRecord(kRecRecipients, kVersion)) {
    std::shared_lock lk(_recipientMtx);
    intfc->WriteRecordData(static_cast<std::uint32_t>(_recipients.size()));
    for (auto& [id, rp] : _recipients) {
      auto& r = *rp;
      intfc->WriteRecordData(id);
      intfc->WriteRecordData(r.phaseStartTime);
      intfc->WriteRecordData(r.phaseDurationDays);
      intfc->WriteRecordData(static_cast<std::uint8_t>(r.phase));

      intfc->WriteRecordData(static_cast<std::uint32_t>(r.sperm.size()));
      for (auto& sp : r.sperm) {
        WriteDonorKey(intfc, sp.donor);
        WriteStr(intfc, sp.donorName);
        intfc->WriteRecordData(sp.donorRaceID);
        intfc->WriteRecordData(static_cast<std::uint8_t>(sp.type));
        intfc->WriteRecordData(sp.depositTime);
        intfc->WriteRecordData(sp.amount);
        intfc->WriteRecordData(sp.volumeML);
        intfc->WriteRecordData(sp.survivalHours);
        intfc->WriteRecordData(static_cast<std::uint8_t>(sp.isCreature ? 1 : 0));
      }

      std::uint8_t hasPreg = r.pregnancy.has_value() ? 1 : 0;
      intfc->WriteRecordData(hasPreg);
      if (r.pregnancy) {
        auto& p = *r.pregnancy;
        intfc->WriteRecordData(p.conceptionTime);
        intfc->WriteRecordData(p.birthTime);
        intfc->WriteRecordData(p.recoveryEndTime);
        intfc->WriteRecordData(p.babyEquipTime);
        WriteDonorKey(intfc, p.father);
        WriteStr(intfc, p.fatherName);
        intfc->WriteRecordData(p.fatherRaceID);
        intfc->WriteRecordData(static_cast<std::uint8_t>(p.creatureFather ? 1 : 0));
      }

      intfc->WriteRecordData(static_cast<std::uint32_t>(r.history.size()));
      for (auto& h : r.history) {
        WriteDonorKey(intfc, h.donor);
        WriteStr(intfc, h.donorName);
        intfc->WriteRecordData(static_cast<std::uint8_t>(h.type));
        intfc->WriteRecordData(h.time);
        intfc->WriteRecordData(h.amount);
        intfc->WriteRecordData(h.volumeML);
        intfc->WriteRecordData(static_cast<std::uint8_t>(h.isCreature ? 1 : 0));
      }

      intfc->WriteRecordData(r.lastUpdateTime);
      WriteStr(intfc, r.lastLocation);
      intfc->WriteRecordData(r.locationLeftTime);
      intfc->WriteRecordData(static_cast<std::uint8_t>(r.isDead ? 1 : 0));
      intfc->WriteRecordData(r.factionRank);
      intfc->WriteRecordData(r.inflationVolume);
      intfc->WriteRecordData(r.volumeLastUpdate);
      intfc->WriteRecordData(r.volumeDecayHalfLife);
    }
  }

  if (intfc->OpenRecord(kRecChildren, kVersion)) {
    std::shared_lock lk(_childMtx);
    intfc->WriteRecordData(static_cast<std::uint32_t>(_children.size()));
    for (auto& c : _children) {
      intfc->WriteRecordData(c.actorID);
      intfc->WriteRecordData(c.motherID);
      WriteDonorKey(intfc, c.father);
      WriteStr(intfc, c.name);
      intfc->WriteRecordData(c.birthTime);
      intfc->WriteRecordData(c.raceID);
    }
    intfc->WriteRecordData(static_cast<std::uint32_t>(_followers.size()));
    for (auto& f : _followers) {
      intfc->WriteRecordData(f.actorID);
      intfc->WriteRecordData(f.childIndex);
    }
  }

  if (intfc->OpenRecord(kRecPlayer, kVersion)) {
    std::lock_guard lk(_playerMtx);
    intfc->WriteRecordData(_player.lastSleepTime);
    intfc->WriteRecordData(_player.staminaDelta);
    intfc->WriteRecordData(_player.magickaDelta);
    intfc->WriteRecordData(_player.babyHealth);
    intfc->WriteRecordData(_player.lastBabyDamage);
    intfc->WriteRecordData(_player.lastBabyDamageTime);
  }

  if (intfc->OpenRecord(kRecEligibility, kVersion)) {
    std::shared_lock lk(_eligMtx);
    intfc->WriteRecordData(static_cast<std::uint32_t>(_allowedActors.size()));
    for (auto aid : _allowedActors)
      intfc->WriteRecordData(aid);
    intfc->WriteRecordData(static_cast<std::uint32_t>(_excludedActors.size()));
    for (auto eid : _excludedActors)
      intfc->WriteRecordData(eid);
  }

  logger::info("[Fertility] Saved: {} donors, {} recipients, {} children", _donors.size(), _recipients.size(), _children.size());
}

void Storage::DoLoad(SKSE::SerializationInterface* intfc)
{
  DoRevert();
  std::uint32_t type, version, length;
  while (intfc->GetNextRecordInfo(type, version, length)) {
    if (version != kVersion) {
      logger::warn("[Fertility] Co-save version mismatch: got {}, want {}", version, kVersion);
      continue;
    }
    switch (type) {

    case kRecDonors: {
      std::unique_lock lk(_donorMtx);
      std::uint32_t count = 0;
      intfc->ReadRecordData(count);
      for (std::uint32_t i = 0; i < count; ++i) {
        auto key           = ReadDonorKey(intfc, true);
        auto prof          = std::make_shared<DonorProfile>();
        prof->key          = key;
        RE::FormID rawRace = 0;
        intfc->ReadRecordData(rawRace);
        intfc->ResolveFormID(rawRace, prof->raceID);
        intfc->ReadRecordData(prof->baseCount);
        intfc->ReadRecordData(prof->survivalHours);
        intfc->ReadRecordData(prof->fertilityMult);
        std::uint8_t locked = 0;
        intfc->ReadRecordData(locked);
        prof->cooldownLocked = locked != 0;
        intfc->ReadRecordData(prof->lastInseminationTime);
        std::uint8_t crt = 0;
        intfc->ReadRecordData(crt);
        prof->isCreature = crt != 0;
        _donors[key]     = std::move(prof);
      }
      break;
    }

    case kRecRecipients: {
      std::unique_lock lk(_recipientMtx);
      std::uint32_t count = 0;
      intfc->ReadRecordData(count);
      for (std::uint32_t i = 0; i < count; ++i) {
        RE::FormID rawID = 0;
        intfc->ReadRecordData(rawID);
        RE::FormID resolved = 0;
        bool valid          = intfc->ResolveFormID(rawID, resolved);
        auto r              = std::make_shared<RecipientData>();
        r->formID           = valid ? resolved : 0;

        intfc->ReadRecordData(r->phaseStartTime);
        intfc->ReadRecordData(r->phaseDurationDays);
        std::uint8_t ph = 0;
        intfc->ReadRecordData(ph);
        r->phase = static_cast<CyclePhase>(ph);

        std::uint32_t sc = 0;
        intfc->ReadRecordData(sc);
        r->sperm.reserve(sc);
        for (std::uint32_t s = 0; s < sc; ++s) {
          SpermDeposit sp;
          sp.donor           = ReadDonorKey(intfc, true);
          sp.donorName       = ReadStr(intfc);
          RE::FormID rawRace = 0;
          intfc->ReadRecordData(rawRace);
          intfc->ResolveFormID(rawRace, sp.donorRaceID);
          std::uint8_t stype = 0;
          intfc->ReadRecordData(stype);
          sp.type = static_cast<InseminationType>(stype);
          intfc->ReadRecordData(sp.depositTime);
          intfc->ReadRecordData(sp.amount);
          intfc->ReadRecordData(sp.volumeML);
          intfc->ReadRecordData(sp.survivalHours);
          std::uint8_t crt = 0;
          intfc->ReadRecordData(crt);
          sp.isCreature = crt != 0;
          r->sperm.push_back(std::move(sp));
        }

        std::uint8_t hasPreg = 0;
        intfc->ReadRecordData(hasPreg);
        if (hasPreg) {
          PregnancyState p;
          intfc->ReadRecordData(p.conceptionTime);
          intfc->ReadRecordData(p.birthTime);
          intfc->ReadRecordData(p.recoveryEndTime);
          intfc->ReadRecordData(p.babyEquipTime);
          p.father           = ReadDonorKey(intfc, true);
          p.fatherName       = ReadStr(intfc);
          RE::FormID rawRace = 0;
          intfc->ReadRecordData(rawRace);
          intfc->ResolveFormID(rawRace, p.fatherRaceID);
          std::uint8_t cf = 0;
          intfc->ReadRecordData(cf);
          p.creatureFather = cf != 0;
          r->pregnancy     = std::move(p);
        }

        std::uint32_t hc = 0;
        intfc->ReadRecordData(hc);
        r->history.reserve(hc);
        for (std::uint32_t h = 0; h < hc; ++h) {
          InseminationRecord rec;
          rec.donor          = ReadDonorKey(intfc, true);
          rec.donorName      = ReadStr(intfc);
          std::uint8_t htype = 0;
          intfc->ReadRecordData(htype);
          rec.type = static_cast<InseminationType>(htype);
          intfc->ReadRecordData(rec.time);
          intfc->ReadRecordData(rec.amount);
          intfc->ReadRecordData(rec.volumeML);
          std::uint8_t hcrt = 0;
          intfc->ReadRecordData(hcrt);
          rec.isCreature = hcrt != 0;
          r->history.push_back(std::move(rec));
        }

        intfc->ReadRecordData(r->lastUpdateTime);
        r->lastLocation = ReadStr(intfc);
        intfc->ReadRecordData(r->locationLeftTime);
        std::uint8_t dead = 0;
        intfc->ReadRecordData(dead);
        r->isDead = dead != 0;
        intfc->ReadRecordData(r->factionRank);
        intfc->ReadRecordData(r->inflationVolume);
        intfc->ReadRecordData(r->volumeLastUpdate);
        intfc->ReadRecordData(r->volumeDecayHalfLife);

        if (valid)
          _recipients[resolved] = std::move(r);
      }
      break;
    }

    case kRecChildren: {
      std::unique_lock lk(_childMtx);
      std::uint32_t cc = 0;
      intfc->ReadRecordData(cc);
      _children.reserve(cc);
      for (std::uint32_t i = 0; i < cc; ++i) {
        ChildRecord c;
        RE::FormID rawA = 0, rawM = 0;
        intfc->ReadRecordData(rawA);
        intfc->ResolveFormID(rawA, c.actorID);
        intfc->ReadRecordData(rawM);
        intfc->ResolveFormID(rawM, c.motherID);
        c.father = ReadDonorKey(intfc, true);
        c.name   = ReadStr(intfc);
        intfc->ReadRecordData(c.birthTime);
        RE::FormID rawR = 0;
        intfc->ReadRecordData(rawR);
        intfc->ResolveFormID(rawR, c.raceID);
        _children.push_back(std::move(c));
      }
      std::uint32_t fc = 0;
      intfc->ReadRecordData(fc);
      _followers.reserve(fc);
      for (std::uint32_t i = 0; i < fc; ++i) {
        FollowerSlot f;
        RE::FormID rawA = 0;
        intfc->ReadRecordData(rawA);
        intfc->ResolveFormID(rawA, f.actorID);
        intfc->ReadRecordData(f.childIndex);
        _followers.push_back(f);
      }
      break;
    }

    case kRecPlayer: {
      std::lock_guard lk(_playerMtx);
      intfc->ReadRecordData(_player.lastSleepTime);
      intfc->ReadRecordData(_player.staminaDelta);
      intfc->ReadRecordData(_player.magickaDelta);
      intfc->ReadRecordData(_player.babyHealth);
      intfc->ReadRecordData(_player.lastBabyDamage);
      intfc->ReadRecordData(_player.lastBabyDamageTime);
      break;
    }

    case kRecEligibility: {
      std::unique_lock lk(_eligMtx);
      std::uint32_t ac = 0;
      intfc->ReadRecordData(ac);
      for (std::uint32_t i = 0; i < ac; ++i) {
        RE::FormID raw = 0;
        intfc->ReadRecordData(raw);
        RE::FormID resolved = 0;
        if (intfc->ResolveFormID(raw, resolved))
          _allowedActors.insert(resolved);
      }
      std::uint32_t ec = 0;
      intfc->ReadRecordData(ec);
      for (std::uint32_t i = 0; i < ec; ++i) {
        RE::FormID raw = 0;
        intfc->ReadRecordData(raw);
        RE::FormID resolved = 0;
        if (intfc->ResolveFormID(raw, resolved))
          _excludedActors.insert(resolved);
      }
      break;
    }

    default:
      logger::warn("[Fertility] Unknown record {:08X}", type);
      break;
    }
  }
  logger::info("[Fertility] Loaded: {} donors, {} recipients, {} children", _donors.size(), _recipients.size(), _children.size());
}

void Storage::OnSave(SKSE::SerializationInterface* intfc)
{
  GetSingleton()->DoSave(intfc);
}
void Storage::OnLoad(SKSE::SerializationInterface* intfc)
{
  GetSingleton()->DoLoad(intfc);
}
void Storage::OnRevert(SKSE::SerializationInterface*)
{
  GetSingleton()->DoRevert();
}

}  // namespace Fertility
