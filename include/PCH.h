#pragma once
#define NOMINMAX
// CommonLibSSE-NG
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
// STL
#include <algorithm>
#include <array>  
#include <cstdint>
#include <format>      
#include <functional>  
#include <memory>
#include <mutex>
#include <optional>  
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <atomic>       // MorphManager, MorphQueue
#include <chrono>       // SpermExpulsion, MorphQueue
#include <deque>        // MorphQueue
#include <random>       // TickManager, FertilityStorage
#include <ranges>       // TickManager, FertilityStorage, RaceOverrides
#include <variant>      // MorphQueue (MorphOp)
// spdlog (via CommonLib)
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>
using namespace std::literals;
namespace logger = SKSE::log;
