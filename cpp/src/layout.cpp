#include "layout.h"
#include <cstring>
#include <cwctype>
namespace ets2 {
const LayoutProfile& layoutProfile(GameId game) {
    static const LayoutProfile ets{GameId::Ets2, 0, 0, 0x5F8B6A,
        {0x48,0x8B,0x1D,0x67,0x5B,0x0B,0x03,0x48,0x8B,0xF9,0x48,0x8B,0x9B,0xB0,0x31,0,0},0};
    static const LayoutProfile ats{GameId::Ats, 0x6ABDFBF2, 0x3973000, 0x5FA02A,
        {0x48,0x8B,0x1D,0x3F,0xED,0x0B,0x03,0x48,0x8B,0xF9,0x48,0x8B,0x9B,0xB0,0x31,0,0},0x22E6500,0x22C2B98,0x224DB18};
    return game == GameId::Ats ? ats : ets;
}
bool layoutFingerprintMatches(const LayoutProfile& p, uint32_t timestamp, uint32_t size,
                              const std::array<uint8_t,17>& bytes) {
    return (!p.timestamp || p.timestamp == timestamp) && (!p.imageSize || p.imageSize == size)
        && bytes == p.rootInstruction;
}
bool verifyGameLayout(const ProcessMemory& memory, uint64_t* imageBase, std::string* error) {
    uint64_t base=0; std::wstring path;
    auto fail=[&]() { if(error) *error="游戏版本或进程身份不匹配：仅支持已核对的 ETS2 1.61.1.1 / ATS 1.61.3.1"; return false; };
    if(!memory.mainImage(&base,&path)) return fail();
    const auto slash=path.find_last_of(L"\\/");
    std::wstring name=path.substr(slash==std::wstring::npos?0:slash+1);
    for(auto& ch:name) ch=static_cast<wchar_t>(std::towlower(ch));
    if(name!=selectedProcessName()) return fail();
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    if(!memory.read(base,&dos,sizeof(dos)) || dos.e_magic!=IMAGE_DOS_SIGNATURE ||
       dos.e_lfanew<0 || dos.e_lfanew>0x100000 ||
       !memory.read(base+dos.e_lfanew,&nt,sizeof(nt)) || nt.Signature!=IMAGE_NT_SIGNATURE ||
       nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return fail();
    const auto& p=layoutProfile(selectedGame());
    std::array<uint8_t,17> bytes{};
    if(nt.OptionalHeader.SizeOfImage<p.rootRva+bytes.size() ||
       !memory.read(base+p.rootRva,bytes.data(),bytes.size()) ||
       !layoutFingerprintMatches(p,nt.FileHeader.TimeDateStamp,nt.OptionalHeader.SizeOfImage,bytes)) return fail();
    if(imageBase) *imageBase=base;
    if(error) error->clear();
    return true;
}
}
