// memory.cpp —— Win32 进程内存引擎实现
#include "memory.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_set>

namespace ets2 {

bool ProcessMemory::mainImage(uint64_t* base, std::wstring* path) const {
    if (!h_ || !base) return false;
    // PEB image base remains readable when Toolhelp module snapshots are denied.
    // This does not establish multiplayer status; that gate continues to fail closed.
    struct BasicInfo { void* reserved; void* peb; void* reserved2[2]; ULONG_PTR pid; void* reserved3; };
    using Query = LONG (NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
    auto query = reinterpret_cast<Query>(::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    BasicInfo info{};
    if (!query || query(h_,0,&info,sizeof(info),nullptr)<0 || !info.peb ||
        !read(reinterpret_cast<uint64_t>(info.peb)+0x10,base,sizeof(*base)) || !*base) return false;
    if (path) {
        wchar_t text[32768]; DWORD length=32768;
        if (!::QueryFullProcessImageNameW(h_,0,text,&length)) return false;
        path->assign(text,length);
    }
    return true;
}

namespace {
const VTypeInfo kTypes[] = {
    {"int32", 4, false}, {"uint32", 4, false}, {"int64", 8, false},
    {"uint64", 8, false}, {"float", 4, true},  {"double", 8, true},
};

const DWORD kMemCommit = 0x1000;
const DWORD kMemImage = 0x1000000;
const DWORD kMemMapped = 0x40000;
const DWORD kPageGuard = 0x100;
const DWORD kPageNoCache = 0x400;
const DWORD kPageReadWrite = 0x04;
const DWORD kPageWriteCopy = 0x08;
const DWORD kPageExecuteReadWrite = 0x40;
const DWORD kPageExecuteWriteCopy = 0x80;
const DWORD kWritable = kPageReadWrite | kPageWriteCopy | kPageExecuteReadWrite |
                        kPageExecuteWriteCopy;

const size_t kChunk = 4 * 1024 * 1024;   // 每次读取 4MB
const size_t kOverlap = 32;              // 块间重叠，防止数值跨界被漏掉
const size_t kSpanGap = 48;              // 读候选地址时的合并间隔

int  widthOf(VType t) { return kTypes[(int)t].size; }
bool isFloatType(VType t) { return kTypes[(int)t].isFloat; }

void storeInt(uint8_t* p, int width, int64_t v) {
    for (int i = 0; i < width; ++i) p[i] = (uint8_t)((uint64_t)v >> (8 * i));
}

//: IEEE-754 全序映射：同号浮点在映射后是按数值大小连续排列的 uint32
inline uint32_t floatOrder(uint32_t bits) {
    return (bits & 0x80000000u) ? ~bits : (bits | 0x80000000u);
}
inline uint32_t floatOrderInverse(uint32_t order) {
    return (order & 0x80000000u) ? (order & 0x7FFFFFFFu) : ~order;
}

//: 计算 float 值在容差范围内的"全序"区间（跨零或区间过大时返回 false，退化为逐值比较）
bool floatOrderRange(float value, float tol, uint32_t* lo, uint32_t* hi) {
    double low = (double)value - (double)tol;
    double high = (double)value + (double)tol;
    if (low < 0.0 && high > 0.0) return false;  // 跨越 0：位模式不连续
    if (low > 3.0e38 || high > 3.0e38 || low < -3.0e38) return false;
    float fLow = (float)low;
    float fHigh = (float)high;
    uint32_t bLow = 0, bHigh = 0;
    ::memcpy(&bLow, &fLow, 4);
    ::memcpy(&bHigh, &fHigh, 4);
    uint32_t o1 = floatOrder(bLow), o2 = floatOrder(bHigh);
    if (o1 > o2) std::swap(o1, o2);
    if ((uint64_t)o2 - (uint64_t)o1 > 4000000ull) return false;  // 区间太大，筛不住
    *lo = o1;
    *hi = o2;
    return true;
}
}  // namespace

const VTypeInfo& vtypeInfo(VType t) { return kTypes[(int)t]; }
const char* vtypeName(VType t) { return kTypes[(int)t].name; }

bool vtypeFromName(const char* name, VType* out) {
    for (int i = 0; i < 6; ++i) {
        if (::strcmp(name, kTypes[i].name) == 0) {
            *out = (VType)i;
            return true;
        }
    }
    return false;
}

std::string valueToString(const Value& v) {
    if (v.isFloat) {
        char buf[64];
        ::snprintf(buf, sizeof(buf), "%.6f", v.d);
        std::string s(buf);
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
        return s;
    }
    return formatInt(v.i);
}

// ==========================================================================
// ProcessMemory
// ==========================================================================
ProcessMemory::~ProcessMemory() { close(); }

bool ProcessMemory::open(DWORD pid, const std::string& name, std::string* error) {
    return openWithAccess(pid, name, error, true);
}

bool ProcessMemory::openReadOnly(DWORD pid, const std::string& name, std::string* error) {
    return openWithAccess(pid, name, error, false);
}

bool ProcessMemory::openWithAccess(DWORD pid, const std::string& name, std::string* error,
                                   bool writable) {
    close();
    const DWORD kRights = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE |
        (writable ? PROCESS_VM_OPERATION | PROCESS_VM_WRITE : 0);
    HANDLE h = ::OpenProcess(kRights, FALSE, pid);
    if (!h) {
        DWORD err = ::GetLastError();
        if (error) {
            *error = (err == 5)
                         ? fmt("打开进程 %s (PID %u) 被拒绝(错误 5)。请用管理员权限运行本程序。",
                               name.c_str(), pid)
                         : fmt("打开进程 %s (PID %u) 失败，Win32 错误码 %lu", name.c_str(), pid,
                               (unsigned long)err);
        }
        return false;
    }
    h_ = h;
    pid_ = pid;
    name_ = name;
    writable_ = writable;
    return true;
}

void ProcessMemory::close() {
    if (h_) {
        ::CloseHandle(h_);
        h_ = nullptr;
    }
    pid_ = 0;
    writable_ = false;
}

bool ProcessMemory::alive() const {
    if (!h_) return false;
    // 退出码可合法等于 STILL_ACTIVE (259)；进程句柄是否 signaled 才是可靠判据。
    return ::WaitForSingleObject(h_, 0) == WAIT_TIMEOUT;
}

bool ProcessMemory::read(uint64_t address, void* dst, size_t size, size_t* got) const {
    if (!h_ || !size) return false;
    SIZE_T readBytes = 0;
    BOOL ok = ::ReadProcessMemory(h_, (LPCVOID)(uintptr_t)address, dst, size, &readBytes);
    if (got) *got = (size_t)readBytes;
    return ok && readBytes > 0;
}

bool ProcessMemory::write(uint64_t address, const void* src, size_t size) const {
    if (!h_ || !writable_ || !size) return false;
    SIZE_T written = 0;
    if (::WriteProcessMemory(h_, (LPVOID)(uintptr_t)address, src, size, &written) &&
        written == size) {
        return true;
    }
    DWORD oldProtect = 0;
    if (!::VirtualProtectEx(h_, (LPVOID)(uintptr_t)address, size, PAGE_EXECUTE_READWRITE,
                            &oldProtect)) {
        return false;
    }
    BOOL ok = ::WriteProcessMemory(h_, (LPVOID)(uintptr_t)address, src, size, &written);
    DWORD restored = 0;
    ::VirtualProtectEx(h_, (LPVOID)(uintptr_t)address, size, oldProtect, &restored);
    return ok && written == size;
}

Value ProcessMemory::readValue(uint64_t address, VType t) const {
    uint8_t buf[8] = {0};
    int width = widthOf(t);
    Value v;
    v.isFloat = isFloatType(t);
    v.valid = false;
    size_t received = 0;
    if (!read(address, buf, (size_t)width, &received) || received != (size_t)width) return v;
    v.valid = true;
    if (v.isFloat) {
        if (width == 4) {
            float f = 0;
            ::memcpy(&f, buf, 4);
            v.d = (double)f;
        } else {
            double d = 0;
            ::memcpy(&d, buf, 8);
            v.d = d;
        }
    } else {
        uint64_t raw = 0;
        ::memcpy(&raw, buf, (size_t)width);
        if (t == VType::Int32) v.i = (int32_t)(uint32_t)raw;
        else if (t == VType::UInt32) v.i = (int64_t)(uint32_t)raw;
        else v.i = (int64_t)raw;
    }
    return v;
}

bool ProcessMemory::writeValue(uint64_t address, VType t, const Value& v) const {
    uint8_t buf[8] = {0};
    int width = widthOf(t);
    if (isFloatType(t)) {
        if (width == 4) {
            float f = (float)v.d;
            ::memcpy(buf, &f, 4);
        } else {
            double d = v.d;
            ::memcpy(buf, &d, 8);
        }
    } else {
        storeInt(buf, width, v.i);
    }
    return write(address, buf, (size_t)width);
}

bool ProcessMemory::writeDouble(uint64_t address, VType t, double v) const {
    Value val;
    val.isFloat = isFloatType(t);
    if (val.isFloat) val.d = v;
    else val.i = (int64_t)v;
    return writeValue(address, t, val);
}

bool ProcessMemory::writeInt(uint64_t address, VType t, int64_t v) const {
    Value val;
    val.isFloat = false;
    val.i = v;
    return writeValue(address, t, val);
}

std::vector<Region> ProcessMemory::regions(size_t maxRegionSize) const {
    std::vector<Region> out;
    if (!h_) return out;
    uint64_t address = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    const uint64_t kMaxUser = 0x00007FFFFFFEFFFFull;
    while (address < kMaxUser) {
        if (!::VirtualQueryEx(h_, (LPCVOID)(uintptr_t)address, &mbi, sizeof(mbi))) break;
        uint64_t base = (uint64_t)(uintptr_t)mbi.BaseAddress;
        uint64_t size = (uint64_t)mbi.RegionSize;
        if (!size) break;
        if (mbi.State == kMemCommit && !(mbi.Protect & kPageGuard)) {
            bool ok = (mbi.Protect & kWritable) != 0;
            if (ok && (mbi.Protect & kPageNoCache)) ok = false;  // 设备/DMA 非缓存内存
            if (ok && mbi.Type == kMemImage) ok = false;
            if (ok && mbi.Type == kMemMapped) ok = false;
            if (ok && maxRegionSize && size > maxRegionSize) ok = false;
            if (ok) out.push_back({base, size, mbi.Protect, mbi.Type});
        }
        address = base + size;
    }
    return out;
}

// ==========================================================================
// ScanSession
// ==========================================================================
ScanSession::ScanSession(ProcessMemory& mem, VType type, int align, double tolerance)
    : mem_(mem), type_(type),
      align_(align > 0 ? align : vtypeInfo(type).size),
      tol_(tolerance) {}

void ScanSession::clear() {
    addrs_.clear();
    prevI_.clear();
    prevD_.clear();
    limitReached_.store(false);
}

bool ScanSession::contains(uint64_t addr) const {
    for (auto a : addrs_) {
        if (a == addr) return true;
    }
    return false;
}

Value ScanSession::decode(const uint8_t* p) const {
    Value v;
    v.isFloat = isFloatType(type_);
    int width = widthOf(type_);
    if (v.isFloat) {
        if (width == 4) {
            float f = 0;
            ::memcpy(&f, p, 4);
            v.d = (double)f;
        } else {
            double d = 0;
            ::memcpy(&d, p, 8);
            v.d = d;
        }
    } else {
        uint64_t raw = 0;
        ::memcpy(&raw, p, (size_t)width);
        if (type_ == VType::Int32) v.i = (int32_t)(uint32_t)raw;
        else if (type_ == VType::UInt32) v.i = (int64_t)(uint32_t)raw;
        else v.i = (int64_t)raw;
    }
    return v;
}

std::vector<uint8_t> ScanSession::packValue(double value) const {
    std::vector<uint8_t> pat((size_t)widthOf(type_), 0);
    if (isFloatType(type_)) {
        if (widthOf(type_) == 4) {
            float f = (float)value;
            ::memcpy(pat.data(), &f, 4);
        } else {
            double d = value;
            ::memcpy(pat.data(), &d, 8);
        }
    } else {
        storeInt(pat.data(), widthOf(type_), (int64_t)value);
    }
    return pat;
}

std::vector<Value> ScanSession::readMany() const {
    std::vector<Value> out(addrs_.size());
    for (auto& v : out) v.valid = false;
    if (addrs_.empty()) return out;
    const int width = widthOf(type_);
    std::vector<size_t> order(addrs_.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return addrs_[a] < addrs_[b]; });
    std::vector<uint8_t> buf;
    size_t i = 0;
    while (i < order.size()) {
        size_t j = i;
        while (j + 1 < order.size() && addrs_[order[j + 1]] - addrs_[order[j]] <= kSpanGap) ++j;
        uint64_t first = addrs_[order[i]];
        size_t length = (size_t)(addrs_[order[j]] - first) + (size_t)width;
        if (buf.size() < length) buf.resize(length);
        size_t got = 0;
        if (mem_.read(first, buf.data(), length, &got)) {
            for (size_t k = i; k <= j; ++k) {
                size_t off = (size_t)(addrs_[order[k]] - first);
                if (off + (size_t)width <= got) {
                    Value v = decode(buf.data() + off);
                    v.valid = true;
                    out[order[k]] = v;
                }
            }
        }
        i = j + 1;
    }
    return out;
}

uint64_t ScanSession::firstScan(double value, const ScanOptions& opt, ProgressFn progress,
                                CancelFn cancel) {
    clear();
    std::vector<Region> regs = mem_.regions(opt.maxRegionSize);
    struct Task {
        uint64_t addr;
        size_t   size;
    };
    std::vector<Task> tasks;
    uint64_t total = 0;
    for (const auto& r : regs) {
        uint64_t off = 0;
        while (off < r.size) {
            size_t piece =
                (size_t)((r.size - off) < (uint64_t)kChunk ? (r.size - off) : (uint64_t)kChunk);
            tasks.push_back({r.base + off, piece});
            total += piece;
            if (piece < kChunk) break;
            off += (uint64_t)piece - (uint64_t)kOverlap;
        }
    }
    if (total == 0) total = 1;

    const bool unknown = std::isnan(value);
    const std::vector<uint8_t> pattern = unknown ? std::vector<uint8_t>() : packValue(value);
    const int width = widthOf(type_);
    const bool isF = isFloatType(type_);
    const double tol = tol_;
    const double target = (isF && width == 4) ? (double)(float)value : value;
    const int align = align_;

    std::atomic<size_t> nextIdx{0};
    std::atomic<uint64_t> doneBytes{0};
    std::mutex outMutex;
    std::vector<uint64_t> hitsAddr;
    std::vector<int64_t> hitsInt;
    std::vector<double> hitsFloat;
    // 分块扫描故意让相邻块重叠 kOverlap 字节，否则跨块的值会漏掉；
    // 代价是重叠区可能重复命中。这里按地址去重（不能靠"跳过重叠起点"，
    // 因为前一块若读取被截断，其尾部其实没被扫到，跳过会真的漏值）。
    std::unordered_set<uint64_t> seenAddrs;
    constexpr size_t kMaxScanHits = 2000000;  // ~32 MB 结果；匹配过多则整次扫描报错

    auto worker = [&]() {
        std::vector<uint8_t> buf(kChunk + 64);
        for (;;) {
            if (limitReached_.load() || (cancel && cancel())) return;
            size_t idx = nextIdx.fetch_add(1);
            if (idx >= tasks.size()) return;
            const Task t = tasks[idx];
            size_t got = 0;
            if (mem_.read(t.addr, buf.data(), t.size, &got) && got >= (size_t)width) {
                std::vector<uint64_t> la;
                std::vector<int64_t> li;
                std::vector<double> ld;
                if (unknown || isF) {
                    size_t misalign = (size_t)(t.addr % (uint64_t)align);
                    size_t startOff = misalign ? (size_t)align - misalign : 0;
                    // 浮点：先用 IEEE-754 全序区间做精确快速筛选（区间内才解码比较）
                    uint32_t rangeLo = 0, rangeHi = 0;
                    const bool haveRange =
                        (isF && !unknown && width == 4 && tol > 0.0f) &&
                        floatOrderRange((float)value, (float)tol, &rangeLo, &rangeHi);
                    for (size_t off = startOff; off + (size_t)width <= got;
                         off += (size_t)align) {
                        if (haveRange) {
                            uint32_t bits = 0;
                            ::memcpy(&bits, buf.data() + off, 4);
                            uint32_t ord = floatOrder(bits);
                            if (ord < rangeLo || ord > rangeHi) continue;
                        }
                        Value v = decode(buf.data() + off);
                        if (!unknown) {
                            if (std::fabs(v.asDouble() - target) > tol) continue;
                        }
                        la.push_back(t.addr + off);
                        if (isF) ld.push_back(v.asDouble());
                        else li.push_back(v.asInt64());
                    }
                } else {
                    size_t limit = got - (size_t)width;
                    size_t pos = 0;
                    while (pos <= limit) {
                        void* found = ::memchr(buf.data() + pos, pattern[0], limit - pos + 1);
                        if (!found) break;
                        size_t off = (size_t)((const uint8_t*)found - buf.data());
                        if (::memcmp(buf.data() + off, pattern.data(), (size_t)width) == 0 &&
                            ((t.addr + off) % (uint64_t)align) == 0) {
                            la.push_back(t.addr + off);
                            li.push_back((int64_t)value);
                        }
                        pos = off + 1;
                    }
                }
                if (!la.empty()) {
                    std::lock_guard<std::mutex> lock(outMutex);
                    for (size_t k = 0; k < la.size(); ++k) {
                        if (!seenAddrs.insert(la[k]).second) continue;  // 重叠区重复命中
                        if (hitsAddr.size() >= kMaxScanHits) {
                            limitReached_.store(true);
                            return;
                        }
                        hitsAddr.push_back(la[k]);
                        if (isF) hitsFloat.push_back(ld[k]);
                        else hitsInt.push_back(li[k]);
                    }
                }
            }
            uint64_t d = doneBytes.fetch_add(t.size) + t.size;
            if (progress && (idx % 8) == 0) progress(d, total);
        }
    };

    int workers = opt.workers < 1 ? 1 : (opt.workers > 16 ? 16 : opt.workers);
    std::vector<std::thread> pool;
    pool.reserve((size_t)workers);
    for (int i = 0; i < workers; ++i) pool.emplace_back(worker);
    for (auto& th : pool) th.join();

    if (limitReached_.load()) {
        // 绝不把截断的部分候选交给用户写入；由 UI 提示收窄扫描范围或避开常见值。
        if (progress) progress(doneBytes.load(), total);
        return 0;
    }
    if (cancel && cancel()) {
        clear();
        if (progress) progress(doneBytes.load(), total);
        return 0;
    }
    addrs_ = std::move(hitsAddr);
    prevI_ = std::move(hitsInt);
    prevD_ = std::move(hitsFloat);
    if (progress) progress(total, total);
    return (uint64_t)addrs_.size();
}

uint64_t ScanSession::nextScan(ScanMode mode, double value, ProgressFn progress, CancelFn cancel) {
    if (addrs_.empty()) return 0;
    const std::vector<Value> cur = readMany();
    const bool isF = isFloatType(type_);
    const double tol = tol_;
    const double target = (isF && widthOf(type_) == 4) ? (double)(float)value : value;
    const size_t n = addrs_.size();
    std::vector<uint64_t> a2;
    std::vector<int64_t> i2;
    std::vector<double> d2;
    a2.reserve(n / 4 + 8);
    bool cancelled = false;
    for (size_t k = 0; k < n; ++k) {
        if (cancel && (k & 0xFFFF) == 0 && cancel()) {
            cancelled = true;
            break;
        }
        if (!cur[k].valid) continue;
        const Value& v = cur[k];
        Value prev;
        prev.isFloat = isF;
        if (isF) prev.d = prevD_[k];
        else prev.i = prevI_[k];
        bool ok = false;
        switch (mode) {
            case ScanMode::Exact:
                ok = isF ? (std::fabs(v.asDouble() - target) <= tol)
                         : (v.asInt64() == (int64_t)value);
                break;
            case ScanMode::Changed:
                ok = isF ? (std::fabs(v.asDouble() - prev.asDouble()) > tol)
                         : (v.asInt64() != prev.asInt64());
                break;
            case ScanMode::Unchanged:
                ok = isF ? (std::fabs(v.asDouble() - prev.asDouble()) <= tol)
                         : (v.asInt64() == prev.asInt64());
                break;
            case ScanMode::Increased:
                ok = (v.asDouble() - prev.asDouble()) > tol;
                break;
            case ScanMode::Decreased:
                ok = (prev.asDouble() - v.asDouble()) > tol;
                break;
        }
        if (!ok) continue;
        a2.push_back(addrs_[k]);
        if (isF) d2.push_back(v.d);
        else i2.push_back(v.i);
        if (progress && (k & 0x3FFF) == 0) progress(k + 1, n);
    }
    // 取消必须是事务性的：不能用已处理的前半段覆盖原候选集。
    if (cancelled) return (uint64_t)addrs_.size();
    addrs_ = std::move(a2);
    prevI_ = std::move(i2);
    prevD_ = std::move(d2);
    if (progress) progress(n, n);
    return (uint64_t)addrs_.size();
}

std::vector<std::tuple<uint64_t, Value, Value>> ScanSession::snapshot(int limit, int offset) const {
    std::vector<std::tuple<uint64_t, Value, Value>> out;
    if (addrs_.empty()) return out;
    size_t begin = offset < 0 ? 0 : (size_t)offset;
    if (begin >= addrs_.size()) return out;
    size_t end = (limit < 0) ? addrs_.size()
                             : (begin + (size_t)limit > addrs_.size() ? addrs_.size()
                                                                      : begin + (size_t)limit);
    const int width = widthOf(type_);
    // 把要显示的这一段地址合并成若干跨度读取（同 readMany 的思路）
    std::vector<uint64_t> slice(addrs_.begin() + (ptrdiff_t)begin, addrs_.begin() + (ptrdiff_t)end);
    std::vector<uint64_t> sorted = slice;
    std::sort(sorted.begin(), sorted.end());
    std::vector<uint8_t> buf;
    size_t i = 0;
    std::map<uint64_t, Value> values;
    while (i < sorted.size()) {
        size_t j = i;
        while (j + 1 < sorted.size() && sorted[j + 1] - sorted[j] <= kSpanGap) ++j;
        size_t length = (size_t)(sorted[j] - sorted[i]) + (size_t)width;
        if (buf.size() < length) buf.resize(length);
        size_t got = 0;
        if (mem_.read(sorted[i], buf.data(), length, &got)) {
            for (size_t k = i; k <= j; ++k) {
                size_t off = (size_t)(sorted[k] - sorted[i]);
                if (off + (size_t)width <= got) {
                    Value v = decode(buf.data() + off);
                    v.valid = true;
                    values[sorted[k]] = v;
                }
            }
        }
        i = j + 1;
    }
    out.reserve(end - begin);
    for (size_t k = begin; k < end; ++k) {
        auto it = values.find(addrs_[k]);
        if (it == values.end()) continue;
        Value prev;
        prev.isFloat = isFloatType(type_);
        if (prev.isFloat) prev.d = prevD_[k];
        else prev.i = prevI_[k];
        out.emplace_back(addrs_[k], it->second, prev);
    }
    return out;
}

size_t ScanSession::writeAllNumber(double value) {
    size_t ok = 0;
    for (auto addr : addrs_) {
        if (mem_.writeDouble(addr, type_, value)) ++ok;
    }
    return ok;
}

size_t ScanSession::writeAllInt(int64_t value) {
    size_t ok = 0;
    for (auto addr : addrs_) {
        if (mem_.writeInt(addr, type_, value)) ++ok;
    }
    return ok;
}

size_t ScanSession::keepIndices(const std::vector<size_t>& indices) {
    std::vector<size_t> sorted = indices;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    std::vector<uint64_t> a2;
    std::vector<int64_t> i2;
    std::vector<double> d2;
    for (size_t idx : sorted) {
        if (idx >= addrs_.size()) continue;
        a2.push_back(addrs_[idx]);
        if (isFloatType(type_)) d2.push_back(prevD_[idx]);
        else i2.push_back(prevI_[idx]);
    }
    addrs_ = std::move(a2);
    prevI_ = std::move(i2);
    prevD_ = std::move(d2);
    return addrs_.size();
}

// ==========================================================================
// Freezer —— 数值锁定
// ==========================================================================
void Freezer::start(ProcessMemory* mem, int intervalMs) {
    stop();
    clear();
    mem_ = mem;
    interval_ = intervalMs < 20 ? 20 : intervalMs;
    stop_.store(false);
    th_ = std::thread([this] { run(); });
}

void Freezer::stop() {
    stop_.store(true);
    if (th_.joinable()) th_.join();
}

void Freezer::add(uint64_t address, VType t, double value) {
    std::lock_guard<std::mutex> lock(m_);
    items_[address] = {t, value};
}

void Freezer::addInt(uint64_t address, VType t, int64_t value) {
    add(address, t, (double)value);
}

void Freezer::remove(uint64_t address) {
    std::lock_guard<std::mutex> lock(m_);
    items_.erase(address);
}

void Freezer::clear() {
    std::lock_guard<std::mutex> lock(m_);
    items_.clear();
}

size_t Freezer::size() const {
    std::lock_guard<std::mutex> lock(m_);
    return items_.size();
}

std::vector<std::tuple<uint64_t, VType, double>> Freezer::entries() const {
    std::lock_guard<std::mutex> lock(m_);
    std::vector<std::tuple<uint64_t, VType, double>> out;
    out.reserve(items_.size());
    for (const auto& kv : items_) out.emplace_back(kv.first, kv.second.first, kv.second.second);
    return out;
}

void Freezer::setWriteGuard(WriteGuard guard) {
    std::lock_guard<std::mutex> lock(guardMutex_);
    guard_ = std::move(guard);
}

std::string Freezer::blockedReason() const {
    std::lock_guard<std::mutex> lock(guardMutex_);
    return blockedReason_;
}

void Freezer::run() {
    while (!stop_.load()) {
        // 每次写入前复查写入闸门（进入联机后必须停止持续写入）
        std::string reason;
        {
            std::lock_guard<std::mutex> lock(guardMutex_);
            if (guard_) reason = guard_();
            blockedReason_ = reason.empty() ? std::string("未启用") : reason;
        }
        std::vector<std::pair<uint64_t, std::pair<VType, double>>> items;
        {
            std::lock_guard<std::mutex> lock(m_);
            items.reserve(items_.size());
            for (const auto& kv : items_) items.push_back(kv);
        }
        if (!reason.empty()) {
            // 被闸门拦下：一次都不写，只累计拦截次数（条目保留，界面可继续提示）
            if (!items.empty()) blocked_.fetch_add(1);
        } else {
            for (const auto& it : items) {
                if (mem_ && mem_->writeDouble(it.first, it.second.first, it.second.second)) {
                    writes_.fetch_add(1);
                } else {
                    fails_.fetch_add(1);
                }
            }
        }
        for (int i = 0; i < interval_ / 20 && !stop_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
}

}  // namespace ets2

