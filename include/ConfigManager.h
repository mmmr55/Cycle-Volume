#pragma once
// =====================================================================
//  ConfigManager.h — JSON 驱动的运行时配置
//
//  来源: Data/SKSE/Plugins/Cycle&Volume.json
//  SKSE Menu 框架直接读写此 JSON
//  DLL 侧通过 ConfigManager::Initialize / Reload 加载到这些变量
//  所有模块直接读 Fertility::xxx
//
//  ★ 修订说明:
//    - 用 X-macro (CONFIG_FIELDS) 消除 ~400 行重复的读写样板
//    - 补齐 TickManager 引用但缺失的 8 个阶段时长变量
//      (menstruationDaysMin/Max, follicularDaysMin/Max, 等)
//    - LoadFromDisk 后执行 ValidateConfig() 钳位危险值
//    - 修正锁语义: _mutex 保护 磁盘 I/O 的序列化,
//      inline 全局变量的线程安全由"只在主线程加载"的约定保证
//    - SaveToDisk: 去掉多余的 shared_lock (全局变量不受它保护)
// =====================================================================
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <Fertilitytypes.h>
#include <Fertilitystorage.h>
#include <nlohmann/json.hpp>
#include <RaceOverrides.h>

namespace Fertility
{

// =========================================================================
//  X-macro 配置表
//
//  格式: X(type, "section", variableName, defaultValue)
//  type 只允许 float / int / bool
//  特殊类型 (RaceInheritance, FormID) 在表外单独处理
//
//  新增/删除配置项只需修改这张表, 读/写/默认值三处自动同步
// =========================================================================

// clang-format off
#define CONFIG_FIELDS(X)                                                              \
  /* ── 周期 ── */                                                                     \
  X(float, "cycle", cycleDuration,         28.0f)                                     \
  X(int,   "cycle", menstruationBegin,     0)                                         \
  X(int,   "cycle", menstruationEnd,       7)                                         \
  X(int,   "cycle", ovulationBegin,        8)                                         \
  X(int,   "cycle", ovulationEnd,          16)                                        \
  /* ── 阶段时长范围 (天) — TickManager::RollPhaseDuration 使用 ── */                   \
  X(float, "cycle", menstruationDaysMin,   3.0f)                                      \
  X(float, "cycle", menstruationDaysMax,   7.0f)                                      \
  X(float, "cycle", follicularDaysMin,     5.0f)                                      \
  X(float, "cycle", follicularDaysMax,     9.0f)                                      \
  X(float, "cycle", ovulationDaysMin,      1.0f)                                      \
  X(float, "cycle", ovulationDaysMax,      3.0f)                                      \
  X(float, "cycle", lutealDaysMin,         10.0f)                                     \
  X(float, "cycle", lutealDaysMax,         14.0f)                                     \
  /* ── 阶段受孕倍率 — TickManager::GetPhaseMultiplier 使用 ── */                  \       
	X(float, "cycle", phaseMultMenstruation, 0.02f)                                     \
  X(float, "cycle", phaseMultFollicular,   0.10f)                                     \
  X(float, "cycle", phaseMultOvulation,    1.00f)                                     \
  X(float, "cycle", phaseMultLuteal,       0.05f)                                       \
  X(float, "cycle", eggLifeDays,           1.0f)                                      \
  X(float, "cycle", defaultSpermLifeHours, 48.0f)                                     \
  X(int,   "cycle", spermLifeDays,         5)                                         \
  X(float, "cycle", spermFullVolume,       100.0f)                                    \
  X(float, "cycle", donorCooldownHours,    1.0f)                                      \
  X(float, "cycle", baseSpermMin,          50.0f)                                     \
  X(float, "cycle", baseSpermMax,          200.0f)                                    \
  X(float, "cycle", baseVolumeMin,         5.0f)                                      \
  X(float, "cycle", baseVolumeMax,         15.0f)                                     \
  X(float, "cycle", volumeDecayHalfLife,   12.0f)                                     \
  /* ── 妊娠 ── */                                                                     \
  X(float, "pregnancy", gestationDays,            30.0f)                              \
  X(float, "pregnancy", recoveryDays,             10.0f)                              \
  X(float, "pregnancy", babyDuration,             5.0f)                               \
  X(int,   "pregnancy", conceptionChance,         80)                                 \
  X(bool,  "pregnancy", pregnancyVarianceEnabled, false)                              \
  X(int,   "pregnancy", pregnancyVariancePercent, 15)                                 \
  X(bool,  "pregnancy", allowVampirePregnancy,    false)                              \
  X(bool,  "pregnancy", allowCreatures,           false)                              \
  /* ── 过滤 ── */                                                                     \
  X(bool,  "filter", uniqueWomenOnly, false)                                          \
  X(bool,  "filter", uniqueMenOnly,   false)                                          \
  X(bool,  "filter", playerOnly,      false)                                          \
  X(int,   "filter", forceGender,     0)                                              \
  /* ── 玩家效果 ── */                                                                  \
  X(float, "effects", pmsStaminaReduction,   0.1f)                                    \
  X(float, "effects", pmsMagickaReduction,   0.1f)                                    \
  X(float, "effects", ovulationStaminaBonus, 0.05f)                                   \
  X(float, "effects", ovulationMagickaBonus, 0.05f)                                   \
  /* ── 体型缩放 ── */                                                                  \
  X(int,   "scaling", scalingMethod,   0)                                             \
  X(float, "scaling", bellyScaleMax,   1.0f)                                          \
  X(float, "scaling", bellyScaleMult,  1.0f)                                          \
  X(float, "scaling", breastScaleMult, 0.5f)                                          \
  /* ── 出生 ── */                                                                     \
  X(int,   "birth", birthType,          2)                                            \
  X(bool,  "birth", miscarriageEnabled, true)                                         \
  X(bool,  "birth", laborEnabled,       true)                                         \
  X(int,   "birth", laborDuration,      30)                                           \
  X(bool,  "birth", spawnEnabled,       true)                                         \
  X(bool,  "birth", adoptionEnabled,    true)                                         \
  X(bool,  "birth", trainingEnabled,    true)                                         \
  X(float, "birth", soundVolume,        1.0f)                                         \
  X(bool,  "birth", babyCombatDamage,   false)                                        \
  /* ── 自动化 ── */                                                                   \
  X(float, "automation", pollingInterval,        1.0f)                                \
  X(bool,  "automation", autoInseminateNpc,      false)                               \
  X(bool,  "automation", autoInseminatePc,       false)                               \
  X(bool,  "automation", autoInseminatePcSleep,  false)                               \
  X(int,   "automation", autoInseminateChance,   5)                                   \
  X(int,   "automation", spouseInseminateChance, 100)                                 \
  X(int,   "automation", cleanupThreshold,       48)                                  \
  /* ── 清理 / 追踪 ── */                                                               \
  X(float, "cleanup", autoCleanupDays,   30.0f)                                      \
  X(int,   "cleanup", maxTrackedMothers, 128)                                         \
  /* ── 训练 ── */                                                                     \
  X(float, "training", trainingDurationDays,  60.0f)                                  \
  X(int,   "training", maxActiveFollowers,    3)                                      \
  X(int,   "training", trainingCost,          2000)                                   \
  X(bool,  "training", allowNonMemberTraining, false)                                 \
  /* ── Widget ── */                                                                   \
  X(bool,  "widget", widgetShown,        true)                                        \
  X(int,   "widget", widgetLeft,         10)                                          \
  X(int,   "widget", widgetTop,          10)                                          \
  X(int,   "widget", widgetDisplayMode,  0)                                           \
  X(float, "widget", widgetTransparency, 100.0f)                                      \
  X(int,   "widget", widgetHotKey,       10)                                          \
  /* ── NSFW 框架 ── */                                                                 \
  X(bool,  "nsfw", enableSexLabSupport, false)                                        \
  X(bool,  "nsfw", enableOStimSupport,  false)                                        \
  X(bool,  "nsfw", enableOStimNPCs,     false)                                        \
  /* ── 法术 ── */                                                                     \
  X(bool,  "spells", inseminateSpellAdded, false)                                     \
  X(bool,  "spells", impregnateSpellAdded, false)                                     \
  X(bool,  "spells", birthSpellAdded,      false)                                     \
  X(bool,  "spells", abortSpellAdded,      false)                                     \
  /* ── 排出系统 ── */                                                                  \
  X(bool,  "expulsion", expulsionEnabled,    true)                                    \
  X(int,   "expulsion", expulsionKey,        42)  /* 42 = Left Shift (DIK code) */    \
  X(float, "expulsion", expulsionHoldSec,    3.0f)                                    \
  X(float, "expulsion", expulsionRatePerSec, 0.10f)  /* 10% per second */             \
  X(bool,  "expulsion", expulsionReequip,    true)  /* 结束后自动穿回 */                \
  X(bool,  "expulsion", expulsionAnimation,  true)  /* 播放蹲下动画 */                  \
  /* ── 调试 ── */                                                                     \
  X(bool,  "debug", verboseMode,      false)                                          \
  X(bool,  "debug", eventMessages,    true)                                           \
  X(bool,  "debug", enabled,          true)                                           \
  X(bool,  "debug", useKeyboardInput, false)
// clang-format on

// =========================================================================
//  运行时配置 — inline 全局变量
//
//  线程安全约定:
//    写入: 仅由 ConfigManager::Initialize / Reload 在主线程执行
//    读取: 任意线程 (Storage tick, Papyrus, etc.)
//    由于写入只发生在主线程且频率极低 (启动/菜单),
//    实际不存在竞态。如果将来需要热加载支持,
//    应改用 atomic<shared_ptr<const ConfigStruct>> 快照模式。
// =========================================================================

// 展开宏: 声明 inline 全局变量
#define DECLARE_CONFIG_VAR(type, section, name, def) inline type name = def;
CONFIG_FIELDS(DECLARE_CONFIG_VAR)
#undef DECLARE_CONFIG_VAR

// 特殊类型 (不能用宏的泛型模板处理)
inline RaceInheritance birthRace    = RaceInheritance::Mother;
inline RE::FormID birthRaceSpecific = 0;

// ─── 便捷 ───
[[nodiscard]] inline int OvulationMidDay()
{
  return ovulationBegin + (ovulationEnd - ovulationBegin) / 2;
}

// =========================================================================
//  ConfigManager — JSON 读写
//
//  SKSE Menu 框架写 JSON → DLL 调 Reload() 热加载
//  无 Papyrus 桥接, 无 ESP GlobalVariable
// =========================================================================

class ConfigManager
{
public:
  static ConfigManager* GetSingleton()
  {
    static ConfigManager instance;
    return &instance;
  }
  ConfigManager(const ConfigManager&)            = delete;
  ConfigManager& operator=(const ConfigManager&) = delete;

  void Initialize()
  {
    _configPath = GetConfigPath();
    if (std::filesystem::exists(_configPath))
      LoadFromDisk();
    else
      SaveToDisk();
    logger::info("[Fertility] Config loaded from {}", _configPath.string());
  }

  void Reload()
  {
    LoadFromDisk();
    logger::info("[Fertility] Config reloaded");
  }

  void SaveToDisk() const
  {
    // _mutex 序列化磁盘 I/O, 不保护 inline 全局变量
    // (全局变量只在主线程写入, 见上方约定)
    std::lock_guard lk(_ioMtx);
    try {
      nlohmann::json j = BuildJson();
      std::filesystem::create_directories(_configPath.parent_path());
      std::ofstream ofs(_configPath);
      ofs << j.dump(4);
    } catch (const std::exception& e) {
      logger::error("[Fertility] Config save error: {}", e.what());
    }
  }

private:
  ConfigManager()  = default;
  ~ConfigManager() = default;

  mutable std::mutex _ioMtx;  // 仅序列化磁盘读写, 不保护全局变量
  std::filesystem::path _configPath;

  static std::filesystem::path GetConfigPath() { return std::filesystem::path("Data/SKSE/Plugins/Cycle&Volume.json"); }

  // ─────────────────────────────────────────────────────────────────
  //  类型安全的 JSON 读取辅助
  // ─────────────────────────────────────────────────────────────────
  template <typename T>
  static T SafeGet(const nlohmann::json& j, const char* section, const char* key, T defaultVal)
  {
    try {
      return j.at(section).at(key).get<T>();
    } catch (...) {
      return defaultVal;
    }
  }

  // ─────────────────────────────────────────────────────────────────
  //  JSON → 全局变量
  // ─────────────────────────────────────────────────────────────────
  static void ApplyJson(const nlohmann::json& j)
  {
// 用 X-macro 一次性读取所有标准字段
#define READ_FIELD(type, section, name, def) name = SafeGet<type>(j, section, #name, def);
    CONFIG_FIELDS(READ_FIELD)
#undef READ_FIELD

    // 特殊类型: 需要 static_cast
    birthRace         = static_cast<RaceInheritance>(SafeGet<int>(j, "pregnancy", "birthRace", 0));
    birthRaceSpecific = static_cast<RE::FormID>(SafeGet<int>(j, "pregnancy", "birthRaceSpecific", 0));
  }

  // ─────────────────────────────────────────────────────────────────
  //  全局变量 → JSON
  // ─────────────────────────────────────────────────────────────────
  static nlohmann::json BuildJson()
  {
    nlohmann::json j;

// 用 X-macro 一次性写出所有标准字段
#define WRITE_FIELD(type, section, name, def) j[section][#name] = name;
    CONFIG_FIELDS(WRITE_FIELD)
#undef WRITE_FIELD

    // 特殊类型
    j["pregnancy"]["birthRace"]         = static_cast<int>(birthRace);
    j["pregnancy"]["birthRaceSpecific"] = static_cast<int>(birthRaceSpecific);

    return j;
  }

  // ─────────────────────────────────────────────────────────────────
  //  验证: 钳位危险值, 修正逻辑矛盾
  // ─────────────────────────────────────────────────────────────────
  static void ValidateConfig()
  {
    // 不允许为 0 或负数的时长
    gestationDays       = std::max(1.0f, gestationDays);
    recoveryDays        = std::max(0.1f, recoveryDays);
    cycleDuration       = std::max(1.0f, cycleDuration);
    eggLifeDays         = std::max(0.01f, eggLifeDays);
    volumeDecayHalfLife = std::max(0.1f, volumeDecayHalfLife);
    babyDuration        = std::max(0.0f, babyDuration);
    autoCleanupDays     = std::max(1.0f, autoCleanupDays);

    // 精子量范围: min ≤ max
    if (baseSpermMin > baseSpermMax)
      std::swap(baseSpermMin, baseSpermMax);
    if (baseVolumeMin > baseVolumeMax)
      std::swap(baseVolumeMin, baseVolumeMax);

    // 阶段时长范围: min ≤ max, 且 min > 0
    auto fixRange = [](float& lo, float& hi, float minVal = 0.5f) {
      lo = std::max(minVal, lo);
      hi = std::max(lo, hi);
    };
    fixRange(menstruationDaysMin, menstruationDaysMax);
    fixRange(follicularDaysMin, follicularDaysMax);
    fixRange(ovulationDaysMin, ovulationDaysMax);
    fixRange(lutealDaysMin, lutealDaysMax);

    // 概率百分比钳位
    conceptionChance         = std::clamp(conceptionChance, 0, 100);
    autoInseminateChance     = std::clamp(autoInseminateChance, 0, 100);
    spouseInseminateChance   = std::clamp(spouseInseminateChance, 0, 100);
    pregnancyVariancePercent = std::clamp(pregnancyVariancePercent, 0, 50);

    // 追踪上限 (0 = 无限, 但不允许负数)
    maxTrackedMothers  = std::max(0, maxTrackedMothers);
    maxActiveFollowers = std::max(1, maxActiveFollowers);

    // 音量
    soundVolume = std::clamp(soundVolume, 0.0f, 1.0f);

    // 排卵区间: begin ≤ end
    if (ovulationBegin > ovulationEnd)
      std::swap(ovulationBegin, ovulationEnd);
    if (menstruationBegin > menstruationEnd)
      std::swap(menstruationBegin, menstruationEnd);
  }

  // ─────────────────────────────────────────────────────────────────
  //  磁盘加载
  // ─────────────────────────────────────────────────────────────────
  void LoadFromDisk()
  {
    std::lock_guard lk(_ioMtx);
    try {
      std::ifstream ifs(_configPath);
      nlohmann::json j = nlohmann::json::parse(ifs);
      ApplyJson(j);
      ValidateConfig();
    } catch (const std::exception& e) {
      logger::warn("[Fertility] Config parse error, using defaults: {}", e.what());
    }
  }
};

}  // namespace Fertility
