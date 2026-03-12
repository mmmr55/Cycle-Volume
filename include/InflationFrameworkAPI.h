#pragma once

// =========================================================================
//  InflationFramework Public API
//
//  用法 (Cycle&Volume 等消费端):
//    #include "InflationFrameworkAPI.h"
//
//    // 在 MessageHandler 里:
//    void MessageHandler(SKSE::MessagingInterface::Message* msg) {
//        InflationFrameworkAPI::HandleMessage(msg);  // 自动接收 API
//    }
//
//    // 使用:
//    auto* api = InflationFrameworkAPI::GetAPI();
//    if (api) {
//        api->SetInflation(actor, MorphType::BellyPregnancy, 0.5f);
//    }
// =========================================================================

#include <cstdint>

namespace InflationFrameworkAPI
{

inline constexpr std::uint32_t kAPIVersion = 1;

// 消息类型
enum : std::uint32_t
{
  kAPIReady = 'IFAP'  // InflationFramework 广播此消息, data = IInflationFrameworkInterface*
};

// Morph 类型 — 与 InflationFramework 内部枚举一致
enum class MorphType : std::uint8_t
{
  Belly          = 0,
  BellyMid       = 1,
  BellyUnder     = 2,
  BellyPregnancy = 3,
  Breasts        = 4,
  Butt           = 5,
};

// ─── API 接口 (虚函数表, 跨 DLL 安全) ───
class IInflationFrameworkInterface
{
public:
  virtual ~IInflationFrameworkInterface() = default;

  virtual std::uint32_t GetVersion() const = 0;

  virtual float GetInflation(RE::Actor* actor, MorphType type) const       = 0;
  virtual void SetInflation(RE::Actor* actor, MorphType type, float value) = 0;
  virtual void ModInflation(RE::Actor* actor, MorphType type, float value) = 0;

  virtual float GetMorphByName(RE::Actor* actor, const char* morphName) const       = 0;
  virtual void SetMorphByName(RE::Actor* actor, const char* morphName, float value) = 0;

  virtual void ApplyMorphs(RE::Actor* actor) = 0;
};

// ─── 消费端: 静态缓存 + 自动接收 ───

namespace detail
{
  inline IInflationFrameworkInterface* g_api = nullptr;
}

// 在消费端的 MessageHandler 里调用, 自动捕获 API
inline void HandleMessage(SKSE::MessagingInterface::Message* msg)
{
  if (msg && msg->type == kAPIReady && msg->data) {
    detail::g_api = static_cast<IInflationFrameworkInterface*>(msg->data);
    SKSE::log::info("[InflationFrameworkAPI] API received (version {})", detail::g_api->GetVersion());
  }
}

// 获取 API 指针 (未收到广播前返回 nullptr)
inline IInflationFrameworkInterface* GetAPI()
{
  return detail::g_api;
}

// 消费端注册监听 (在 SKSEPluginLoad 里调用)
inline void RegisterListener()
{
  auto* messaging = SKSE::GetMessagingInterface();
  if (messaging) {
    messaging->RegisterListener("InflationFramework", [](SKSE::MessagingInterface::Message* msg) {
      HandleMessage(msg);
    });
  }
}

}  // namespace InflationFrameworkAPI
