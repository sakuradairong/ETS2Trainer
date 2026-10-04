// Same-profile duplication; game progress remains byte-for-byte unchanged.
#pragma once
#include "gameplay.h"
#include "saves.h"
#include <functional>
namespace ets2 {
struct SaveCopyRequest {
    GameId game = GameId::Ets2;
    SaveSlot source;
    std::string displayName;
};
struct SaveCopyResult {
    bool ok = false;
    GameId game = GameId::Ets2;
    std::wstring slotDir, slotName;
    std::string displayName, error;
};
struct SaveCopyOptions {
    std::function<bool()> canceled;
    std::function<bool(std::string*)> guard;
    // Optional validated SII progress replacement; lifetime must cover the synchronous copy.
    // Only the staged game.sii is replaced, after the source snapshot is verified.
    const std::string* replacementGameText = nullptr;
    // Fault injection for isolated fixtures; production leaves these empty.
    std::function<bool(size_t)> afterFile;
    std::function<void(const std::wstring&)> beforePublish;
};
bool siiEscapeName(const std::string& utf8, std::string* out, std::string* error);
std::string siiUnescapeName(const std::string& escaped);
bool patchSaveInfo(std::string& text, const std::string& name, uint64_t fileTime, std::string* error);
std::string readSaveDisplayName(const SaveSlot& slot);
bool copyBlockedByRunningGame(GameId game, std::string* error);
// Never replaces a destination. Retry one transient Windows file lock, revalidating each attempt.
void publishNewDirectory(const std::wstring& stage, const std::wstring& target,
                         const std::function<void()>& validate);
// Generic filesystem core, with strict same-profile boundaries. Tests use synthetic profiles.
SaveCopyResult copySaveSlotInProfile(const SaveSlot&, const std::string&, const SaveCopyOptions& = {});
// UI always uses this wrapper: selected-game membership and process gates cannot be skipped.
SaveCopyResult copySlotToNewSave(const SaveCopyRequest&, std::function<bool()> canceled = {},
                               const std::string* replacementGameText = nullptr);
}
