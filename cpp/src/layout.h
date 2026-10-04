#pragma once
#include "gameplay.h"
#include "memory.h"
#include <array>
namespace ets2 {
struct LayoutProfile {
    GameId game;
    uint32_t timestamp, imageSize;
    uint64_t rootRva;
    std::array<uint8_t, 17> rootInstruction;
    uint64_t engineVtableRva;
    uint64_t bankVtableRva = 0, economyVtableRva = 0;
};
const LayoutProfile& layoutProfile(GameId game);
bool layoutFingerprintMatches(const LayoutProfile&, uint32_t timestamp, uint32_t imageSize,
                              const std::array<uint8_t, 17>& instruction);
// Checks the actual executable identity, loaded PE header and code anchor, without write access.
bool verifyGameLayout(const ProcessMemory&, uint64_t* imageBase, std::string* error);
}
