// common.cpp —— 公共实现
#include "common.h"

#include <ctime>
#include <cstdarg>
#include <mutex>

namespace ets2 {

std::string W2U(const std::wstring& w) {
    if (w.empty()) return {};
    int need = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)need, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), need, nullptr, nullptr);
    return out;
}

std::wstring U2W(const std::string& s) {
    if (s.empty()) return {};
    int need = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)need, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), need);
    return out;
}

std::string fmt(const char* f, ...) {
    char stackBuf[1024];
    va_list ap;
    va_start(ap, f);
    int n = ::vsnprintf(stackBuf, sizeof(stackBuf), f, ap);
    va_end(ap);
    if (n < 0) return {};
    if ((size_t)n < sizeof(stackBuf)) return std::string(stackBuf, (size_t)n);
    std::string big((size_t)n + 1, '\0');
    va_start(ap, f);
    ::vsnprintf(big.data(), big.size(), f, ap);
    va_end(ap);
    big.resize((size_t)n);
    return big;
}

std::wstring fmtW(const wchar_t* f, ...) {
    // _vsnwprintf_s 在"截断"和"出错"两种情况下都返回 -1，直接用它判断会把
    // 超过缓冲区的消息整条丢掉；先问长度再一次性格式化。
    va_list probe;
    va_start(probe, f);
    const int needed = _vscwprintf(f, probe);
    va_end(probe);
    if (needed < 0) return {};
    std::wstring out((size_t)needed + 1, L'\0');
    va_list ap;
    va_start(ap, f);
    const int written = _vsnwprintf_s(out.data(), out.size(), _TRUNCATE, f, ap);
    va_end(ap);
    if (written < 0) return {};
    out.resize((size_t)written);
    return out;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool startsWith(const std::string& s, const char* prefix) {
    size_t n = ::strlen(prefix);
    return s.size() >= n && ::memcmp(s.data(), prefix, n) == 0;
}

std::string hexAddr(uint64_t v) {
    return fmt("0x%016llX", (unsigned long long)v);
}

std::string formatInt(int64_t v) { return fmt("%lld", (long long)v); }

std::string formatSize(uint64_t bytes) {
    if (bytes >= 1024ull * 1024 * 1024) return fmt("%.2f GB", bytes / 1073741824.0);
    if (bytes >= 1024ull * 1024) return fmt("%.1f MB", bytes / 1048576.0);
    return fmt("%.1f KB", bytes / 1024.0);
}

uint64_t nowMs() {
    return (uint64_t)::GetTickCount64();
}

std::string localTimeString(uint64_t unixSeconds) {
    time_t t = (time_t)unixSeconds;
    struct tm tmv {};
    ::localtime_s(&tmv, &t);
    char buf[64];
    ::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

namespace {
std::mutex g_logMutex;
std::vector<std::string> g_log;
HANDLE g_logFile = INVALID_HANDLE_VALUE;
std::wstring g_logPath;
std::string g_logError;

void appendLogLocked(const std::string& line) {
    if (g_logFile == INVALID_HANDLE_VALUE) return;
    const std::string bytes = line + "\r\n";
    DWORD written = 0;
    if (!::WriteFile(g_logFile, bytes.data(), (DWORD)bytes.size(), &written, nullptr) ||
        written != bytes.size() || !::FlushFileBuffers(g_logFile)) {
        g_logError = fmt("实时日志写入失败（错误 %lu）：%s", (unsigned long)::GetLastError(),
                         W2U(g_logPath).c_str());
        ::CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;
    }
}
}  // namespace

void logLine(const std::string& text) {
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    std::string line = fmt("[%04d-%02d-%02d %02d:%02d:%02d] %s", st.wYear, st.wMonth,
                           st.wDay, st.wHour, st.wMinute, st.wSecond, text.c_str());
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_log.push_back(line);
    appendLogLocked(line);
    if (g_log.size() > 2000) g_log.erase(g_log.begin(), g_log.begin() + 500);
}

std::vector<std::string> logSnapshot() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    return g_log;
}

void logClear() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_log.clear();
    appendLogLocked("[界面日志已清空，磁盘会话日志继续保留]");
}

bool logStartFile(const std::wstring& path, std::string* error) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const std::string reason = fmt("无法创建实时日志（错误 %lu）：%s",
            (unsigned long)::GetLastError(), W2U(path).c_str());
        if (error) *error = reason;
        g_logError = reason;
        return false;
    }
    if (g_logFile != INVALID_HANDLE_VALUE) ::CloseHandle(g_logFile);
    g_logFile = file;
    g_logPath = path;
    g_logError.clear();
    return true;
}

void logStopFile() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile != INVALID_HANDLE_VALUE) ::CloseHandle(g_logFile);
    g_logFile = INVALID_HANDLE_VALUE;
}

std::wstring logFilePath() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    return g_logPath;
}

std::string logFileError() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    return g_logError;
}

void logSaveToFile(const std::wstring& path) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"wb") != 0 || !fp) return;
    std::lock_guard<std::mutex> lock(g_logMutex);
    for (const auto& line : g_log) {
        fwrite(line.data(), 1, line.size(), fp);
        fwrite("\r\n", 1, 2, fp);
    }
    fclose(fp);
}

}  // namespace ets2
