#pragma once
#include "saves.h"

namespace ets2 {
struct SaveProfileGroup {
    std::wstring profileDir;
    std::string name;
    bool cloud = false;
    std::vector<int> slots;  // Indices into the original list, never a reordered copy.
};
struct SaveProfileSelection { int profile = -1, slot = -1; };
std::vector<SaveProfileGroup> groupSaveProfiles(const std::vector<SaveSlot>& slots);
SaveProfileSelection restoreSaveProfileSelection(const std::vector<SaveSlot>& slots,
    const std::vector<SaveProfileGroup>& groups,const std::wstring& profileDir,bool cloud,
    const std::wstring& slotDir);
int saveProfileForSlot(const std::vector<SaveProfileGroup>& groups,int slot);
}
