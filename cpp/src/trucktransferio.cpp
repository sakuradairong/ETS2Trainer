#include "trucktransferio.h"
#include "bsii.h"
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
