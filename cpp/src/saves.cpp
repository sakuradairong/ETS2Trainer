// saves.cpp —— 存档处理实现
#include "saves.h"

#include <bcrypt.h>
#include <shlobj.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <io.h>
#include <ctime>
#include <map>
#include <set>

#pragma comment(lib, "bcrypt.lib")

namespace ets2 {

namespace {

//: ScsC 使用的固定 AES-256 密钥
const uint8_t kSiiKey[32] = {0x2a, 0x5f, 0xcb, 0x17, 0x91, 0xd2, 0x2f, 0xb6, 0x02, 0x45, 0xb3,
                             0xd8, 0x36, 0x9e, 0xd0, 0xb2, 0xc2, 0x73, 0x71, 0x56, 0x3f, 0xbf,
                             0x1f, 0x3c, 0x9e, 0xdf, 0x6b, 0x11, 0x82, 0x5a, 0x5d, 0x0a};

// ======================= DEFLATE 解压（RFC1951）=======================
const uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19, 23, 27,
                               31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                               2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1,    2,    3,    4,    5,    7,    9,    13,   17,   25,
                                33,   49,   65,   97,   129,  193,  257,  385,  513,  769,
                                1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

struct BitReader {
    const uint8_t* p = nullptr;
    size_t size = 0;
    size_t pos = 0;
    uint32_t bitBuf = 0;
    int bitCnt = 0;
    bool err = false;

    bool ensure(int n) {
        while (bitCnt < n) {
            if (pos >= size) {
                err = true;
                return false;
            }
            bitBuf |= (uint32_t)p[pos++] << bitCnt;
            bitCnt += 8;
        }
        return true;
    }
    uint32_t bits(int n) {
        if (n == 0) return 0;
        if (!ensure(n)) return 0;
        uint32_t v = bitBuf & ((1u << n) - 1u);
        bitBuf >>= n;
        bitCnt -= n;
        return v;
    }
    void alignByte() {
        bitBuf = 0;
        bitCnt = 0;
    }
};

struct Huffman {
    uint16_t counts[16] = {0};
    std::vector<uint16_t> symbols;

    void build(const uint8_t* lengths, int n) {
        for (int i = 0; i < 16; ++i) counts[i] = 0;
        for (int i = 0; i < n; ++i) counts[lengths[i]]++;
        counts[0] = 0;
        uint16_t offs[16] = {0};
        for (int i = 1; i < 16; ++i) offs[i] = (uint16_t)(offs[i - 1] + counts[i - 1]);
        symbols.assign((size_t)(offs[15] + counts[15]), 0);
        for (int i = 0; i < n; ++i) {
            if (lengths[i]) symbols[offs[lengths[i]]++] = (uint16_t)i;
        }
    }
    int decode(BitReader& br) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; ++len) {
            code |= (int)br.bits(1);
            int count = counts[len];
            if (code - first < count) return symbols[(size_t)(index + (code - first))];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        return -1;
    }
};

void buildFixedLengths(std::vector<uint8_t>& lit, std::vector<uint8_t>& dist) {
    lit.assign(288, 0);
    for (int i = 0; i < 144; ++i) lit[(size_t)i] = 8;
    for (int i = 144; i < 256; ++i) lit[(size_t)i] = 9;
    for (int i = 256; i < 280; ++i) lit[(size_t)i] = 7;
    for (int i = 280; i < 288; ++i) lit[(size_t)i] = 8;
    dist.assign(30, 5);
}

bool inflateBlockData(BitReader& br, const Huffman& lenH, const Huffman& distH,
                      std::vector<uint8_t>& out, std::string* err) {
    for (;;) {
        int sym = lenH.decode(br);
        if (sym < 0) {
            if (err) *err = "解压失败：字面量/长度符号无效";
            return false;
        }
        if (sym < 256) {
            out.push_back((uint8_t)sym);
            continue;
        }
        if (sym == 256) return true;
        int li = sym - 257;
        if (li >= 29) {
            if (err) *err = "解压失败：长度符号超出范围";
            return false;
        }
        size_t length = (size_t)kLenBase[li] + br.bits(kLenExtra[li]);
        int dsym = distH.decode(br);
        if (dsym < 0 || dsym >= 30) {
            if (err) *err = "解压失败：距离符号无效";
            return false;
        }
        size_t distance = (size_t)kDistBase[dsym] + br.bits(kDistExtra[dsym]);
        if (distance > out.size()) {
            if (err) *err = "解压失败：距离超出已输出数据";
            return false;
        }
        size_t start = out.size() - distance;
        for (size_t i = 0; i < length; ++i) out.push_back(out[start + i]);
    }
}

}  // namespace

bool inflateRaw(const uint8_t* in, size_t inSize, std::vector<uint8_t>& out, std::string* err) {
    BitReader br;
    br.p = in;
    br.size = inSize;
    out.clear();
    out.reserve(inSize * 4 + 64);

    std::vector<uint8_t> fixedLit, fixedDist;
    buildFixedLengths(fixedLit, fixedDist);
    Huffman fixedLenH, fixedDistH;
    fixedLenH.build(fixedLit.data(), (int)fixedLit.size());
    fixedDistH.build(fixedDist.data(), (int)fixedDist.size());

    bool last = false;
    while (!last) {
        last = br.bits(1) != 0;
        uint32_t type = br.bits(2);
        if (type == 0) {  // 未压缩块
            br.alignByte();
            if (br.pos + 4 > br.size) {
                if (err) *err = "解压失败：存储块头不完整";
                return false;
            }
            uint16_t len = (uint16_t)(in[br.pos] | (in[br.pos + 1] << 8));
            br.pos += 4;
            if (br.pos + len > br.size) {
                if (err) *err = "解压失败：存储块数据不完整";
                return false;
            }
            out.insert(out.end(), in + br.pos, in + br.pos + len);
            br.pos += len;
        } else if (type == 1) {
            if (!inflateBlockData(br, fixedLenH, fixedDistH, out, err)) return false;
        } else if (type == 2) {
            int hlit = (int)br.bits(5) + 257;
            int hdist = (int)br.bits(5) + 1;
            int hclen = (int)br.bits(4) + 4;
            static const uint8_t kOrder[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                               11, 4,  12, 3, 13, 2, 14, 1, 15};
            uint8_t clLens[19] = {0};
            for (int i = 0; i < hclen; ++i) clLens[kOrder[i]] = (uint8_t)br.bits(3);
            Huffman clH;
            clH.build(clLens, 19);
            std::vector<uint8_t> lengths((size_t)(hlit + hdist), 0);
            size_t idx = 0;
            while (idx < lengths.size()) {
                int sym = clH.decode(br);
                if (sym < 0) {
                    if (err) *err = "解压失败：码长符号无效";
                    return false;
                }
                if (sym < 16) {
                    lengths[idx++] = (uint8_t)sym;
                } else if (sym == 16) {
                    if (idx == 0) {
                        if (err) *err = "解压失败：重复上次码长但无前值";
                        return false;
                    }
                    uint8_t prev = lengths[idx - 1];
                    int rep = 3 + (int)br.bits(2);
                    while (rep-- > 0 && idx < lengths.size()) lengths[idx++] = prev;
                } else if (sym == 17) {
                    int rep = 3 + (int)br.bits(3);
                    while (rep-- > 0 && idx < lengths.size()) lengths[idx++] = 0;
                } else {
                    int rep = 11 + (int)br.bits(7);
                    while (rep-- > 0 && idx < lengths.size()) lengths[idx++] = 0;
                }
            }
            Huffman lenH, distH;
            lenH.build(lengths.data(), hlit);
            distH.build(lengths.data() + hlit, hdist);
            if (!inflateBlockData(br, lenH, distH, out, err)) return false;
        } else {
            if (err) *err = "解压失败：未知块类型";
            return false;
        }
        if (br.err) {
            if (err) *err = "解压失败：数据提前结束";
            return false;
        }
    }
    return true;
}

// ======================= AES-256-CBC（Windows CNG）=======================
namespace {

class AesCbc {
public:
    ~AesCbc() { close(); }

    bool open(std::string* err) {
        NTSTATUS st = ::BCryptOpenAlgorithmProvider(&alg_, BCRYPT_AES_ALGORITHM, nullptr, 0);
        if (st < 0) {
            if (err) *err = "BCryptOpenAlgorithmProvider 失败";
            return false;
        }
        st = ::BCryptSetProperty(alg_, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC,
                                 sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
        if (st < 0) {
            if (err) *err = "BCryptSetProperty(CBC) 失败";
            return false;
        }
        return true;
    }

    void close() {
        if (key_) {
            ::BCryptDestroyKey(key_);
            key_ = nullptr;
        }
        if (alg_) {
            ::BCryptCloseAlgorithmProvider(alg_, 0);
            alg_ = nullptr;
        }
    }

    bool decrypt(const uint8_t* iv, const uint8_t* data, size_t size, std::vector<uint8_t>& out,
                 std::string* err) {
        if (size == 0 || (size % 16) != 0) {
            if (err) *err = "密文长度不是 16 的倍数";
            return false;
        }
        if (!key_) {
            NTSTATUS st = ::BCryptGenerateSymmetricKey(alg_, &key_, nullptr, 0, (PUCHAR)kSiiKey,
                                                       sizeof(kSiiKey), 0);
            if (st < 0) {
                if (err) *err = "BCryptGenerateSymmetricKey 失败";
                return false;
            }
        }
        out.resize(size);
        uint8_t ivBuf[16];
        ::memcpy(ivBuf, iv, 16);
        ULONG done = 0;
        NTSTATUS st = ::BCryptDecrypt(key_, (PUCHAR)data, (ULONG)size, nullptr, ivBuf, 16,
                                      out.data(), (ULONG)out.size(), &done, 0);
        if (st < 0) {
            if (err) *err = fmt("BCryptDecrypt 失败: 0x%08X", (unsigned)st);
            return false;
        }
        out.resize(done);
        return true;
    }

private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_KEY_HANDLE key_ = nullptr;
};

}  // namespace

std::string detectFormat(const std::vector<uint8_t>& data) {
    if (data.size() >= 4) {
        if (::memcmp(data.data(), "ScsC", 4) == 0) return "scs";
        if (::memcmp(data.data(), "BSII", 4) == 0) return "bsii";
        if (data.size() >= 8 && ::memcmp(data.data(), "SiiNunit", 8) == 0) return "text";
        if (::memcmp(data.data(), "NSii", 4) == 0) return "text";
        if (::memcmp(data.data(), "3nK", 3) == 0) return "3nk";
    }
    return "unknown";
}

bool readFileBytes(const std::wstring& path, std::vector<uint8_t>& out) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || !fp) return false;
    fseek(fp, 0, SEEK_END);
    long long size = _ftelli64(fp);
    fseek(fp, 0, SEEK_SET);
    if (size < 0) {
        fclose(fp);
        return false;
    }
    out.resize((size_t)size);
    size_t read = size ? fread(out.data(), 1, out.size(), fp) : 0;
    fclose(fp);
    out.resize(read);
    return true;
}

bool writeFileBytes(const std::wstring& path, const std::vector<uint8_t>& data) {
    // 原子写入：先写同目录临时文件并落盘，再整体替换目标。
    // 直接以 "wb" 截断原文件时，一旦崩溃/断电就是"存档或 config.cfg 被清空"，
    // 对游戏存档是不可接受的数据损失。
    const std::wstring temp = path + L".trainer.tmp";
    {
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, temp.c_str(), L"wb") != 0 || !fp) return false;
        size_t written = data.empty() ? 0 : fwrite(data.data(), 1, data.size(), fp);
        const bool flushed = (fflush(fp) == 0);
        const bool synced = flushed && (_commit(_fileno(fp)) == 0);
        fclose(fp);
        if (written != data.size() || !flushed) {
            ::DeleteFileW(temp.c_str());
            return false;
        }
        (void)synced;  // _commit 失败只说明缓冲未强制落盘，不影响替换的正确性
    }
    if (!::MoveFileExW(temp.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        // 替换失败（目标被占用等）：清理临时文件，保留原文件不动。
        ::DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

bool decryptSave(const std::wstring& path, std::vector<uint8_t>& inner, DecryptInfo* info,
                 std::string* err) {
    std::vector<uint8_t> raw;
    if (!readFileBytes(path, raw)) {
        if (err) *err = "读取存档文件失败";
        return false;
    }
    DecryptInfo local;
    local.innerFormat = detectFormat(raw);
    local.innerSize = raw.size();
    if (local.innerFormat != "scs") {  // 已经是明文/二进制
        inner = std::move(raw);
        if (info) *info = local;
        return true;
    }
    if (raw.size() < 56) {
        if (err) *err = "ScsC 文件过短";
        return false;
    }
    local.encrypted = true;
    local.declaredSize = 0;
    for (int i = 0; i < 4; ++i) {
        local.declaredSize |= (uint64_t)raw[52 + (size_t)i] << (8 * i);
    }
    const uint8_t* iv = raw.data() + 36;
    size_t cipherSize = raw.size() - 56;
    cipherSize -= cipherSize % 16;
    AesCbc aes;
    std::string aesErr;
    if (!aes.open(&aesErr)) {
        if (err) *err = aesErr;
        return false;
    }
    std::vector<uint8_t> payload;
    if (!aes.decrypt(iv, raw.data() + 56, cipherSize, payload, err)) return false;
    aes.close();
    // 去掉 PKCS7 填充
    if (!payload.empty()) {
        uint8_t pad = payload.back();
        if (pad > 0 && pad <= 16 && (size_t)pad <= payload.size()) {
            payload.resize(payload.size() - pad);
        }
    }
    local.payloadSize = payload.size();
    // zlib 头 0x78 -> 解压
    if (!payload.empty() && payload[0] == 0x78) {
        std::vector<uint8_t> plain;
        std::string zerr;
        if (!inflateRaw(payload.data() + 2, payload.size() - 2, plain, &zerr)) {
            if (err) *err = zerr;
            return false;
        }
        payload = std::move(plain);
        local.compressed = true;
    }
    local.innerFormat = detectFormat(payload);
    local.innerSize = payload.size();
    inner = std::move(payload);
    if (info) *info = local;
    return true;
}

bool exportDecrypted(const SaveSlot& slot, const std::wstring& outPath, DecryptInfo* info,
                     std::string* err) {
    std::vector<uint8_t> inner;
    if (!decryptSave(slot.gameSii, inner, info, err)) return false;
    if (!writeFileBytes(outPath, inner)) {
        if (err) *err = "写入导出文件失败";
        return false;
    }
    return true;
}

// ======================= 路径与存档定位 =======================
namespace {

std::wstring exeDir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = ::GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(wchar_t)));
    std::wstring path(buf, n);
    size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(0, pos);
}

bool isDir(const std::wstring& path) {
    DWORD attr = ::GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool isFile(const std::wstring& path) {
    DWORD attr = ::GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::vector<std::wstring> listDirs(const std::wstring& parent) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd{};
    std::wstring pattern = parent + L"\\*";
    HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            std::wstring name = fd.cFileName;
            if (name != L"." && name != L"..") out.push_back(name);
        }
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

uint64_t fileMtimeUnix(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    ULARGE_INTEGER li;
    li.LowPart = data.ftLastWriteTime.dwLowDateTime;
    li.HighPart = data.ftLastWriteTime.dwHighDateTime;
    if (li.QuadPart < 116444736000000000ULL) return 0;
    return (li.QuadPart - 116444736000000000ULL) / 10000000ULL;
}

uint64_t fileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    ULARGE_INTEGER li;
    li.LowPart = data.nFileSizeLow;
    li.HighPart = data.nFileSizeHigh;
    return li.QuadPart;
}

int hexVal(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

std::wstring decodeProfileName(const std::wstring& folder) {
    if (folder.size() >= 6 && (folder.size() % 2) == 0) {
        std::string bytes;
        bytes.reserve(folder.size() / 2);
        bool ok = true;
        for (size_t i = 0; i + 1 < folder.size() && ok; i += 2) {
            int hi = hexVal(folder[i]), lo = hexVal(folder[i + 1]);
            if (hi < 0 || lo < 0) ok = false;
            else bytes.push_back((char)((hi << 4) | lo));
        }
        if (ok && !bytes.empty()) {
            std::wstring wide = U2W(bytes);
            if (!wide.empty()) return wide;
        }
    }
    return folder;
}

std::string describeFormat(const std::string& fmt) {
    if (fmt == "scs") return "加密(ScsC)";
    if (fmt == "text") return "明文(SiiNunit)";
    if (fmt == "bsii") return "二进制(BSII)";
    if (fmt == "3nk") return "旧版加密(3nK)";
    return "未知";
}

std::string makeLabel(const SaveSlot& s) {
    return fmt("%s / %s [%s]", W2U(s.profileName).c_str(), W2U(s.slotName).c_str(),
               s.cloud ? "Steam云" : "本地");
}

}  // namespace

std::wstring documentsDir() {
    std::wstring base;
    PWSTR known = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &known)) && known) {
        base = known;
        ::CoTaskMemFree(known);
    } else {
        wchar_t buf[MAX_PATH];
        if (::GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH)) {
            base = std::wstring(buf) + L"\\Documents";
        }
    }
    std::wstring candidates[] = {base + L"\\Euro Truck Simulator 2",
                                 base + L"\\OneDrive\\Documents\\Euro Truck Simulator 2"};
    for (const auto& c : candidates) {
        if (isDir(c)) return c;
    }
    return candidates[0];
}

std::wstring steamInstallDir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD size = (DWORD)(sizeof(buf) - 2);
    if (::RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ,
                       nullptr, buf, &size) == ERROR_SUCCESS) {
        std::wstring path(buf);
        if (isDir(path)) return path;
    }
    size = (DWORD)(sizeof(buf) - 2);
    if (::RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam",
                       L"InstallPath", RRF_RT_REG_SZ, nullptr, buf,
                       &size) == ERROR_SUCCESS) {
        std::wstring path(buf);
        if (isDir(path)) return path;
    }
    const wchar_t* defaults[] = {L"C:\\Program Files (x86)\\Steam",
                                 L"C:\\Program Files\\Steam"};
    for (const auto* d : defaults) {
        if (isDir(d)) return d;
    }
    return L"";
}

std::vector<std::wstring> cloudProfileDirs() {
    std::vector<std::wstring> out;
    std::wstring steam = steamInstallDir();
    if (steam.empty()) return out;
    std::wstring userdata = steam + L"\\userdata";
    for (const auto& user : listDirs(userdata)) {
        std::wstring profiles = userdata + L"\\" + user + L"\\227300\\remote\\profiles";
        if (isDir(profiles)) out.push_back(profiles);
    }
    return out;
}

std::wstring applicationDir() { return exeDir(); }

std::wstring backupRoot() { return applicationDir() + L"\\backups"; }

std::vector<SaveSlot> listSlots() {
    std::vector<SaveSlot> slots;
    std::wstring docs = documentsDir();
    struct Root {
        std::wstring dir;
        bool         cloud;
    };
    std::vector<Root> roots = {{docs + L"\\profiles", false},
                               {docs + L"\\steam_profiles", false}};
    for (const auto& c : cloudProfileDirs()) roots.push_back({c, true});

    for (const auto& root : roots) {
        if (!isDir(root.dir)) continue;
        for (const auto& profile : listDirs(root.dir)) {
            std::wstring profileDir = root.dir + L"\\" + profile;
            std::wstring saveRoot = profileDir + L"\\save";
            if (!isDir(saveRoot)) continue;
            for (const auto& slotName : listDirs(saveRoot)) {
                std::wstring slotDir = saveRoot + L"\\" + slotName;
                std::wstring gameSii = slotDir + L"\\game.sii";
                if (!isFile(gameSii)) continue;
                SaveSlot slot;
                slot.profileDir = profileDir;
                slot.slotDir = slotDir;
                slot.gameSii = gameSii;
                slot.profileName = decodeProfileName(profile);
                slot.slotName = slotName;
                slot.cloud = root.cloud;
                slot.mtime = fileMtimeUnix(gameSii);
                slot.size = fileSize(gameSii);
                std::vector<uint8_t> head;
                if (readFileBytes(gameSii, head)) {
                    if (head.size() > 32) head.resize(32);
                    slot.format = detectFormat(head);
                } else {
                    slot.format = "unknown";
                }
                slot.formatText = describeFormat(slot.format);
                slot.label = makeLabel(slot);
                slots.push_back(std::move(slot));
            }
        }
    }
    std::sort(slots.begin(), slots.end(),
              [](const SaveSlot& a, const SaveSlot& b) { return a.mtime > b.mtime; });
    return slots;
}

// ======================= 明文存档：读 / 改 数值 =======================
namespace {

bool parseIntLine(const std::string& line, std::string* nameOut, int64_t* valueOut,
                  size_t* digitsStart, size_t* digitsEnd) {
    size_t colon = line.find(':');
    if (colon == std::string::npos) return false;
    size_t i = line.find_first_not_of(" \t");
    if (i == std::string::npos || i >= colon) return false;
    // 冒号前可能还有空格（例如 "bank : xyz"），先去掉
    size_t nameEnd = colon;
    while (nameEnd > i && (line[nameEnd - 1] == ' ' || line[nameEnd - 1] == '\t')) --nameEnd;
    if (nameEnd == i) return false;
    for (size_t k = i; k < nameEnd; ++k) {
        char c = line[k];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '[' || c == ']';
        if (!ok) return false;
    }
    *nameOut = line.substr(i, nameEnd - i);
    size_t v = colon + 1;
    while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;
    const size_t signedStart = v;
    if (v < line.size() && line[v] == '-') ++v;
    size_t d0 = v;
    while (v < line.size() && line[v] >= '0' && line[v] <= '9') ++v;
    if (v == d0) return false;
    size_t d1 = v;
    size_t t = v;
    while (t < line.size() &&
           (line[t] == ' ' || line[t] == '\t' || line[t] == '\r' || line[t] == '\n')) {
        ++t;
    }
    if (t != line.size()) return false;
    int64_t val = 0;
    const auto parsed = std::from_chars(line.data() + signedStart, line.data() + d1, val);
    if (parsed.ec != std::errc{} || parsed.ptr != line.data() + d1) return false;
    *valueOut = val;
    *digitsStart = signedStart;  // 原值带 '-' 时，替换必须覆盖符号，否则 -500 -> 1000 会变 -1000
    *digitsEnd = d1;
    return true;
}

bool parseUnitLine(const std::string& line, std::string* unitOut) {
    size_t end = line.size();
    while (end > 0 && (line[end - 1] == '\r' || line[end - 1] == '\n' || line[end - 1] == ' ' ||
                       line[end - 1] == '\t')) {
        --end;
    }
    if (end == 0 || line[end - 1] != '{') return false;
    size_t colon = line.find(':');
    if (colon == std::string::npos) return false;
    size_t i = line.find_first_not_of(" \t");
    if (i == std::string::npos || i >= colon) return false;
    // 冒号前常有空格（"bank : _nameless.xxx {"），要去掉后再校验名字
    size_t nameEnd = colon;
    while (nameEnd > i && (line[nameEnd - 1] == ' ' || line[nameEnd - 1] == '\t')) --nameEnd;
    if (nameEnd == i) return false;
    for (size_t k = i; k < nameEnd; ++k) {
        char c = line[k];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    *unitOut = line.substr(i, nameEnd - i);
    return true;
}

enum class TextKind { None, Money, BankMoney, Experience };

TextKind matchRule(const std::string& unit, const std::string& name) {
    // 部分版本将玩家金额保存在 bank，部分存在 economy；优先 economy，缺失时回退 bank。
    if (unit == "bank" && name == "money_account") return TextKind::BankMoney;
    if (unit == "economy" && name == "money_account") return TextKind::Money;
    if (unit == "economy" && name == "experience_points") return TextKind::Experience;
    return TextKind::None;
}

//: 遍历明文存档的数值行。回调参数:
//:   lineOffset - 该行在文件中的起始偏移；ds/de - 数字在该行内的范围；kind - 匹配到的语义
template <typename Fn>
void forEachValueLine(const std::string& text, Fn&& fn) {
    std::vector<std::string> stack;
    size_t p = 0;
    while (p < text.size()) {
        size_t nl = text.find('\n', p);
        size_t lineEnd = (nl == std::string::npos) ? text.size() : nl + 1;
        std::string line = text.substr(p, lineEnd - p);
        size_t first = line.find_first_not_of(" \t\r\n");
        if (first != std::string::npos) {
            if (line[first] == '#' ||
                (line[first] == '/' && first + 1 < line.size() && line[first + 1] == '/')) {
                p = lineEnd;
                continue;
            }
            std::string unit;
            if (parseUnitLine(line, &unit)) {
                stack.push_back(unit);
            } else {
                // 注意：存档里的闭合括号是缩进的（如 "    }"），必须从首个非空白字符开始数
                size_t closes = 0;
                for (size_t k = first; k < line.size() && line[k] == '}'; ++k) ++closes;
                if (closes) {
                    for (size_t k = 0; k < closes && !stack.empty(); ++k) stack.pop_back();
                } else if (!stack.empty() && !stack.back().empty()) {
                    std::string name;
                    int64_t value = 0;
                    size_t ds = 0, de = 0;
                    if (parseIntLine(line, &name, &value, &ds, &de)) {
                        TextKind kind = matchRule(stack.back(), name);
                        if (kind != TextKind::None) {
                            fn(stack.back(), name, p, ds, de, value, kind);
                        }
                    }
                }
            }
        }
        p = lineEnd;
    }
}

}  // namespace

bool readTextValues(const std::wstring& path, TextValues* out, std::string* err) {
    std::vector<uint8_t> bytes;
    if (!readFileBytes(path, bytes)) {
        if (err) *err = "读取存档失败";
        return false;
    }
    std::string text(bytes.begin(), bytes.end());
    TextValues values;
    forEachValueLine(text, [&](const std::string&, const std::string&, size_t, size_t, size_t,
                               int64_t value, TextKind kind) {
        if (kind == TextKind::Money || (kind == TextKind::BankMoney && !values.hasMoney)) {
            // 无论文件顺序如何，economy 金额覆盖 bank 回退值。
            values.hasMoney = true;
            values.money = value;
        } else if (kind == TextKind::Experience && !values.hasExperience) {
            values.hasExperience = true;
            values.experience = value;
        }
    });
    if (out) *out = values;
    return values.hasMoney || values.hasExperience;
}

bool patchTextSave(const std::wstring& path, bool setMoney, int64_t money, bool setExp,
                   int64_t experience, std::string* err) {
    std::vector<uint8_t> bytes;
    if (!readFileBytes(path, bytes)) {
        if (err) *err = "读取存档失败";
        return false;
    }
    std::string text(bytes.begin(), bytes.end());

    struct Edit {
        size_t  start;   // 文件中数字的起始偏移
        size_t  end;
        int64_t value;
    };
    std::vector<Edit> edits;
    bool hasEconomyMoney = false;
    forEachValueLine(text, [&](const std::string&, const std::string&, size_t, size_t, size_t,
                                int64_t, TextKind kind) {
        if (kind == TextKind::Money) hasEconomyMoney = true;
    });
    bool doneMoney = false, doneExp = false;
    forEachValueLine(text, [&](const std::string&, const std::string&, size_t lineOffset,
                               size_t ds, size_t de, int64_t, TextKind kind) {
        if (setMoney && !doneMoney &&
            (kind == TextKind::Money || (kind == TextKind::BankMoney && !hasEconomyMoney))) {
            edits.push_back({lineOffset + ds, lineOffset + de, money});
            doneMoney = true;
        } else if (kind == TextKind::Experience && setExp && !doneExp) {
            edits.push_back({lineOffset + ds, lineOffset + de, experience});
            doneExp = true;
        }
    });

    if (edits.empty()) {
        if (err) *err = "没有在该存档中找到可修改的字段（可能不是标准明文存档）";
        return false;
    }
    // 从后往前替换，避免前面的修改影响后面的偏移
    std::sort(edits.begin(), edits.end(),
              [](const Edit& a, const Edit& b) { return a.start > b.start; });
    for (const auto& e : edits) {
        text.replace(e.start, e.end - e.start, std::to_string(e.value));
    }
    std::vector<uint8_t> outBytes(text.begin(), text.end());
    if (!writeFileBytes(path, outBytes)) {
        if (err) *err = "写入存档失败";
        return false;
    }
    logLine(fmt("明文存档已修改 %d 处数值", (int)edits.size()));
    return true;
}

// ======================= 备份 / 还原 =======================
namespace {

std::vector<std::wstring> listFiles(const std::wstring& dir) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd{};
    std::wstring pattern = dir + L"\\*";
    HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(fd.cFileName);
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

bool ensureDirRecursive(const std::wstring& dir) {
    if (dir.empty()) return false;
    if (isDir(dir)) return true;
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 2) ensureDirRecursive(dir.substr(0, slash));
    ::CreateDirectoryW(dir.c_str(), nullptr);
    return isDir(dir);
}

std::wstring nowStamp() {
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    return fmtW(L"%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                st.wSecond);
}

std::wstring safeName(const std::wstring& text) {
    std::wstring out;
    for (wchar_t c : text) {
        if (c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'/' || c == L'\\' ||
            c == L'|' || c == L'?' || c == L'*' || c == L' ') {
            out += L'_';
        } else {
            out += c;
        }
        if (out.size() >= 60) break;
    }
    if (out.empty()) out = L"unnamed";
    return out;
}

std::string iniValue(const std::string& text, const std::string& key) {
    std::string needle = key + "=";
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line =
            text.substr(pos, (nl == std::string::npos) ? std::string::npos : nl - pos);
        if (line.compare(0, needle.size(), needle) == 0) {
            std::string value = line.substr(needle.size());
            while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
                value.pop_back();
            }
            return value;
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return {};
}

std::vector<std::wstring> splitSemi(const std::string& text) {
    std::vector<std::wstring> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t sep = text.find(';', pos);
        std::string item =
            (sep == std::string::npos) ? text.substr(pos) : text.substr(pos, sep - pos);
        if (!item.empty()) out.push_back(U2W(item));
        if (sep == std::string::npos) break;
        pos = sep + 1;
    }
    return out;
}

}  // namespace

std::wstring backupSlot(const SaveSlot& slot, const char* label, std::string* err) {
    std::wstring root = backupRoot();
    if (!ensureDirRecursive(root)) {
        if (err) *err = "创建备份目录失败";
        return L"";
    }
    const std::wstring base = root + L"\\" + nowStamp() + L"_" + safeName(slot.profileName) +
                              L"_" + safeName(slot.slotName);
    std::wstring dir = base;
    for (int suffix = 1; isDir(dir) && suffix < 10000; ++suffix) {
        dir = base + fmtW(L"_%02d", suffix);
    }
    if (isDir(dir)) {
        if (err) *err = "创建唯一备份目录失败";
        return L"";
    }
    std::wstring filesDir = dir + L"\\files";
    if (!ensureDirRecursive(filesDir)) {
        if (err) *err = "创建备份子目录失败";
        return L"";
    }
    const std::vector<std::wstring> sourceFiles = listFiles(slot.slotDir);
    if (sourceFiles.empty()) {
        if (err) *err = "备份失败：存档目录里没有文件";
        return L"";
    }
    std::vector<std::wstring> copied;
    for (const auto& f : sourceFiles) {
        std::wstring src = slot.slotDir + L"\\" + f;
        std::wstring dst = filesDir + L"\\" + f;
        if (!::CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
            if (err) {
                *err = fmt("备份失败：无法复制 %s（Win32 错误码 %lu）", W2U(f).c_str(),
                           (unsigned long)::GetLastError());
            }
            return L"";
        }
        copied.push_back(f);
    }
    std::string ini;
    ini += "time=" + localTimeString((uint64_t)::time(nullptr)) + "\n";
    ini += "profile=" + W2U(slot.profileName) + "\n";
    ini += "slot=" + W2U(slot.slotName) + "\n";
    ini += fmt("cloud=%d\n", slot.cloud ? 1 : 0);
    ini += std::string("label=") + (label ? label : "manual") + "\n";
    ini += "original=" + W2U(slot.slotDir) + "\n";
    ini += "files=";
    for (size_t i = 0; i < copied.size(); ++i) {
        if (i) ini += ";";
        ini += W2U(copied[i]);
    }
    ini += "\n";
    if (!writeFileBytes(dir + L"\\backup.ini", std::vector<uint8_t>(ini.begin(), ini.end()))) {
        if (err) *err = "备份文件已复制，但写入 backup.ini 失败";
        return L"";
    }
    return dir;
}

std::vector<BackupInfo> listBackups() {
    std::vector<BackupInfo> out;
    std::wstring root = backupRoot();
    if (!isDir(root)) return out;
    for (const auto& name : listDirs(root)) {
        std::wstring dir = root + L"\\" + name;
        std::vector<uint8_t> bytes;
        if (!readFileBytes(dir + L"\\backup.ini", bytes)) continue;
        std::string ini(bytes.begin(), bytes.end());
        BackupInfo info;
        info.path = dir;
        info.filesDir = dir + L"\\files";
        info.time = U2W(iniValue(ini, "time"));
        info.profile = U2W(iniValue(ini, "profile"));
        info.slot = U2W(iniValue(ini, "slot"));
        info.label = U2W(iniValue(ini, "label"));
        info.originalDir = U2W(iniValue(ini, "original"));
        info.files = splitSemi(iniValue(ini, "files"));
        out.push_back(std::move(info));
    }
    // 目录名以时间戳开头，倒序即最新在前
    std::sort(out.begin(), out.end(),
              [](const BackupInfo& a, const BackupInfo& b) { return a.path > b.path; });
    return out;
}

bool restoreBackup(const BackupInfo& info, std::string* err) {
    if (!isDir(info.filesDir)) {
        if (err) *err = "备份文件目录不存在";
        return false;
    }
    if (info.originalDir.empty()) {
        if (err) *err = "该备份里没有记录原始存档位置";
        return false;
    }
    if (!ensureDirRecursive(info.originalDir)) {
        if (err) *err = "创建目标存档目录失败";
        return false;
    }
    const std::vector<std::wstring> files = listFiles(info.filesDir);
    if (files.empty()) {
        if (err) *err = "还原失败：备份中没有文件";
        return false;
    }
    int copied = 0;
    for (const auto& f : files) {
        std::wstring src = info.filesDir + L"\\" + f;
        std::wstring dst = info.originalDir + L"\\" + f;
        if (!::CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
            if (err) {
                *err = fmt("还原失败：无法复制 %s（Win32 错误码 %lu）", W2U(f).c_str(),
                           (unsigned long)::GetLastError());
            }
            return false;
        }
        ++copied;
    }
    logLine(fmt("已还原 %d 个文件到 %s", copied, W2U(info.originalDir).c_str()));
    return true;
}

// ---------- 地图传送用的城市列表 ----------
namespace {

bool isLowerWord(char c) {
    return (c >= 'a' && c <= 'z') || c == '_';
}

// 只做有界扫描：遍历出现过的 `<前缀>.<后缀>` 字符串对。
void collectNamePairs(const std::string& text, std::map<std::string, std::set<std::string>>* pairs) {
    const size_t size = text.size();
    size_t index = 0;
    while (index < size) {
        if (!(text[index] >= 'a' && text[index] <= 'z')) {
            ++index;
            continue;
        }
        // 前缀边界：前一个字符不能是字母/数字/下划线/点
        if (index > 0) {
            const char previous = text[index - 1];
            if ((previous >= 'a' && previous <= 'z') || (previous >= '0' && previous <= '9') ||
                previous == '_' || previous == '.') {
                ++index;
                continue;
            }
        }
        size_t prefixEnd = index;
        while (prefixEnd < size &&
               ((text[prefixEnd] >= 'a' && text[prefixEnd] <= 'z') ||
                (text[prefixEnd] >= '0' && text[prefixEnd] <= '9') || text[prefixEnd] == '_')) {
            ++prefixEnd;
        }
        if (prefixEnd >= size || text[prefixEnd] != '.' || prefixEnd == index ||
            prefixEnd - index > 16) {
            index = prefixEnd + 1;
            continue;
        }
        const size_t suffixBegin = prefixEnd + 1;
        size_t suffixEnd = suffixBegin;
        while (suffixEnd < size && isLowerWord(text[suffixEnd])) ++suffixEnd;
        const size_t suffixLength = suffixEnd - suffixBegin;
        const bool boundaryOk =
            suffixEnd >= size ||
            !(((text[suffixEnd] >= 'a' && text[suffixEnd] <= 'z') ||
               (text[suffixEnd] >= '0' && text[suffixEnd] <= '9')) || text[suffixEnd] == '_' ||
              text[suffixEnd] == '.');
        if (suffixLength >= 3 && suffixLength <= 24 && boundaryOk) {
            (*pairs)[text.substr(suffixBegin, suffixLength)].insert(text.substr(index, prefixEnd - index));
        }
        index = suffixEnd;
    }
}

}  // namespace

std::vector<std::string> extractMapCities(const std::vector<uint8_t>& bytes) {
    std::map<std::string, std::set<std::string>> pairs;
    const std::string text(bytes.begin(), bytes.end());
    collectNamePairs(text, &pairs);

    std::set<std::string> candidates;
    for (const auto& item : pairs) {
        if (item.second.size() < 2) continue;  // 只和一家公司配对的多半是车型/配件名
        candidates.insert(item.first);
    }
    // 去掉 `<城市>x` 这类仓库后缀（其短形式本身也是候选城市）与已知的非地名
    static const char* kJunk[] = {"sii", "addon_hookup", "tobj", "pmd", "pmg", "dds", "mat",
                                  "sfx"};
    std::vector<std::string> cities;
    for (const auto& name : candidates) {
        bool junk = false;
        for (const char* item : kJunk) {
            if (name == item) {
                junk = true;
                break;
            }
        }
        if (junk) continue;
        if (name.size() > 4 && candidates.count(name.substr(0, name.size() - 1))) continue;
        cities.push_back(name);
    }
    std::sort(cities.begin(), cities.end());
    return cities;
}

bool listMapCities(const std::wstring& savePath, std::vector<std::string>* cities,
                   std::string* error) {
    std::vector<uint8_t> inner;
    DecryptInfo info;
    if (!decryptSave(savePath, inner, &info, error)) return false;
    if (inner.empty()) {
        if (error) *error = "存档内容为空";
        return false;
    }
    std::vector<std::string> found = extractMapCities(inner);
    if (found.empty()) {
        if (error) *error = "存档里没有找到城市名（可能不是欧卡2 的地图存档）";
        return false;
    }
    if (cities) *cities = std::move(found);
    return true;
}

}  // namespace ets2

