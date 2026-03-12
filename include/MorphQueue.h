#pragma once
// =====================================================================
//  MorphQueue.h — 体型变形操作队列
//
//  每帧从队列中取出 batchSize 个操作, 调用 InflationFramework API
//  超过阈值时自动去重 (保留每个 actor+morphType 的最新值)
//
//  ★ 修订说明:
//    - _batchSize 改为 atomic, 消除 Set/Get 与 ProcessBatch 的竞争
//    - QueueStats.processed 改为累计值 (totalProcessed)
//    - 新增 Stop() / IsRunning(), ScheduleNextFrame 可优雅停止
//    - GetStats 同时持两把锁, 消除 _queue.size() 的数据竞争
//    - Flush 拆分: 先取出再执行, 不在锁内调游戏 API
// =====================================================================


#include "InflationFrameworkAPI.h"
#include "Fertilitytypes.h"
#include <RaceOverrides.h>
#include <atomic>
#include <deque>
#include <mutex>
#include <variant>

namespace Fertility
{

struct MorphOp_Update
{
  RE::FormID actorID                         = 0;
  InflationFrameworkAPI::MorphType morphType = InflationFrameworkAPI::MorphType::Belly;
  float value                                = 0.0f;
};

struct MorphOp_Clear
{
  RE::FormID actorID = 0;
};

using MorphOp = std::variant<MorphOp_Update, MorphOp_Clear>;

struct QueueStats
{
  std::size_t pending        = 0;  // 当前待处理
  std::size_t totalProcessed = 0;  // 累计已处理
  std::size_t totalDropped   = 0;  // 累计去重丢弃
  float lastBatchMs          = 0;  // 上一批耗时
};

class MorphQueue
{
public:
  static MorphQueue* GetSingleton()
  {
    static MorphQueue instance;
    return &instance;
  }
  MorphQueue(const MorphQueue&)            = delete;
  MorphQueue& operator=(const MorphQueue&) = delete;

  void Initialize();
  void Stop();
  bool IsRunning() const { return _running.load(std::memory_order_acquire); }

  void Enqueue(MorphOp op);
  void ProcessBatch();

  void SetBatchSize(int size);
  int GetBatchSize() const;

  QueueStats GetStats() const;
  std::size_t GetPendingCount() const;
  bool IsEmpty() const;
  void Flush();

private:
  MorphQueue() = default;

  void ExecuteOp(const MorphOp& op);
  void DoUpdate(const MorphOp_Update& op);
  void DoClear(const MorphOp_Clear& op);
  void DeduplicateQueue();  // 调用者已持有 _queueMutex

  // ── 队列 ──
  mutable std::mutex _queueMutex;
  std::deque<MorphOp> _queue;
  std::atomic<int> _batchSize{4};

  // ── 统计 ──
  mutable std::mutex _statsMutex;
  QueueStats _stats;

  // ── 生命周期 ──
  std::atomic<bool> _running{false};
};

class MorphFrameTask
{
public:
  static void Register();
};

}  // namespace Fertility
