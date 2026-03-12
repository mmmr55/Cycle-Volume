// =====================================================================
// =====================================================================

#include "MorphManager.h"
#include "ConfigManager.h"
#include "MorphQueue.h"

#include <algorithm>

namespace Fertility
{

namespace IFAPI = InflationFrameworkAPI;
using MType     = IFAPI::MorphType;

// 静态成员定义
std::atomic<IFAPI::IInflationFrameworkInterface*> MorphManager::_api{nullptr};

// =====================================================================
//  生命周期
// =====================================================================

void MorphManager::Initialize()
{
  auto* api = IFAPI::GetAPI();
  _api.store(api, std::memory_order_release);

  if (api)
    logger::info("[Fertility] MorphManager: InflationFramework API v{}", api->GetVersion());
  else
    logger::warn("[Fertility] MorphManager: InflationFramework API not available"
                 " — morph disabled");
}

bool MorphManager::IsAPIAvailable() const
{
  auto* api = _api.load(std::memory_order_acquire);
  if (api)
    return true;

  // 延迟重试: 某些 SKSE 插件加载顺序不确定
  api = IFAPI::GetAPI();
  if (api) {
    _api.store(api, std::memory_order_release);
    logger::info("[Fertility] MorphManager: InflationFramework API acquired (late)");
    return true;
  }
  return false;
}

void MorphManager::Revert()
{
  // 存档切换时清空队列中的残留操作,
  // 避免旧存档的 morph 指令应用到新存档的 actor 上
  MorphQueue::GetSingleton()->Flush();
}

// =====================================================================
//  孕肚 + 乳房
//
//  bellyScaleMult:  缩放系数
//  breastScaleMult: 乳房缩放系数
//  钳位由 LIF NG 内部处理, 此处只负责缩放
// =====================================================================

void MorphManager::UpdatePregnancyMorph(RE::FormID motherID, float progress)
{
  progress = std::clamp(progress, 0.0f, 1.0f);

  // ── 孕肚 ──
  {
    MorphOp_Update op;
    op.actorID   = motherID;
    op.morphType = MType::BellyPregnancy;
    op.value     = progress * Fertility::bellyScaleMult;
    MorphQueue::GetSingleton()->Enqueue(std::move(op));
  }

  // ── 乳房 (breastScaleMult 配置项终于生效) ──
  if (Fertility::breastScaleMult > 0.0f) {
    MorphOp_Update op;
    op.actorID   = motherID;
    op.morphType = MType::Breasts;
    op.value     = progress * Fertility::breastScaleMult;
    MorphQueue::GetSingleton()->Enqueue(std::move(op));
  }
}

// =====================================================================
//  精液膨胀
// =====================================================================

void MorphManager::UpdateSpermInflation(RE::FormID actorID, float normalized)
{
  normalized = std::clamp(normalized, 0.0f, 1.5f);

  MorphOp_Update op;
  op.actorID   = actorID;
  op.morphType = MType::Belly;
  op.value     = normalized;
  MorphQueue::GetSingleton()->Enqueue(std::move(op));
}

// =====================================================================
//  清零  (同时归零 Breasts)
// =====================================================================

void MorphManager::ClearMorphs(RE::FormID motherID)
{
  MorphOp_Clear op;
  op.actorID = motherID;
  MorphQueue::GetSingleton()->Enqueue(std::move(op));

  // Clear 操作默认只归零 Belly + BellyPregnancy,
  // 乳房需要额外发一个 Update(0)
  if (Fertility::breastScaleMult > 0.0f) {
    MorphOp_Update breastReset;
    breastReset.actorID   = motherID;
    breastReset.morphType = MType::Breasts;
    breastReset.value     = 0.0f;
    MorphQueue::GetSingleton()->Enqueue(std::move(breastReset));
  }
}

}  // namespace Fertility
