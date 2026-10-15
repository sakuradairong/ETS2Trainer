#include "homeaccessorytest.h"
#include "trucktransferio.h"
#include "homepreset.h"
#include <filesystem>
#include <stdexcept>

namespace ets2 {
namespace {
const char* kHome = R"SII(SiiNunit
{
player : _nameless.1 {
 trucks: 2
 trucks[0]: _nameless.2
 trucks[1]: _nameless.3
 note: "/def/vehicle/addon/old.sii"
}
vehicle : _nameless.2 {
 accessories: 2
 accessories[0]: _nameless.a.1
 accessories[1]: _nameless.a.2
 fuel_relative: 0.5
}
vehicle : _nameless.3 {
 accessories: 1
 accessories[0]: _nameless.b.1
}
vehicle_accessory : _nameless.a.1 {
 data_path: "/def/vehicle/truck/scania.s/data.sii"
 addon: _nameless.a.3
}
vehicle_wheel_accessory : _nameless.a.2 {
 data_path: "/def/vehicle/wheel/old.sii"
}
// /def/vehicle/addon/old.sii stays here and in the player's quoted note.
vehicle_addon_accessory : _nameless.a.3 {
 data_path: "/def/vehicle/addon/old.sii"
 subtype: "gps"
}
vehicle_accessory : _nameless.b.1 {
 data_path: "/def/vehicle/truck/volvo.fh16/data.sii"
}
}
)SII";
std::string changed(std::string text,const std::string& from,const std::string& to) {
    const auto pos=text.find(from);
    if(pos==std::string::npos) throw std::runtime_error("fixture marker missing");
    text.replace(pos,from.size(),to);return text;
}
void put(const std::filesystem::path& path,const std::string& text) {
    if(!writeFileBytes(path.wstring(),{text.begin(),text.end()})) throw std::runtime_error("fixture write failed");
}
std::string get(const std::filesystem::path& path) {
    std::vector<uint8_t> bytes;
    if(!readFileBytes(path.wstring(),bytes)) throw std::runtime_error("fixture read failed");
    return {bytes.begin(),bytes.end()};
}
}
std::vector<TunerTestItem> runHomeAccessoryTests() {
    std::vector<TunerTestItem> results;
    auto check=[&](const char* name,bool ok,const std::string& detail=std::string()) {
        results.push_back({std::string("home-accessory-")+name,ok,detail});
    };
    const std::string source=kHome,newPath="/def/vehicle/addon/new.sii";
    std::string error,out;
    HomeAccessoryInventory inventory;
    const bool inspected=inspectHomeAccessoriesText(source,&inventory,&error);
    bool nested=false;
    for(const auto& entry:inventory.entries)
        if(entry.unitId=="_nameless.a.3") nested=!entry.shared && entry.subtype=="gps";
    check("owned-and-nested-inspection",inspected && inventory.entries.size()==4 && nested,error);
    const bool replaced=replaceHomeAccessoryPath(source,"_nameless.2","_nameless.a.3",newPath,&out,&error);
    const auto expected=changed(source,"data_path: \"/def/vehicle/addon/old.sii\"",
                                      "data_path: \""+newPath+"\"");
    check("only-selected-token-changed",replaced && out==expected,error);
    HomeAccessoryInventory after;
    check("edited-document-reparses",replaced && inspectHomeAccessoriesText(out,&after,&error) && after.entries.size()==4,error);
    const auto atom=changed(source,"data_path: \"/def/vehicle/addon/old.sii\"",
                                  "data_path: /def/vehicle/addon/old.sii");
    check("unquoted-path-replacement",replaceHomeAccessoryPath(atom,"_nameless.2","_nameless.a.3",newPath,&out,&error) && out==expected,error);
    for(const auto& path:std::vector<std::string>{"","def/a.sii","/def/a.txt","/def/../a.sii","/def/./a.sii",
         "/def//a.sii","/def/.sii","/def/a\".sii","/def/a\\b.sii","/def/a b.sii","/def/a\nb.sii",
         "/def/a:{b}.sii","/def/"+std::string(260,'a')+".sii"}) {
        out="stale";
        check("invalid-path-refused",!replaceHomeAccessoryPath(source,"_nameless.2","_nameless.a.3",path,&out,&error) && out.empty() && !error.empty(),path);
    }
    out="stale";
    check("other-truck-accessory-refused",!replaceHomeAccessoryPath(source,"_nameless.2","_nameless.b.1",newPath,&out,&error) && out.empty(),error);
    out="stale";
    check("missing-unit-refused",!replaceHomeAccessoryPath(source,"_nameless.2","_nameless.missing",newPath,&out,&error) && out.empty(),error);
    out="stale";
    check("unowned-truck-refused",!replaceHomeAccessoryPath(source,"_nameless.99","_nameless.a.3",newPath,&out,&error) && out.empty(),error);
    const auto shared=changed(source,"accessories[0]: _nameless.b.1","accessories[0]: _nameless.a.1");
    out="stale";
    check("shared-ancestor-refused",!replaceHomeAccessoryPath(shared,"_nameless.2","_nameless.a.3",newPath,&out,&error) && out.empty(),error);
    inspectHomeAccessoriesText(shared,&inventory,&error);nested=false;
    for(const auto& entry:inventory.entries) if(entry.unitId=="_nameless.a.3") nested=entry.shared;
    check("shared-ancestor-displayed",nested,error);
    const auto direct=changed(source,"accessories[0]: _nameless.b.1","accessories[0]: _nameless.a.2");
    out="stale";
    check("shared-direct-refused",!replaceHomeAccessoryPath(direct,"_nameless.2","_nameless.a.2",newPath,&out,&error) && out.empty(),error);
    out="stale";
    check("malformed-source-refused",!replaceHomeAccessoryPath(source.substr(0,source.size()-3),"_nameless.2","_nameless.a.3",newPath,&out,&error) && out.empty(),error);
    out="stale";
    check("cancel-refused",!replaceHomeAccessoryPath(source,"_nameless.2","_nameless.a.3",newPath,&out,&error,[]{return true;}) && out.empty(),error);
    HomeAccessoryRequest request; request.source.slotDir=L"not-a-user-save";
    check("publish-cancel-gate",!homeAccessoryToNewSave(request,[]{return true;}).ok);
    setSelectedGame(GameId::Ats);request.game=GameId::Ats;
    check("ats-publish-refused",!homeAccessoryToNewSave(request).ok);
    setSelectedGame(GameId::Ets2);
    const std::string preset=changed(changed(source,"player : _nameless.1 {",
        "economy : _nameless.0 { player: _nameless.1 bus_job_log: _nameless.f.1 }\n"
        "bus_job_log : _nameless.f.1 { version: 0 entries: 0 }\nplayer : _nameless.1 {"),
        "scania.s/data.sii","scania.s_2016/data.sii");
    std::string tvText;
    const bool tvOk=installScaniaTvPresetText(preset,"_nameless.2",&tvText,&error);
    check("tv-adds-base-and-registration",tvOk && tvText.find("entries[0]: \"etzhome.tv1\"")!=std::string::npos &&
        tvText.find("accessories[2]: _nameless.7ffd.1")!=std::string::npos &&
        tvText.find("slot_hookup[0]: \"etzhome.tv1\"")!=std::string::npos &&
        tvText.find("addon: _nameless.a.3")!=std::string::npos &&
        tvText.find("fuel_relative: 0.5")!=std::string::npos,error);
    check("tv-output-reparses",tvOk && inspectHomeAccessoriesText(tvText,&inventory,&error) && inventory.entries.size()==5,error);
    out="stale";
    check("tv-duplicate-install-refused",!installScaniaTvPresetText(tvText,"_nameless.2",&out,&error) && out.empty(),error);
    out="stale";
    check("tv-other-model-refused",!installScaniaTvPresetText(preset,"_nameless.3",&out,&error) && out.empty(),error);
    out="stale";
    check("tv-missing-bus-log-refused",!installScaniaTvPresetText(changed(preset,"bus_job_log: _nameless.f.1","no_log: _nameless.f.1"),"_nameless.2",&out,&error) && out.empty(),error);
    out="stale";
    check("tv-new-bus-schema-refused",!installScaniaTvPresetText(changed(preset,"version: 0","version: 1"),"_nameless.2",&out,&error) && out.empty(),error);
    out="stale";
    check("tv-existing-job-refused",!installScaniaTvPresetText(changed(preset,"entries: 0","entries: 1 entries[0]: _nameless.1"),"_nameless.2",&out,&error) && out.empty(),error);
    out="stale";
    check("tv-malformed-register-array-refused",!installScaniaTvPresetText(changed(preset,"entries: 0","entries: 1"),"_nameless.2",&out,&error) && out.empty(),error);
    auto prior=changed(preset,"entries: 0","entries: 1 entries[0]: \"other.h\"");
    prior.insert(prior.rfind('}'),"\naccessory_hookup_data : other.h { model: \"/home/other.pmd\" }\n");
    check("tv-preserves-existing-registry",installScaniaTvPresetText(prior,"_nameless.2",&out,&error) &&
        out.find("entries[0]: \"other.h\"")!=std::string::npos && out.find("entries[1]: \"etzhome.tv1\"")!=std::string::npos,error);
    auto collision=preset;collision.insert(collision.rfind('}'),
        "\naccessory_hookup_data : etzhome.tv1 { model: \"/home/existing.pmd\" }\n"
        "vehicle_addon_accessory : _nameless.7ffd.0001 { data_path: \"/def/existing.sii\" }\n");
    check("tv-allocates-unique-ids",installScaniaTvPresetText(collision,"_nameless.2",&out,&error) &&
        out.find("entries[0]: \"etzhome.tv2\"")!=std::string::npos &&
        out.find("accessories[2]: _nameless.7ffd.2")!=std::string::npos,error);
    auto withBase=changed(preset,"accessories: 2","accessories: 3 accessories[2]: _nameless.c.9");
    const std::string glass="\nvehicle_addon_accessory : _nameless.c.9 { slot_name: 1 slot_name[0]: slot_0 "
        "slot_hookup: 1 slot_hookup[0]: \"existing.addon_hookup\" "
        "data_path: \"/def/vehicle/truck/scania.s_2016/accessory/set_glass/set1.dlc_toys.sii\" refund: 123 }\n";
    withBase.insert(withBase.rfind('}'),glass);
    check("tv-preserves-existing-mounts",installScaniaTvPresetText(withBase,"_nameless.2",&out,&error) &&
        out.find("slot_hookup[0]: \"existing.addon_hookup\"")!=std::string::npos &&
        out.find("slot_hookup[1]: \"etzhome.tv1\"")!=std::string::npos && out.find("refund: 123")!=std::string::npos,error);
    out="stale";
    check("tv-occupied-slot-refused",!installScaniaTvPresetText(changed(withBase,"slot_name[0]: slot_0","slot_name[0]: slot_1"),"_nameless.2",&out,&error) && out.empty(),error);
    out="stale";
    check("tv-shared-base-refused",!installScaniaTvPresetText(changed(withBase,"accessories[0]: _nameless.b.1","accessories[0]: _nameless.c.9"),"_nameless.2",&out,&error) && out.empty(),error);
    out="stale";
    check("tv-cancel-refused",!installScaniaTvPresetText(preset,"_nameless.2",&out,&error,[]{return true;}) && out.empty(),error);
    check("tv-required-dlc-accepted",validateScaniaTvPresetInfo("SiiNunit { save_container : _nameless.1 { dependencies: 2 dependencies[0]: \"rdlc|eut2_toys|DLC - Cabin Accessories\" dependencies[1]: \"dlc|eut2_north|DLC - Scandinavia\" } }",&error),error);
    check("tv-required-dlc-dlc-prefix-accepted",validateScaniaTvPresetInfo("SiiNunit { save_container : _nameless.1 { dependencies: 1 dependencies[0]: \"dlc|eut2_toys|DLC - Cabin Accessories\" } }",&error),error);
    check("tv-required-dlc-legacy-bare-accepted",validateScaniaTvPresetInfo("SiiNunit { save_container : _nameless.1 { dependencies: 1 dependencies[0]: \"dlc_toys\" } }",&error),error);
    check("tv-missing-dlc-refused",!validateScaniaTvPresetInfo("SiiNunit { save_container : _nameless.1 { dependencies: 0 } }",&error),error);
    check("tv-other-dlcs-refused",!validateScaniaTvPresetInfo("SiiNunit { save_container : _nameless.1 { dependencies: 2 dependencies[0]: \"rdlc|eut2_halloween|DLC - Halloween Paint Jobs Pack\" dependencies[1]: \"dlc|eut2_iberia|DLC - Iberia\" } }",&error),error);
    namespace fs=std::filesystem;
    const fs::path base=fs::canonical(applicationDir());
    const fs::path root=base/(L"home_fixture_"+std::to_wstring(GetCurrentProcessId())+L"_"+std::to_wstring(GetTickCount64()));
    bool owned=false;
    auto cleanup=[&] {
        if(!owned) return;
        if(fs::canonical(root).parent_path()!=base || (GetFileAttributesW(root.c_str())&FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("fixture cleanup path mismatch");
        fs::remove_all(root);owned=false;
    };
    try {
        if(!fs::create_directory(root)) throw std::runtime_error("fixture collision");
        owned=true;const fs::path slotDir=root/L"save"/L"1";fs::create_directories(slotDir);
        const std::string info="SiiNunit { save_container : _nameless.1 { name: \"original\" time: 1 file_time: 1 version: 1 dependencies: 0 } }";
        put(slotDir/L"game.sii",source);put(slotDir/L"info.sii",info);put(slotDir/L"preview.tga","pixels");
        SaveSlot slot;slot.profileDir=root.wstring();slot.slotDir=slotDir.wstring();slot.gameSii=(slotDir/L"game.sii").wstring();
        check("slot-inspection",inspectHomeAccessoriesSlot(slot,&inventory,&error) && inventory.entries.size()==4,error);
        SaveCopyOptions options;options.replacementGameText=&expected;
        const auto copy=copySaveSlotInProfile(slot,"Home 中文副本",options);
        check("publish-copy-preserves-source",copy.ok && get(slotDir/L"game.sii")==source && get(slotDir/L"info.sii")==info &&
              get(fs::path(copy.slotDir)/L"game.sii")==expected && get(fs::path(copy.slotDir)/L"preview.tga")=="pixels",copy.error);
        SaveSlot edited=slot;edited.slotDir=copy.slotDir;edited.gameSii=(fs::path(copy.slotDir)/L"game.sii").wstring();
        check("published-copy-readable",copy.ok && inspectHomeAccessoriesSlot(edited,&inventory,&error),error);
        put(slotDir/L"game.sii","BSII invalid fixture");
        check("invalid-bsii-slot-refused",!inspectHomeAccessoriesSlot(slot,&inventory,&error) && inventory.entries.empty(),error);
        const auto assetsHome=root/L"asset_home";fs::create_directory(assetsHome);
        if(scaniaTvPresetAvailable()) {
            {
                ScaniaTvPresetAssets assets;
                const bool installed=assets.install(assetsHome.wstring(),&error);
                check("tv-embedded-assets-install",installed && assets.createdNew() &&
                    fs::file_size(assetsHome/L"MoRan/scania/TV/model/MoRan_3TV.pmg")==183156,error);
                check("tv-assets-rollback",assets.rollback(&error) && !fs::exists(assetsHome/L"MoRan"),error);
            }
            {
                ScaniaTvPresetAssets assets;int calls=0;
                const bool ok=assets.install(assetsHome.wstring(),&error,[&]{if(++calls>=8)throw std::runtime_error("fixture cancellation");});
                check("tv-asset-cancel-cleans-stage",!ok && fs::is_empty(assetsHome),error);
            }
            {
                ScaniaTvPresetAssets assets;
                check("tv-assets-commit",assets.install(assetsHome.wstring(),&error),error);assets.commit();
            }
            {
                ScaniaTvPresetAssets assets;
                const bool ok=assets.install(assetsHome.wstring(),&error);
                check("tv-identical-assets-reused",ok && !assets.createdNew(),error);
                check("tv-reuse-rollback-preserves-files",assets.rollback(&error) &&
                    fs::exists(assetsHome/L"MoRan/scania/TV/model/MoRan_3TV.pmd"),error);
            }
            const auto pmd=assetsHome/L"MoRan/scania/TV/model/MoRan_3TV.pmd";
            put(pmd,"user-owned-model");
            {
                ScaniaTvPresetAssets assets;
                check("tv-resource-conflict-preserves-user-file",!assets.install(assetsHome.wstring(),&error) && get(pmd)=="user-owned-model",error);
            }
            const auto transactionHome=root/L"transaction_home";fs::create_directory(transactionHome);
            put(slotDir/L"game.sii",preset);
            ScaniaTvPresetAssets assets;
            const bool installed=assets.install(transactionHome.wstring(),&error);
            SaveCopyOptions imported;imported.replacementGameText=&tvText;
            const auto result=copySaveSlotInProfile(slot,"三屏电视测试",imported);
            check("tv-save-and-assets-transaction",installed && result.ok && get(slotDir/L"game.sii")==preset &&
                get(fs::path(result.slotDir)/L"game.sii")==tvText,result.error);
            if(result.ok) assets.commit();else assets.rollback(&error);
            const auto failHome=root/L"failure_home";fs::create_directory(failHome);
            ScaniaTvPresetAssets failedAssets;
            const bool installedFail=failedAssets.install(failHome.wstring(),&error);
            imported.afterFile=[](size_t){return false;};
            const auto failure=copySaveSlotInProfile(slot,"失败测试",imported);
            check("tv-copy-failure-rolls-back-assets",installedFail && !failure.ok && failedAssets.rollback(&error) &&
                  fs::is_empty(failHome) && get(slotDir/L"game.sii")==preset,error);
        } else check("tv-resource-tests-unavailable",true,"构建未包含本地预设素材，未验证资源安装");
        cleanup();
    } catch(const std::exception& e) {
        check("isolated-filesystem-fixture",false,e.what());
        try {cleanup();} catch(const std::exception& detail) {check("fixture-cleanup",false,detail.what());}
    }
    return results;
}
}
