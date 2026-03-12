#pragma once

namespace Fertility::Events
{
/// 注册所有事件 Sink (Death, Location, ObjectLoaded)
/// 在 main.cpp kDataLoaded 时调用
void Register();
}  // namespace Fertility::Events
