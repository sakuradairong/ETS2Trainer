#include "savecopytest.h"
#include "savecopy.h"
#include <filesystem>
#include <stdexcept>
#include <bcrypt.h>
#include <cstring>
#include <atomic>
#include <thread>

namespace ets2 {
namespace {
namespace fs = std::filesystem;
const std::string infoText = "SiiNunit\r\n{\r\nsave_container : _nameless.1 {\r\n name: 1\r\n time: 125\r\n file_time: 100\r\n version: 102\r\n dependencies: 1\r\n dependencies[0]: \"dlc_test\"\r\n}\r\n}\r\n";
void put(const fs::path& p, const std::string& text) {
    if (!writeFileBytes(p.wstring(), {text.begin(), text.end()})) throw std::runtime_error("fixture write failed");
}
std::string get(const fs::path& p) {
    std::vector<uint8_t> b;
    if (!readFileBytes(p.wstring(), b)) throw std::runtime_error("fixture read failed");
    return {b.begin(), b.end()};
}
bool stageGone(const fs::path& root) {
    for (const auto& e : fs::directory_iterator(root))
        if (e.path().filename().wstring().find(L".trainer_copy_") == 0) return false;
    return true;
}
// Synthetic ScsC metadata, using the same public save-format AES key as the decoder.
const uint8_t fixtureKey[32] = {0x2a,0x5f,0xcb,0x17,0x91,0xd2,0x2f,0xb6,0x02,0x45,0xb3,
    0xd8,0x36,0x9e,0xd0,0xb2,0xc2,0x73,0x71,0x56,0x3f,0xbf,0x1f,0x3c,0x9e,0xdf,0x6b,0x11,0x82,0x5a,0x5d,0x0a};
std::vector<uint8_t> pkcs7(size_t n) {
    const uint8_t p = static_cast<uint8_t>(16 - (n % 16));
    return std::vector<uint8_t>(p, p);
}
uint32_t adlerOf(const std::vector<uint8_t>& d) {
    uint32_t a = 1, b = 0;
    for (uint8_t c : d) { a = (a + c) % 65521u; b = (b + a) % 65521u; }
    return (b << 16) | a;
}
// Stored-block zlib stream (BFINAL=1, BTYPE=00) with a correct Adler32 trailer.
std::vector<uint8_t> zlibStored(const std::string& text) {
    std::vector<uint8_t> z;
    z.push_back(0x78); z.push_back(0x9C);
    const size_t total = text.size();
    size_t off = 0; bool last = false;
    do {
        size_t len = total - off; if (len > 65535) len = 65535;
        last = (off + len) == total;
        z.push_back(last ? 0x01 : 0x00);
        z.push_back(static_cast<uint8_t>(len & 0xFF)); z.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        const uint16_t nlen = static_cast<uint16_t>(~(static_cast<uint16_t>(len)));
        z.push_back(static_cast<uint8_t>(nlen & 0xFF)); z.push_back(static_cast<uint8_t>(nlen >> 8));
        for (size_t i = off; i < off + len; ++i) z.push_back(static_cast<uint8_t>(text[i]));
        off += len;
    } while (!last);
    const uint32_t ad = adlerOf(std::vector<uint8_t>(text.begin(), text.end()));
    z.push_back(static_cast<uint8_t>(ad >> 24)); z.push_back(static_cast<uint8_t>(ad >> 16));
    z.push_back(static_cast<uint8_t>(ad >> 8)); z.push_back(static_cast<uint8_t>(ad));
    return z;
}
std::vector<uint8_t> encryptPayload(const std::vector<uint8_t>& payload, uint32_t declared,
                                    const std::vector<uint8_t>& padBytes) {
    std::vector<uint8_t> body = payload;
    body.insert(body.end(), padBytes.begin(), padBytes.end());
    if (body.empty() || (body.size() % 16) != 0) throw std::runtime_error("AES fixture alignment");
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_KEY_HANDLE key = nullptr;
    auto close = [&] { if (key) BCryptDestroyKey(key); if (alg) BCryptCloseAlgorithmProvider(alg,0); };
    try {
        if (BCryptOpenAlgorithmProvider(&alg,BCRYPT_AES_ALGORITHM,nullptr,0)<0 ||
            BCryptSetProperty(alg,BCRYPT_CHAINING_MODE,(PUCHAR)BCRYPT_CHAIN_MODE_CBC,sizeof(BCRYPT_CHAIN_MODE_CBC),0)<0 ||
            BCryptGenerateSymmetricKey(alg,&key,nullptr,0,(PUCHAR)fixtureKey,32,0)<0) throw std::runtime_error("AES fixture init");
        std::vector<uint8_t> result(56+body.size()); memcpy(result.data(),"ScsC",4);
        memcpy(result.data()+52,&declared,4);
        uint8_t iv[16] = {}; ULONG written = 0;
        if (BCryptEncrypt(key,body.data(),(ULONG)body.size(),nullptr,iv,16,result.data()+56,(ULONG)body.size(),&written,0)<0 || written!=body.size())
            throw std::runtime_error("AES fixture encrypt");
        close(); return result;
    } catch (...) { close(); throw; }
}
std::vector<uint8_t> encryptedInfo(const std::string& text) {
    return encryptPayload(std::vector<uint8_t>(text.begin(),text.end()),static_cast<uint32_t>(text.size()),pkcs7(text.size()));
}
}
std::vector<TunerTestItem> runSaveCopyTests() {
    std::vector<TunerTestItem> items;
    auto check = [&](const char* name, bool ok, const std::string& detail = {}) {items.push_back({std::string("存档复制 / ")+name,ok,detail});};
    const fs::path base = fs::canonical(applicationDir());
    const fs::path root = base/(L"savecopy_fixture_"+std::to_wstring(GetCurrentProcessId())+L"_"+std::to_wstring(GetTickCount64()));
    bool owned = false;
    auto cleanup = [&] {
        if (!owned) return;
        if (fs::canonical(root).parent_path()!=base || (GetFileAttributesW(root.c_str())&FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("fixture cleanup path mismatch");
        fs::remove_all(root); owned=false;
    };
    try {
        if (!fs::create_directory(root)) throw std::runtime_error("fixture root collision");
        owned=true; fs::create_directories(root/L"save"/L"1");
        const fs::path source = root/L"save"/L"1";
        const std::string game("BSII\0\x81\xFFpayload",14), preview("preview\0pixels",14);
        put(source/L"game.sii",game); put(source/L"info.sii",infoText);
        put(source/L"preview.tga",preview); put(source/L"game_bak.sii",game);
        SaveSlot slot; slot.profileDir=root.wstring(); slot.slotDir=source.wstring(); slot.gameSii=(source/L"game.sii").wstring();
        const std::string name = "中文 \"副本\" \\x41";
        std::string escaped,error;
        check("中文引号反斜杠往返",siiEscapeName(name,&escaped,&error) && siiUnescapeName(escaped)==name,error);
        auto text=infoText;
        check("仅修改名称与文件时间",patchSaveInfo(text,name,123456,&error) && text.find(" time: 125\r\n")!=std::string::npos &&
              text.find("file_time: 123456")!=std::string::npos && text.find("dependencies[0]: \"dlc_test\"")!=std::string::npos,error);
        for (const auto& bad : std::vector<std::string>{"", "   ", "a\nb", std::string("a\0b",3), std::string(65,'a'), std::string("\xC0\xAF",2)})
            check("拒绝无效名称", !siiEscapeName(bad,&escaped,&error));
        check("64 字符边界",siiEscapeName(std::string(64,'a'),&escaped,&error));
        for (const auto& bad : std::vector<std::string>{"BSII", "SiiNunit { save_container : a { name: \"unterminated } }",
            "SiiNunit { save_container : a { name: a name: b file_time: 1 } }",
            "SiiNunit { save_container : a { name: a file_time: nope } }",
            "SiiNunit { save_container : a { name: a file_time: 1 } save_container : b { name: b file_time: 2 } }",
            "SiiNunit { save_container : a { name: a } }", "SiiNunit { save_container : a { name: a file_time: 1 nested { } } }"}) {
            auto copy=bad; check("拒绝损坏元信息且不改文本",!patchSaveInfo(copy,"copy",100,&error) && copy==bad);
        }
        auto comments=std::string("SiiNunit { save_container : a {\n# name: fake {\n name: \"brace } // text\"\n file_time: 1 // old\n} }");
        check("忽略注释与名称内括号",patchSaveInfo(comments,name,2,&error),error);
        auto result=copySaveSlotInProfile(slot,name);
        const fs::path dest(result.slotDir);
        check("成功创建槽位2",result.ok && result.slotName==L"2",result.error);
        if (result.ok) {
            check("进度预览备份逐字节保留",get(dest/L"game.sii")==game && get(dest/L"game_bak.sii")==game && get(dest/L"preview.tga")==preview);
            SaveSlot clone=slot; clone.slotDir=result.slotDir;
            check("副本名称回读",readSaveDisplayName(clone)==name);
        }
        check("源四个文件不变",get(source/L"game.sii")==game && get(source/L"info.sii")==infoText && get(source/L"preview.tga")==preview && get(source/L"game_bak.sii")==game);
        fs::create_directory(root/L"save"/L"3"); put(root/L"save"/L"4","occupied");
        result=copySaveSlotInProfile(slot,"next"); check("跳过目录和文件占用编号",result.ok && result.slotName==L"5",result.error);
        SaveCopyOptions options; options.afterFile=[](size_t){return false;};
        result=copySaveSlotInProfile(slot,"fault",options);
        check("中途失败只清理暂存",!result.ok && stageGone(root) && !fs::exists(root/L"save"/L"6"),result.error);
        options={}; int calls=0; options.canceled=[&]{return ++calls>=4;};
        result=copySaveSlotInProfile(slot,"cancel",options);
        check("取消不发布且清理暂存",!result.ok && stageGone(root) && !fs::exists(root/L"save"/L"6"),result.error);
        options={}; options.beforePublish=[](const std::wstring& path){put(fs::path(path),"racer");};
        result=copySaveSlotInProfile(slot,"race",options);
        check("发布碰撞保留已有文件",!result.ok && get(root/L"save"/L"6")=="racer" && stageGone(root),result.error);
        options={}; int gates=0; options.guard=[&](std::string*){return ++gates<6;};
        result=copySaveSlotInProfile(slot,"gate",options);
        check("发布前条件改变取消",!result.ok && stageGone(root) && !fs::exists(root/L"save"/L"7"),result.error);
        fs::create_directory(source/L"nested"); result=copySaveSlotInProfile(slot,"nested");
        check("拒绝子目录",!result.ok && stageGone(root)); fs::remove(source/L"nested");
        const fs::path alias=root/L"save"/L"alias";
        if (CreateSymbolicLinkW(alias.c_str(),source.c_str(),SYMBOLIC_LINK_FLAG_DIRECTORY|2)) {
            SaveSlot linked=slot; linked.slotDir=alias.wstring();
            check("拒绝目录重解析点",!copySaveSlotInProfile(linked,"linked").ok);
            fs::remove(alias);
        } else check("重解析点测试环境",true,"当前权限不支持创建符号链接，未验证该分支");
        const fs::path linkedFile=source/L"linked.tga";
        if (CreateSymbolicLinkW(linkedFile.c_str(),(source/L"preview.tga").c_str(),2)) {
            check("拒绝文件重解析点",!copySaveSlotInProfile(slot,"linkedfile").ok);
            fs::remove(linkedFile);
        } else check("文件重解析点测试环境",true,"当前权限不支持创建符号链接，未验证该分支");
        SaveSlot wrong=slot; wrong.profileDir=(root/L"save").wstring();
        check("拒绝不属档案直接子槽位",!copySaveSlotInProfile(wrong,"wrong").ok);
        wrong=slot; wrong.slotDir=(source/L"..").wstring();
        check("拒绝点目录跳转",!copySaveSlotInProfile(wrong,"wrong").ok);
        HANDLE writer=CreateFileW((source/L"game.sii").c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
        result=copySaveSlotInProfile(slot,"writer");
        check("拒绝正在写入源文件",writer!=INVALID_HANDLE_VALUE && !result.ok);
        if (writer!=INVALID_HANDLE_VALUE) CloseHandle(writer);
        fs::remove(source/L"info.sii"); check("缺少元信息拒绝",!copySaveSlotInProfile(slot,"missing").ok);
        put(source/L"info.sii","ScsC corrupt"); check("损坏加密元信息拒绝",!copySaveSlotInProfile(slot,"corrupt").ok);
        auto encrypted=encryptedInfo(infoText); writeFileBytes((source/L"info.sii").wstring(),encrypted);
        result=copySaveSlotInProfile(slot,"加密副本");
        check("ScsC元信息解密并写为SiiN",result.ok && get(fs::path(result.slotDir)/L"info.sii").find("SiiNunit")==0,result.error);
        check("加密源不变",get(source/L"info.sii")==std::string(encrypted.begin(),encrypted.end()));
        std::vector<uint8_t> decoded; DecryptInfo di;
        check("元信息解码长度限制",!decryptSave((source/L"info.sii").wstring(),decoded,&di,&error,16));
        uint8_t stored[]={1,3,0,252,255,'a','b','c'};
        check("存储块解压限制",!inflateRaw(stored,sizeof(stored),decoded,&error,2) && decoded.size()<=2);
        check("存储块允许精确边界",inflateRaw(stored,sizeof(stored),decoded,&error,3) && decoded.size()==3);
        const uint8_t emptyFixed[]={3,0};
        const uint8_t paddedFixed[]={3,252};
        check("空固定块完整流通过",inflateRaw(emptyFixed,sizeof(emptyFixed),decoded,&error,1) && decoded.empty());
        check("末字节非零填充位合法",inflateRaw(paddedFixed,sizeof(paddedFixed),decoded,&error,1) && decoded.empty());
        const uint8_t trailingFixed[]={3,0,255};
        error.clear();
        check("固定块尾随整字节拒绝",!inflateRaw(trailingFixed,sizeof(trailingFixed),decoded,&error,1) &&
              error.find("多余数据")!=std::string::npos,error);
        std::vector<uint8_t> trailingStored(stored,stored+sizeof(stored)); trailingStored.push_back(0);
        check("存储块尾随整字节拒绝",!inflateRaw(trailingStored.data(),trailingStored.size(),decoded,&error,3),error);
        const uint8_t fixed[]={75,76,28,5,163,96,20,12,119,0,0};
        check("固定码表回溯解压限制",!inflateRaw(fixed,sizeof(fixed),decoded,&error,999) && decoded.size()<=999);
        check("固定码表边界",inflateRaw(fixed,sizeof(fixed),decoded,&error,1000) && decoded==std::vector<uint8_t>(1000,'a'));
        const uint8_t dynamic[]={237,203,181,97,2,0,0,0,48,160,72,113,119,167,197,221,29,254,255,138,43,216,146,61,129,96,232,39,28,137,198,126,227,137,100,42,157,201,230,242,133,98,169,92,169,214,234,141,102,171,221,233,246,250,131,191,255,225,104,60,153,206,230,139,229,106,189,217,238,246,135,227,233,124,185,222,238,143,231,235,29,240,125,223,247,125,223,247,125,223,247,125,223,247,125,223,247,125,223,247,125,223,247,125,223,247,125,223,247,125,223,247,125,223,247,125,223,247,191,250,63};
        check("动态码表解压限制",!inflateRaw(dynamic,sizeof(dynamic),decoded,&error,12799) && decoded.size()<=12799);
        bool full=inflateRaw(dynamic,sizeof(dynamic),decoded,&error,12800) && decoded.size()==12800;
        for(size_t i=0;full && i<decoded.size();++i) full=decoded[i]==i%64;
        check("动态码表边界",full);
        put(source/L"info.sii.trainer.tmp","companion");
        result=copySaveSlotInProfile(slot,"companions");
        check("任意常规伴随文件保留",result.ok && get(fs::path(result.slotDir)/L"info.sii.trainer.tmp")=="companion",result.error);
        const GameId original=selectedGame();
        for (GameId gameId : kAllGameIds) {
            setSelectedGame(gameId);
            SaveCopyRequest request{gameId==GameId::Ets2?GameId::Ats:GameId::Ets2,slot,"wronggame"};
            check(gameId==GameId::Ets2 ? "欧卡入口拒绝跨游戏" : "美卡入口拒绝跨游戏",!copySlotToNewSave(request).ok);
            request.game=gameId;
            auto rejected=copySlotToNewSave(request);
            check(gameId==GameId::Ets2 ? "欧卡入口拒绝模拟路径或运行中游戏" : "美卡入口拒绝模拟路径或运行中游戏",!rejected.ok,rejected.error);
        }
        setSelectedGame(original);
        check("全部失败路径无暂存残留",stageGone(root));
        // Regression: a source membership change during copy must fail closed instead of publishing an incomplete clone.
        auto nextFree=[&]{for(unsigned n=1;n<=1000000;++n) if(!fs::exists(root/L"save"/std::to_wstring(n))) return n; return 0u;};
        {
            const fs::path target=root/L"save"/std::to_wstring(nextFree()), added=source/L"late_after.tga";
            const std::string gameBefore=get(source/L"game.sii"),infoBefore=get(source/L"info.sii");
            SaveCopyOptions opts; opts.afterFile=[&](size_t){put(added,"late");return true;};
            auto r=copySaveSlotInProfile(slot,"lateafter",opts);
            check("复制中新增常规文件失败关闭",
                !r.ok && r.slotDir.empty() && r.slotName.empty() && !fs::exists(target) && stageGone(root) &&
                fs::is_regular_file(added) && get(added)=="late" &&
                get(source/L"game.sii")==gameBefore && get(source/L"info.sii")==infoBefore,r.error);
            fs::remove(added);
        }
        {
            const fs::path target=root/L"save"/std::to_wstring(nextFree()), added=source/L"late_publish.tga";
            const std::string gameBefore=get(source/L"game.sii"),infoBefore=get(source/L"info.sii");
            SaveCopyOptions opts; opts.beforePublish=[&](const std::wstring&){put(added,"late");};
            auto r=copySaveSlotInProfile(slot,"latepublish",opts);
            check("发布前新增常规文件失败关闭",
                !r.ok && r.slotDir.empty() && r.slotName.empty() && !fs::exists(target) && stageGone(root) &&
                fs::is_regular_file(added) && get(added)=="late" &&
                get(source/L"game.sii")==gameBefore && get(source/L"info.sii")==infoBefore,r.error);
            fs::remove(added);
        }
        {
            const fs::path target=root/L"save"/std::to_wstring(nextFree()), added=source/L"late_dir";
            const std::string gameBefore=get(source/L"game.sii"),infoBefore=get(source/L"info.sii");
            SaveCopyOptions opts; opts.beforePublish=[&](const std::wstring&){fs::create_directory(added);};
            auto r=copySaveSlotInProfile(slot,"latedir",opts);
            check("发布前新增子目录失败关闭",
                !r.ok && r.slotDir.empty() && r.slotName.empty() && !fs::exists(target) && stageGone(root) &&
                fs::is_directory(added) &&
                get(source/L"game.sii")==gameBefore && get(source/L"info.sii")==infoBefore,r.error);
            fs::remove(added);
        }
        // ---- decryptSave 加固回归（仍位于同一隔离夹具根下）----
        {
            std::vector<uint8_t> out; DecryptInfo di; std::string de;
            auto runDec=[&](const std::vector<uint8_t>& bytes,size_t cap,std::vector<uint8_t>& o,DecryptInfo& d,std::string& e){
                if(!writeFileBytes((root/L"dec_tmp.sii").wstring(),bytes)) throw std::runtime_error("dec fixture write failed");
                return decryptSave((root/L"dec_tmp.sii").wstring(),o,&d,&e,cap);
            };
            const std::vector<uint8_t> plainText(infoText.begin(),infoText.end());
            const std::vector<uint8_t> plainGame(game.begin(),game.end());
            check("明文文本直接通过",runDec(plainText,0,out,di,de) && out==plainText,de);
            check("明文BSII逐字节保留",runDec(plainGame,0,out,di,de) && out==plainGame,de);
            const auto zbytes=zlibStored(infoText);
            const auto zcrypt=encryptPayload(zbytes,static_cast<uint32_t>(infoText.size()),pkcs7(zbytes.size()));
            check("压缩ScsC解码",runDec(zcrypt,0,out,di,de) && out==plainText && di.compressed,de);
            auto zTrailing=zbytes; zTrailing.insert(zTrailing.end()-4,0xFF);
            check("ScsC压缩流尾随垃圾拒绝且清空输出",
                  !runDec(encryptPayload(zTrailing,static_cast<uint32_t>(infoText.size()),pkcs7(zTrailing.size())),0,out,di,de) &&
                  out.empty() && di.innerFormat.empty() && di.innerSize==0 && de.find("多余数据")!=std::string::npos,de);
            const std::string unknownInner="Qnknown binary \x01\x02 payload";
            const auto uz=zlibStored(unknownInner);
            check("压缩未知内容拒绝",!runDec(encryptPayload(uz,static_cast<uint32_t>(unknownInner.size()),pkcs7(uz.size())),0,out,di,de) && out.empty(),de);
            const std::vector<uint8_t> unknownBytes={0x11,0x22,0x33,0x44,0x7A,0x7A};
            check("未知输入拒绝且清空输出",!runDec(unknownBytes,0,out,di,de) && out.empty() && di.innerSize==0,de);
            auto trunc=zcrypt; trunc.pop_back();
            check("密文截断字节拒绝",!runDec(trunc,0,out,di,de) && out.empty(),de);
            auto extra=zcrypt; extra.push_back(0);
            check("密文多余字节拒绝",!runDec(extra,0,out,di,de),de);
            std::vector<uint8_t> badPad(16,3); badPad[14]=7;
            check("PKCS7字节不符拒绝",!runDec(encryptPayload(std::vector<uint8_t>(plainText.begin(),plainText.begin()+16),16,badPad),0,out,di,de) && out.empty(),de);
            std::vector<uint8_t> badRng(16,0); badRng[0]=1;
            check("PKCS7范围无效拒绝",!runDec(encryptPayload(std::vector<uint8_t>(plainText.begin(),plainText.begin()+16),16,badRng),0,out,di,de),de);
            auto zFchk=zbytes; zFchk[1]=0x9D;
            check("zlib FCHECK拒绝",!runDec(encryptPayload(zFchk,static_cast<uint32_t>(infoText.size()),pkcs7(zFchk.size())),0,out,di,de),de);
            auto zFdict=zbytes; zFdict[1]=0x20;
            check("zlib FDICT拒绝",!runDec(encryptPayload(zFdict,static_cast<uint32_t>(infoText.size()),pkcs7(zFdict.size())),0,out,di,de),de);
            auto zAdler=zbytes; zAdler.back()^=0xFF;
            check("zlib Adler不匹配拒绝",!runDec(encryptPayload(zAdler,static_cast<uint32_t>(infoText.size()),pkcs7(zAdler.size())),0,out,di,de),de);
            auto zTrunc=zbytes; zTrunc.resize(zTrunc.size()-5);
            check("zlib数据截断拒绝",!runDec(encryptPayload(zTrunc,static_cast<uint32_t>(infoText.size()),pkcs7(zTrunc.size())),0,out,di,de),de);
            check("声明长度不符拒绝",!runDec(encryptPayload(zbytes,static_cast<uint32_t>(infoText.size())+1,pkcs7(zbytes.size())),0,out,di,de),de);
            check("输出上限拒绝",!runDec(zcrypt,infoText.size()-1,out,di,de) && out.empty(),de);
            check("输出上限边界通过",runDec(zcrypt,infoText.size(),out,di,de) && out==plainText,de);
        }
        // ---- exportDecrypted 非覆盖导出回归 ----
        {
            const fs::path exports=root/L"exports"; fs::create_directory(exports);
            SaveSlot ex; ex.gameSii=(source/L"game.sii").wstring();
            DecryptInfo di; std::string de;
            put(source/L"game.sii",infoText);
            check("明文导出成功",exportDecrypted(ex,(exports/L"t1.sii").wstring(),&di,&de) &&
                  get(exports/L"t1.sii")==infoText,de);
            put(source/L"game.sii",game);
            check("二进制导出逐字节保留",exportDecrypted(ex,(exports/L"b.sii").wstring(),&di,&de) &&
                  get(exports/L"b.sii")==game && di.innerFormat=="bsii",de);
            put(exports/L"e.sii","sentinel");
            check("已存在目标不覆盖",!exportDecrypted(ex,(exports/L"e.sii").wstring(),&di,&de) &&
                  get(exports/L"e.sii")=="sentinel",de);
            check("目录目标拒绝",!exportDecrypted(ex,exports.wstring(),&di,&de) && fs::is_directory(exports),de);
            check("源等于目标拒绝",!exportDecrypted(ex,(source/L"game.sii").wstring(),&di,&de) &&
                  get(source/L"game.sii")==game,de);
            check("不存在目录导出失败无残留",!exportDecrypted(ex,(root/L"absent_dir"/L"n.sii").wstring(),&di,&de) &&
                  !fs::exists(root/L"absent_dir"),de);
            put(source/L"game.sii","corrupt not a known format");
            check("损坏源无输出",!exportDecrypted(ex,(exports/L"c.sii").wstring(),&di,&de) &&
                  !fs::exists(exports/L"c.sii"),de);
            bool noTemp=true;
            for (const auto& entry : fs::recursive_directory_iterator(root))
                if (entry.path().filename().wstring().find(L"~ets2export_")==0) noTemp=false;
            check("导出无临时残留",noTemp);
        }
        {
            put(source/L"game.sii",game);
            put(source/L"info.sii",infoText);
            const std::string replacement="SiiNunit { vehicle : _nameless.1 { accessories: 0 } }";
            SaveCopyOptions options; options.replacementGameText=&replacement;
            auto imported=copySaveSlotInProfile(slot,"车辆导入测试",options);
            check("车辆导入只替换新槽位进度",imported.ok &&
                get(fs::path(imported.slotDir)/L"game.sii")==replacement &&
                get(source/L"game.sii")==game && get(source/L"info.sii")==infoText &&
                get(fs::path(imported.slotDir)/L"preview.tga")==preview,imported.error);
            if(imported.ok) {
                auto plaintext=slot;
                plaintext.slotDir=imported.slotDir;
                plaintext.gameSii=(fs::path(imported.slotDir)/L"game.sii").wstring();
                const auto repeated=copySaveSlotInProfile(plaintext,"文本存档再次复制");
                check("已导入文本存档可再次复制",repeated.ok && repeated.slotDir!=imported.slotDir &&
                      get(fs::path(repeated.slotDir)/L"game.sii")==replacement &&
                      get(plaintext.gameSii)==replacement && stageGone(root),repeated.error);
            }
            const std::string invalid="BSIIinvalid";
            options.replacementGameText=&invalid;
            auto rejected=copySaveSlotInProfile(slot,"无效车辆导入",options);
            check("车辆导入非文本替换拒绝",!rejected.ok && rejected.slotDir.empty() && stageGone(root),rejected.error);
            options.replacementGameText=&replacement;
            bool canceled=false;
            options.canceled=[&]{return canceled;};
            options.beforePublish=[&](const std::wstring&){canceled=true;};
            rejected=copySaveSlotInProfile(slot,"取消车辆导入",options);
            check("车辆导入取消保留原档且清理暂存",!rejected.ok && rejected.slotDir.empty() && stageGone(root) &&
                get(source/L"game.sii")==game && get(source/L"info.sii")==infoText,rejected.error);
        }
        cleanup(); check("隔离夹具清理",!fs::exists(root));
    } catch (const std::exception& e) { check("夹具异常",false,e.what()); try {cleanup();} catch(...) {check("清理失败",false);} }
    return items;
}

std::vector<TunerTestItem> runReadableExportTests() {
    std::vector<TunerTestItem> items;
    auto check = [&](const char* name, bool ok, const std::string& detail = "") {
        items.push_back({std::string("可读导出 / ") + name, ok, detail});
    };
    fs::path base = fs::absolute(fs::temp_directory_path()).lexically_normal();
    if (base.filename().empty()) base = base.parent_path();
    const fs::path root = base / (L"readable_export_fixture_" +
        std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    bool owned = false;
    auto cleanup = [&] {
        if (!owned) return;
        if (fs::absolute(root).lexically_normal().parent_path() != base ||
            (GetFileAttributesW(root.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("readable export fixture cleanup path mismatch");
        fs::remove_all(root);
        owned = false;
    };
    try {
        if (!fs::create_directory(root)) throw std::runtime_error("readable export fixture collision");
        owned = true;
        const fs::path exports = root / L"exports";
        fs::create_directory(exports);
        std::vector<std::pair<fs::path, std::string>> originals;
        auto source = [&](const wchar_t* name, const std::string& bytes) {
            const fs::path path = root / name;
            put(path, bytes);
            originals.emplace_back(path, bytes);
            SaveSlot slot; slot.gameSii = path.wstring();
            return slot;
        };
        // Wire literals are independent of the decoder: garage=1224619291, koln=1349667.
        std::string binary = "BSII";
        auto u8 = [&](uint8_t v) { binary.push_back(static_cast<char>(v)); };
        auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) u8(static_cast<uint8_t>(v >> (8 * i))); };
        auto u64 = [&](uint64_t v) { for (int i = 0; i < 8; ++i) u8(static_cast<uint8_t>(v >> (8 * i))); };
        auto str = [&](const std::string& s) { u32(static_cast<uint32_t>(s.size())); binary += s; };
        u32(3);
        u32(0); u8(1); u32(1); str("garage");
        u32(0x03); str("city");
        u32(0x04); str("cities");
        u32(0);
        u32(1); u8(2); u64(1224619291ull); u64(1349667ull);
        u64(1349667ull);
        u32(2); u64(1349667ull); u64(1224619291ull | (1ull << 63));
        u32(0); u8(0);

        const SaveSlot plain = source(L"text.sii", infoText);
        const SaveSlot bsii = source(L"binary.sii", binary);
        ReadableExportInfo ri; std::string err = "STALE";
        auto out = [&](const wchar_t* name) { return (exports / name).wstring(); };
        auto clearInfo = [&] {
            return ri.innerFormat.empty() && !ri.innerSize && !ri.textSize && !ri.unitCount;
        };
        auto readable = [&](const SaveSlot& slot, const wchar_t* name) {
            return exportReadableSii(slot, out(name), &ri, &err);
        };
        const bool plainOk = readable(plain, L"text.sii");
        check("明文逐字节保留且清除旧错误", plainOk && get(out(L"text.sii")) == infoText &&
            ri.innerFormat == "text" && ri.textSize == infoText.size() && err.empty(), err);
        const bool binaryOk = readable(bsii, L"binary.sii");
        const std::string converted = binaryOk ? get(out(L"binary.sii")) : "";
        check("BSII对象名与标量名称方向正确", binaryOk &&
            converted.rfind("SiiNunit\n", 0) == 0 &&
            converted.find("garage : garage.koln {") != std::string::npos &&
            converted.find("city: koln") != std::string::npos, err);
        check("BSII数组名称与高位标记正确", binaryOk &&
            converted.find("cities[0]: koln") != std::string::npos &&
            converted.find("cities[1]: garage") != std::string::npos, err);
        check("输出元信息准确", binaryOk && ri.innerFormat == "bsii" &&
            ri.innerSize == binary.size() && ri.textSize == converted.size() && ri.unitCount == 1);
        DecryptInfo di;
        check("原格式导出仍保留BSII字节", exportDecrypted(bsii, out(L"raw.sii"), &di, &err) &&
            get(out(L"raw.sii")) == binary && di.innerFormat == "bsii", err);
        const SaveSlot encryptedBinary = source(L"encrypted_binary.sii",
            [&] { auto b = encryptedInfo(binary); return std::string(b.begin(), b.end()); }());
        check("加密BSII自动解密转换", readable(encryptedBinary, L"encrypted_binary.sii") &&
            get(out(L"encrypted_binary.sii")) == converted, err);
        const SaveSlot encryptedText = source(L"encrypted_text.sii",
            [&] { auto b = encryptedInfo(infoText); return std::string(b.begin(), b.end()); }());
        check("加密文本原样导出", readable(encryptedText, L"encrypted_text.sii") &&
            get(out(L"encrypted_text.sii")) == infoText, err);
        const auto compressed = zlibStored(binary);
        const auto encryptedZlib = encryptPayload(compressed, static_cast<uint32_t>(binary.size()), pkcs7(compressed.size()));
        const SaveSlot zbsii = source(L"zlib_binary.sii", {encryptedZlib.begin(), encryptedZlib.end()});
        check("加密压缩BSII自动转换", readable(zbsii, L"zlib_binary.sii") &&
            get(out(L"zlib_binary.sii")) == converted, err);
        put(out(L"existing.sii"), "sentinel");
        check("目标已存在不覆盖", !readable(bsii, L"existing.sii") &&
            get(out(L"existing.sii")) == "sentinel" && clearInfo() && !err.empty(), err);
        check("目录目标拒绝", !exportReadableSii(bsii, exports.wstring(), &ri, &err) &&
            fs::is_directory(exports) && clearInfo(), err);
        check("源等于目标拒绝", !exportReadableSii(bsii, bsii.gameSii, &ri, &err) &&
            get(bsii.gameSii) == binary && clearInfo(), err);
        std::string v4 = binary; v4[4] = 4;
        const SaveSlot unknown = source(L"v4.sii", v4);
        check("未知BSII版本明确拒绝无输出", !readable(unknown, L"v4.sii") &&
            !fs::exists(out(L"v4.sii")) && clearInfo() && err.find("v4") != std::string::npos, err);
        const SaveSlot truncated = source(L"truncated.sii", binary.substr(0, 8));
        check("截断BSII拒绝无输出", !readable(truncated, L"truncated.sii") &&
            !fs::exists(out(L"truncated.sii")) && clearInfo(), err);
        const SaveSlot legacy = source(L"legacy.sii", "3nKlegacy");
        check("3nK拒绝可读转换", !readable(legacy, L"legacy.sii") &&
            !fs::exists(out(L"legacy.sii")) && clearInfo(), err);
        check("3nK仍可原格式导出", exportDecrypted(legacy, out(L"legacy_raw.sii"), &di, &err) &&
            get(out(L"legacy_raw.sii")) == "3nKlegacy", err);
        const SaveSlot corrupt = source(L"corrupt.sii", "corrupt save");
        check("损坏源拒绝无输出", !readable(corrupt, L"corrupt.sii") &&
            !fs::exists(out(L"corrupt.sii")) && clearInfo(), err);
        auto badBytes = encryptedInfo(infoText); badBytes.pop_back();
        const SaveSlot badCrypto = source(L"bad_crypto.sii", {badBytes.begin(), badBytes.end()});
        check("损坏加密源拒绝无输出", !readable(badCrypto, L"bad_crypto.sii") &&
            !fs::exists(out(L"bad_crypto.sii")) && clearInfo(), err);
        check("明文输出超限拒绝", !exportReadableSii(plain, out(L"limit_text.sii"), &ri, &err, {}, infoText.size() - 1) &&
            !fs::exists(out(L"limit_text.sii")) && clearInfo(), err);
        check("明文输出边界通过", exportReadableSii(plain, out(L"limit_text_ok.sii"), &ri, &err, {}, infoText.size()), err);
        check("BSII文本输出超限拒绝", !converted.empty() &&
            !exportReadableSii(bsii, out(L"limit_bsii.sii"), &ri, &err, {}, converted.size() - 1) &&
            !fs::exists(out(L"limit_bsii.sii")) && clearInfo(), err);
        check("BSII文本输出边界通过", !converted.empty() &&
            exportReadableSii(bsii, out(L"limit_bsii_ok.sii"), &ri, &err, {}, converted.size()), err);
        check("零输出上限拒绝", !exportReadableSii(plain, out(L"zero_limit.sii"), &ri, &err, {}, 0) &&
            !fs::exists(out(L"zero_limit.sii")) && clearInfo(), err);
        SaveSlot absent; absent.gameSii = (root / L"absent.sii").wstring();
        check("开始前取消不读取源", !exportReadableSii(absent, out(L"cancel_early.sii"), &ri, &err, [] { return true; }) &&
            !fs::exists(out(L"cancel_early.sii")) && err.find("取消") != std::string::npos && clearInfo(), err);
        int polls = 0;
        check("转换期间取消无输出", !exportReadableSii(bsii, out(L"cancel_decode.sii"), &ri, &err,
            [&] { return ++polls >= 3; }) && polls >= 3 &&
            !fs::exists(out(L"cancel_decode.sii")) && err.find("取消") != std::string::npos && clearInfo(), err);
        auto stagedSize = [&] {
            for (const auto& e : fs::directory_iterator(exports)) {
                if (e.path().filename().wstring().rfind(L"~ets2export_", 0) != 0) continue;
                std::error_code ec;
                const uintmax_t size = fs::file_size(e.path(), ec);
                if (!ec) return size;
            }
            return uintmax_t{0};
        };
        std::string large = "SiiNunit\n{\n//"; large.append(3 * 1024 * 1024, 'x'); large += "\n}\n";
        const SaveSlot big = source(L"large.sii", large);
        bool sawPartial = false;
        check("分块写入期间取消清理临时文件", !exportReadableSii(big, out(L"cancel_write.sii"), &ri, &err,
            [&] { sawPartial = sawPartial || stagedSize() >= 1024 * 1024; return sawPartial; }) &&
            sawPartial && !fs::exists(out(L"cancel_write.sii")) && clearInfo(), err);
        bool sawComplete = false;
        check("发布前取消不生成最终文件", !exportReadableSii(plain, out(L"cancel_publish.sii"), &ri, &err,
            [&] { sawComplete = sawComplete || stagedSize() == infoText.size(); return sawComplete; }) &&
            sawComplete && !fs::exists(out(L"cancel_publish.sii")) && clearInfo(), err);
        check("原格式导出发布前取消", !exportDecrypted(plain, out(L"cancel_raw.sii"), &di, &err,
            [&] { return stagedSize() == infoText.size(); }) && !fs::exists(out(L"cancel_raw.sii")) && di.innerFormat.empty(), err);
        check("缺失目标目录不创建且无输出", !exportReadableSii(plain, (root / L"absent_dir" / L"out.sii").wstring(), &ri, &err) &&
            !fs::exists(root / L"absent_dir") && clearInfo(), err);
        std::atomic<int> ready{0}; std::atomic<bool> go{false};
        bool success[2] = {false, false};
        auto racer = [&](int i) {
            ready.fetch_add(1);
            while (!go.load()) std::this_thread::yield();
            ReadableExportInfo info; std::string error;
            success[i] = exportReadableSii(bsii, out(L"race.sii"), &info, &error);
        };
        std::thread a(racer, 0), b(racer, 1);
        while (ready.load() != 2) std::this_thread::yield();
        go = true; a.join(); b.join();
        check("并发导出仅一项成功且不覆盖", success[0] != success[1] && get(out(L"race.sii")) == converted);
        bool unchanged = true;
        for (const auto& entry : originals) unchanged = unchanged && get(entry.first) == entry.second;
        check("全部源文件逐字节未改变", unchanged);
        bool noTemp = true;
        for (const auto& e : fs::directory_iterator(exports))
            noTemp = noTemp && e.path().filename().wstring().rfind(L"~ets2export_", 0) != 0;
        check("取消与失败无临时残留", noTemp);
        cleanup(); check("隔离夹具清理", !fs::exists(root));
    } catch (const std::exception& e) {
        check("夹具异常", false, e.what());
        try { cleanup(); } catch (const std::exception& ce) { check("清理失败", false, ce.what()); }
    }
    return items;
}
}
