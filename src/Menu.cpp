// =====================================================================
//  Menu.cpp — SKSEMenuFramework 集成
//
//  两条数据路线:
//    A. 配置 (Config route)  — 读写 Fertility:: inline 全局变量
//       用户修改 → 内存立即生效 → 关闭菜单时 SaveToDisk()
//       数据来源: ConfigManager.h 的 inline 变量
//
//    B. 状态 (Status route)  — 只读 co-save 运行时数据
//       来源: Storage::GetRecipient / GetPregnancy / GetChild 等
//       这些数据随游戏进行动态变化, 不写回 JSON
//
//  架构:
//    Settings    — 全局开关 + 调试 (Config)
//    Cycle       — 周期参数 (Config)
//    Pregnancy   — 妊娠参数 (Config)
//    Birth       — 分娩/出生参数 (Config)
//    Scaling     — 体型缩放 (Config)
//    Automation  — 自动化 (Config)
//    Tracking    — 当前目标状态 (Status, co-save)
//    Children    — 子代列表 (Status, co-save)
//    Debug       — 调试信息 (混合)
// =====================================================================

#include "Menu.h"

#include "ConfigManager.h"
#include "FertilityStorage.h"
#include "SpermExpulsion.h"
#include "TickManager.h"

#include "magic_enum/magic_enum.hpp"
#include "nlohmann/json.hpp"

namespace Fertility
{

// 便于直接访问 Fertility:: inline 全局变量
using namespace Fertility;

// =====================================================================
//  哈希 + 字面量
// =====================================================================

constexpr std::uint32_t hash(const char* data, size_t const size) noexcept
{
  uint32_t hash = 'CycleAndVolume';
  for (const char* c = data; c < data + size; ++c)
    hash = ((hash << 5) + hash) + (unsigned char)*c;
  return hash;
}

constexpr std::uint32_t operator""_h(const char* str, size_t size) noexcept
{
  return hash(str, size);
}

// =====================================================================
//  本地化
// =====================================================================

namespace Localization
{
  struct StringMap
  {
    std::string label;
    std::string desc;
  };

  static std::unordered_map<std::uint32_t, StringMap> stringMaps;
  static std::unordered_map<std::uint32_t, std::vector<StringMap>> comboMaps;

  void LoadLocalization()
  {
    constexpr std::string_view path = "Data/SKSE/Plugins/Cycle&Volume_Localization.json";

    nlohmann::json j;
    try {
      std::ifstream ifs(path.data());
      j = nlohmann::json::parse(ifs);
    } catch (const std::exception& e) {
      logger::error("[Cycle&Volume] Localization load failed: {}", e.what());
      return;
    }

    if (!j.is_object()) {
      logger::error("[Cycle&Volume] Localization file is not a valid JSON object");
      return;
    }

    stringMaps.reserve(j.size());
    for (const auto& [key, value] : j.items()) {
      if (!value.is_object())
        continue;
      stringMaps[hash(key.data(), key.size())] = {
          value.value("label", ""),
          value.value("desc", ""),
      };
    }

    // 构建枚举下拉列表映射
    auto buildComboMap = []<typename E>() {
      const auto& enumName   = magic_enum::enum_type_name<E>();
      const auto& valueNames = magic_enum::enum_names<E>();
      std::vector<StringMap> maps;
      maps.reserve(valueNames.size());
      for (const auto& name : valueNames) {
        auto it = stringMaps.find(hash(name.data(), name.size()));
        if (it != stringMaps.end())
          maps.push_back(it->second);
        else
          maps.push_back({std::string(name), ""});
      }
      comboMaps[hash(enumName.data(), enumName.size())] = std::move(maps);
    };

    buildComboMap.operator()<CyclePhase>();
    buildComboMap.operator()<RaceInheritance>();
    buildComboMap.operator()<TrainingStatus>();
  }
}  // namespace Localization

// =====================================================================
//  ImGui 包装 (与 SKSEMenuFramework 对接)
// =====================================================================

namespace ImGui
{
  using StringMap = Localization::StringMap;

  inline const StringMap& GetStringMap(std::uint32_t h)
  {
    static const StringMap unknown = {"???", ""};
    auto it                        = Localization::stringMaps.find(h);
    return (it != Localization::stringMaps.end()) ? it->second : unknown;
  }

  void SetSection(std::uint32_t h)
  {
    SKSEMenuFramework::SetSection(GetStringMap(h).label.data());
  }

  void AddSectionItem(std::uint32_t h, SKSEMenuFramework::Model::RenderFunction func)
  {
    SKSEMenuFramework::AddSectionItem(GetStringMap(h).label.data(), func);
  }

  void Checkbox(std::uint32_t h, bool* v, std::function<void()> onChange = nullptr)
  {
    const auto& map = GetStringMap(h);
    if (ImGuiMCP::Checkbox(map.label.data(), v))
      if (onChange)
        onChange();
    if (ImGuiMCP::IsItemHovered() && !map.desc.empty())
      ImGuiMCP::SetTooltip(map.desc.data());
  }

  void DragInt(std::uint32_t h, int* v, float speed = 1.0f, int vmin = 0, int vmax = 0, const char* fmt = "%d",
               std::function<void()> onChange = nullptr)
  {
    const auto& map = GetStringMap(h);
    if (ImGuiMCP::DragInt(map.label.data(), v, speed, vmin, vmax, fmt, ImGuiMCP::ImGuiSliderFlags_None))
      if (onChange)
        onChange();
    if (ImGuiMCP::IsItemHovered() && !map.desc.empty())
      ImGuiMCP::SetTooltip(map.desc.data());
  }

  void DragFloat(std::uint32_t h, float* v, float speed = 1.0f, float vmin = 0.0f, float vmax = 0.0f, const char* fmt = "%.1f",
                 std::function<void()> onChange = nullptr)
  {
    const auto& map = GetStringMap(h);
    if (ImGuiMCP::DragFloat(map.label.data(), v, speed, vmin, vmax, fmt, ImGuiMCP::ImGuiSliderFlags_None))
      if (onChange)
        onChange();
    if (ImGuiMCP::IsItemHovered() && !map.desc.empty())
      ImGuiMCP::SetTooltip(map.desc.data());
  }

  template <typename T>
  void Combo(std::uint32_t h, T* current, std::function<void()> onChange = nullptr)
  {
    const auto& map = GetStringMap(h);
    auto comboIt    = Localization::comboMaps.find(h);
    if (comboIt == Localization::comboMaps.end())
      return;

    auto& items = comboIt->second;
    int idx     = static_cast<int>(*current);
    if (idx < 0 || idx >= static_cast<int>(items.size()))
      idx = 0;

    if (ImGuiMCP::BeginCombo(map.label.data(), items[idx].label.data())) {
      for (size_t n = 0; n < items.size(); ++n) {
        bool selected = (idx == static_cast<int>(n));
        if (ImGuiMCP::Selectable(items[n].label.data(), selected)) {
          *current = static_cast<T>(n);
          if (onChange)
            onChange();
        }
        if (ImGuiMCP::IsItemHovered() && !items[n].desc.empty())
          ImGuiMCP::SetTooltip(items[n].desc.data());
        if (selected)
          ImGuiMCP::SetItemDefaultFocus();
      }
      ImGuiMCP::EndCombo();
    }
  }

  // 只读文本 + 可选 tooltip
  void TextInfo(const char* label, const std::string& value)
  {
    ImGuiMCP::Text(std::format("{}: {}", label, value).data());
  }

  void Separator()
  {
    ImGuiMCP::Separator();
  }

}  // namespace ImGui

// =====================================================================
//  辅助: 获取十字准心目标 或 玩家
// =====================================================================

static RE::Actor* GetTargetActor()
{
  if (auto* cross = RE::CrosshairPickData::GetSingleton()) {
    auto ref = cross->target[0].get();
    if (ref) {
      if (auto* actor = ref.get()->As<RE::Actor>())
        return actor;
    }
  }
  return RE::PlayerCharacter::GetSingleton();
}

// =====================================================================
//  ██ Config Route: 设置页面 ██
// =====================================================================

void Menu::Settings()
{
  // ── 全局 ──
  ImGui::Checkbox("enabled"_h, &enabled);

  ImGui::Separator();

  // ── 过滤 ──
  ImGui::Checkbox("uniqueWomenOnly"_h, &uniqueWomenOnly);
  ImGui::Checkbox("uniqueMenOnly"_h, &uniqueMenOnly);
  ImGui::Checkbox("playerOnly"_h, &playerOnly);
  ImGui::DragInt("forceGender"_h, &forceGender, 1.0f, 0, 2);

  ImGui::Separator();

  // ── NSFW 框架 ──
  ImGui::Checkbox("enableSexLabSupport"_h, &enableSexLabSupport);
  ImGui::Checkbox("enableOStimSupport"_h, &enableOStimSupport);
  ImGui::Checkbox("enableOStimNPCs"_h, &enableOStimNPCs);
}

void Menu::Cycle()
{
  // ── 基本周期 ──
  ImGui::DragFloat("cycleDuration"_h, &cycleDuration, 0.5f, 1.0f, 120.0f);
  ImGui::DragInt("menstruationBegin"_h, &menstruationBegin, 1.0f, 0, 60);
  ImGui::DragInt("menstruationEnd"_h, &menstruationEnd, 1.0f, 0, 60);
  ImGui::DragInt("ovulationBegin"_h, &ovulationBegin, 1.0f, 0, 60);
  ImGui::DragInt("ovulationEnd"_h, &ovulationEnd, 1.0f, 0, 60);

  ImGui::Separator();

  // ── 阶段时长范围 (状态机) ──
  ImGui::DragFloat("menstruationDaysMin"_h, &menstruationDaysMin, 0.5f, 0.5f, 30.0f);
  ImGui::DragFloat("menstruationDaysMax"_h, &menstruationDaysMax, 0.5f, 0.5f, 30.0f);
  ImGui::DragFloat("follicularDaysMin"_h, &follicularDaysMin, 0.5f, 0.5f, 30.0f);
  ImGui::DragFloat("follicularDaysMax"_h, &follicularDaysMax, 0.5f, 0.5f, 30.0f);
  ImGui::DragFloat("ovulationDaysMin"_h, &ovulationDaysMin, 0.5f, 0.5f, 30.0f);
  ImGui::DragFloat("ovulationDaysMax"_h, &ovulationDaysMax, 0.5f, 0.5f, 30.0f);
  ImGui::DragFloat("lutealDaysMin"_h, &lutealDaysMin, 0.5f, 0.5f, 30.0f);
  ImGui::DragFloat("lutealDaysMax"_h, &lutealDaysMax, 0.5f, 0.5f, 30.0f);

  ImGui::Separator();

  // ── 卵子 / 精子 ──
  ImGui::DragFloat("eggLifeDays"_h, &eggLifeDays, 0.1f, 0.01f, 10.0f);
  ImGui::DragFloat("defaultSpermLifeHours"_h, &defaultSpermLifeHours, 1.0f, 1.0f, 240.0f);
  ImGui::DragFloat("donorCooldownHours"_h, &donorCooldownHours, 0.1f, 0.0f, 24.0f);
  ImGui::DragFloat("baseSpermMin"_h, &baseSpermMin, 1.0f, 1.0f, 1000.0f);
  ImGui::DragFloat("baseSpermMax"_h, &baseSpermMax, 1.0f, 1.0f, 1000.0f);
  ImGui::DragFloat("baseVolumeMin"_h, &baseVolumeMin, 0.5f, 0.0f, 100.0f);
  ImGui::DragFloat("baseVolumeMax"_h, &baseVolumeMax, 0.5f, 0.0f, 100.0f);
  ImGui::DragFloat("volumeDecayHalfLife"_h, &volumeDecayHalfLife, 0.5f, 0.1f, 100.0f);
  ImGui::DragFloat("spermFullVolume"_h, &spermFullVolume, 1.0f, 1.0f, 500.0f);
}

void Menu::Pregnancy()
{
  ImGui::DragFloat("gestationDays"_h, &gestationDays, 0.5f, 1.0f, 365.0f);
  ImGui::DragFloat("recoveryDays"_h, &recoveryDays, 0.5f, 0.1f, 60.0f);
  ImGui::DragFloat("babyDuration"_h, &babyDuration, 0.5f, 0.0f, 30.0f);
  ImGui::DragInt("conceptionChance"_h, &conceptionChance, 1.0f, 0, 100, "%d%%");
  ImGui::Checkbox("pregnancyVarianceEnabled"_h, &pregnancyVarianceEnabled);
  ImGui::DragInt("pregnancyVariancePercent"_h, &pregnancyVariancePercent, 1.0f, 0, 50, "%d%%");

  ImGui::Separator();

  // 种族
  auto birthRaceInt = static_cast<int>(birthRace);
  ImGui::Combo<int>("RaceInheritance"_h, &birthRaceInt, [&]() {
    birthRace = static_cast<RaceInheritance>(birthRaceInt);
  });
  ImGui::Checkbox("allowVampirePregnancy"_h, &allowVampirePregnancy);
  ImGui::Checkbox("allowCreatures"_h, &allowCreatures);
}

void Menu::Birth()
{
  ImGui::DragInt("birthType"_h, &birthType, 1.0f, 0, 3);
  ImGui::Checkbox("miscarriageEnabled"_h, &miscarriageEnabled);
  ImGui::Checkbox("laborEnabled"_h, &laborEnabled);
  ImGui::DragInt("laborDuration"_h, &laborDuration, 1.0f, 5, 120, "%d sec");
  ImGui::DragFloat("soundVolume"_h, &soundVolume, 0.01f, 0.0f, 1.0f, "%.2f");
  ImGui::Checkbox("babyCombatDamage"_h, &babyCombatDamage);

  ImGui::Separator();

  ImGui::Checkbox("spawnEnabled"_h, &spawnEnabled);
  ImGui::Checkbox("adoptionEnabled"_h, &adoptionEnabled);
  ImGui::Checkbox("trainingEnabled"_h, &trainingEnabled);
}

void Menu::Expulsion()
{
  ImGui::Checkbox("expulsionEnabled"_h, &expulsionEnabled);
  ImGui::DragInt("expulsionKey"_h, &expulsionKey, 1.0f, 1, 255);
  ImGuiMCP::Text("(DirectInput scancode: 42=LShift, 29=LCtrl, 47=V)");
  ImGui::DragFloat("expulsionHoldSec"_h, &expulsionHoldSec, 0.5f, 1.0f, 10.0f, "%.1f sec");
  ImGui::DragFloat("expulsionRatePerSec"_h, &expulsionRatePerSec, 0.01f, 0.01f, 1.0f, "%.2f");
  ImGuiMCP::Text("(0.10 = 10%% per second, empties in ~10s)");
  ImGui::Checkbox("expulsionReequip"_h, &expulsionReequip);
  ImGui::Checkbox("expulsionAnimation"_h, &expulsionAnimation);

  ImGui::Separator();
  ImGuiMCP::Text("Strip slots & race overrides: see CycleVolume_Advanced.ini");
}

void Menu::Scaling()
{
  ImGui::DragInt("scalingMethod"_h, &scalingMethod, 1.0f, 0, 2);
  ImGui::DragFloat("bellyScaleMax"_h, &bellyScaleMax, 0.05f, 0.0f, 5.0f, "%.2f");
  ImGui::DragFloat("bellyScaleMult"_h, &bellyScaleMult, 0.05f, 0.0f, 5.0f, "%.2f");
  ImGui::DragFloat("breastScaleMult"_h, &breastScaleMult, 0.05f, 0.0f, 5.0f, "%.2f");
}

void Menu::Automation()
{
  ImGui::DragFloat("pollingInterval"_h, &pollingInterval, 0.1f, 0.1f, 10.0f, "%.1f sec");
  ImGui::Checkbox("autoInseminateNpc"_h, &autoInseminateNpc);
  ImGui::Checkbox("autoInseminatePc"_h, &autoInseminatePc);
  ImGui::Checkbox("autoInseminatePcSleep"_h, &autoInseminatePcSleep);
  ImGui::DragInt("autoInseminateChance"_h, &autoInseminateChance, 1.0f, 0, 100, "%d%%");
  ImGui::DragInt("spouseInseminateChance"_h, &spouseInseminateChance, 1.0f, 0, 100, "%d%%");

  ImGui::Separator();

  ImGui::DragFloat("autoCleanupDays"_h, &autoCleanupDays, 1.0f, 1.0f, 365.0f);
  ImGui::DragInt("maxTrackedMothers"_h, &maxTrackedMothers, 1.0f, 0, 9999);

  ImGui::Separator();

  ImGui::DragFloat("trainingDurationDays"_h, &trainingDurationDays, 1.0f, 1.0f, 365.0f);
  ImGui::DragInt("maxActiveFollowers"_h, &maxActiveFollowers, 1.0f, 1, 10);
  ImGui::DragInt("trainingCost"_h, &trainingCost, 100.0f, 0, 50000);
  ImGui::Checkbox("allowNonMemberTraining"_h, &allowNonMemberTraining);
}

// =====================================================================
//  ██ Status Route: 追踪页面 (co-save 只读) ██
// =====================================================================

void Menu::Tracking()
{
  auto* target = GetTargetActor();
  if (!target) {
    ImGuiMCP::Text("No target");
    return;
  }

  ImGuiMCP::Text(std::format("Target: {} ({:08X})", target->GetDisplayFullName(), target->GetFormID()).data());

  auto* storage = Storage::GetSingleton();
  auto id       = target->GetFormID();

  // ── 资格 ──
  auto elig = storage->DiagnoseEligibility(target);
  ImGui::TextInfo("Eligible", elig.eligible ? "Yes" : "No");
  ImGui::TextInfo("Reason", std::string(magic_enum::enum_name(elig.reason)));

  if (!storage->IsRecipientTracked(id)) {
    ImGuiMCP::Text("(Not tracked as recipient)");
    return;
  }

  auto r = storage->GetRecipient(id);
  if (!r) {
    ImGuiMCP::Text("(Recipient data unavailable)");
    return;
  }

  auto* cal = RE::Calendar::GetSingleton();
  float now = cal ? cal->GetCurrentGameTime() : 0.0f;

  ImGui::Separator();

  // ── 周期 ──
  ImGui::TextInfo("Phase", std::string(magic_enum::enum_name(r->phase)));
  ImGui::TextInfo("Phase Start", std::format("{:.1f}", r->phaseStartTime));
  ImGui::TextInfo("Phase Duration", std::format("{:.1f}d", r->phaseDurationDays));

  if (r->phase == CyclePhase::Ovulation) {
    float phaseAge = now - r->phaseStartTime;
    ImGui::TextInfo("Ovulation Day", std::format("{:.1f}d / {:.1f}d", phaseAge, r->phaseDurationDays));
  }

  ImGui::Separator();

  // ── 精子 ──
  ImGui::TextInfo("Sperm Deposits", std::format("{}", r->sperm.size()));
  if (!r->sperm.empty()) {
    float totalViable = r->TotalViableSperm(now);
    ImGui::TextInfo("Viable Sperm", std::format("{:.0f}", totalViable));
    ImGui::TextInfo("Competition", storage->GetCompetitionSummary(id));
    ImGui::TextInfo("Unique Donors", std::format("{}", storage->GetUniqueDonorCount(id)));
  }

  float vol = r->GetCurrentVolume(now);
  if (vol > 0.01f)
    ImGui::TextInfo("Volume", std::format("{:.1f} mL", vol));

  ImGui::Separator();

  // ── 怀孕 ──
  if (r->IsPregnant() && r->pregnancy.has_value()) {
    auto& p    = *r->pregnancy;
    float gd   = std::max(1.0f, gestationDays);
    float days = p.DaysSinceConception(now);

    ImGui::TextInfo("Pregnant", "Yes");
    ImGui::TextInfo("Father", p.fatherName.empty() ? "Unknown" : p.fatherName);
    ImGui::TextInfo("Days", std::format("{:.1f} / {:.0f}", days, gd));
    ImGui::TextInfo("Progress", std::format("{:.1f}%", std::clamp(days / gd, 0.0f, 1.0f) * 100.0f));

    if (p.creatureFather)
      ImGui::TextInfo("Type", "Creature");

    auto* tickMgr = TickManager::GetSingleton();
    if (tickMgr->IsInLabor(id))
      ImGui::TextInfo("Labor", "IN PROGRESS");

  } else if (r->InRecovery(now, recoveryDays)) {
    ImGui::TextInfo("Pregnant", "No (Recovery)");
    if (r->pregnancy.has_value() && r->pregnancy->birthTime > 0.0f) {
      float recDays = now - r->pregnancy->birthTime;
      ImGui::TextInfo("Recovery", std::format("{:.1f} / {:.0f}d", recDays, recoveryDays));
    }
  } else {
    ImGui::TextInfo("Pregnant", "No");
  }

  // ── 排出状态 ──
  if (target->IsPlayerRef() && SpermExpulsion::GetSingleton()->IsExpelling())
    ImGui::TextInfo("Expulsion", "IN PROGRESS");

  ImGui::Separator();

  // ── 历史 ──
  ImGui::TextInfo("Total Inseminations", std::format("{}", storage->GetInseminationCount(id)));
  ImGui::TextInfo("Last Update", std::format("{:.1f}", r->lastUpdateTime));
  if (!r->lastLocation.empty())
    ImGui::TextInfo("Location", r->lastLocation);
}

// =====================================================================
//  ██ Status Route: 子代页面 (co-save 只读) ██
// =====================================================================

void Menu::Children()
{
  auto* storage  = Storage::GetSingleton();
  int childCount = storage->ChildCount();

  ImGuiMCP::Text(std::format("Children: {}  |  Followers: {} / {}", childCount, storage->FollowerCount(), maxActiveFollowers).data());

  ImGui::Separator();

  for (int i = 0; i < childCount; ++i) {
    auto child = storage->GetChild(i);
    if (!child)
      continue;

    std::string label = std::format("[{}] {} ({:08X})", i, child->name, child->actorID);

    bool following = storage->IsFollowing(i);
    if (following)
      label += " [Following]";

    ImGuiMCP::Text(label.data());
  }

  if (childCount == 0)
    ImGuiMCP::Text("(No children yet)");
}

// =====================================================================
//  ██ Debug (混合: Config + Status) ██
// =====================================================================

void Menu::Debug()
{
  // Config
  ImGui::Checkbox("verboseMode"_h, &verboseMode);
  ImGui::Checkbox("eventMessages"_h, &eventMessages);
  ImGui::Checkbox("useKeyboardInput"_h, &useKeyboardInput);

  ImGui::Separator();

  // Status
  auto* storage = Storage::GetSingleton();
  auto* tickMgr = TickManager::GetSingleton();

  ImGui::TextInfo("Recipients", std::format("{}", storage->RecipientCount()));
  ImGui::TextInfo("Donors", std::format("{}", storage->DonorCount()));
  ImGui::TextInfo("Children", std::format("{}", storage->ChildCount()));
  
  float gameTime = 0.0f;
  if (auto* cal = RE::Calendar::GetSingleton())
    gameTime = cal->GetCurrentGameTime();
  ImGui::TextInfo("Game Time", std::format("{:.2f}", gameTime));
}

// =====================================================================
//  事件监听: 关闭菜单 → 保存配置
// =====================================================================

void __stdcall Menu::EventListener(SKSEMenuFramework::Model::EventType eventType)
{
  switch (eventType) {
  case SKSEMenuFramework::Model::EventType::kOpenMenu:
    break;
  case SKSEMenuFramework::Model::EventType::kCloseMenu:
    ConfigManager::GetSingleton()->SaveToDisk();
    break;
  }
}

// =====================================================================
//  构造: 注册所有菜单页
// =====================================================================

Menu::Menu()
{
  Localization::LoadLocalization();

  if (!SKSEMenuFramework::IsInstalled())
    return;

  SKSEMenuFramework::SetSection("CycleAndVolume");

  ImGui::AddSectionItem("Settings"_h, Settings);
  ImGui::AddSectionItem("Cycle"_h, Cycle);
  ImGui::AddSectionItem("Pregnancy"_h, Pregnancy);
  ImGui::AddSectionItem("Birth"_h, Birth);
  ImGui::AddSectionItem("Expulsion"_h, Expulsion);
  ImGui::AddSectionItem("Scaling"_h, Scaling);
  ImGui::AddSectionItem("Automation"_h, Automation);
  ImGui::AddSectionItem("Tracking"_h, Tracking);
  ImGui::AddSectionItem("Children"_h, Children);
  ImGui::AddSectionItem("Debug"_h, Debug);

  event = new SKSEMenuFramework::Model::Event(EventListener, static_cast<float>("Cycle&Volume"_h));

  logger::info("[Cycle&Volume] Menu: SKSEMenuFramework v{} loaded", SKSEMenuFramework::GetMenuFrameworkVersion());
}

}  // namespace Fertility
