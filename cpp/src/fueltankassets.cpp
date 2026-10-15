#include "fueltankassets.h"
#include "common.h"
#include <filesystem>
#include <stdexcept>
#include <algorithm>
namespace ets2 {
namespace {
namespace fs=std::filesystem;
void needFuelAsset(bool ok,const char* error) {if(!ok)throw std::runtime_error(error);}
bool sameFuelAsset(HANDLE handle,const std::vector<std::uint8_t>& bytes) {
    LARGE_INTEGER size{},start{};
    if(!GetFileSizeEx(handle,&size)||size.QuadPart!=(LONGLONG)bytes.size()||
       !SetFilePointerEx(handle,start,nullptr,FILE_BEGIN))return false;
    std::vector<std::uint8_t> actual(bytes.size());DWORD count=0;
    return ReadFile(handle,actual.data(),(DWORD)actual.size(),&count,nullptr) && count==bytes.size() && actual==bytes;
}
}
struct FuelTankModAssets::Impl {
    std::vector<HANDLE> parents;
    HANDLE file=INVALID_HANDLE_VALUE;
    bool created=false,committed=false;
    ~Impl() {
        if(file!=INVALID_HANDLE_VALUE) {
            if(created&&!committed) {FILE_DISPOSITION_INFO info{TRUE};SetFileInformationByHandle(file,FileDispositionInfo,&info,sizeof(info));}
            CloseHandle(file);
        }
        for(auto it=parents.rbegin();it!=parents.rend();++it)CloseHandle(*it);
    }
    void holdDirectory(const fs::path& p) {
        const auto h=CreateFileW(p.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        BY_HANDLE_FILE_INFORMATION info{};
        if(h==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(h,&info)||
           !(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) {
            if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);
            throw std::runtime_error("Mod 目录不可用、正在变化或含重解析点");
        }
        parents.push_back(h);
    }
};
FuelTankModAssets::FuelTankModAssets():impl_(std::make_unique<Impl>()){}
FuelTankModAssets::~FuelTankModAssets()=default;
bool FuelTankModAssets::install(const std::wstring& gameHome,const std::string& fileName,
    const std::vector<std::uint8_t>& bytes,std::string* error,std::function<void()> guard) {
    if(error)error->clear();
    try {
        needFuelAsset(impl_->file==INVALID_HANDLE_VALUE,"Mod 安装器不能重复使用");
        needFuelAsset(fileName.rfind("ets2trainer_fuel_",0)==0 && fileName.size()>25 && fileName.size()<100 &&
            fileName.substr(fileName.size()-4)==".scs" && fileName.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_.")==std::string::npos,
            "油箱 Mod 文件名无效");
        needFuelAsset(!bytes.empty() && bytes.size()<=4u*1024u*1024u,"油箱 Mod 体积无效");
        if(guard)guard();
        const fs::path home(gameHome);
        needFuelAsset(home.is_absolute()&&home.lexically_normal()==home,"游戏文档目录必须是规范绝对路径");
        fs::path parent=home.root_path();impl_->holdDirectory(parent);
        for(const auto& part:home.relative_path()){parent/=part;impl_->holdDirectory(parent);}
        const auto mod=home/L"mod";
        if(!CreateDirectoryW(mod.c_str(),nullptr))needFuelAsset(GetLastError()==ERROR_ALREADY_EXISTS,"无法创建 Mod 目录");
        impl_->holdDirectory(mod);
        if(guard)guard();
        const auto target=mod/fs::path(fileName);
        impl_->file=CreateFileW(target.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,FILE_SHARE_READ,nullptr,CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(impl_->file==INVALID_HANDLE_VALUE) {
            needFuelAsset(GetLastError()==ERROR_FILE_EXISTS||GetLastError()==ERROR_ALREADY_EXISTS,"无法创建油箱 Mod 文件");
            impl_->file=CreateFileW(target.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            BY_HANDLE_FILE_INFORMATION info{};
            needFuelAsset(impl_->file!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(impl_->file,&info)&&
                !(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)),"无法稳定读取已有油箱 Mod");
            needFuelAsset(sameFuelAsset(impl_->file,bytes),"已有同名 Mod 的内容不同，已保留原文件");
        } else {
            impl_->created=true;DWORD written=0;
            needFuelAsset(WriteFile(impl_->file,bytes.data(),(DWORD)bytes.size(),&written,nullptr)&&written==bytes.size()&&
                FlushFileBuffers(impl_->file)&&sameFuelAsset(impl_->file,bytes),"油箱 Mod 写入或回读校验失败");
        }
        if(guard)guard();
        return true;
    } catch(const std::exception& e) {
        if(error)*error=e.what();std::string rollbackError;
        if(!rollback(&rollbackError)&&error)*error+="；"+rollbackError;
        return false;
    }
}
bool FuelTankModAssets::rollback(std::string* error) {
    if(error)error->clear();
    if(impl_->file==INVALID_HANDLE_VALUE||!impl_->created||impl_->committed)return true;
    FILE_DISPOSITION_INFO info{TRUE};
    if(!SetFileInformationByHandle(impl_->file,FileDispositionInfo,&info,sizeof(info))) {
        if(error)*error="未能回滚新油箱 Mod，请检查文档目录 mod 文件夹";
        return false;
    }
    CloseHandle(impl_->file);impl_->file=INVALID_HANDLE_VALUE;impl_->created=false;return true;
}
void FuelTankModAssets::commit(){impl_->committed=true;}
}
