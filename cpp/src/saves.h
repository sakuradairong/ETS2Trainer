// saves.h —— 欧卡2 存档：ScsC(AES-256-CBC) 解密、DEFLATE 解压、定位、备份、明文改数值
#pragma once
#include "common.h"

#include <string>
#include <vector>

namespace ets2 {

struct SaveSlot {
    std::wstring profileDir;
    std::wstring slotDir;
    std::wstring gameSii;
    std::wstring profileName;
    std::wstring slotName;
    std::string  displayName;
    bool         cloud = false;
    uint64_t     mtime = 0;      // Unix 秒
    uint64_t     size = 0;
    std::string  format;         // scs / text / bsii / unknown
    std::string  formatText;     // 中文描述
    std::string  label;          // 显示标签（UTF-8）
};

struct DecryptInfo {
    bool        encrypted = false;
    bool        compressed = false;
    std::string innerFormat;     // text / bsii / unknown
    uint64_t    innerSize = 0;
    uint64_t    declaredSize = 0;
    uint64_t    payloadSize = 0;
};

struct TextValues {
    bool    hasMoney = false;
    bool    hasExperience = false;
    int64_t money = 0;
    int64_t experience = 0;
};

struct BackupInfo {
    std::wstring path;          // 备份目录
    std::wstring filesDir;      // 备份目录\files
    std::wstring time;
    std::wstring profile;
    std::wstring slot;
    std::wstring label;
    std::wstring game;          // 备份所属游戏（ets2/ats）；空 = 历史欧卡2 备份
    std::wstring originalDir;
    std::vector<std::wstring> files;
};

// ---------- 路径 ----------
std::wstring documentsDir();                              // 文档\（当前选择游戏的目录）
std::wstring steamInstallDir();                           // Steam 安装目录
std::vector<std::wstring> cloudProfileDirs();             // 云存档 profiles 目录
std::vector<SaveSlot>     listSlots();
std::wstring              applicationDir();                // 当前可执行文件所在目录
std::wstring              backupRoot();                   // 修改器目录\backups（美卡在 \ats 子目录）

// ---------- 文件 ----------
bool readFileBytes(const std::wstring& path, std::vector<uint8_t>& out);
bool writeFileBytes(const std::wstring& path, const std::vector<uint8_t>& data);

// ---------- 解密 ----------
std::string detectFormat(const std::vector<uint8_t>& data);
bool        decryptSave(const std::wstring& path, std::vector<uint8_t>& inner,
                        DecryptInfo* info, std::string* err, size_t maxInnerBytes = 0);
bool        inflateRaw(const uint8_t* in, size_t inSize, std::vector<uint8_t>& out,
                       std::string* err, size_t maxOutputBytes = 0);
bool        exportDecrypted(const SaveSlot& slot, const std::wstring& outPath,
                            DecryptInfo* info, std::string* err);

// ---------- 明文存档数值 ----------
bool readTextValues(const std::wstring& path, TextValues* out, std::string* err);
bool patchTextSave(const std::wstring& path, bool setMoney, int64_t money, bool setExp,
                   int64_t experience, std::string* err);

// ---------- 备份 / 还原 ----------
std::wstring              backupSlot(const SaveSlot& slot, const char* label, std::string* err);
std::vector<BackupInfo>   listBackups();
bool                      restoreBackup(const BackupInfo& info, std::string* err);

// ---------- 地图传送用的城市列表 ----------
// 从存档字节里提取城市内部名（欧卡2 的仓库单位名是 `<公司>.<城市>`，例如 tradeaux.berlin）。
// 只保留与至少两家公司配对、且看起来像地名的候选；纯函数，便于离线自检。
std::vector<std::string> extractMapCities(const std::vector<uint8_t>& bytes);

// 读取存档（ScsC 会自动解密）并提取城市内部名，用于控制台 goto 传送。
bool listMapCities(const std::wstring& savePath, std::vector<std::string>* cities,
                   std::string* error);

}  // namespace ets2
