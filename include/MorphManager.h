#pragma once
// =====================================================================
//  MorphManager.h
//
//  一个膨胀效果 = 一个滑条:
//    BellyPregnancy — 孕肚 (progress × bellyScaleMult)
//    Belly          — 精液体积 (0 ~ 1.5)
//    Breasts        — 乳房 (progress × breastScaleMult)
//
//  钳位由 LIF NG 内部处理, MorphManager 只负责缩放
//
//  配置来源: Fertility::bellyScaleMult 等 inline 全局变量 (JSON)
//  SLIF NG 接管: 持久化 / 3D 重载 / 立即应用
//
//  ★ 修订说明:
//    - API 指针改用 atomic, 消除 Initialize/IsAPIAvailable 竞争
//    - 删除 SafeGetActor / CanApplyMorph 死代码 (MorphQueue 有独立副本)
//    - 新增 breast morph 支持 (breastScaleMult 配置项终于生效)
//    - UpdatePregnancyMorph 增加 breast morph (钳位由 LIF NG 内部处理)
//    - Revert 清空 MorphQueue
// =====================================================================


#include "InflationFrameworkAPI.h"
#include "Fertilitytypes.h"
#include <RaceOverrides.h>
#include <atomic>

namespace Fertility
{

class MorphManager
{
public:
  static MorphManager* GetSingleton()
  {
    static MorphManager instance;
    return &instance;
  }
  MorphManager(const MorphManager&)            = delete;
  MorphManager& operator=(const MorphManager&) = delete;

  void Initialize();
  bool IsAPIAvailable() const;

  /// 孕肚 + 乳房, progress 0~1
  void UpdatePregnancyMorph(RE::FormID motherID, float progress);

  /// 精液膨胀 → Belly, normalized 0~1.5
  void UpdateSpermInflation(RE::FormID actorID, float normalized);

  /// 归零所有 morph 滑条
  void ClearMorphs(RE::FormID motherID);

  /// 存档切换时清空队列残留
  void Revert();

private:
  MorphManager()  = default;
  ~MorphManager() = default;

  // 线程安全的 API 指针 (Initialize 写, IsAPIAvailable/其他线程读)
  static std::atomic<InflationFrameworkAPI::IInflationFrameworkInterface*> _api;
};

}  // namespace Fertility
