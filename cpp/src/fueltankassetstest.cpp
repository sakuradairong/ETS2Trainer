#include "fueltankassetstest.h"
#include "fueltankassets.h"
#include "fueltank.h"
#include "fueltank_definitions.h"
#include "trucktransferio.h"
#include <filesystem>
#include <fstream>
#include <stdexcept>
namespace ets2 {
std::vector<TunerTestItem> runFuelTankAssetTests() {
    std::vector<TunerTestItem> tests;
    auto check=[&](const char* name,bool ok,const std::string& detail=""){tests.push_back({name,ok,detail});};
    if(std::size(kFuelTankDefinitions)==0) {
        HomeAccessoryRequest request;request.game=GameId::Ats;request.tankCapacity=5000;
        check("fuel-without-local-catalog-rejects-ats",!homeAccessoryToNewSave(request).ok);
        request.game=GameId::Ets2;
        check("fuel-without-local-catalog-cancel-before-files",!homeAccessoryToNewSave(request,[]{return true;}).ok);
        return tests;
    }
    std::string error;FuelTankMod mod;
    const std::string original="/def/vehicle/truck/volvo.fh_2024/chassis/6x4.sii";
    const bool built=buildFuelTankMod(original,3000,&mod,&error);
    check("fuel-assets-builder-ready",built,error);if(!built)return tests;
    const std::string source="SiiNunit {\nplayer : _nameless.1 { trucks: 2 trucks[0]: _nameless.2 trucks[1]: _nameless.3 }\n"
        "vehicle : _nameless.2 { accessories: 1 accessories[0]: _nameless.a fuel_relative: 0.5 }\n"
        "vehicle : _nameless.3 { accessories: 1 accessories[0]: _nameless.b fuel_relative: 0.7 }\n"
        "vehicle_accessory : _nameless.a { data_path: \""+original+"\" }\n"
        "vehicle_accessory : _nameless.b { data_path: \""+original+"\" }\n}\n";
    auto expected=source;expected.replace(expected.find(original),original.size(),mod.chassisPath);
    std::string edited;
    check("fuel-single-truck-save-edit-is-one-path-only",replaceHomeAccessoryPath(source,"_nameless.2","_nameless.a",mod.chassisPath,
        &edited,&error)&&edited==expected,error);
    {
        FuelTankMod battery;const std::string path="/def/vehicle/truck/scania.s_2024e/chassis/4x2.sii";
        const bool ready=buildFuelTankMod(path,5000,&battery,&error);
        auto electricSource=source;
        std::size_t at=0;while((at=electricSource.find(original,at))!=std::string::npos){electricSource.replace(at,original.size(),path);at+=path.size();}
        auto electricExpected=electricSource;electricExpected.replace(electricExpected.find(path),path.size(),battery.chassisPath);
        check("battery-single-truck-edit-preserves-other-truck-and-charge-ratios",ready&&
            replaceHomeAccessoryPath(electricSource,"_nameless.2","_nameless.a",battery.chassisPath,&edited,&error)&&
            edited==electricExpected,error);
    }
    HomeAccessoryRequest request;request.game=GameId::Ats;request.tankCapacity=3000;
    check("fuel-production-rejects-ats-before-install",!homeAccessoryToNewSave(request).ok);
    request.game=GameId::Ets2;
    check("fuel-production-cancel-before-install",!homeAccessoryToNewSave(request,[]{return true;}).ok);
    wchar_t temp[MAX_PATH]{};GetTempPathW(MAX_PATH,temp);
    const auto home=std::filesystem::path(temp)/(L"ets2-fuel-assets-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    const bool created=CreateDirectoryW(home.c_str(),nullptr)!=FALSE;
    check("fuel-assets-isolated-test-home",created);if(!created)return tests;
    const auto target=home/L"mod"/std::filesystem::path(mod.fileName);
    auto exists=[&]{return GetFileAttributesW(target.c_str())!=INVALID_FILE_ATTRIBUTES;};
    {
        FuelTankModAssets assets;
        check("fuel-assets-install-new",assets.install(home.wstring(),mod.fileName,mod.archive,&error)&&exists(),error);
    }
    check("fuel-assets-uncommitted-install-rolls-back",!exists());
    {
        FuelTankModAssets assets;int checks=0;
        const auto canceled=assets.install(home.wstring(),mod.fileName,mod.archive,&error,[&]{if(++checks==3)throw std::runtime_error("canceled after write");});
        check("fuel-assets-late-cancel-removes-own-file",!canceled&&!exists(),error);
    }
    {
        FuelTankModAssets assets;
        const bool ok=assets.install(home.wstring(),mod.fileName,mod.archive,&error);if(ok)assets.commit();
        check("fuel-assets-commit-persists",ok,error);
    }
    check("fuel-assets-committed-file-remains",exists());
    {
        FuelTankModAssets assets;
        const bool reused=assets.install(home.wstring(),mod.fileName,mod.archive,&error);
        check("fuel-assets-reuses-identical-package",reused,error);
        check("fuel-assets-reuse-rollback-preserves-original",assets.rollback(&error)&&exists(),error);
    }
    {
        FuelTankModAssets assets;auto different=mod.archive;different.back()^=1;
        check("fuel-assets-conflict-refuses-overwrite",!assets.install(home.wstring(),mod.fileName,different,&error)&&exists(),error);
    }
    {
        std::ifstream file(target,std::ios::binary);std::vector<uint8_t> actual((std::istreambuf_iterator<char>(file)),{});
        check("fuel-assets-conflict-preserves-all-bytes",actual==mod.archive);
    }
    {
        FuelTankModAssets assets;
        check("fuel-assets-refuses-path-traversal",!assets.install(home.wstring(),"ets2trainer_fuel_../escape.scs",mod.archive,&error),error);
    }
    check("fuel-assets-test-cleanup",DeleteFileW(target.c_str())&&RemoveDirectoryW((home/L"mod").c_str())&&RemoveDirectoryW(home.c_str()));
    return tests;
}
}
