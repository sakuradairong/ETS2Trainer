#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
namespace ets2 {
// Holds parent directories and the installed package open until the save copy commits.
// Existing packages must be byte-identical. Uncommitted newly created files roll back.
class FuelTankModAssets {
public:
    FuelTankModAssets();
    ~FuelTankModAssets();
    bool install(const std::wstring& gameHome,const std::string& fileName,
                 const std::vector<std::uint8_t>& bytes,std::string* error,
                 std::function<void()> guard = {});
    bool rollback(std::string* error);
    void commit();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
