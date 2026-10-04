#pragma once
#include "savecopy.h"
#include <functional>

namespace ets2 {
struct ProfileSplitItem {
    SaveSlot source;
    std::string profileName;
};
struct ProfileSplitRequest {
    GameId game = GameId::Ets2;
    std::vector<ProfileSplitItem> items;
};
struct SplitProfile {
    std::string name;
    std::wstring profileDir, slotDir;
};
struct ProfileSplitResult {
    GameId game = GameId::Ets2;
    bool ok = false;
    // Published profiles remain here even when a later item fails or is canceled.
    std::vector<SplitProfile> created;
    std::string error;
};
struct ProfileSplitPaths {
    std::wstring localProfiles;
    std::wstring cloudLocalProfiles;
    std::vector<std::wstring> nameCheckRoots;
};
struct ProfileSplitOptions {
    std::function<bool()> canceled;
    std::function<bool(std::string*)> guard;
    // Isolated-test fault injection; production leaves these empty.
    std::function<bool(size_t)> afterFile;
    std::function<void(size_t, const std::wstring&)> beforePublish;
};
bool profileFolderName(const std::string& name, std::wstring* hex, std::string* error);
bool patchProfileText(std::string& text, const std::string& name, uint64_t created,
                      uint64_t saved, std::string* error);
// Paths are explicit so regression tests never write to the user's Documents or Steam data.
ProfileSplitResult splitProfilesAt(const ProfileSplitRequest&, const ProfileSplitPaths&,
                                   const ProfileSplitOptions& = {});
// Production entry point: same-game membership, game process and known profile roots are checked.
ProfileSplitResult splitSelectedProfiles(const ProfileSplitRequest&, std::function<bool()> canceled = {});
}
