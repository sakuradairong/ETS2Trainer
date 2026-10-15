// Stock game text is generated locally into cpp/build and is not distributed.
#pragma once
#include <array>
namespace ets2 {
struct FuelTankDefinition { const char* path; const char* text; bool electric=false; };
}
#if __has_include("../build/fueltank_definitions.generated.h")
#include "../build/fueltank_definitions.generated.h"
#else
namespace ets2 {
inline constexpr std::array<FuelTankDefinition,0> kFuelTankDefinitions{};
}
#endif
