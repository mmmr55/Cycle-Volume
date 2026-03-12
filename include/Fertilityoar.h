#pragma once
// =====================================================================
//  FertilityOAR.h — OAR 自定义条件
//
//  在 main.cpp 的 kMessage_PostLoad 中调用:
//    Fertility::OAR::RegisterConditions();
//
//  动画包 config.json 示例:
//    {"condition": "FertilityIsPregnant"}
//    {"condition": "FertilityPregnancyProgress", "Comparison": ">=", "Value": 0.5}
//    {"condition": "FertilityTrimester", "Comparison": "==", "Value": 3}
//    {"condition": "FertilityIsInLabor"}
//    {"condition": "FertilityIsInRecovery"}
//    {"condition": "FertilityCyclePhase", "Comparison": "==", "Value": 2}
// =====================================================================

#include "OpenAnimationReplacer-ConditionTypes.h"
#include "OpenAnimationReplacerAPI-Conditions.h"
#include <RaceOverrides.h>

#include "FertilityStorage.h"
#include "Fertilitytypes.h"

namespace Fertility::OAR
{

inline RecipientData* GetRecipient(RE::TESObjectREFR* a_refr)
{
  return a_refr ? Storage::GetSingleton()->GetRecipient(a_refr->GetFormID()) : nullptr;
}

inline float Now()
{
  auto* cal = RE::Calendar::GetSingleton();
  return cal ? cal->GetCurrentGameTime() : 0.0f;
}

// ─── FertilityIsPregnant ─────────────────────────────────────────────
class ConditionIsPregnant : public Conditions::CustomCondition
{
public:
  static constexpr std::string_view CONDITION_NAME = "FertilityIsPregnant"sv;
  [[nodiscard]] RE::BSString GetName() const override { return CONDITION_NAME.data(); }
  [[nodiscard]] RE::BSString GetDescription() const override { return "Whether the actor is currently pregnant."; }
  [[nodiscard]] REL::Version GetRequiredVersion() const override { return {1, 0, 0}; }

protected:
  bool EvaluateImpl(RE::TESObjectREFR* a_refr, RE::hkbClipGenerator*, void*) const override
  {
    auto* r = GetRecipient(a_refr);
    return r && r->IsPregnant();
  }
};

// ─── FertilityPregnancyProgress ──────────────────────────────────────
class ConditionPregnancyProgress : public Conditions::CustomCondition
{
public:
  static constexpr std::string_view CONDITION_NAME = "FertilityPregnancyProgress"sv;

  ConditionPregnancyProgress()
  {
    auto* api   = OAR_API::Conditions::GetAPI();
    _comparison = static_cast<Conditions::IComparisonConditionComponent*>(
        AddComponent(api->GetConditionComponentFactory(Conditions::ConditionComponentType::kComparison), "Comparison", "How to compare"));
    _threshold = static_cast<Conditions::INumericConditionComponent*>(
        AddComponent(api->GetConditionComponentFactory(Conditions::ConditionComponentType::kNumeric), "Value", "Threshold (0.0 ~ 1.0)"));
  }

  [[nodiscard]] RE::BSString GetName() const override { return CONDITION_NAME.data(); }
  [[nodiscard]] RE::BSString GetDescription() const override { return "Pregnancy progress 0.0 ~ 1.0."; }
  [[nodiscard]] REL::Version GetRequiredVersion() const override { return {1, 0, 0}; }

  [[nodiscard]] RE::BSString GetCurrent(RE::TESObjectREFR* a_refr) const override
  {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", GetVal(a_refr));
    return buf;
  }

protected:
  bool EvaluateImpl(RE::TESObjectREFR* a_refr, RE::hkbClipGenerator*, void*) const override
  {
    return _comparison && _threshold && _comparison->GetComparisonResult(GetVal(a_refr), _threshold->GetNumericValue(a_refr));
  }

private:
  float GetVal(RE::TESObjectREFR* a_refr) const
  {
    auto* r = GetRecipient(a_refr);
    if (!r || !r->IsPregnant())
      return 0.0f;
    float gd = std::max(1.0f, Storage::GetSingleton()->config.gestationDays);
    return std::clamp(r->pregnancy->DaysSinceConception(Now()) / gd, 0.0f, 1.0f);
  }

  Conditions::IComparisonConditionComponent* _comparison = nullptr;
  Conditions::INumericConditionComponent* _threshold     = nullptr;
};

// ─── FertilityTrimester ──────────────────────────────────────────────
class ConditionTrimester : public Conditions::CustomCondition
{
public:
  static constexpr std::string_view CONDITION_NAME = "FertilityTrimester"sv;

  ConditionTrimester()
  {
    auto* api   = OAR_API::Conditions::GetAPI();
    _comparison = static_cast<Conditions::IComparisonConditionComponent*>(
        AddComponent(api->GetConditionComponentFactory(Conditions::ConditionComponentType::kComparison), "Comparison"));
    _value = static_cast<Conditions::INumericConditionComponent*>(
        AddComponent(api->GetConditionComponentFactory(Conditions::ConditionComponentType::kNumeric), "Trimester", "0=none, 1/2/3"));
  }

  [[nodiscard]] RE::BSString GetName() const override { return CONDITION_NAME.data(); }
  [[nodiscard]] RE::BSString GetDescription() const override { return "Pregnancy trimester (0 if not pregnant)."; }
  [[nodiscard]] REL::Version GetRequiredVersion() const override { return {1, 0, 0}; }

  [[nodiscard]] RE::BSString GetCurrent(RE::TESObjectREFR* a_refr) const override
  {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%d", GetVal(a_refr));
    return buf;
  }

protected:
  bool EvaluateImpl(RE::TESObjectREFR* a_refr, RE::hkbClipGenerator*, void*) const override
  {
    return _comparison && _value && _comparison->GetComparisonResult(static_cast<float>(GetVal(a_refr)), _value->GetNumericValue(a_refr));
  }

private:
  int GetVal(RE::TESObjectREFR* a_refr) const
  {
    auto* r = GetRecipient(a_refr);
    if (!r || !r->IsPregnant())
      return 0;
    auto& c    = Storage::GetSingleton()->config;
    int triLen = std::max(1, static_cast<int>(std::ceil(c.gestationDays / 3.0f)));
    int d      = static_cast<int>(r->pregnancy->DaysSinceConception(Now()));
    return (d < triLen) ? 1 : (d < triLen * 2) ? 2 : 3;
  }

  Conditions::IComparisonConditionComponent* _comparison = nullptr;
  Conditions::INumericConditionComponent* _value         = nullptr;
};

// ─── FertilityIsInLabor ──────────────────────────────────────────────
class ConditionIsInLabor : public Conditions::CustomCondition
{
public:
  static constexpr std::string_view CONDITION_NAME = "FertilityIsInLabor"sv;
  [[nodiscard]] RE::BSString GetName() const override { return CONDITION_NAME.data(); }
  [[nodiscard]] RE::BSString GetDescription() const override { return "Whether the actor is in labor."; }
  [[nodiscard]] REL::Version GetRequiredVersion() const override { return {1, 0, 0}; }

protected:
  bool EvaluateImpl(RE::TESObjectREFR* a_refr, RE::hkbClipGenerator*, void*) const override
  {
    // 直接查派系 rank == 4 (避免 TickManager 头文件循环依赖)
    auto* actor = a_refr ? a_refr->As<RE::Actor>() : nullptr;
    if (!actor)
      return false;
    auto* fac = RE::TESForm::LookupByID<RE::TESFaction>(0xFE000800);
    return fac && actor->IsInFaction(fac) && actor->GetFactionRank(fac, false) == 4;
  }
};

// ─── FertilityIsInRecovery ───────────────────────────────────────────
class ConditionIsInRecovery : public Conditions::CustomCondition
{
public:
  static constexpr std::string_view CONDITION_NAME = "FertilityIsInRecovery"sv;
  [[nodiscard]] RE::BSString GetName() const override { return CONDITION_NAME.data(); }
  [[nodiscard]] RE::BSString GetDescription() const override { return "Whether the actor is in postpartum recovery."; }
  [[nodiscard]] REL::Version GetRequiredVersion() const override { return {1, 0, 0}; }

protected:
  bool EvaluateImpl(RE::TESObjectREFR* a_refr, RE::hkbClipGenerator*, void*) const override
  {
    auto* r = GetRecipient(a_refr);
    return r && r->InRecovery(Now(), Storage::GetSingleton()->config.recoveryDays);
  }
};

// ─── FertilityCyclePhase ─────────────────────────────────────────────
class ConditionCyclePhase : public Conditions::CustomCondition
{
public:
  static constexpr std::string_view CONDITION_NAME = "FertilityCyclePhase"sv;

  ConditionCyclePhase()
  {
    auto* api   = OAR_API::Conditions::GetAPI();
    _comparison = static_cast<Conditions::IComparisonConditionComponent*>(
        AddComponent(api->GetConditionComponentFactory(Conditions::ConditionComponentType::kComparison), "Comparison"));
    _value = static_cast<Conditions::INumericConditionComponent*>(
        AddComponent(api->GetConditionComponentFactory(Conditions::ConditionComponentType::kNumeric), "Phase",
                     "0=Menstruation 1=Follicular 2=Ovulation 3=Luteal"));
  }

  [[nodiscard]] RE::BSString GetName() const override { return CONDITION_NAME.data(); }
  [[nodiscard]] RE::BSString GetDescription() const override { return "Menstrual cycle phase."; }
  [[nodiscard]] REL::Version GetRequiredVersion() const override { return {1, 0, 0}; }

protected:
  bool EvaluateImpl(RE::TESObjectREFR* a_refr, RE::hkbClipGenerator*, void*) const override
  {
    auto* r = GetRecipient(a_refr);
    return r && _comparison && _value && _comparison->GetComparisonResult(static_cast<float>(r->phase), _value->GetNumericValue(a_refr));
  }

  Conditions::IComparisonConditionComponent* _comparison = nullptr;
  Conditions::INumericConditionComponent* _value         = nullptr;
};

// =====================================================================
//  注册入口 — main.cpp kMessage_PostLoad 中调用
// =====================================================================
inline bool RegisterConditions()
{
  using namespace OAR_API::Conditions;
  bool ok = true;

  auto reg = [&](APIResult res, const char* name) {
    if (res != APIResult::OK && res != APIResult::AlreadyRegistered) {
      logger::warn("[Fertility] OAR condition failed: {}", name);
      ok = false;
    } else {
      logger::info("[Fertility] OAR condition: {}", name);
    }
  };

  reg(AddCustomCondition<ConditionIsPregnant>(), "FertilityIsPregnant");
  reg(AddCustomCondition<ConditionPregnancyProgress>(), "FertilityPregnancyProgress");
  reg(AddCustomCondition<ConditionTrimester>(), "FertilityTrimester");
  reg(AddCustomCondition<ConditionIsInLabor>(), "FertilityIsInLabor");
  reg(AddCustomCondition<ConditionIsInRecovery>(), "FertilityIsInRecovery");
  reg(AddCustomCondition<ConditionCyclePhase>(), "FertilityCyclePhase");

  return ok;
}

}  // namespace Fertility::OAR
