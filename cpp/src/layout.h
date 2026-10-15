#pragma once
#include "gameplay.h"
#include "memory.h"
#include <array>
#include <vector>
namespace ets2 {
struct LayoutProfile {
    GameId game;
    uint32_t timestamp, imageSize;
    uint64_t rootRva;
    std::array<uint8_t, 17> rootInstruction;
    uint64_t engineVtableRva;
    uint64_t bankVtableRva = 0, economyVtableRva = 0;
    uint64_t fuelCorrectionOffset = 0x1158;
    const char* version = "";
    uint64_t fuelFormulaRva = 0;
    std::array<uint8_t,16> fuelFormulaInstruction{};
};
const std::vector<LayoutProfile>& layoutProfiles(GameId game);
const LayoutProfile& layoutProfile(GameId game);
bool layoutFingerprintMatches(const LayoutProfile&, uint32_t timestamp, uint32_t imageSize,
                              const std::array<uint8_t, 17>& instruction);
bool layoutFuelFormulaMatches(const LayoutProfile&,const std::array<uint8_t,16>& instruction);
// Checks the actual executable identity, loaded PE header and code anchor, without write access.
bool verifyGameLayout(const ProcessMemory&, uint64_t* imageBase, std::string* error,
                      const LayoutProfile** matched = nullptr);
}
