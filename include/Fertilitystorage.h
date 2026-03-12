#pragma once

#include <ConfigManager.h>
#include <Fertilitytypes.h>
#include <RaceOverrides.h>
#include <TickManager.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Fertility
{

class Storage
{
public:
  static Storage* GetSingleton()
  {
    static Storage inst;
    return &inst;
  }

  void Initialize();
  float Now() const;

  // ── Eligibility ──

  struct EligibilityResult
  {
    bool eligible = false;
    enum class Reason
    {
      NullActor,
      ManualAllow,
      ManualExclude,
      KeywordNPC,
      HeuristicPass,
      HeuristicFail,
    } reason = Reason::NullActor;
  };

  bool IsEligible(RE::Actor* actor) const;
  EligibilityResult DiagnoseEligibility(RE::Actor* actor) const;
  bool IsRaceEligible(RE::TESRace* race) const;

  void AllowActor(RE::FormID id);
  void RevokeAllow(RE::FormID id);
  bool IsManuallyAllowed(RE::FormID id) const;
  void ExcludeActor(RE::FormID id);
  void RevokeExclude(RE::FormID id);
  bool IsManuallyExcluded(RE::FormID id) const;

  // ── Player state (mutex-protected) ──

  void SetPlayerBabyHealth(float v)
  {
    std::lock_guard lk(_playerMtx);
    _player.babyHealth = v;
  }

  float GetPlayerBabyHealth() const
  {
    std::lock_guard lk(_playerMtx);
    return _player.babyHealth;
  }

  void SetPlayerLastBabyDamage(float damage, float time)
  {
    std::lock_guard lk(_playerMtx);
    _player.lastBabyDamage     = damage;
    _player.lastBabyDamageTime = time;
  }

  void SetPlayerStaminaMagickaDelta(float stamina, float magicka)
  {
    std::lock_guard lk(_playerMtx);
    _player.staminaDelta = stamina;
    _player.magickaDelta = magicka;
  }

  void SetPlayerLastSleepTime(float t)
  {
    std::lock_guard lk(_playerMtx);
    _player.lastSleepTime = t;
  }

  PlayerState GetPlayerState() const
  {
    std::lock_guard lk(_playerMtx);
    return _player;
  }

  void ResetPlayerState()
  {
    std::lock_guard lk(_playerMtx);
    _player = {};
  }

  template <typename Fn>
  void WithPlayerState(Fn&& fn)
  {
    std::lock_guard lk(_playerMtx);
    fn(_player);
  }

  template <typename Fn>
  void WithPlayerStateRead(Fn&& fn) const
  {
    std::lock_guard lk(_playerMtx);
    fn(_player);
  }

  // ── Donors ──

  std::shared_ptr<DonorProfile> GetOrCreateDonor(RE::Actor* actor);
  std::shared_ptr<DonorProfile> FindDonor(const DonorKey& key);
  void RemoveDonor(const DonorKey& key);
  std::size_t DonorCount() const;
  void ForEachDonor(const std::function<void(const DonorKey&, DonorProfile&)>& fn);

  // ── Recipients ──

  std::shared_ptr<RecipientData> RegisterRecipient(RE::Actor* actor);
  std::shared_ptr<RecipientData> GetRecipient(RE::FormID id);
  bool UnregisterRecipient(RE::FormID id, const char* reason = "");
  bool IsRecipientTracked(RE::FormID id) const;
  std::size_t RecipientCount() const;
  void ClearRecipients();
  void ForEachRecipient(const std::function<void(RE::FormID, RecipientData&)>& fn);

  template <typename Fn>
  bool WithRecipientWrite(RE::FormID id, Fn&& fn)
  {
    std::unique_lock lk(_recipientMtx);
    auto it = _recipients.find(id);
    if (it == _recipients.end())
      return false;
    fn(*it->second);
    return true;
  }

  template <typename Fn>
  bool WithRecipientRead(RE::FormID id, Fn&& fn) const
  {
    std::shared_lock lk(_recipientMtx);
    auto it = _recipients.find(id);
    if (it == _recipients.end())
      return false;
    fn(*it->second);
    return true;
  }

  // ── Insemination ──

  bool Inseminate(RE::Actor* recipient, RE::Actor* donor, InseminationType type, bool creatureDonor);
  bool InseminateRaw(RE::FormID recipientID, SpermDeposit deposit);
  void PurgeExpiredSperm(RE::FormID recipientID);
  void ClearSperm(RE::FormID recipientID);
  float GetViableSpermFrom(RE::FormID recipientID, const DonorKey& donor) const;
  float ExpelPortion(RE::FormID recipientID, float fraction);

  // ── Conception / Pregnancy ──

  bool Conceive(RE::FormID recipientID);
  bool GiveBirth(RE::FormID recipientID);
  bool IsPregnant(RE::FormID recipientID) const;
  float PregnancyProgress(RE::FormID recipientID) const;
  std::optional<PregnancyState> GetPregnancy(RE::FormID recipientID) const;

  // ── Inflation ──

  float GetInflation(RE::FormID recipientID) const;
  float GetSpermVolume(RE::FormID recipientID) const;

  // ── Cycle ──

  // ── Competition ──

  int GetUniqueDonorCount(RE::FormID recipientID) const;
  std::string GetCompetitionSummary(RE::FormID recipientID) const;

  // ── Faction ──

  void UpdateFactionRank(RE::FormID id, RE::TESFaction* faction);

  // ── Children / Followers ──

  int AddChild(const ChildRecord& child);
  bool RemoveChild(int index);
  std::optional<ChildRecord> GetChild(int index) const;
  int ChildCount() const;
  bool AddFollower(RE::FormID actorID, int childIdx);
  bool RemoveFollower(int childIdx);
  void DismissAll();
  bool IsFollowing(int childIdx) const;
  int FollowerCount() const;

  // ── Death ──

  void OnActorDeath(RE::Actor* dead);

  // ── Cleanup ──

  bool HasActiveData(const RecipientData& r) const;
  void AutoCleanup();
  void OnLocationChange(const std::string& oldLoc, const std::string& newLoc);

  // ── History ──

  int GetInseminationCount(RE::FormID recipientID, int typeFilter = -1) const;

  // ── Events ──

  static void FireEvent(const char* name, RE::Actor* actor, float n1 = 0.0f, float n2 = 0.0f);

  // ── Co-Save ──

  static void OnSave(SKSE::SerializationInterface* intfc);
  static void OnLoad(SKSE::SerializationInterface* intfc);
  static void OnRevert(SKSE::SerializationInterface*);

  static constexpr std::uint32_t kRecDonors      = 'FDON';
  static constexpr std::uint32_t kRecRecipients  = 'FREC';
  static constexpr std::uint32_t kRecChildren    = 'FCHI';
  static constexpr std::uint32_t kRecPlayer      = 'FPLY';
  static constexpr std::uint32_t kRecEligibility = 'FELG';
  static constexpr std::uint32_t kVersion        = 3;

private:
  Storage() = default;

  static void WriteStr(SKSE::SerializationInterface* i, const std::string& s);
  static std::string ReadStr(SKSE::SerializationInterface* i);
  static void WriteDonorKey(SKSE::SerializationInterface* i, const DonorKey& k);
  static DonorKey ReadDonorKey(SKSE::SerializationInterface* i, bool resolve);

  void DoSave(SKSE::SerializationInterface* intfc) const;
  void DoLoad(SKSE::SerializationInterface* intfc);
  void DoRevert();

  void ResolveVanillaKeywords() const;
  bool RaceHeuristic(RE::TESRace* race) const;
  void AutoCleanupLocked(float now);

  mutable std::mutex _playerMtx;
  PlayerState _player;

  mutable std::shared_mutex _donorMtx;
  std::unordered_map<DonorKey, std::shared_ptr<DonorProfile>, DonorKey::Hash> _donors;

  mutable std::shared_mutex _recipientMtx;
  std::unordered_map<RE::FormID, std::shared_ptr<RecipientData>> _recipients;

  mutable std::shared_mutex _childMtx;
  std::vector<ChildRecord> _children;
  std::vector<FollowerSlot> _followers;

  mutable std::shared_mutex _eligMtx;
  std::unordered_set<RE::FormID> _allowedActors;
  std::unordered_set<RE::FormID> _excludedActors;

  mutable std::shared_mutex _raceCacheMtx;
  mutable std::unordered_map<RE::FormID, bool> _raceEligCache;

  mutable std::mutex _kwInitMtx;
  mutable std::atomic<bool> _kwResolved{false};
  mutable RE::BGSKeyword* _kwActorTypeNPC = nullptr;
};

}  // namespace Fertility
