#include "saveprofiles.h"
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <unordered_map>

namespace ets2 {
namespace {
std::wstring pathKey(const std::wstring& path) {
    if(path.empty()) return {};
    auto key=std::filesystem::path(path).lexically_normal().wstring();
    std::replace(key.begin(),key.end(),L'/',L'\\');
    while(key.size()>3 && key.back()==L'\\') key.pop_back();
    std::transform(key.begin(),key.end(),key.begin(),[](wchar_t c){return std::towlower(c);});
    return key;
}
}
std::vector<SaveProfileGroup> groupSaveProfiles(const std::vector<SaveSlot>& slots) {
    std::vector<SaveProfileGroup> groups;
    std::unordered_map<std::wstring,size_t> indices;
    for(size_t i=0;i<slots.size();++i) {
        const auto& slot=slots[i];
        const auto key=pathKey(slot.profileDir)+(slot.cloud ? L"|cloud" : L"|local");
        const auto inserted=indices.emplace(key,groups.size());
        if(inserted.second) {
            SaveProfileGroup group;
            group.profileDir=slot.profileDir;group.cloud=slot.cloud;
            group.name=W2U(slot.profileName);
            if(group.name.empty()) group.name=W2U(std::filesystem::path(slot.profileDir).filename().wstring());
            if(group.name.empty()) group.name="未命名档案";
            groups.push_back(std::move(group));
        }
        groups[inserted.first->second].slots.push_back((int)i);
    }
    return groups;
}
int saveProfileForSlot(const std::vector<SaveProfileGroup>& groups,int slot) {
    if(slot<0) return -1;
    for(int i=0;i<(int)groups.size();++i)
        if(std::find(groups[i].slots.begin(),groups[i].slots.end(),slot)!=groups[i].slots.end()) return i;
    return -1;
}
SaveProfileSelection restoreSaveProfileSelection(const std::vector<SaveSlot>& slots,
    const std::vector<SaveProfileGroup>& groups,const std::wstring& profileDir,bool cloud,
    const std::wstring& slotDir) {
    SaveProfileSelection selection;
    const auto profileKey=pathKey(profileDir),slotKey=pathKey(slotDir);
    for(int i=0;i<(int)groups.size();++i)
        if(groups[i].cloud==cloud && !profileKey.empty() && pathKey(groups[i].profileDir)==profileKey) {
            selection.profile=i;break;
        }
    if(!slotKey.empty() && selection.profile>=0)
        for(int index:groups[selection.profile].slots)
            if(index>=0 && index<(int)slots.size() && pathKey(slots[index].slotDir)==slotKey) {
                selection.slot=index;break;
            }
    if(selection.profile<0 && !groups.empty()) selection.profile=0;
    return selection;
}
}
