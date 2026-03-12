#pragma once
// =====================================================================
//  SpermExpulsion.h — 按键长按排出系统
//
//  状态机: Idle → Holding (≥N秒) → Expelling (每秒10%) → Idle
//
//  两层 DD 兼容 (由 RaceOverrides 驱动):
//    Layer 1 — BlockingKeywords: 穿着 DD 贞操带/塞子等
//              → IsExpulsionBlocked() → 拒绝排出, 显示通知
//    Layer 2 — NoStripKeywords: 脱衣时跳过 SexLabNoStrip 标记物品
//              → HasNoStripKeyword() → 不脱该物品
//
//  全部关键词通过 EditorID 字符串在 INI 配置, 运行时解析。
//  线程: 纯主线程 (输入事件 + SKSE task)
// =====================================================================

#include "FertilityStorage.h"
#include "RaceOverrides.h"
#include "Fertilitytypes.h"

#include <RaceOverrides.h>
#include <chrono>
#include <vector>

namespace Fertility
{

class SpermExpulsion : public RE::BSTEventSink<RE::InputEvent*>
{
public:
  static SpermExpulsion* GetSingleton()
  {
    static SpermExpulsion instance;
    return &instance;
  }
  SpermExpulsion(const SpermExpulsion&)            = delete;
  SpermExpulsion& operator=(const SpermExpulsion&) = delete;

  void Initialize();
  void Update();
  bool IsExpelling() const { return _expelling; }

protected:
  RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* events, RE::BSTEventSource<RE::InputEvent*>*) override;

private:
  SpermExpulsion() = default;

  using Clock = std::chrono::steady_clock;

  // ── 状态 ──
  bool _keyHeld   = false;
  bool _expelling = false;
  Clock::time_point _holdStart;
  Clock::time_point _lastExpelTick;
  float _initialTotal = 0.0f;

  // ── 脱除记录 ──
  struct StrippedItem
  {
    RE::TESObjectARMO* armor = nullptr;
  };
  std::vector<StrippedItem> _strippedItems;

  // ── 方法 ──
  void BeginExpulsion(RE::Actor* player);
  void TickExpulsion(RE::Actor* player);
  void EndExpulsion(RE::Actor* player);

  void StripArmor(RE::Actor* player);
  void ReequipArmor(RE::Actor* player);

  float GetTotalSperm(RE::FormID id) const;
  static void ScheduleFrame();
};

}  // namespace Fertility
