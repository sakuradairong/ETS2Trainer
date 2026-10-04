#include "profilesplittest.h"
#include "profilesplit.h"
#include <atomic>
#include <filesystem>
#include <stdexcept>

namespace ets2 {
namespace {
namespace fs = std::filesystem;
std::string splitDiagnostic;
ProfileSplitResult splitFixture(const ProfileSplitRequest& r, const ProfileSplitPaths& p, const ProfileSplitOptions& o = {}) {
    auto result = splitProfilesAt(r, p, o); splitDiagnostic = result.error; return result;
}
const std::string profileText = R"(SiiNunit
{
user_profile : _nameless.1 {
 profile_name: "Original"
 creation_time: 10
 save_time: 20
 use_cloud: true
 cached_experience: 500
 active_mods: 2
 active_mods[0]: "map_a"
 active_mods[1]: "map_b"
 user_data: 2
 user_data[0]: "quoted profile_name: and { braces }"
 user_data[1]: "preserve me"
 online_user_name: "fixture-user"
 online_password: "fixture-password"
 // profile_name: "comment must survive"
}
}
)";
const std::string infoText = "SiiNunit\n{\nsave_container : _nameless.2 {\n name: \"Source\"\n file_time: 123\n version: 102\n dependencies: 1\n dependencies[0]: \"dlc|test|Test\"\n}\n}\n";
void put(const fs::path& p, const std::string& text) {
    if (!writeFileBytes(p.wstring(), {text.begin(), text.end()})) throw std::runtime_error("fixture write failed");
}
std::string get(const fs::path& p) {
    std::vector<uint8_t> b; if (!readFileBytes(p.wstring(), b)) throw std::runtime_error("fixture read failed");
    return {b.begin(), b.end()};
}
struct Fixture {
    fs::path base, source, target;
    ProfileSplitRequest request;
    ProfileSplitPaths paths;
    Fixture() {
        static std::atomic<unsigned> serial{0};
        base = fs::path(applicationDir())/(L"profilesplit_fixture_" + std::to_wstring(::GetCurrentProcessId()) + L"_" +
            std::to_wstring(::GetTickCount64()) + L"_" + std::to_wstring(++serial));
        if (!fs::create_directory(base)) throw std::runtime_error("fixture creation failed");
        source = base/L"source/Original"; target = base/L"output/profiles";
        fs::create_directories(source/L"save/1"); fs::create_directories(source/L"save/2");
        fs::create_directories(target);
        put(source/L"profile.sii", profileText); put(source/L"config.cfg", "uset config keep\n");
        put(source/L"controls.sii", "controller settings\n"); put(source/L"gearbox_layout_test.sii", "gearbox\n");
        put(source/L"profile.bak.sii", "old profile name"); put(source/L"last_session_config.sii", "private server token");
        for (int i = 1; i <= 2; ++i) {
            const auto slot = source/L"save"/std::to_wstring(i);
            put(slot/L"game.sii", "SiiNunit\n{\nprogress : p { value: " + std::to_string(i) + " }\n}\n");
            put(slot/L"info.sii", infoText); put(slot/L"preview.mat", "preview " + std::to_string(i));
            SaveSlot s; s.profileDir = source.wstring(); s.slotDir = slot.wstring(); s.gameSii = (slot/L"game.sii").wstring();
            request.items.push_back({s, i == 1 ? "TMP" : "联运"});
        }
        request.game = selectedGame(); paths.localProfiles = target.wstring();
    }
    ~Fixture() {
        // Only this exclusively created fixture, under the test executable directory, is owned.
        std::error_code e;
        if (base.parent_path() == fs::path(applicationDir()) && base.filename().wstring().find(L"profilesplit_fixture_") == 0)
            fs::remove_all(base, e);
    }
    bool clean() const {
        for (const auto& e : fs::directory_iterator(target.parent_path()))
            if (e.path().filename().wstring().find(L".trainer_profile_") == 0) return false;
        return true;
    }
    bool unchanged() const { return get(source/L"profile.sii") == profileText && get(source/L"save/1/info.sii") == infoText; }
};
}
std::vector<TunerTestItem> runProfileSplitTests() {
    std::vector<TunerTestItem> out;
    auto test = [&](const char* name, const std::function<bool()>& fn) {
        bool ok = false; std::string error; splitDiagnostic.clear();
        try { ok = fn(); } catch (const std::exception& e) { error = e.what(); }
        out.push_back({std::string("档案拆分：") + name, ok, error.empty() && !ok ? splitDiagnostic : error});
    };
    test("中文档案名按 UTF-8 编码", [] {
        std::wstring hex; std::string error;
        return profileFolderName("联运", &hex, &error) && hex == L"E88194E8BF90";
    });
    for (const auto& invalid : {std::string(), std::string(" TMP"), std::string("TMP "), std::string("x\ny"), std::string("\xff"), std::string(121, 'a')})
        test("拒绝空白、控制字符及无效或超长名称", [invalid] { std::wstring hex; std::string e; return !profileFolderName(invalid, &hex, &e) && !e.empty(); });
    test("仅更改档案名称、时间与云状态", [] {
        auto text = profileText; std::string error;
        if (!patchProfileText(text, "联运 \"A\"", 456, 123, &error)) return false;
        return text.find("creation_time: 456") != std::string::npos && text.find("save_time: 123") != std::string::npos &&
            text.find("use_cloud: false") != std::string::npos && text.find("active_mods[1]: \"map_b\"") != std::string::npos &&
            text.find("online_password: \"fixture-password\"") != std::string::npos && text.find("// profile_name:") != std::string::npos;
    });
    for (const auto& bad : {std::string("SiiNunit\n{\nuser_profile : x { profile_name: \"a\" }\n}\n"),
        profileText.substr(0, profileText.size()-3), profileText + "user_profile : extra {}", std::string("BSII invalid")})
        test("损坏元信息失败并保留输入", [bad] { auto s = bad; std::string e; return !patchProfileText(s, "TMP", 1, 2, &e) && s == bad; });
    test("重复名称字段拒绝", [] { auto s = profileText; s.insert(s.find("creation_time:"), "profile_name: \"duplicate\"\n "); auto before = s; std::string e; return !patchProfileText(s, "TMP", 1, 2, &e) && s == before; });
    test("两个档案只携带各自选中的完整存档", [] {
        Fixture f; const auto r = splitFixture(f.request, f.paths);
        if (!r.ok || r.created.size() != 2 || !f.clean() || !f.unchanged()) return false;
        for (size_t i = 0; i < r.created.size(); ++i) {
            const fs::path p(r.created[i].profileDir), slot(r.created[i].slotDir);
            if (get(slot/L"game.sii") != get(fs::path(f.request.items[i].source.slotDir)/L"game.sii") ||
                get(slot/L"info.sii") != infoText || get(slot/L"preview.mat") != "preview " + std::to_string(i+1) ||
                get(p/L"controls.sii") != "controller settings\n" || fs::exists(p/L"profile.bak.sii") ||
                fs::exists(p/L"last_session_config.sii") || std::distance(fs::directory_iterator(p/L"save"), fs::directory_iterator()) != 1) return false;
        }
        return true;
    });
    test("本地同名档案无覆盖且整批预检", [] {
        Fixture f; fs::create_directory(f.target/L"544D50"); put(f.target/L"544D50/marker", "existing");
        auto r = splitFixture(f.request, f.paths);
        return !r.ok && r.created.empty() && get(f.target/L"544D50/marker") == "existing" && f.clean() && f.unchanged();
    });
    test("云档案同名拒绝", [] {
        Fixture f; auto cloud = f.base/L"cloud"; fs::create_directories(cloud/L"E88194E8BF90"); f.paths.nameCheckRoots.push_back(cloud.wstring());
        auto r = splitFixture(f.request, f.paths); return !r.ok && r.created.empty() && f.clean();
    });
    test("批内名称重复拒绝", [] { Fixture f; f.request.items[1].profileName = "TMP"; auto r = splitFixture(f.request, f.paths); return !r.ok && r.created.empty(); });
    test("源存档重复拒绝", [] { Fixture f; f.request.items[1].source = f.request.items[0].source; auto r = splitFixture(f.request, f.paths); return !r.ok && r.created.empty(); });
    test("跨源档案及目录跳转拒绝", [] {
        Fixture f; f.request.items[1].source.profileDir = f.target.wstring();
        auto r = splitFixture(f.request, f.paths); if (r.ok || !r.created.empty()) return false;
        f.request.items[1].source.profileDir = (f.source/L"../Original").wstring();
        r = splitFixture(f.request, f.paths); return !r.ok && r.created.empty();
    });
    test("缺失 profile.sii 失败", [] { Fixture f; fs::remove(f.source/L"profile.sii"); auto r = splitFixture(f.request, f.paths); return !r.ok && r.created.empty() && f.clean(); });
    test("二进制档案元信息不猜测修改", [] { Fixture f; put(f.source/L"profile.sii", "BSII binary"); auto r = splitFixture(f.request, f.paths); return !r.ok && r.created.empty() && f.clean(); });
    test("损坏加密进度拒绝且不发布先前项", [] { Fixture f; put(f.source/L"save/2/game.sii", "ScsC invalid"); auto r = splitFixture(f.request, f.paths); return !r.ok && r.created.empty() && f.clean() && f.unchanged(); });
    test("BSII 进度按原字节复制", [] { Fixture f; const std::string binary = "BSII\x01\x02\x03\x04"; put(f.source/L"save/1/game.sii", binary); auto r = splitFixture(f.request, f.paths); return r.ok && get(fs::path(r.created[0].slotDir)/L"game.sii") == binary; });
    test("存档子目录拒绝", [] { Fixture f; fs::create_directory(f.source/L"save/2/nested"); auto r = splitFixture(f.request, f.paths); return !r.ok && r.created.empty() && f.clean(); });
    test("取消清理全部暂存且不改源", [] {
        Fixture f; bool canceled = false; ProfileSplitOptions o; o.canceled = [&] { return canceled; };
        o.afterFile = [&](size_t) { canceled = true; return true; };
        auto r = splitFixture(f.request, f.paths, o); return !r.ok && r.created.empty() && f.clean() && f.unchanged();
    });
    test("写入失败清理暂存", [] { Fixture f; ProfileSplitOptions o; o.afterFile = [](size_t) { return false; }; auto r = splitFixture(f.request, f.paths, o); return !r.ok && r.created.empty() && f.clean() && f.unchanged(); });
    test("发布时名称被占用保持外部文件", [] {
        Fixture f; ProfileSplitOptions o; o.beforePublish = [](size_t, const std::wstring& target) { fs::create_directory(target); put(fs::path(target)/L"marker", "concurrent"); };
        auto r = splitFixture(f.request, f.paths, o); return !r.ok && r.created.empty() && get(f.target/L"544D50/marker") == "concurrent" && f.clean();
    });
    test("部分发布返回准确名单并保留已生成档案", [] {
        Fixture f; ProfileSplitOptions o; o.beforePublish = [](size_t i, const std::wstring& target) { if (i == 1) fs::create_directory(target); };
        auto r = splitFixture(f.request, f.paths, o); return !r.ok && r.created.size() == 1 && r.created[0].name == "TMP" && fs::exists(fs::path(r.created[0].slotDir)/L"game.sii") && f.clean();
    });
    test("源成员变化中止", [] {
        Fixture f; ProfileSplitOptions o; o.beforePublish = [&](size_t, const std::wstring&) { put(f.source/L"save/1/extra", "changed"); };
        auto r = splitFixture(f.request, f.paths, o); return !r.ok && r.created.empty() && f.clean();
    });
    test("云源合并本地控制器配置", [] {
        Fixture f; const auto local = f.base/L"steam_profiles/Original"; fs::create_directories(local);
        put(local/L"controls.sii", "local controller"); put(local/L"config_local.cfg", "local config");
        for (auto& item : f.request.items) item.source.cloud = true;
        f.paths.cloudLocalProfiles = local.parent_path().wstring();
        auto r = splitFixture(f.request, f.paths);
        return r.ok && get(fs::path(r.created[0].profileDir)/L"controls.sii") == "local controller" &&
            get(fs::path(r.created[1].profileDir)/L"config_local.cfg") == "local config" && get(f.source/L"controls.sii") == "controller settings\n";
    });
    test("游戏闸门变化拒绝发布", [] {
        Fixture f; bool blocked = false; ProfileSplitOptions o;
        o.guard = [&](std::string* e) { if (blocked) *e = "fixture process started"; return !blocked; };
        o.beforePublish = [&](size_t, const std::wstring&) { blocked = true; };
        auto r = splitFixture(f.request, f.paths, o); return !r.ok && r.created.empty() && f.clean();
    });
    test("生产入口拒绝切换游戏后的请求", [] {
        Fixture f; f.request.game = selectedGame() == GameId::Ets2 ? GameId::Ats : GameId::Ets2;
        auto r = splitSelectedProfiles(f.request); return !r.ok && r.created.empty() && !r.error.empty();
    });
    test("生产入口拒绝非本游戏列表的测试路径", [] { Fixture f; auto r = splitSelectedProfiles(f.request); return !r.ok && r.created.empty() && !r.error.empty() && f.unchanged(); });
    return out;
}
}
