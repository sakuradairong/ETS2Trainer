#pragma once
#include <functional>
#include <memory>
#include <string>

namespace ets2 {
bool scaniaTvPresetAvailable();
// Installs only the 14 embedded, hash-checked assets at their fixed /home paths.
// Existing different files are never overwritten. Uncommitted new installs roll back.
class ScaniaTvPresetAssets {
public:
    ScaniaTvPresetAssets();
    ~ScaniaTvPresetAssets();
    bool install(const std::wstring& gameHome,std::string* error,std::function<void()> guard = {});
    bool rollback(std::string* error);
    void commit();
    bool createdNew() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
