#include "layout.h"
#include <cstring>
#include <cwctype>
namespace ets2 {
const std::vector<LayoutProfile>& layoutProfiles(GameId game) {
    static const std::vector<LayoutProfile> ets{
        {GameId::Ets2, 0, 0, 0x5F8B6A,
         {0x48,0x8B,0x1D,0x67,0x5B,0x0B,0x03,0x48,0x8B,0xF9,0x48,0x8B,0x9B,0xB0,0x31,0,0},0,0,0,0x1158,"1.61.1.1"},
        {GameId::Ets2, 0x6ACDF049, 0x398B000, 0x5F8E8A,
         {0x48,0x8B,0x1D,0x37,0x69,0x0B,0x03,0x48,0x8B,0xF9,0x48,0x8B,0x9B,0xB0,0x31,0,0},0,0,0,0x1180,"1.61.1.9",
         0x64B9B0,{0xF3,0x0F,0x10,0xB3,0xB8,0x01,0,0,0xF3,0x0F,0x58,0xB1,0x80,0x11,0,0}}
    };
    static const std::vector<LayoutProfile> ats{{GameId::Ats, 0x6ABDFBF2, 0x3973000, 0x5FA02A,
        {0x48,0x8B,0x1D,0x3F,0xED,0x0B,0x03,0x48,0x8B,0xF9,0x48,0x8B,0x9B,0xB0,0x31,0,0},0x22E6500,0x22C2B98,0x224DB18,
        0x1158,"1.61.3.1"}};
    return game == GameId::Ats ? ats : ets;
}
const LayoutProfile& layoutProfile(GameId game) {return layoutProfiles(game).front();}
bool layoutFingerprintMatches(const LayoutProfile& p, uint32_t timestamp, uint32_t size,
                              const std::array<uint8_t,17>& bytes) {
    return (!p.timestamp || p.timestamp == timestamp) && (!p.imageSize || p.imageSize == size)
        && bytes == p.rootInstruction;
}
bool layoutFuelFormulaMatches(const LayoutProfile& p,const std::array<uint8_t,16>& bytes) {
    return !p.fuelFormulaRva || bytes==p.fuelFormulaInstruction;
}
bool verifyGameLayout(const ProcessMemory& memory, uint64_t* imageBase, std::string* error,
                      const LayoutProfile** matched) {
    if(imageBase)*imageBase=0;
    if(matched)*matched=nullptr;
    uint64_t base=0; std::wstring path;
    auto fail=[&](const std::string& reason) {if(error)*error=reason;return false;};
    if(!memory.mainImage(&base,&path))return fail("无法读取游戏主映像，请重新附加并检查进程权限。");
    const auto slash=path.find_last_of(L"\\/");
    std::wstring name=path.substr(slash==std::wstring::npos?0:slash+1);
    for(auto& ch:name) ch=static_cast<wchar_t>(std::towlower(ch));
    if(name!=selectedProcessName())return fail("游戏进程身份不匹配，请检查欧卡 / 美卡选择并重新附加。");
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    if(!memory.read(base,&dos,sizeof(dos)) || dos.e_magic!=IMAGE_DOS_SIGNATURE ||
       dos.e_lfanew<0 || dos.e_lfanew>0x100000 ||
       !memory.read(base+dos.e_lfanew,&nt,sizeof(nt)) || nt.Signature!=IMAGE_NT_SIGNATURE ||
       nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return fail("游戏主映像 PE 头不可读或不是受支持的 64 位映像。");
    for(const auto& p:layoutProfiles(selectedGame())) {
        if((p.timestamp&&p.timestamp!=nt.FileHeader.TimeDateStamp) ||
           (p.imageSize&&p.imageSize!=nt.OptionalHeader.SizeOfImage))continue;
        std::array<uint8_t,17> bytes{};
        if(nt.OptionalHeader.SizeOfImage<p.rootRva+bytes.size() ||
           !memory.read(base+p.rootRva,bytes.data(),bytes.size()) ||
           !layoutFingerprintMatches(p,nt.FileHeader.TimeDateStamp,nt.OptionalHeader.SizeOfImage,bytes))continue;
        if(p.fuelFormulaRva) {
            std::array<uint8_t,16> formula{};
            if(nt.OptionalHeader.SizeOfImage<p.fuelFormulaRva+formula.size() ||
               !memory.read(base+p.fuelFormulaRva,formula.data(),formula.size()) ||
               !layoutFuelFormulaMatches(p,formula))continue;
        }
        if(imageBase)*imageBase=base;
        if(matched)*matched=&p;
        if(error)error->clear();
        return true;
    }
    return fail(fmt("游戏版本或指令不匹配（PE 时间戳 %08X，映像 %X）：支持 ETS2 1.61.1.1 / 1.61.1.9，ATS 1.61.3.1；已拒绝使用未核对的偏移。",
        nt.FileHeader.TimeDateStamp,nt.OptionalHeader.SizeOfImage));
}
}
