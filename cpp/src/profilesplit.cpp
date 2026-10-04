#include "profilesplit.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <set>
#include <stdexcept>

namespace ets2 {
namespace {
namespace fs = std::filesystem;
constexpr uint64_t fileLimit = 128ull * 1024 * 1024, totalLimit = 1024ull * 1024 * 1024;
constexpr size_t metadataLimit = 4 * 1024 * 1024;
struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE value) : h(value) {}
    ~Handle() { if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
    Handle(const Handle&) = delete;
};
using Held = std::unique_ptr<Handle>;
bool same(const fs::path& a, const fs::path& b) {
    auto left = a.lexically_normal(), right = b.lexically_normal();
    left.make_preferred(); right.make_preferred();
    return ::_wcsicmp(left.c_str(), right.c_str()) == 0;
}
bool hasEntry(const fs::path& p) {
    if (::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    const auto e = ::GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return false;
    throw std::runtime_error("无法检查档案目录或文件是否存在");
}
void noReparse(const fs::path& p) {
    const auto attr = ::GetFileAttributesW(p.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("目录或文件不存在、或是重解析点，已停止拆分");
}
fs::path physical(const fs::path& p) {
    if (!p.is_absolute()) throw std::runtime_error("档案路径必须是绝对路径");
    for (const auto& part : p) if (part == L"." || part == L"..")
        throw std::runtime_error("档案路径含相对目录跳转");
    for (auto ancestor = p; !ancestor.empty();) {
        noReparse(ancestor);
        const auto parent = ancestor.parent_path();
        if (parent == ancestor) break;
        ancestor = parent;
    }
    return fs::canonical(p);
}
Held lockDir(const fs::path& p) {
    noReparse(p);
    auto h = std::make_unique<Handle>(::CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (h->h == INVALID_HANDLE_VALUE || !fs::is_directory(p)) throw std::runtime_error("无法锁定档案目录");
    return h;
}
uint64_t sizeOf(HANDLE h) {
    LARGE_INTEGER n{};
    if (!::GetFileSizeEx(h, &n) || n.QuadPart < 0) throw std::runtime_error("无法读取文件长度");
    return static_cast<uint64_t>(n.QuadPart);
}
void rewindFile(HANDLE h) {
    LARGE_INTEGER zero{};
    if (!::SetFilePointerEx(h, zero, nullptr, FILE_BEGIN)) throw std::runtime_error("无法定位文件");
}
void check(const ProfileSplitOptions& o, bool full = true) {
    if (o.canceled && o.canceled()) throw std::runtime_error("拆分已取消");
    std::string error;
    if (full && o.guard && !o.guard(&error)) throw std::runtime_error(error.empty() ? "拆分条件发生变化" : error);
}
struct Token { std::string value; size_t begin, end; bool quoted; };
struct Field { std::string name; Token value; };
// A single flat SII metadata unit, with comments and string escapes respected.
std::vector<Field> fields(const std::string& s, const char* type) {
    if (s.size() > metadataLimit || s.find('\0') != std::string::npos) throw std::runtime_error("档案元信息过大或含空字节");
    std::vector<Token> t;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == ' ' || c == '\r' || c == '\n' || c == '\t') { ++i; continue; }
        if (c == '#' || (c == '/' && i + 1 < s.size() && s[i+1] == '/')) {
            i = s.find('\n', i); if (i == std::string::npos) break; continue;
        }
        size_t b = i; bool q = c == '"';
        if (q) {
            ++i; bool closed = false;
            while (i < s.size()) {
                if (s[i] == '\\') { if (i + 1 >= s.size() || s[i+1] == '\r' || s[i+1] == '\n') break; i += 2; }
                else if (s[i] == '"') { ++i; closed = true; break; }
                else if (s[i] == '\r' || s[i] == '\n') break;
                else ++i;
            }
            if (!closed) throw std::runtime_error("档案元信息的引号或转义不完整");
        } else if (std::strchr("{}:[]", c)) ++i;
        else while (i < s.size() && !std::strchr(" \t\r\n{}:[]\"", s[i])) ++i;
        t.push_back({s.substr(b, i-b), b, i, q});
    }
    auto is = [&](size_t i, const char* v) { return i < t.size() && !t[i].quoted && t[i].value == v; };
    if (t.size() < 8 || !is(0, "SiiNunit") || !is(1, "{") || !is(2, type) || !is(3, ":") ||
        t[4].quoted || t[4].value.empty() || t[4].value.find_first_of("{}:[]") != std::string::npos ||
        !is(5, "{") || !is(t.size()-2, "}") || !is(t.size()-1, "}"))
        throw std::runtime_error("档案元信息不是完整、唯一的预期 SII 单位");
    std::vector<Field> out; std::set<std::string> names;
    for (size_t i = 6; i < t.size()-2;) {
        auto key = t[i++];
        if (key.quoted || key.value.find_first_of("{}:[]") != std::string::npos)
            throw std::runtime_error("档案元信息字段无效");
        if (is(i, "[")) {
            ++i;
            if (!is(i, "]")) {
                if (i >= t.size() || t[i].quoted || t[i].value.empty() ||
                    t[i].value.find_first_not_of("0123456789") != std::string::npos)
                    throw std::runtime_error("档案元信息数组编号无效");
                key.value += "[" + t[i++].value + "]";
            } else key.value += "[]";
            if (!is(i++, "]")) throw std::runtime_error("档案元信息数组括号不完整");
        }
        if (!is(i++, ":") || i >= t.size()-2) throw std::runtime_error("档案元信息字段缺少值");
        auto value = t[i++];
        if (!value.quoted && value.value.find_first_of("{}:[]") != std::string::npos)
            throw std::runtime_error("档案元信息含嵌套单位或无效值");
        if (key.value.size() < 2 || key.value.substr(key.value.size()-2) != "[]") {
            if (!names.insert(key.value).second) throw std::runtime_error("档案元信息含重复字段");
        }
        out.push_back({key.value, std::move(value)});
    }
    return out;
}
const Token& required(const std::vector<Field>& f, const char* key) {
    for (const auto& item : f) if (item.name == key) return item.value;
    throw std::runtime_error(std::string("档案元信息缺少字段：") + key);
}
uint64_t number(const Token& t) {
    if (t.quoted || t.value.empty() || t.value.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("档案元信息时间戳无效");
    try { return std::stoull(t.value); } catch (...) { throw std::runtime_error("档案元信息时间戳超出范围"); }
}
std::string decodeText(const fs::path& p) {
    std::vector<uint8_t> b; DecryptInfo info; std::string error;
    if (!decryptSave(p.wstring(), b, &info, &error, metadataLimit)) throw std::runtime_error("档案元信息读取失败：" + error);
    if (detectFormat(b) != "text") throw std::runtime_error("profile.sii / info.sii 需要可解密的 SII 文本元信息");
    return {b.begin(), b.end()};
}
struct File { fs::path source, relative; Held held; uint64_t size; };
File openSource(const fs::path& p, const fs::path& relative, uint64_t& total) {
    noReparse(p);
    auto h = std::make_unique<Handle>(::CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (h->h == INVALID_HANDLE_VALUE || !::GetFileInformationByHandle(h->h, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        throw std::runtime_error("源文件不能锁定读取或文件类型已变化");
    const auto n = sizeOf(h->h);
    if (n > fileLimit || (total += n) > totalLimit ||
        ((p.filename() == L"profile.sii" || p.filename() == L"info.sii") && n > metadataLimit))
        throw std::runtime_error("本次拆分超过文件大小上限");
    return {p, relative, std::move(h), n};
}
std::vector<fs::path> slotEntries(const fs::path& p) {
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(p)) {
        noReparse(e.path());
        if (!e.is_regular_file() || files.size() >= 256) throw std::runtime_error("存档含子目录或过多文件");
        files.push_back(e.path().filename());
    }
    std::sort(files.begin(), files.end());
    if (std::find(files.begin(), files.end(), fs::path(L"game.sii")) == files.end() ||
        std::find(files.begin(), files.end(), fs::path(L"info.sii")) == files.end())
        throw std::runtime_error("存档缺少 game.sii 或 info.sii");
    return files;
}
bool profileConfig(const fs::path& file) {
    const auto name = file.filename().wstring();
    return name == L"config.cfg" || name == L"config_local.cfg" || name == L"controls.sii" ||
        name == L"tutorial_hint_data.sii" ||
        (name.find(L"gearbox_layout_") == 0 && file.extension() == L".sii");
}
std::vector<fs::path> configEntries(const fs::path& p) {
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(p)) if (profileConfig(e.path())) {
        noReparse(e.path());
        if (!e.is_regular_file() || files.size() >= 128) throw std::runtime_error("档案配置文件类型无效或数量过多");
        files.push_back(e.path().filename());
    }
    std::sort(files.begin(), files.end()); return files;
}
void copyFile(const File& f, const fs::path& dest, const ProfileSplitOptions& o) {
    Handle out(::CreateFileW(dest.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (out.h == INVALID_HANDLE_VALUE) throw std::runtime_error("无法创建新档案文件");
    std::array<uint8_t, 65536> a{}, b{}; rewindFile(f.held->h);
    for (uint64_t n = 0; n < f.size;) {
        check(o, false); DWORD got = 0, wrote = 0;
        DWORD count = static_cast<DWORD>((std::min)(uint64_t(a.size()), f.size-n));
        if (!::ReadFile(f.held->h, a.data(), count, &got, nullptr) || got != count ||
            !::WriteFile(out.h, a.data(), count, &wrote, nullptr) || wrote != count) throw std::runtime_error("档案文件复制失败");
        n += count;
    }
    if (!::FlushFileBuffers(out.h) || sizeOf(out.h) != f.size || sizeOf(f.held->h) != f.size) throw std::runtime_error("档案副本长度校验失败");
    rewindFile(f.held->h); rewindFile(out.h);
    for (uint64_t n = 0; n < f.size;) {
        check(o, false); DWORD got = 0, copied = 0;
        DWORD count = static_cast<DWORD>((std::min)(uint64_t(a.size()), f.size-n));
        if (!::ReadFile(f.held->h, a.data(), count, &got, nullptr) || got != count ||
            !::ReadFile(out.h, b.data(), count, &copied, nullptr) || copied != count ||
            ::memcmp(a.data(), b.data(), count)) throw std::runtime_error("档案副本内容校验失败");
        n += count;
    }
}
void replaceStaged(const fs::path& p, const std::string& text) {
    Handle out(::CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    DWORD wrote = 0;
    if (out.h == INVALID_HANDLE_VALUE || !::WriteFile(out.h, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr) ||
        wrote != text.size() || !::FlushFileBuffers(out.h)) throw std::runtime_error("新档案元信息写入失败");
}
struct Stage {
    fs::path dir, target;
    std::vector<fs::path> files;
};
bool cleanStage(const fs::path& parent, const Stage& stage) {
    if (stage.dir.empty()) return true;
    try {
        if (!same(physical(stage.dir).parent_path(), parent)) return false;
        bool ok = true;
        for (const auto& file : stage.files) {
            const auto path = stage.dir/file;
            if (!hasEntry(path)) continue;
            // Refuse recursive deletion or following replaced directories.
            if (!same(physical(path).parent_path(), (stage.dir/file).parent_path()) || !::DeleteFileW(path.c_str())) ok = false;
        }
        for (const auto& rel : {fs::path(L"save/1"), fs::path(L"save"), fs::path()}) {
            const auto path = stage.dir/rel;
            if (hasEntry(path)) { physical(path); if (!::RemoveDirectoryW(path.c_str())) ok = false; }
        }
        return ok;
    } catch (...) { return false; }
}
void freeName(const std::wstring& folder, const ProfileSplitPaths& paths) {
    std::vector<std::wstring> roots = paths.nameCheckRoots; roots.push_back(paths.localProfiles);
    if (!paths.cloudLocalProfiles.empty()) roots.push_back(paths.cloudLocalProfiles);
    for (const auto& root : roots) if (!root.empty() && hasEntry(fs::path(root)/folder))
        throw std::runtime_error("新档案名称已被本地或云档案使用，请更换名称");
}
} // namespace

bool profileFolderName(const std::string& name, std::wstring* hex, std::string* error) {
    if (hex) hex->clear();
    std::string escaped;
    if (!siiEscapeName(name, &escaped, error)) return false;
    if (name.size() > 120 || trim(name) != name) {
        if (error) *error = "档案名首尾不能有空格，UTF-8 长度不能超过 120 字节"; return false;
    }
    std::wstring value; const wchar_t* digits = L"0123456789ABCDEF";
    for (unsigned char c : name) { value += digits[c >> 4]; value += digits[c & 15]; }
    if (hex) *hex = std::move(value);
    return true;
}
bool patchProfileText(std::string& text, const std::string& name, uint64_t created, uint64_t saved, std::string* error) {
    std::wstring folder; std::string escaped;
    if (!profileFolderName(name, &folder, error) || !siiEscapeName(name, &escaped, error)) return false;
    try {
        const auto f = fields(text, "user_profile");
        const auto& oldName = required(f, "profile_name");
        if (!oldName.quoted || siiUnescapeName(oldName.value.substr(1, oldName.value.size()-2)).empty())
            throw std::runtime_error("profile.sii 档案名无效");
        number(required(f, "creation_time")); number(required(f, "save_time"));
        struct Edit { Token token; std::string value; };
        std::vector<Edit> edits = {{oldName, "\"" + escaped + "\""},
            {required(f, "creation_time"), std::to_string(created)}, {required(f, "save_time"), std::to_string(saved)}};
        // Some versions explicitly store cloud state; new profiles always live in local profiles/.
        for (const auto& item : f) if (item.name == "use_cloud") {
            if (item.value.quoted || (item.value.value != "true" && item.value.value != "false"))
                throw std::runtime_error("profile.sii 的 use_cloud 字段无效");
            edits.push_back({item.value, "false"});
        }
        std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.token.begin > b.token.begin; });
        auto copy = text;
        for (const auto& e : edits) copy.replace(e.token.begin, e.token.end-e.token.begin, e.value);
        const auto parsed = fields(copy, "user_profile");
        const auto& read = required(parsed, "profile_name");
        if (siiUnescapeName(read.value.substr(1, read.value.size()-2)) != name) throw std::runtime_error("新档案名称回读不一致");
        text.swap(copy); if (error) error->clear(); return true;
    } catch (const std::exception& e) { if (error) *error = e.what(); return false; }
}
ProfileSplitResult splitProfilesAt(const ProfileSplitRequest& request, const ProfileSplitPaths& paths, const ProfileSplitOptions& o) {
    ProfileSplitResult result; result.game = request.game;
    fs::path stageParent; std::vector<Stage> stages;
    try {
        check(o);
        if (request.items.empty() || request.items.size() > 16) throw std::runtime_error("请选择 1 至 16 个存档");
        const fs::path rootInput(paths.localProfiles);
        // Stage beside profiles/, so incomplete directories never appear as game profiles.
        stageParent = physical(rootInput.parent_path());
        auto parentLock = lockDir(stageParent);
        if (!hasEntry(rootInput) && !::CreateDirectoryW(rootInput.c_str(), nullptr)) throw std::runtime_error("无法创建本地 profiles 目录");
        const auto root = physical(rootInput); auto rootLock = lockDir(root);
        if (!same(root.parent_path(), stageParent)) throw std::runtime_error("本地档案目录边界无效");
        fs::path profile, save;
        std::set<std::wstring> folders, selected;
        std::vector<std::wstring> names;
        for (const auto& item : request.items) {
            std::wstring folder; std::string error;
            if (!profileFolderName(item.profileName, &folder, &error)) throw std::runtime_error(error);
            if (!folders.insert(folder).second) throw std::runtime_error("新档案名称不能重复");
            freeName(folder, paths); names.push_back(folder);
            const auto p = physical(item.source.profileDir), s = physical(item.source.slotDir);
            if (profile.empty()) { profile = p; save = physical(profile/L"save"); }
            if (!same(p, profile) || !same(s.parent_path(), save) || !fs::is_directory(s))
                throw std::runtime_error("请选择同一档案 save 目录内的存档");
            auto key = s.wstring(); std::transform(key.begin(), key.end(), key.begin(), ::towlower);
            if (!selected.insert(key).second) throw std::runtime_error("同一个源存档不能重复拆分");
            if (item.source.cloud != request.items.front().source.cloud) throw std::runtime_error("源档案位置状态不一致");
        }
        auto profileLock = lockDir(profile), saveLock = lockDir(save);
        const auto configs = configEntries(profile);
        fs::path cloudLocal; std::vector<fs::path> localConfigs; Held cloudLock;
        if (request.items.front().source.cloud && !paths.cloudLocalProfiles.empty()) {
            const auto candidate = fs::path(paths.cloudLocalProfiles)/profile.filename();
            if (hasEntry(candidate)) { cloudLocal = physical(candidate); cloudLock = lockDir(cloudLocal); localConfigs = configEntries(cloudLocal); }
        }
        uint64_t total = 0;
        std::vector<File> common; common.push_back(openSource(profile/L"profile.sii", L"profile.sii", total));
        std::set<fs::path> localSet(localConfigs.begin(), localConfigs.end());
        for (const auto& f : configs) if (!localSet.count(f)) common.push_back(openSource(profile/f, f, total));
        // steam_profiles holds local input/controller configuration for a cloud profile.
        for (const auto& f : localConfigs) common.push_back(openSource(cloudLocal/f, f, total));
        const auto originalProfile = decodeText(profile/L"profile.sii");
        std::vector<std::vector<File>> slots; std::vector<std::vector<fs::path>> memberships; std::vector<Held> slotLocks;
        static std::atomic<unsigned> serial{0}; size_t filesDone = 0;
        for (size_t i = 0; i < request.items.size(); ++i) {
            check(o); const auto slot = physical(request.items[i].source.slotDir);
            slotLocks.push_back(lockDir(slot)); memberships.push_back(slotEntries(slot));
            std::vector<File> snapshot;
            for (const auto& f : memberships.back()) snapshot.push_back(openSource(slot/f, fs::path(L"save/1")/f, total));
            // Validate progress framing without converting it: copied game.sii remains byte-for-byte identical.
            std::vector<uint8_t> progress; DecryptInfo di; std::string error;
            if (!decryptSave((slot/L"game.sii").wstring(), progress, &di, &error, fileLimit))
                throw std::runtime_error("源存档进度校验失败：" + error);
            auto infoText = decodeText(slot/L"info.sii");
            const auto saved = number(required(fields(infoText, "save_container"), "file_time"));
            std::string dummy = infoText;
            if (!patchSaveInfo(dummy, "校验", saved, &error)) throw std::runtime_error(error);
            auto newProfile = originalProfile;
            if (!patchProfileText(newProfile, request.items[i].profileName, static_cast<uint64_t>(::time(nullptr)), saved, &error))
                throw std::runtime_error(error);
            Stage stage; stage.target = root/names[i];
            for (unsigned n = 0; n < 16; ++n) {
                const auto candidate = stageParent/(L".trainer_profile_" + std::to_wstring(::GetCurrentProcessId()) + L"_" +
                    std::to_wstring(::GetTickCount64()) + L"_" + std::to_wstring(++serial));
                if (::CreateDirectoryW(candidate.c_str(), nullptr)) { stage.dir = candidate; break; }
                if (::GetLastError() != ERROR_ALREADY_EXISTS) break;
            }
            if (stage.dir.empty()) throw std::runtime_error("无法创建档案暂存目录");
            stages.push_back(std::move(stage)); auto& staged = stages.back();
            if (!::CreateDirectoryW((staged.dir/L"save").c_str(), nullptr) || !::CreateDirectoryW((staged.dir/L"save/1").c_str(), nullptr))
                throw std::runtime_error("无法创建新档案存档目录");
            for (const auto& f : common) {
                staged.files.push_back(f.relative); copyFile(f, staged.dir/f.relative, o);
                if (o.afterFile && !o.afterFile(++filesDone)) throw std::runtime_error("档案复制在校验阶段中止");
            }
            for (const auto& f : snapshot) {
                staged.files.push_back(f.relative); copyFile(f, staged.dir/f.relative, o);
                if (o.afterFile && !o.afterFile(++filesDone)) throw std::runtime_error("存档复制在校验阶段中止");
            }
            replaceStaged(staged.dir/L"profile.sii", newProfile);
            if (decodeText(staged.dir/L"profile.sii") != newProfile) throw std::runtime_error("新档案元信息回读失败");
            slots.push_back(std::move(snapshot));
        }
        // All copies are prepared first. A publication race can still produce partial success;
        // keep the published profiles and explicitly return their names instead of deleting them.
        for (size_t i = 0; i < stages.size(); ++i) {
            auto& stage = stages[i]; check(o);
            if (o.beforePublish) o.beforePublish(i, stage.target.wstring());
            publishNewDirectory(stage.dir.wstring(), stage.target.wstring(), [&] {
            check(o); freeName(names[i], paths);
            if (configEntries(profile) != configs || (!cloudLocal.empty() && configEntries(cloudLocal) != localConfigs))
                throw std::runtime_error("源档案配置在拆分期间发生变化");
            for (size_t j = 0; j < request.items.size(); ++j)
                if (slotEntries(fs::path(request.items[j].source.slotDir)) != memberships[j])
                    throw std::runtime_error("源存档文件列表在拆分期间发生变化");
            if (!same(physical(stage.dir).parent_path(), stageParent) || !same(physical(root), root))
                throw std::runtime_error("发布前目录状态发生变化");
            });
            stage.dir.clear();
            result.created.push_back({request.items[i].profileName, stage.target.wstring(), (stage.target/L"save/1").wstring()});
        }
        result.ok = true;
    } catch (const std::exception& e) {
        result.error = e.what();
        for (const auto& stage : stages) if (!cleanStage(stageParent, stage))
            result.error += "；暂存清理未完成：" + W2U(stage.dir.wstring());
    }
    return result;
}
ProfileSplitResult splitSelectedProfiles(const ProfileSplitRequest& request, std::function<bool()> canceled) {
    ProfileSplitResult failure; failure.game = request.game;
    if (request.game != selectedGame()) { failure.error = "游戏已切换，请重新选择存档"; return failure; }
    if (copyBlockedByRunningGame(request.game, &failure.error)) return failure;
    auto fresh = request; const auto listed = listSlots();
    for (auto& item : fresh.items) {
        bool found = false;
        for (const auto& slot : listed) if (same(fs::path(slot.slotDir), fs::path(item.source.slotDir)) &&
            same(fs::path(slot.profileDir), fs::path(item.source.profileDir))) { item.source = slot; found = true; break; }
        if (!found) { failure.error = "源存档不属于当前游戏的存档列表"; return failure; }
    }
    ProfileSplitPaths paths; paths.localProfiles = documentsDir() + L"\\profiles";
    paths.cloudLocalProfiles = documentsDir() + L"\\steam_profiles"; paths.nameCheckRoots = cloudProfileDirs();
    ProfileSplitOptions o; o.canceled = std::move(canceled);
    o.guard = [game = request.game](std::string* error) {
        if (selectedGame() != game) { if (error) *error = "拆分期间游戏选择已变化"; return false; }
        return !copyBlockedByRunningGame(game, error);
    };
    return splitProfilesAt(fresh, paths, o);
}
}
