#pragma once


#include <Fertilitystorage.h>
#include <Fertilitytypes.h>
#include <RaceOverrides.h>
#include <SKSEMenuFramework.h>

namespace Fertility
{

class Menu
{
public:
  static Menu* GetSingleton()
  {
    static Menu instance;
    return &instance;
  }

  Menu(const Menu&)            = delete;
  Menu& operator=(const Menu&) = delete;

  // Config route
  static void Settings();
  static void Cycle();
  static void Pregnancy();
  static void Birth();
  static void Expulsion();
  static void Scaling();
  static void Automation();

  // Status route (co-save 只读)
  static void Tracking();
  static void Children();

  // 混合
  static void Debug();

  static void __stdcall EventListener(SKSEMenuFramework::Model::EventType eventType);

private:
  Menu();
  ~Menu() = default;

  SKSEMenuFramework::Model::Event* event = nullptr;
};

}  // namespace Fertility
