#include "savecopy.h"
#include "bsii.h"
#include "gameio.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <memory>
#include <set>
#include <stdexcept>
#include <ctime>
#include <tlhelp32.h>

namespace ets2 {
namespace {
namespace fs = std::filesystem;
constexpr uint64_t kFileLimit = 128ull * 1024 * 1024;
constexpr uint64_t kTotalLimit = 512ull * 1024 * 1024;
constexpr size_t kInfoLimit = 4 * 1024 * 1024;
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : value(h) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE) ::CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return value != INVALID_HANDLE_VALUE; }
};
bool samePath(const fs::path& a, const fs::path& b) { return ::_wcsicmp(a.c_str(), b.c_str()) == 0; }
bool reparse(const fs::path& p) {
    const DWORD a = ::GetFileAttributesW(p.c_str());
    return a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_REPARSE_POINT);
}
bool dots(const fs::path& p) { for (const auto& part : p) if (part == L".." || part == L".") return true; return false; }
uint64_t fileSize(HANDLE h) {
    LARGE_INTEGER n{};
    if (!::GetFileSizeEx(h,&n) || n.QuadPart < 0) throw std::runtime_error("无法读取文件长度");
    return static_cast<uint64_t>(n.QuadPart);
}
void rewind(HANDLE h) {
    LARGE_INTEGER zero{};
    if (!::SetFilePointerEx(h,zero,nullptr,FILE_BEGIN)) throw std::runtime_error("无法重置文件读取位置");
}
std::unique_ptr<Handle> lockDirectory(const fs::path& p) {
    auto h = std::make_unique<Handle>(::CreateFileW(p.c_str(),FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr));
    if (!*h) throw std::runtime_error("无法锁定存档目录，请结束其他文件操作后重试");
    return h; // no FILE_SHARE_DELETE: prevents the directory being renamed during the transaction
}
struct Token { std::string text; size_t begin, end; int depth; bool quoted; };
std::vector<Token> tokenize(const std::string& s) {
    std::vector<Token> out;
    int depth = 0;
    for (size_t i=0;i<s.size();) {
        const unsigned char c=static_cast<unsigned char>(s[i]);
        if (c==' ' || c=='\t' || c=='\r' || c=='\n') { ++i; continue; }
        if (c=='#' || (c=='/' && i+1<s.size() && s[i+1]=='/')) {
            i=s.find('\n',i); if(i==std::string::npos) break; continue;
        }
        const size_t begin=i; bool quoted=false;
        if (c=='"') {
            quoted=true; ++i; bool closed=false;
            while (i<s.size()) {
                if (s[i]=='\\') { if(i+1>=s.size()) break; i+=2; }
                else if (s[i]=='"') { ++i; closed=true; break; }
                else if (s[i]=='\r' || s[i]=='\n' || s[i]=='\0') break;
                else ++i;
            }
            if(!closed) throw std::runtime_error("info.sii 引号或转义不完整");
        } else if (std::strchr("{}:[]",c)) ++i;
        else {
            while(i<s.size() && !std::strchr(" \t\r\n{}:[]\"",s[i])) {
                if(s[i]=='\0') throw std::runtime_error("info.sii 含无效空字节");
                ++i;
            }
        }
        out.push_back({s.substr(begin,i-begin),begin,i,depth,quoted});
        if(!quoted && c=='{') ++depth;
        if(!quoted && c=='}' && --depth<0) throw std::runtime_error("info.sii 括号不匹配");
    }
    if(depth) throw std::runtime_error("info.sii 括号不完整");
    return out;
}
struct InfoFields { Token name; Token time; };
InfoFields infoFields(const std::string& text) {
    if(text.size()>kInfoLimit) throw std::runtime_error("info.sii 超过元信息长度上限");
    const auto t=tokenize(text);
    if(t.size()<10 || t[0].text!="SiiNunit" || t[1].text!="{" ||
       t[2].text!="save_container" || t[2].depth!=1 || t[3].text!=":" ||
       t[4].quoted || t[4].text.empty() || t[5].text!="{" ||
       t[t.size()-2].text!="}" || t[t.size()-2].depth!=2 ||
       t.back().text!="}" || t.back().depth!=1)
        throw std::runtime_error("info.sii 不是完整、唯一的 save_container 文本");
    InfoFields fields{}; int names=0,times=0;
    for(size_t i=6;i<t.size()-2;++i) {
        if(!t[i].quoted && (t[i].text=="{" || t[i].text=="}" || t[i].depth!=2))
            throw std::runtime_error("info.sii 含额外或嵌套单位");
        if(i+2>=t.size()-2 || t[i].quoted || t[i].depth!=2 || t[i+1].text!=":") continue;
        if(t[i].text=="name") { fields.name=t[i+2]; ++names; }
        if(t[i].text=="file_time") { fields.time=t[i+2]; ++times; }
    }
    if(names!=1 || times!=1) throw std::runtime_error("info.sii 的 name / file_time 必须各出现一次");
    if(fields.time.quoted || fields.time.text.empty() ||
       fields.time.text.find_first_not_of("0123456789")!=std::string::npos)
        throw std::runtime_error("info.sii 的 file_time 不是有效时间戳");
    if(!fields.name.quoted && (fields.name.text.empty() ||
       fields.name.text.find_first_of("{}:[]")!=std::string::npos))
        throw std::runtime_error("info.sii 的存档名称无效");
    return fields;
}
std::string nameFromInfo(const std::string& text) {
    const auto name=infoFields(text).name;
    return name.quoted ? siiUnescapeName(name.text.substr(1,name.text.size()-2)) : name.text;
}
std::string loadInfo(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if(!::GetFileAttributesExW(p.c_str(),GetFileExInfoStandard,&attr) ||
       attr.nFileSizeHigh || attr.nFileSizeLow>kInfoLimit) throw std::runtime_error("info.sii 读取失败或文件过大");
    std::vector<uint8_t> bytes; DecryptInfo info; std::string error;
    if(!decryptSave(p.wstring(),bytes,&info,&error,kInfoLimit)) throw std::runtime_error("读取元信息失败："+error);
    if(info.innerFormat=="bsii") {
        std::string text;
        if(!decodeBsiiText(bytes,&text,&error,{},kInfoLimit))
            throw std::runtime_error("元信息转换失败："+error);
        return text;
    }
    if(detectFormat(bytes)!="text" || bytes.size()>kInfoLimit) throw std::runtime_error("info.sii 必须是可读取的 SiiN 文本");
    return {bytes.begin(),bytes.end()};
}
void checkOptions(const SaveCopyOptions& options, bool full = true) {
    if(options.canceled && options.canceled()) throw std::runtime_error("复制已取消，原存档未修改");
    std::string error;
    if(full && options.guard && !options.guard(&error)) throw std::runtime_error(error.empty()?"复制条件发生变化":error);
}
bool existsEntry(const fs::path& p) {
    if(::GetFileAttributesW(p.c_str())!=INVALID_FILE_ATTRIBUTES) return true;
    const DWORD error=::GetLastError();
    if(error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND) return false;
    throw std::runtime_error("无法检查目标槽位是否存在");
}
fs::path physicalProfile(const SaveSlot& source, fs::path* slot) {
    const fs::path profileInput(source.profileDir), slotInput(source.slotDir);
    if(profileInput.empty() || slotInput.empty() || dots(profileInput) || dots(slotInput))
        throw std::runtime_error("存档路径为空或含相对目录跳转");
    const fs::path profile=fs::canonical(profileInput);
    const fs::path save=profile/L"save";
    if(reparse(profileInput) || reparse(save) || reparse(slotInput))
        throw std::runtime_error("存档目录是重解析点或不存在，已拒绝复制");
    *slot=fs::canonical(slotInput);
    if(!samePath(slot->parent_path(),save) || !fs::is_directory(*slot))
        throw std::runtime_error("源槽位不是该档案 save 目录的直接子目录");
    return profile;
}
void copyAndVerify(HANDLE source,const fs::path& dest,uint64_t length,const SaveCopyOptions& options) {
    Handle output(::CreateFileW(dest.c_str(),GENERIC_WRITE | GENERIC_READ,0,nullptr,
        CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(!output) throw std::runtime_error("创建副本文件失败");
    std::array<uint8_t,65536> original{},copied{};
    uint64_t done=0; rewind(source);
    while(done<length) {
        checkOptions(options, false);
        DWORD got=0,written=0;
        DWORD count=static_cast<DWORD>((std::min)(uint64_t(original.size()),length-done));
        if(!::ReadFile(source,original.data(),count,&got,nullptr) || got!=count ||
           !::WriteFile(output.value,original.data(),got,&written,nullptr) || written!=got)
            throw std::runtime_error("复制文件失败，源文件可能已变化");
        done+=got;
    }
    if(!::FlushFileBuffers(output.value) || fileSize(output.value)!=length || fileSize(source)!=length)
        throw std::runtime_error("副本长度或写入刷新校验失败");
    rewind(source); rewind(output.value); done=0;
    while(done<length) {
        checkOptions(options, false);
        DWORD a=0,b=0; DWORD count=static_cast<DWORD>((std::min)(uint64_t(original.size()),length-done));
        if(!::ReadFile(source,original.data(),count,&a,nullptr) || a!=count ||
           !::ReadFile(output.value,copied.data(),count,&b,nullptr) || b!=count ||
           ::memcmp(original.data(),copied.data(),count)) throw std::runtime_error("副本内容逐字节校验失败");
        done+=count;
    }
}
bool cleanupStage(const fs::path& profile,const fs::path& stage,const std::vector<fs::path>& files) {
    try {
        if(stage.empty()) return true;
        if(reparse(stage) || !samePath(fs::canonical(stage).parent_path(),profile)) return false;
        bool ok=true;
        for(const auto& file:files) {
            const auto p=stage/file;
            if(!existsEntry(p)) continue;
            if(!samePath(p.parent_path(),stage) || reparse(p) || !::DeleteFileW(p.c_str())) ok=false;
        }
        if(!::RemoveDirectoryW(stage.c_str())) ok=false;
        return ok;
    } catch(...) { return false; }
}
} // namespace

bool siiEscapeName(const std::string& utf8,std::string* out,std::string* error) {
    if(out) out->clear();
    auto fail=[&](const char* msg){if(error)*error=msg;return false;};
    if(utf8.empty() || utf8.size()>256 || trim(utf8).empty()) return fail("副本名称不能为空或过长");
    const int count=::MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,utf8.data(),static_cast<int>(utf8.size()),nullptr,0);
    if(!count) return fail("副本名称不是有效 UTF-8");
    std::wstring wide(count,L'\0');
    ::MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,utf8.data(),static_cast<int>(utf8.size()),wide.data(),count);
    size_t points=0;
    for(wchar_t c:wide) {
        if(c<0x20 || (c>=0x7F && c<=0x9F)) return fail("副本名称不能包含控制字符");
        if(c<0xDC00 || c>0xDFFF) ++points;
    }
    if(points>64) return fail("副本名称最多 64 个字符");
    std::string escaped; const char* hex="0123456789ABCDEF";
    for(unsigned char c:utf8) {
        if(c=='"' || c=='\\') { escaped+='\\'; escaped+=char(c); }
        else if(c>=0x80) { escaped+="\\x"; escaped+=hex[c>>4]; escaped+=hex[c&15]; }
        else escaped+=char(c);
    }
    if(out) *out=std::move(escaped);
    if(error) error->clear();
    return true;
}
std::string siiUnescapeName(const std::string& text) {
    std::string out;
    auto hex=[](char c)->int {if(c>='0'&&c<='9')return c-'0';if(c>='A'&&c<='F')return c-'A'+10;if(c>='a'&&c<='f')return c-'a'+10;return -1;};
    for(size_t i=0;i<text.size();++i) {
        if(text[i]!='\\') {out+=text[i];continue;}
        if(++i>=text.size()) return {};
        if(text[i]=='x') {
            if(i+2>=text.size() || hex(text[i+1])<0 || hex(text[i+2])<0) return {};
            out+=char(hex(text[i+1])*16+hex(text[i+2])); i+=2;
        } else if(text[i]=='\\' || text[i]=='"') out+=text[i];
        else if(text[i]=='n') out+='\n';
        else if(text[i]=='r') out+='\r';
        else if(text[i]=='t') out+='\t';
        else return {};
    }
    return out;
}
bool patchSaveInfo(std::string& text,const std::string& name,uint64_t time,std::string* error) {
    std::string escaped;
    if(!siiEscapeName(name,&escaped,error)) return false;
    try {
        const auto fields=infoFields(text);
        struct Edit {size_t begin,end; std::string replacement;};
        std::array<Edit,2> edits{{{fields.name.begin,fields.name.end,"\""+escaped+"\""},
                                 {fields.time.begin,fields.time.end,std::to_string(time)}}};
        std::sort(edits.begin(),edits.end(),[](const Edit& a,const Edit& b){return a.begin>b.begin;});
        auto copy=text;
        for(const auto& edit:edits) copy.replace(edit.begin,edit.end-edit.begin,edit.replacement);
        if(nameFromInfo(copy)!=name) throw std::runtime_error("副本名称回读不一致");
        text.swap(copy);
        if(error) error->clear();
        return true;
    } catch(const std::exception& e) {if(error)*error=e.what();return false;}
}
std::string readSaveDisplayName(const SaveSlot& slot) {
    try {return nameFromInfo(loadInfo(fs::path(slot.slotDir)/L"info.sii"));} catch(...) {return {};}
}
bool copyBlockedByRunningGame(GameId game,std::string* error) {
    Handle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));
    if(!snapshot) {if(error)*error="无法确认游戏是否已退出，复制已取消";return true;}
    PROCESSENTRY32W entry{}; entry.dwSize=sizeof(entry);
    if(!::Process32FirstW(snapshot.value,&entry)) {if(error)*error="无法枚举进程，复制已取消";return true;}
    do {
        if(::_wcsicmp(entry.szExeFile,gameDescriptor(game).processName)==0) {
            if(error)*error=std::string("请先退出")+gameDescriptor(game).shortName+"再复制存档";
            return true;
        }
    } while(::Process32NextW(snapshot.value,&entry));
    if(::GetLastError()!=ERROR_NO_MORE_FILES) {if(error)*error="进程检查未完成，复制已取消";return true;}
    return false;
}
void publishNewDirectory(const std::wstring& stage,const std::wstring& target,const std::function<void()>& validate) {
    for (int attempt=0;attempt<2;++attempt) {
        if(validate) validate();
        if(existsEntry(fs::path(target))) throw std::runtime_error("目标目录已存在，已拒绝覆盖");
        if(::MoveFileExW(stage.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH)) return;
        const DWORD error=::GetLastError();
        if(attempt==0 && (error==ERROR_ACCESS_DENIED || error==ERROR_SHARING_VIOLATION || error==ERROR_LOCK_VIOLATION) &&
           !existsEntry(fs::path(target))) { ::Sleep(200); continue; }
        throw std::runtime_error("副本发布失败（Windows 错误 "+std::to_string(error)+"），请检查文件占用后重试");
    }
}
SaveCopyResult copySaveSlotInProfile(const SaveSlot& source,const std::string& name,const SaveCopyOptions& options) {
    SaveCopyResult result; result.displayName=name;
    fs::path profile,slot,stage; std::vector<fs::path> created;
    try {
        std::string escaped;
        if(!siiEscapeName(name,&escaped,&result.error)) return result;
        if(options.replacementGameText && (options.replacementGameText->size()>kFileLimit ||
           options.replacementGameText->compare(0,8,"SiiNunit")!=0))
            throw std::runtime_error("导入结果不是受支持大小的 SII 文本");
        checkOptions(options);
        profile=physicalProfile(source,&slot);
        auto profileLock=lockDirectory(profile), saveLock=lockDirectory(profile/L"save"), slotLock=lockDirectory(slot);
        std::vector<fs::path> entries;
        for(const auto& e:fs::directory_iterator(slot)) {
            if(entries.size()>=256 || reparse(e.path()) || !e.is_regular_file())
                throw std::runtime_error("源存档含子目录、重解析点或过多文件，已拒绝复制");
            entries.push_back(e.path().filename());
        }
        std::sort(entries.begin(),entries.end());
        if(std::find(entries.begin(),entries.end(),fs::path(L"game.sii"))==entries.end() ||
           std::find(entries.begin(),entries.end(),fs::path(L"info.sii"))==entries.end())
            throw std::runtime_error("源存档缺少 game.sii 或 info.sii");
        std::vector<std::unique_ptr<Handle>> held; std::vector<uint64_t> sizes;
        uint64_t total=0;
        for(const auto& file:entries) {
            auto h=std::make_unique<Handle>(::CreateFileW((slot/file).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,
                OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
            BY_HANDLE_FILE_INFORMATION info{};
            if(!*h || !::GetFileInformationByHandle(h->value,&info) ||
               info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
                throw std::runtime_error("源文件无法锁定读取或类型已改变");
            const auto size=fileSize(h->value);
            if(size>kFileLimit || (total+=size)>kTotalLimit || (file==L"info.sii" && size>kInfoLimit))
                throw std::runtime_error("存档文件超出本次复制的大小上限");
            if(file==L"game.sii") {
                std::vector<uint8_t> head(8); DWORD got=0;
                if(!::ReadFile(h->value,head.data(),8,&got,nullptr) || got<4)
                    throw std::runtime_error("源 game.sii 格式无法识别");
                head.resize(got);
                if(detectFormat(head)=="unknown")
                    throw std::runtime_error("源 game.sii 格式无法识别");
            }
            sizes.push_back(size); held.push_back(std::move(h));
        }
        // Source handles are held together until after publication; no writes/deletes can enter mid-copy.
        auto metadata=loadInfo(slot/L"info.sii"); std::string error;
        if(!patchSaveInfo(metadata,name,static_cast<uint64_t>(::time(nullptr)),&error))
            throw std::runtime_error(error);
        unsigned next=1;
        for(;next<=1000000;++next) if(!existsEntry(profile/L"save"/std::to_wstring(next))) break;
        if(next>1000000) throw std::runtime_error("没有可用的新存档编号");
        const auto target=profile/L"save"/std::to_wstring(next);
        static std::atomic<unsigned> serial{0};
        for(unsigned attempt=0;attempt<16;++attempt) {
            auto candidate=profile/(L".trainer_copy_"+std::to_wstring(::GetCurrentProcessId())+L"_"+
                std::to_wstring(::GetTickCount64())+L"_"+std::to_wstring(++serial));
            if(::CreateDirectoryW(candidate.c_str(),nullptr)) {stage=candidate;break;}
            if(::GetLastError()!=ERROR_ALREADY_EXISTS) break;
        }
        if(stage.empty()) throw std::runtime_error("创建独立暂存目录失败");
        for(size_t i=0;i<entries.size();++i) {
            checkOptions(options);
            created.push_back(entries[i]);
            copyAndVerify(held[i]->value,stage/entries[i],sizes[i],options);
            if(options.afterFile && !options.afterFile(i)) throw std::runtime_error("复制在校验阶段被中止");
        }
        // info changes are intentional; all OTHER files have already passed byte comparison.
        {
            Handle output(::CreateFileW((stage/L"info.sii").c_str(),GENERIC_WRITE,0,nullptr,
                TRUNCATE_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
            DWORD written=0;
            if(!output || !::WriteFile(output.value,metadata.data(),static_cast<DWORD>(metadata.size()),&written,nullptr) ||
               written!=metadata.size() || !::FlushFileBuffers(output.value))
                throw std::runtime_error("副本元信息写入失败");
        }
        if(loadInfo(stage/L"info.sii")!=metadata) throw std::runtime_error("副本元信息回读失败");
        if(options.replacementGameText) {
            const auto& replacement=*options.replacementGameText;
            {
                Handle output(::CreateFileW((stage/L"game.sii").c_str(),GENERIC_WRITE,0,nullptr,
                    TRUNCATE_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
                DWORD written=0;
                if(!output || !::WriteFile(output.value,replacement.data(),static_cast<DWORD>(replacement.size()),&written,nullptr) ||
                   written!=replacement.size() || !::FlushFileBuffers(output.value))
                    throw std::runtime_error("车辆导入的暂存进度写入失败");
            }
            std::vector<uint8_t> verified;
            if(!readFileBytes((stage/L"game.sii").wstring(),verified) || verified.size()!=replacement.size() ||
               ::memcmp(verified.data(),replacement.data(),replacement.size()))
                throw std::runtime_error("车辆导入的暂存进度回读校验失败");
        }
        if(options.beforePublish) options.beforePublish(target.wstring());
        publishNewDirectory(stage.wstring(),target.wstring(),[&] {
        checkOptions(options);
        // Hooks may have added or replaced source entries; re-verify the bounded membership before publishing.
        {
            std::vector<fs::path> published; bool entriesStable=true;
            for(const auto& e:fs::directory_iterator(slot)) {
                if(published.size()>=256 || reparse(e.path()) || !e.is_regular_file()) {entriesStable=false;break;}
                published.push_back(e.path().filename());
            }
            std::sort(published.begin(),published.end());
            if(!entriesStable || published!=entries)
                throw std::runtime_error("源存档在复制期间发生变化，已取消复制；请在同步完成后重试");
        }
        if(!samePath(fs::canonical(stage).parent_path(),profile) || reparse(stage) ||
           !samePath(fs::canonical(profile/L"save"),profile/L"save") || existsEntry(target))
            throw std::runtime_error("发布前目录状态已变化或目标编号已存在");
        });
        stage.clear();
        result.ok=true; result.slotDir=target.wstring(); result.slotName=std::to_wstring(next);
        return result;
    } catch(const std::exception& e) {
        result.error=e.what();
        result.slotDir.clear(); result.slotName.clear();
        if(!cleanupStage(profile,stage,created)) result.error+="；暂存清理未完成，位置："+W2U(stage.wstring());
        return result;
    }
}
SaveCopyResult copySlotToNewSave(const SaveCopyRequest& request,std::function<bool()> canceled,
                               const std::string* replacementGameText) {
    SaveCopyResult failure; failure.game=request.game;
    if(selectedGame()!=request.game) {failure.error="游戏已切换，请重新选择该游戏的存档";return failure;}
    if(copyBlockedByRunningGame(request.game,&failure.error)) return failure;
    SaveSlot fresh; bool member=false;
    for(const auto& slot:listSlots()) {
        if(::_wcsicmp(slot.slotDir.c_str(),request.source.slotDir.c_str())==0 &&
           ::_wcsicmp(slot.profileDir.c_str(),request.source.profileDir.c_str())==0) {fresh=slot;member=true;break;}
    }
    if(!member) {failure.error="源存档不属于当前游戏的存档列表";return failure;}
    SaveCopyOptions options; options.canceled=std::move(canceled);
    options.replacementGameText=replacementGameText;
    options.guard=[game=request.game](std::string* error) {
        if(selectedGame()!=game) {if(error)*error="复制期间游戏选择发生变化";return false;}
        return !copyBlockedByRunningGame(game,error);
    };
    auto result=copySaveSlotInProfile(fresh,request.displayName,options);
    result.game=request.game;
    return result;
}
}
