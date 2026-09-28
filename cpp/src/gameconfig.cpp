// gameconfig.cpp —— config.cfg 读取与行保留改写
#include "gameconfig.h"

#include "saves.h"

#include <vector>

namespace ets2 {

namespace {

// 判断一行是否是 `uset <key> ...`，是则返回值的起止位置。
bool parseUsetLine(const std::string& line, const std::string& key, size_t* valueBegin,
                   size_t* valueEnd) {
    const std::string prefix = "uset " + key;
    if (line.compare(0, prefix.size(), prefix) != 0) return false;
    if (line.size() > prefix.size()) {
        const char next = line[prefix.size()];
        if (next != ' ' && next != '\t' && next != '"') return false;
    }
    const size_t firstQuote = line.find('"', prefix.size());
    if (firstQuote == std::string::npos) return false;
    const size_t secondQuote = line.find('"', firstQuote + 1);
    if (secondQuote == std::string::npos) return false;
    *valueBegin = firstQuote + 1;
    *valueEnd = secondQuote;
    return true;
}

}  // namespace

std::wstring gameConfigPath() { return documentsDir() + L"\\config.cfg"; }

bool readConfigKey(const std::string& text, const std::string& key, std::string* value) {
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t newline = text.find('\n', pos);
        const size_t lineEnd = (newline == std::string::npos) ? text.size() : newline;
        const std::string line = text.substr(pos, lineEnd - pos);
        size_t begin = 0, end = 0;
        if (parseUsetLine(line, key, &begin, &end)) {
            if (value) *value = line.substr(begin, end - begin);
            return true;
        }
        if (newline == std::string::npos) break;
        pos = newline + 1;
    }
    return false;
}

bool rewriteConfigKey(std::string* text, const std::string& key, const std::string& value,
                      bool* changed) {
    if (!text) return false;
    if (changed) *changed = false;
    const std::string replacement = "uset " + key + " \"" + value + "\"";

    std::string out;
    out.reserve(text->size() + replacement.size() + 2);
    size_t pos = 0;
    bool wrote = false;
    while (pos <= text->size()) {
        const size_t newline = text->find('\n', pos);
        const size_t lineEnd = (newline == std::string::npos) ? text->size() : newline;
        const std::string line = text->substr(pos, lineEnd - pos);
        size_t begin = 0, end = 0;
        if (parseUsetLine(line, key, &begin, &end)) {
            const bool same = line.substr(begin, end - begin) == value;
            out += replacement;
            if (!same && changed) *changed = true;
            wrote = true;
        } else {
            out += line;
        }
        if (newline == std::string::npos) break;
        // 保留原始换行字节（可能同时有 \r\n）
        const size_t nextPos = newline + 1;
        out.append(text->data() + lineEnd, nextPos - lineEnd);
        pos = nextPos;
    }
    if (!wrote) {
        if (!out.empty() && out.back() != '\n') out += "\n";
        out += replacement + "\n";
        if (changed) *changed = true;
    }
    *text = std::move(out);
    return true;
}

bool readGameConfigText(std::string* text, std::string* error) {
    std::vector<uint8_t> bytes;
    const std::wstring path = gameConfigPath();
    if (!readFileBytes(path, bytes) || bytes.empty()) {
        if (error) *error = "读不到 config.cfg（" + W2U(path) + "）";
        return false;
    }
    text->assign(bytes.begin(), bytes.end());
    return true;
}

bool writeGameConfigText(const std::string& text, std::string* error,
                         std::wstring* backupPath) {
    const std::wstring path = gameConfigPath();
    std::vector<uint8_t> original;
    if (readFileBytes(path, original) && !original.empty()) {
        const std::wstring backup = path + L".trainer.bak";
        if (::GetFileAttributesW(backup.c_str()) == INVALID_FILE_ATTRIBUTES) {
            if (!writeFileBytes(backup, original)) {
                if (error) *error = "无法写入 config.cfg.trainer.bak，为避免丢失原配置已取消修改";
                return false;
            }
        }
        if (backupPath) *backupPath = backup;
    }
    std::vector<uint8_t> bytes(text.begin(), text.end());
    if (!writeFileBytes(path, bytes)) {
        if (error) *error = "写入 config.cfg 失败（文件可能只读或被占用）";
        return false;
    }
    return true;
}

}  // namespace ets2
