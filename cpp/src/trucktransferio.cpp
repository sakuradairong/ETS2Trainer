#include "trucktransferio.h"
#include "bsii.h"
#include "homepreset.h"
#include "fueltank.h"
#include "fueltankassets.h"
#include <filesystem>
#include <cstring>
#include <memory>
#include <stdexcept>
namespace ets2 {
namespace {
namespace fs=std::filesystem;
struct ReadLock {
    HANDLE h=INVALID_HANDLE_VALUE;
    explicit ReadLock(const std::wstring& path) {
        h=::CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                       FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        BY_HANDLE_FILE_INFORMATION info{};
        if(h==INVALID_HANDLE_VALUE || !::GetFileInformationByHandle(h,&info) ||
           (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))) {
            if(h!=INVALID_HANDLE_VALUE) ::CloseHandle(h);
            h=INVALID_HANDLE_VALUE;
            throw std::runtime_error("无法稳定读取存档，请退出游戏并等待同步完成；重解析文件不支持导入");
        }
    }
    ~ReadLock(){if(h!=INVALID_HANDLE_VALUE)::CloseHandle(h);}
    ReadLock(const ReadLock&)=delete;
};
bool loadText(const std::wstring& path,std::string* out,std::string* error,size_t limit,
              const std::function<bool()>& canceled = {}) {
    out->clear();
    if(canceled && canceled()) { if(error)*error="读取存档已取消";return false; }
    std::vector<uint8_t> bytes; DecryptInfo info;
    if(!decryptSave(path,bytes,&info,error,limit)) return false;
    if(info.innerFormat=="bsii") {
        std::string detail;
        if(decodeBsiiText(bytes,out,&detail,canceled,limit)) return true;
        if(error)*error="二进制存档转换失败："+detail;
        return false;
    }
    if(info.innerFormat!="text" || bytes.size()<8 || ::memcmp(bytes.data(),"SiiNunit",8)) {
        if(error)*error="存档格式无法识别；支持加密存档、BSII v3 和 SiiNunit 文本存档。";
        return false;
    }
    out->assign(bytes.begin(),bytes.end()); return true;
}
bool sameSlot(const SaveSlot& a,const SaveSlot& b) {
    return ::_wcsicmp(a.slotDir.c_str(),b.slotDir.c_str())==0 &&
           ::_wcsicmp(a.profileDir.c_str(),b.profileDir.c_str())==0;
}
}
bool inspectTruckTransferSlot(const SaveSlot& slot,TransferInventory* inventory,std::string* error,
                              std::function<bool()> canceled) {
    if(inventory)*inventory={};
    try {
        ReadLock game(slot.gameSii);
        std::string text;
        return loadText(slot.gameSii,&text,error,128ull*1024*1024,canceled) &&
               inspectTruckTransferText(text,inventory,error,canceled);
    } catch(const std::exception& e) {if(error)*error=e.what();return false;}
}
bool inspectFleetSlot(const SaveSlot& slot,FleetInventory* inventory,std::string* error,
                     std::function<bool()> canceled) {
    if(inventory) *inventory={};
    try {
        if(selectedGame()!=GameId::Ets2) throw std::runtime_error("车队经济管理目前仅支持欧卡2");
        ReadLock game(slot.gameSii); std::string text;
        return loadText(slot.gameSii,&text,error,128ull*1024*1024,canceled) &&
               inspectFleetText(text,inventory,error,canceled);
    } catch(const std::exception& e) {if(error)*error=e.what();return false;}
}
SaveCopyResult fleetToNewSave(const FleetRequest& request,std::function<bool()> canceled) {
    SaveCopyResult failure;failure.game=request.game;
    try {
        auto gate=[&] {
            if(canceled && canceled()) throw std::runtime_error("车队扩充已取消");
            if(request.game!=GameId::Ets2 || selectedGame()!=request.game)
                throw std::runtime_error("车队经济管理目前仅支持欧卡2，请重新选择存档");
            std::string error;
            if(copyBlockedByRunningGame(request.game,&error)) throw std::runtime_error(error);
        };
        gate();
        SaveSlot fresh;bool found=false;
        for(const auto& slot:listSlots()) if(sameSlot(slot,request.source)) {fresh=slot;found=true;break;}
        if(!found) throw std::runtime_error("存档不属于当前游戏，请刷新后重新选择");
        ReadLock game(fresh.gameSii),info((fs::path(fresh.slotDir)/L"info.sii").wstring());
        std::string text,result,error,escaped;
        if(!siiEscapeName(request.displayName,&escaped,&error) ||
           !loadText(fresh.gameSii,&text,&error,128ull*1024*1024,canceled) ||
           !expandFleetText(text,request.expansion,&result,&error,canceled))
            throw std::runtime_error(error);
        gate();
        return copySlotToNewSave({request.game,fresh,request.displayName},std::move(canceled),&result);
    } catch(const std::exception& e) {failure.error=e.what();return failure;}
}
bool inspectHomeAccessoriesSlot(const SaveSlot& slot,HomeAccessoryInventory* inventory,
                               std::string* error,std::function<bool()> canceled) {
    if(inventory) *inventory={};
    if(selectedGame()!=GameId::Ets2) {
        if(error) *error="Home 配件代码目前仅支持欧卡2";
        return false;
    }
    try {
        ReadLock game(slot.gameSii);
        std::string text;
        return loadText(slot.gameSii,&text,error,128ull*1024*1024,canceled) &&
               inspectHomeAccessoriesText(text,inventory,error,canceled);
    } catch(const std::exception& e) {if(error)*error=e.what();return false;}
}

SaveCopyResult homeAccessoryToNewSave(const HomeAccessoryRequest& request,
                                     std::function<bool()> canceled) {
    SaveCopyResult failure; failure.game=request.game;
    try {
        auto gate=[&] {
            if(request.game!=GameId::Ets2 || selectedGame()!=request.game)
                throw std::runtime_error("Home 配件代码目前仅支持欧卡2，请重新选择存档");
            if(canceled && canceled()) throw std::runtime_error("Home 配件修改已取消");
            std::string error;
            if(copyBlockedByRunningGame(request.game,&error)) throw std::runtime_error(error);
        };
        gate();
        std::string error,escaped;
        if(request.scaniaTvPreset && request.tankCapacity) throw std::runtime_error("配件预设与油箱扩容不能同时执行");
        if((!request.scaniaTvPreset && !request.tankCapacity && !validateHomeAccessoryPath(request.newPath,&error)) ||
           !siiEscapeName(request.displayName,&escaped,&error)) throw std::runtime_error(error);
        SaveSlot fresh; bool found=false;
        for(const auto& slot:listSlots()) {
            if(sameSlot(slot,request.source)){fresh=slot;found=true;break;}
        }
        if(!found) throw std::runtime_error("源存档不属于当前游戏，请刷新并重新选择");
        ReadLock game(fresh.gameSii);
        ReadLock info((fs::path(fresh.slotDir)/L"info.sii").wstring());
        std::string text,replacement;
        if(request.scaniaTvPreset) {
            std::string metadata;
            if(!loadText((fs::path(fresh.slotDir)/L"info.sii").wstring(),&metadata,&error,4ull*1024*1024,canceled) ||
               !validateScaniaTvPresetInfo(metadata,&error) ||
               !loadText(fresh.gameSii,&text,&error,128ull*1024*1024,canceled) ||
               !installScaniaTvPresetText(text,request.truckId,&replacement,&error,canceled))
                throw std::runtime_error(error);
            gate();
            ScaniaTvPresetAssets assets;
            if(!assets.install(documentsDir(),&error,gate)) throw std::runtime_error(error);
            auto result=copySlotToNewSave({request.game,fresh,request.displayName},canceled,&replacement);
            if(result.ok) assets.commit();
            else {
                std::string rollbackError;
                if(!assets.rollback(&rollbackError)) result.error+="；"+rollbackError;
            }
            return result;
        }
        HomeAccessoryInventory inventory;
        if(!loadText(fresh.gameSii,&text,&error,128ull*1024*1024,canceled) ||
           !inspectHomeAccessoriesText(text,&inventory,&error,canceled))
            throw std::runtime_error(error);
        const HomeAccessoryEntry* chosen=nullptr;
        for(const auto& entry:inventory.entries)
            if(entry.truckId==request.truckId && entry.unitId==request.unitId) {chosen=&entry;break;}
        if(!chosen || chosen->shared || chosen->dataPath!=request.originalPath)
            throw std::runtime_error("所选配件已改变或被共享，请重新读取并预览");
        if(request.tankCapacity) {
            if(chosen->type!="vehicle_accessory" || chosen->dataPath.find("/chassis/")==std::string::npos)
                throw std::runtime_error("请选择受支持的原厂底盘配件");
            std::vector<std::string> engines;
            for(const auto& accessory:inventory.entries)if(accessory.truckId==request.truckId &&
                accessory.dataPath.find("/engine/")!=std::string::npos)engines.push_back(accessory.dataPath);
            FuelTankMod mod;
            if(!buildFuelTankMod(chosen->dataPath,request.tankCapacity,&mod,&error) ||
               !validateFuelTankPowertrain(mod,engines,&error) ||
               !replaceHomeAccessoryPath(text,request.truckId,request.unitId,mod.chassisPath,&replacement,&error,canceled))
                throw std::runtime_error(error);
            gate();FuelTankModAssets assets;
            if(!assets.install(documentsDir(),mod.fileName,mod.archive,&error,gate))throw std::runtime_error(error);
            auto result=copySlotToNewSave({request.game,fresh,request.displayName},canceled,&replacement);
            if(result.ok) {
                assets.commit();
                result.note=std::string(mod.electric?"永久电池 ":"永久油箱 ")+std::to_string(mod.originalCapacity)+" → "+std::to_string(mod.capacity)+
                    (mod.electric?" kWh":" 升")+"。请先在 Mod 管理器启用「"+mod.displayName+"」，再加载这个副本；使用副本期间保持启用。"
                    "Mod 文件："+mod.fileName+"。原底盘路径："+mod.originalPath+"。此方案供单人使用。";
            } else {
                std::string rollbackError;if(!assets.rollback(&rollbackError))result.error+="；"+rollbackError;
            }
            return result;
        }
        if(request.newPath==chosen->dataPath) throw std::runtime_error("新路径与当前路径相同，无需修改");
        if(!replaceHomeAccessoryPath(text,request.truckId,request.unitId,request.newPath,
                                     &replacement,&error,canceled)) throw std::runtime_error(error);
        gate();
        return copySlotToNewSave({request.game,fresh,request.displayName},std::move(canceled),&replacement);
    } catch(const std::exception& e) {failure.error=e.what();return failure;}
}
bool inspectTrailerSwapSlot(const SaveSlot& slot,TrailerSwapInventory* inventory,
                            std::string* error,std::function<bool()> canceled) {
    if(inventory) *inventory={};
    try {
        ReadLock game(slot.gameSii);
        std::string text;
        return loadText(slot.gameSii,&text,error,128ull*1024*1024,canceled) &&
               inspectTrailerSwapText(text,inventory,error,canceled);
    } catch(const std::exception& e) {if(error)*error=e.what();return false;}
}

SaveCopyResult trailerSwapToNewSave(const TrailerSwapRequest& request,
                                    std::function<bool()> canceled) {
    SaveCopyResult failure;failure.game=request.game;
    try {
        auto gate=[&] {
            if(canceled && canceled()) throw std::runtime_error("更换挂车已取消");
            if(selectedGame()!=request.game)
                throw std::runtime_error("游戏选择已改变，请刷新并重新选择存档");
            std::string error;
            if(copyBlockedByRunningGame(request.game,&error)) throw std::runtime_error(error);
        };
        gate();
        std::string error,escaped;
        if(!siiEscapeName(request.displayName,&escaped,&error)) throw std::runtime_error(error);
        SaveSlot fresh; bool found=false;
        for(const auto& slot:listSlots()) {
            if(sameSlot(slot,request.source)){fresh=slot;found=true;break;}
        }
        if(!found) throw std::runtime_error("源存档不属于当前游戏，请刷新并重新选择");
        ReadLock game(fresh.gameSii);
        ReadLock info((fs::path(fresh.slotDir)/L"info.sii").wstring());
        std::string text,replacement;
        if(!loadText(fresh.gameSii,&text,&error,128ull*1024*1024,canceled) ||
           !swapTrailerText(text,request.targetTrailerId,&replacement,&error,canceled))
            throw std::runtime_error(error);
        gate();
        return copySlotToNewSave({request.game,fresh,request.displayName},std::move(canceled),
                                 &replacement);
    } catch(const std::exception& e) {failure.error=e.what();return failure;}
}

bool inspectTruckTransferTarget(const SaveSlot& source,const SaveSlot& target,
                                TransferInventory* inventory,std::string* error,
                                std::function<bool()> canceled) {
    if(inventory)*inventory={};
    try {
        ReadLock sourceInfo((fs::path(source.slotDir)/L"info.sii").wstring());
        ReadLock targetInfo((fs::path(target.slotDir)/L"info.sii").wstring());
        std::string srcInfo,dstInfo;
        if(!loadText((fs::path(source.slotDir)/L"info.sii").wstring(),&srcInfo,error,4*1024*1024,canceled) ||
           !loadText((fs::path(target.slotDir)/L"info.sii").wstring(),&dstInfo,error,4*1024*1024,canceled) ||
           !validateTruckTransferMetadata(srcInfo,dstInfo,error)) return false;
        return inspectTruckTransferSlot(target,inventory,error,canceled);
    } catch(const std::exception& e) {if(error)*error=e.what();return false;}
}
SaveCopyResult transferTruckToNewSave(const TruckTransferRequest& request,std::function<bool()> canceled) {
    SaveCopyResult failure;failure.game=request.game;
    try {
        auto gate=[&] {
            if(canceled && canceled()) throw std::runtime_error("车辆导入已取消");
            if(selectedGame()!=request.game) throw std::runtime_error("游戏选择已改变，请重新选择源与目标");
            std::string error;
            if(copyBlockedByRunningGame(request.game,&error)) throw std::runtime_error(error);
        };
        gate();
        SaveSlot source,target; bool hasSource=false,hasTarget=false;
        for(const auto& slot:listSlots()) {
            if(sameSlot(slot,request.source)){source=slot;hasSource=true;}
            if(sameSlot(slot,request.target)){target=slot;hasTarget=true;}
        }
        if(!hasSource || !hasTarget) throw std::runtime_error("源或目标存档不属于当前游戏，请刷新后重新选择");
        if(fs::equivalent(source.gameSii,target.gameSii))
            throw std::runtime_error("请选择另一个目标存档，源存档与目标不能相同");
        // Keep both game and metadata snapshots immutable until the new slot is published.
        ReadLock sourceGame(source.gameSii),targetGame(target.gameSii);
        ReadLock sourceInfo((fs::path(source.slotDir)/L"info.sii").wstring());
        ReadLock targetInfo((fs::path(target.slotDir)/L"info.sii").wstring());
        std::string src,dst,srcInfo,dstInfo,error,result;
        if(!loadText(source.gameSii,&src,&error,128ull*1024*1024,canceled) ||
           !loadText(target.gameSii,&dst,&error,128ull*1024*1024,canceled) ||
           !loadText((fs::path(source.slotDir)/L"info.sii").wstring(),&srcInfo,&error,4*1024*1024,canceled) ||
           !loadText((fs::path(target.slotDir)/L"info.sii").wstring(),&dstInfo,&error,4*1024*1024,canceled))
            throw std::runtime_error(error);
        if(!validateTruckTransferMetadata(srcInfo,dstInfo,&error))
            throw std::runtime_error(error);
        gate();
        if(!transferTruckText(src,dst,request.truckId,request.garageId,&result,&error,canceled))
            throw std::runtime_error("车辆数据校验失败："+error);
        gate();
        SaveCopyRequest copy{request.game,target,request.displayName};
        return copySlotToNewSave(copy,std::move(canceled),&result);
    } catch(const std::exception& e) {failure.error=e.what();return failure;}
}
}
