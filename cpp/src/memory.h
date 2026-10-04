// memory.h —— Win32 进程内存引擎（对应 Python 版 core/winmem.py）
#pragma once
#include "common.h"

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>

namespace ets2 {

enum class VType { Int32, UInt32, Int64, UInt64, Float, Double };

struct VTypeInfo {
    const char* name;
    int size;
    bool isFloat;
};
const VTypeInfo& vtypeInfo(VType t);
const char*       vtypeName(VType t);
bool              vtypeFromName(const char* name, VType* out);

//: 数值（整数用 int64 保存，避免 double 精度损失）
struct Value {
    bool    isFloat = false;
    bool    valid = true;      // 读取失败时为 false
    int64_t i = 0;
    double  d = 0.0;
    double  asDouble() const { return isFloat ? d : (double)i; }
    int64_t asInt64() const { return isFloat ? (int64_t)d : i; }
};

//: 把数值转成显示用字符串（浮点去掉多余的 0）
std::string valueToString(const Value& v);

struct Region {
    uint64_t base;
    uint64_t size;
    DWORD    protect;
    DWORD    type;
};

class ProcessMemory {
public:
    static const size_t kDefaultMaxRegion = (size_t)1024 * 1024 * 1024;  // 跳过 >1GB 的内存池

    ProcessMemory() = default;
    ~ProcessMemory();
    ProcessMemory(const ProcessMemory&) = delete;
    ProcessMemory& operator=(const ProcessMemory&) = delete;

    bool open(DWORD pid, const std::string& name, std::string* error);
    bool openReadOnly(DWORD pid, const std::string& name, std::string* error);
    bool canWrite() const { return h_ && writable_; }
    bool mainImage(uint64_t* base, std::wstring* path = nullptr) const;
    void close();
    bool alive() const;
    bool isOpen() const { return h_ != nullptr; }
    DWORD pid() const { return pid_; }
    const std::string& name() const { return name_; }

    // 基础读写（线程安全：不共享缓冲，由调用方提供）
    bool read(uint64_t address, void* dst, size_t size, size_t* got = nullptr) const;
    bool write(uint64_t address, const void* src, size_t size) const;

    Value readValue(uint64_t address, VType t) const;
    bool  writeValue(uint64_t address, VType t, const Value& v) const;
    bool  writeDouble(uint64_t address, VType t, double v) const;
    bool  writeInt(uint64_t address, VType t, int64_t v) const;

    std::vector<Region> regions(size_t maxRegionSize = kDefaultMaxRegion) const;

private:
    bool openWithAccess(DWORD, const std::string&, std::string*, bool writable);
    HANDLE      h_ = nullptr;
    DWORD       pid_ = 0;
    std::string name_;
    bool writable_ = false;
};

// ---------------- 扫描 ----------------
enum class ScanMode { Exact, Changed, Unchanged, Increased, Decreased };

struct ScanOptions {
    size_t maxRegionSize = ProcessMemory::kDefaultMaxRegion;
    int    workers = 4;
    bool   includeImage = false;
    bool   includeMapped = false;
};

using ProgressFn = std::function<void(uint64_t done, uint64_t total)>;
using CancelFn   = std::function<bool()>;

class ScanSession {
public:
    ScanSession(ProcessMemory& mem, VType type, int align = 0, double tolerance = 0.0);

    // value 为 NaN 表示"未知初始值"（记录所有对齐位置）
    uint64_t firstScan(double value, const ScanOptions& opt, ProgressFn progress, CancelFn cancel);
    uint64_t nextScan(ScanMode mode, double value, ProgressFn progress, CancelFn cancel);

    size_t count() const { return addrs_.size(); }
    bool   empty() const { return addrs_.empty(); }
    bool   limitReached() const { return limitReached_.load(); }
    const std::vector<uint64_t>& addresses() const { return addrs_; }
    VType  type() const { return type_; }
    int    alignment() const { return align_; }
    double tolerance() const { return tol_; }
    bool   contains(uint64_t addr) const;

    std::vector<std::tuple<uint64_t, Value, Value>> snapshot(int limit = 500, int offset = 0) const;
    size_t writeAllNumber(double value);
    size_t writeAllInt(int64_t value);
    size_t keepIndices(const std::vector<size_t>& indices);
    void   clear();

private:
    std::vector<uint8_t> packValue(double value) const;
    std::vector<Value>   readMany() const;    // 按 addrs_ 顺序批量读取
    Value                decode(const uint8_t* p) const;

    ProcessMemory& mem_;
    VType   type_;
    int     align_;
    double  tol_;
    std::vector<uint64_t> addrs_;
    std::vector<int64_t>  prevI_;
    std::vector<double>   prevD_;
    std::atomic<bool> limitReached_{false};
};

// ---------------- 数值锁定 ----------------
class Freezer {
public:
    // 写入闸门：返回空串=允许写入；否则是拒绝原因（联机保护）。
    // 每次实际写入前调用；被拦下时不写入（锁定条目保留，状态可查）。
    using WriteGuard = std::function<std::string()>;

    ~Freezer() { stop(); }
    void start(ProcessMemory* mem, int intervalMs = 250);
    void stop();
    void add(uint64_t address, VType t, double value);
    void addInt(uint64_t address, VType t, int64_t value);
    void remove(uint64_t address);
    void clear();
    size_t size() const;
    std::vector<std::tuple<uint64_t, VType, double>> entries() const;
    uint64_t writes() const { return writes_.load(); }
    uint64_t failures() const { return fails_.load(); }
    // 被闸门拦下的累计次数 + 最近一次拒绝原因（供界面如实提示）
    uint64_t blocked() const { return blocked_.load(); }
    std::string blockedReason() const;
    void setWriteGuard(WriteGuard guard);

private:
    void run();
    std::thread th_;
    std::atomic<bool> stop_{true};
    mutable std::mutex m_;
    std::map<uint64_t, std::pair<VType, double>> items_;
    ProcessMemory* mem_ = nullptr;
    int interval_ = 250;
    std::atomic<uint64_t> writes_{0};
    std::atomic<uint64_t> fails_{0};
    std::atomic<uint64_t> blocked_{0};
    mutable std::mutex guardMutex_;
    WriteGuard guard_;
    std::string blockedReason_ = "未启用";
};

}  // namespace ets2
