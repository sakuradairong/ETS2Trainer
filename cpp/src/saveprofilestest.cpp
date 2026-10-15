#include "saveprofilestest.h"
#include "saveprofiles.h"

namespace ets2 {
namespace {
SaveSlot slot(const wchar_t* profileDir, const wchar_t* profileName, bool cloud,
              const wchar_t* slotDir) {
    SaveSlot s;
    s.profileDir = profileDir;
    s.profileName = profileName;
    s.cloud = cloud;
    s.slotDir = slotDir;
    s.slotName = slotDir;
    s.displayName = W2U(profileName);
    return s;
}
bool sameSlots(const std::vector<int>& a, const std::vector<int>& b) { return a == b; }
}  // namespace

std::vector<TunerTestItem> runSaveProfileTests() {
    std::vector<TunerTestItem> items;
    auto check = [&](const char* name, bool ok, const std::string& detail = {}) {
        items.push_back({std::string("存档分组 / ") + name, ok, detail});
    };

    // 1) 同名档案位于不同目录：必须分成两组，且不被合并。
    {
        std::vector<SaveSlot> slots = {
            slot(L"profiles\\42\\save", L"同一名字", false, L"1"),
            slot(L"profiles\\43\\save", L"同一名字", false, L"2"),
            slot(L"profiles\\42\\save", L"同一名字", false, L"3"),
        };
        auto groups = groupSaveProfiles(slots);
        check("同名不同目录独立分组", groups.size() == 2,
              "groups=" + std::to_string(groups.size()));
        if (groups.size() == 2) {
            check("同名不同目录：各自槽位", sameSlots(groups[0].slots, {0, 2}) && sameSlots(groups[1].slots, {1}),
                  "g0=" + std::to_string(groups[0].slots.size()) + " g1=" + std::to_string(groups[1].slots.size()));
        }
    }

    // 2) 同一目录但本地/云：必须分成两组。
    {
        std::vector<SaveSlot> slots = {
            slot(L"profiles\\50\\save", L"云与本地的区别", false, L"1"),
            slot(L"profiles\\50\\save", L"云与本地的区别", true, L"1"),
        };
        auto groups = groupSaveProfiles(slots);
        check("local/cloud 独立", groups.size() == 2, "groups=" + std::to_string(groups.size()));
        if (groups.size() == 2)
            check("local/cloud：两组槽位不同", groups[0].slots != groups[1].slots);
    }

    // 3) 保留原槽位索引，不因分组重排。
    {
        std::vector<SaveSlot> slots = {
            slot(L"profiles\\a\\save", L"A", false, L"1"),
            slot(L"profiles\\b\\save", L"B", false, L"2"),
            slot(L"profiles\\a\\save", L"A", false, L"3"),
            slot(L"profiles\\c\\save", L"C", false, L"4"),
            slot(L"profiles\\a\\save", L"A", false, L"5"),
        };
        auto groups = groupSaveProfiles(slots);
        bool ok = groups.size() == 3 && sameSlots(groups[0].slots, {0, 2, 4}) &&
                  sameSlots(groups[1].slots, {1}) && sameSlots(groups[2].slots, {3});
        // 验证映射回原列表仍指向相同槽位。
        for (int i = 0; i < (int)groups.size(); ++i)
            for (int s : groups[i].slots)
                if (s < 0 || s >= (int)slots.size() || saveProfileForSlot(groups, s) != i) ok = false;
        check("保留原始槽位索引", ok);
    }

    // 4) 刷新后重排（顺序变化）仍按路径恢复档案与槽位。
    {
        std::vector<SaveSlot> oldSlots = {
            slot(L"profiles\\x\\save", L"X", false, L"1"),
            slot(L"profiles\\y\\save", L"Y", false, L"2"),
            slot(L"profiles\\x\\save", L"X", false, L"3"),
        };
        auto oldGroups = groupSaveProfiles(oldSlots);
        int oldProfile = saveProfileForSlot(oldGroups, 2);  // profiles\x, slotDir 3
        check("刷新前映射正确", oldProfile == 0, "profile=" + std::to_string(oldProfile));
        std::vector<SaveSlot> newSlots = {
            slot(L"profiles\\y\\save", L"Y", false, L"2"),
            slot(L"profiles\\x\\save", L"X", false, L"3"),
            slot(L"profiles\\x\\save", L"X", false, L"1"),
        };
        auto newGroups = groupSaveProfiles(newSlots);
        auto sel = restoreSaveProfileSelection(newSlots, newGroups, L"profiles\\x\\save", false, L"3");
        check("刷新重排后恢复路径", sel.profile >= 0 && sel.slot == 1 &&
              saveProfileForSlot(newGroups, sel.slot) == sel.profile,
              "profile=" + std::to_string(sel.profile) + " slot=" + std::to_string(sel.slot));
    }

    // 5) 删除某槽位后档案仍保留（另一槽位仍在同一组）。
    {
        std::vector<SaveSlot> slots = {
            slot(L"profiles\\keep\\save", L"保留", false, L"1"),
            slot(L"profiles\\keep\\save", L"保留", false, L"2"),
        };
        auto groups = groupSaveProfiles(slots);
        check("删除前两组槽位", groups.size() == 1 && groups[0].slots.size() == 2);
        // 删除槽位 0（只保留 1）。
        std::vector<SaveSlot> after = {slots[1]};
        auto afterGroups = groupSaveProfiles(after);
        check("删槽保留档案（组仍存在）", afterGroups.size() == 1 && afterGroups[0].slots.size() == 1,
              "groups=" + std::to_string(afterGroups.size()));
        const auto sel=restoreSaveProfileSelection(after,afterGroups,L"profiles\\keep\\save",false,L"1");
        check("删槽保留档案并清空槽选择",sel.profile==0 && sel.slot==-1);
    }

    // 6) 档案所有槽位删除后组消失；恢复逻辑不选中任何槽位。
    {
        std::vector<SaveSlot> slots = {
            slot(L"profiles\\gone\\save", L"会被删", false, L"1"),
            slot(L"profiles\\other\\save", L"另一个", false, L"9"),
        };
        std::vector<SaveSlot> after = {slots[1]};
        auto afterGroups = groupSaveProfiles(after);
        auto sel = restoreSaveProfileSelection(after, afterGroups, L"profiles\\gone\\save", false, L"1");
        check("删档后组消失", afterGroups.size() == 1);
        check("删档回退不选槽", sel.slot == -1, "slot=" + std::to_string(sel.slot));
    }

    // 7) 路径大小写与尾斜线：应视为同一档案组。
    {
        std::vector<SaveSlot> slots = {
            slot(L"Profiles\\Case\\Save", L"大小写", false, L"1"),
            slot(L"profiles\\case\\save\\", L"大小写", false, L"2"),
            slot(L"PROFILES/CASE/SAVE", L"大小写", false, L"3"),
        };
        auto groups = groupSaveProfiles(slots);
        check("路径大小写与尾斜线归一", groups.size() == 1 && groups[0].slots.size() == 3,
              "groups=" + std::to_string(groups.size()));
        auto sel = restoreSaveProfileSelection(slots, groups, L"profiles\\CASE\\save\\", false, L"3");
        check("归一化路径恢复选择", sel.profile == 0 && sel.slot == 2,
              "profile=" + std::to_string(sel.profile) + " slot=" + std::to_string(sel.slot));
    }

    // 8) 空列表：无组、无可选槽位。
    {
        std::vector<SaveSlot> empty;
        auto groups = groupSaveProfiles(empty);
        auto sel = restoreSaveProfileSelection(empty, groups, L"profiles\\any\\save", false, L"1");
        check("空列表无组不选槽", groups.empty() && sel.profile == -1 && sel.slot == -1);
    }

    return items;
}
}  // namespace ets2
