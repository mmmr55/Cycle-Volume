// =====================================================================

// =====================================================================

#include "MorphQueue.h"
#include "ConfigManager.h"
#include "MorphManager.h"

#include <algorithm>
#include <chrono>
#include <ranges>
#include <unordered_map>
#include <vector>

namespace Fertility
{

// =====================================================================
//  辅助
// =====================================================================

static InflationFrameworkAPI::IInflationFrameworkInterface* GetAPI()
{
  return InflationFrameworkAPI::GetAPI();
}

using MType = InflationFrameworkAPI::MorphType;

static RE::Actor* SafeGetActor(RE::FormID id)
{
  if (id == 0)
    return nullptr;
  auto* form = RE::TESForm::LookupByID(id);
  return form ? form->As<RE::Actor>() : nullptr;
}

static bool CanApplyMorph(RE::Actor* actor)
{
  return actor && !actor->IsDisabled() && !actor->IsDead() && actor->Is3DLoaded();
}

// =====================================================================
//  生命周期
// =====================================================================

void MorphQueue::Initialize()
{
  _running.store(true, std::memory_order_release);
  MorphFrameTask::Register();
  logger::info("[Fertility] MorphQueue: Initialized (batchSize={})", _batchSize.load(std::memory_order_relaxed));
}

void MorphQueue::Stop()
{
  _running.store(false, std::memory_order_release);
  logger::info("[Fertility] MorphQueue: Stopped");
}

// =====================================================================
//  入队
// =====================================================================

void MorphQueue::Enqueue(MorphOp op)
{
  std::lock_guard lk(_queueMutex);
  _queue.push_back(std::move(op));

  // 队列过长时去重 (阈值 64)
  if (_queue.size() > 64)
    DeduplicateQueue();
}

// =====================================================================
//  批处理  (每帧调用)
// =====================================================================

void MorphQueue::ProcessBatch()
{
  std::vector<MorphOp> batch;
  std::size_t pendingAfter = 0;

  {
    std::lock_guard lk(_queueMutex);

    // ★ 修复: empty() 检查在锁内
    if (_queue.empty())
      return;

    int count = std::min(_batchSize.load(std::memory_order_relaxed), static_cast<int>(_queue.size()));
    batch.reserve(count);
    for (int i = 0; i < count; ++i) {
      batch.push_back(std::move(_queue.front()));
      _queue.pop_front();
    }

    // ★ 修复: pending 在锁内读取
    pendingAfter = _queue.size();
  }

  // 执行 (锁外, 因为游戏 API 可能阻塞)
  auto t0 = std::chrono::high_resolution_clock::now();

  for (auto& op : batch)
    ExecuteOp(op);

  auto t1  = std::chrono::high_resolution_clock::now();
  float ms = std::chrono::duration<float, std::milli>(t1 - t0).count();

  // 更新统计
  {
    std::lock_guard lk(_statsMutex);
    _stats.totalProcessed += batch.size();  // ★ 修复: 累加
    _stats.pending     = pendingAfter;
    _stats.lastBatchMs = ms;
  }

  if (Fertility::verboseMode && !batch.empty())
    logger::trace("[Fertility] MorphQueue: {} ops {:.2f}ms, {} remaining", batch.size(), ms, pendingAfter);
}

// =====================================================================
//  操作执行
// =====================================================================

void MorphQueue::ExecuteOp(const MorphOp& op)
{
  std::visit(
      [this](const auto& o) {
        using T = std::decay_t<decltype(o)>;
        if constexpr (std::is_same_v<T, MorphOp_Update>)
          DoUpdate(o);
        else if constexpr (std::is_same_v<T, MorphOp_Clear>)
          DoClear(o);
      },
      op);
}

void MorphQueue::DoUpdate(const MorphOp_Update& op)
{
  auto* api = GetAPI();
  if (!api)
    return;
  auto* actor = SafeGetActor(op.actorID);
  if (!CanApplyMorph(actor))
    return;
  api->SetInflation(actor, op.morphType, op.value);
}

void MorphQueue::DoClear(const MorphOp_Clear& op)
{
  auto* api = GetAPI();
  if (!api)
    return;
  auto* actor = SafeGetActor(op.actorID);
  if (!CanApplyMorph(actor))
    return;
  api->SetInflation(actor, MType::Belly, 0.0f);
  api->SetInflation(actor, MType::BellyPregnancy, 0.0f);
}

// =====================================================================
//  去重  (调用者已持有 _queueMutex)
//
//  对同一 actor+morphType, 只保留最后一个 (最新值);
//  Clear 操作用特殊 morphType 标记。
//  单遍从后往前扫描: 第一次见到的 key 保留, 之后的标记删除。
// =====================================================================

void MorphQueue::DeduplicateQueue()
{
  struct OpKey
  {
    RE::FormID actorID                  = 0;
    std::uint32_t morphTag              = 0;
    bool operator==(const OpKey&) const = default;
  };

  struct OpKeyHash
  {
    std::size_t operator()(const OpKey& k) const { return std::hash<RE::FormID>{}(k.actorID) ^ (std::hash<std::uint32_t>{}(k.morphTag) << 16); }
  };

  // 从后往前扫描: 对每个 key, 第一次出现的 (最新的) 保留
  std::unordered_map<OpKey, bool, OpKeyHash> seen;
  std::size_t originalSize = _queue.size();

  // 标记: 将重复项的 actorID 置 0
  for (auto it = _queue.rbegin(); it != _queue.rend(); ++it) {
    OpKey key;
    std::visit(
        [&key](const auto& o) {
          using T     = std::decay_t<decltype(o)>;
          key.actorID = o.actorID;
          if constexpr (std::is_same_v<T, MorphOp_Update>)
            key.morphTag = static_cast<std::uint32_t>(o.morphType);
          else
            key.morphTag = 0xFFFFFFFF;  // Clear 操作的标记
        },
        *it);

    if (key.actorID == 0)
      continue;

    auto [_, inserted] = seen.try_emplace(key, true);
    if (!inserted) {
      // 已见过 → 这是旧值, 标记删除
      std::visit(
          [](auto& o) {
            o.actorID = 0;
          },
          *it);
    }
  }

  // 清除标记项
  auto isMarked = [](const MorphOp& op) {
    return std::visit(
        [](const auto& o) {
          return o.actorID == 0;
        },
        op);
  };
  std::erase_if(_queue, isMarked);

  std::size_t dropped = originalSize - _queue.size();
  if (dropped > 0) {
    std::lock_guard lk(_statsMutex);
    _stats.totalDropped += dropped;
  }
}

// =====================================================================
//  配置
// =====================================================================

void MorphQueue::SetBatchSize(int size)
{
  _batchSize.store(std::clamp(size, 1, 20), std::memory_order_relaxed);
}

int MorphQueue::GetBatchSize() const
{
  return _batchSize.load(std::memory_order_relaxed);
}

// =====================================================================
//  查询
// =====================================================================

QueueStats MorphQueue::GetStats() const
{
  // ★ 修复: 同时持两把锁 (固定顺序: queue → stats), 消除 pending 的竞争
  std::lock_guard lkQ(_queueMutex);
  std::lock_guard lkS(_statsMutex);
  QueueStats s = _stats;
  s.pending    = _queue.size();
  return s;
}

std::size_t MorphQueue::GetPendingCount() const
{
  std::lock_guard lk(_queueMutex);
  return _queue.size();
}

bool MorphQueue::IsEmpty() const
{
  std::lock_guard lk(_queueMutex);
  return _queue.empty();
}

// =====================================================================
//  Flush — 立即执行所有待处理操作
//
//  ★ 修复: 先取出再执行, 不在锁内调用游戏 API
// =====================================================================

void MorphQueue::Flush()
{
  std::vector<MorphOp> all;
  {
    std::lock_guard lk(_queueMutex);
    all.assign(std::make_move_iterator(_queue.begin()), std::make_move_iterator(_queue.end()));
    _queue.clear();
  }

  // 锁外执行
  for (auto& op : all)
    ExecuteOp(op);

  // 更新统计
  if (!all.empty()) {
    std::lock_guard lk(_statsMutex);
    _stats.totalProcessed += all.size();
    _stats.pending = 0;
  }
}

// =====================================================================
//  帧任务  (支持优雅停止)
// =====================================================================

static void ScheduleNextFrame()
{
  auto* taskIntfc = SKSE::GetTaskInterface();
  if (!taskIntfc)
    return;

  taskIntfc->AddTask([]() {
    auto* q = MorphQueue::GetSingleton();

    // ★ 修复: 检查 running 标志, 支持优雅停止
    if (!q->IsRunning())
      return;

    q->ProcessBatch();
    ScheduleNextFrame();
  });
}

void MorphFrameTask::Register()
{
  ScheduleNextFrame();
  logger::info("[Fertility] MorphFrameTask: Registered per-frame callback");
}

}  // namespace Fertility
