// bsii.cpp -- bounded, pure in-memory BSII v3 -> SiiNunit text converter.
//
// Wire format reference (docs only, MPL-2.0):
//   https://github.com/TheLazyTomcat/SII_Decrypt/tree/master/Documents
// The code below is original C++ written from the documented protocol facts.
//
// No filesystem access, no partial output: every failure clears *out.
#include "bsii.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ets2 {
namespace {

constexpr size_t kMaxUnits           = 500000;
constexpr size_t kMaxSchemas         = 65536;
constexpr size_t kMaxFieldsPerSchema = 65536;
constexpr size_t kMaxEnumEntries     = 1u << 20;
constexpr size_t kMaxArrayCount      = 16u * 1024 * 1024;
constexpr size_t kMaxStringBytes     = 64u * 1024 * 1024;
constexpr size_t kMaxTokenChars      = 12;

// digits[1..37] -> "0123456789abcdefghijklmnopqrstuvwxyz_"
const char kTokenDigits[] = "0123456789abcdefghijklmnopqrstuvwxyz_";

// ---------------------------------------------------------------- reader --

struct Reader {
    const uint8_t* d = nullptr;
    size_t n = 0;
    size_t p = 0;
    bool ok = true;
    std::string err;

    explicit Reader(const std::vector<uint8_t>& b) : d(b.data()), n(b.size()) {}

    size_t remaining() const { return ok ? (n - p) : 0; }

    bool fail(const std::string& m) {
        if (ok) { ok = false; err = m; }
        return false;
    }
    bool u8(uint8_t& v) {
        if (n - p < 1) return fail("premature end of BSII stream");
        v = d[p++];
        return true;
    }
    bool u16(uint16_t& v) {
        if (n - p < 2) return fail("premature end of BSII stream");
        v = (uint16_t)((uint32_t)d[p] | ((uint32_t)d[p + 1] << 8));
        p += 2;
        return true;
    }
    bool u32(uint32_t& v) {
        if (n - p < 4) return fail("premature end of BSII stream");
        v = (uint32_t)d[p] | ((uint32_t)d[p + 1] << 8) |
            ((uint32_t)d[p + 2] << 16) | ((uint32_t)d[p + 3] << 24);
        p += 4;
        return true;
    }
    bool u64(uint64_t& v) {
        if (n - p < 8) return fail("premature end of BSII stream");
        v = 0;
        for (int i = 0; i < 8; ++i) v |= (uint64_t)d[p + i] << (8 * i);
        p += 8;
        return true;
    }
    bool f32(float& v) {
        uint32_t bits = 0;
        if (!u32(bits)) return false;
        std::memcpy(&v, &bits, sizeof(v));
        return true;
    }
};

bool readI32(Reader& r, int32_t& v) {
    uint32_t bits = 0;
    if (!r.u32(bits)) return false;
    std::memcpy(&v, &bits, sizeof(v));
    return true;
}

bool readI64(Reader& r, int64_t& v) {
    uint64_t bits = 0;
    if (!r.u64(bits)) return false;
    std::memcpy(&v, &bits, sizeof(v));
    return true;
}

bool readStr(Reader& r, std::string& s) {
    uint32_t len = 0;
    if (!r.u32(len)) return false;
    if ((size_t)len > kMaxStringBytes) return r.fail("string length exceeds limit");
    if ((size_t)len > r.remaining()) return r.fail("string extends past end of BSII stream");
    s.assign((const char*)r.d + r.p, (size_t)len);
    r.p += (size_t)len;
    return true;
}

// --------------------------------------------------------------- helpers --

bool safeMetadata(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s) {
        if (c < 0x21 || c == 0x7f) return false;                 // control / space
        if (c == '{' || c == '}' || c == '"' || c == '\\' || c == ':') return false;
    }
    return true;
}

std::string hexFloat(float v) {
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    char buf[16];
    std::snprintf(buf, sizeof(buf), "&%08x", (unsigned)bits);
    return buf;
}

std::string decFloat(float v) {
    if (!std::isfinite(v)) return hexFloat(v);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.9g", (double)v);
    return buf;
}

std::string quoteString(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 2);
    o.push_back('"');
    for (unsigned char c : s) {
        if (c == '"') {
            o += "\\\"";
        } else if (c == '\\') {
            o += "\\\\";
        } else if (c < 0x20 || c == 0x7f) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02x", (unsigned)c);
            o += buf;
        } else {
            o.push_back((char)c);  // pass UTF-8 bytes through untouched
        }
    }
    o.push_back('"');
    return o;
}

std::string anonymousId(uint64_t v) {
    if (v == 0) return "_nameless.0";
    std::string out = "_nameless";
    bool first = true;
    for (int shift = 48; shift >= 0; shift -= 16) {
        unsigned part = (unsigned)((v >> shift) & 0xffffu);
        if (first && part == 0) continue;
        char buf[8];
        std::snprintf(buf, sizeof(buf), first ? ".%x" : ".%04x", part);
        out += buf;
        first = false;
    }
    return out;
}

bool decodeToken(uint64_t raw, std::string& out, std::string& err) {
    out.clear();
    uint64_t n = raw & ~(1ull << 63);  // bit 63 is a flag, not data
    if (n == 0) return true;           // empty token
    char tmp[kMaxTokenChars];
    size_t len = 0;
    while (n != 0) {
        uint64_t digit = n % 38;
        if (digit == 0) { err = "invalid encoded token: zero digit in nonzero value"; return false; }
        if (len >= kMaxTokenChars) { err = "invalid encoded token: exceeds 12 characters"; return false; }
        tmp[len++] = kTokenDigits[digit - 1];
        n /= 38;
    }
    out.assign(tmp, tmp + len);
    // The first character occupies the least significant base-38 digit.
    return true;
}

// Complex unit id: u8 length. 0 = null, 255 = raw anonymous u64, otherwise
// `length` encoded u64 tokens joined by '.'.
bool readComplexId(Reader& r, std::string& out, bool& isNull, std::string& err) {
    uint8_t len = 0;
    if (!r.u8(len)) { err = r.err; return false; }
    isNull = false;
    if (len == 0) { isNull = true; out = "null"; return true; }
    if (len == 255) {
        uint64_t v = 0;
        if (!r.u64(v)) { err = r.err; return false; }
        out = anonymousId(v);
        return true;
    }
    out.clear();
    for (uint32_t i = 0; i < (uint32_t)len; ++i) {
        uint64_t v = 0;
        if (!r.u64(v)) { err = r.err; return false; }
        std::string part;
        if (!decodeToken(v, part, err)) return false;
        if (part.empty()) { err = "unit id contains an empty token"; return false; }
        if (i != 0) out.push_back('.');
        out += part;
    }
    return true;
}

// ----------------------------------------------------------- type table --

bool isScalarType(uint32_t t) {
    switch (t) {
        case 0x01: case 0x03: case 0x05: case 0x07: case 0x09: case 0x11:
        case 0x17: case 0x19: case 0x25: case 0x27: case 0x2b: case 0x2f:
        case 0x31: case 0x33: case 0x35: case 0x37: case 0x39: case 0x3b:
        case 0x3d:
            return true;
        default:
            return false;
    }
}

bool isArrayType(uint32_t t) {
    switch (t) {
        case 0x02: case 0x04: case 0x06: case 0x0a: case 0x12: case 0x18:
        case 0x1a: case 0x26: case 0x28: case 0x2c: case 0x32: case 0x34:
        case 0x36: case 0x3a: case 0x3c:
            return true;
        default:
            return false;
    }
}

bool isUndocumentedType(uint32_t t) { return t == 0x08 || t == 0x38 || t == 0x3e; }

uint32_t arrayElementType(uint32_t t) {
    switch (t) {
        case 0x02: return 0x01;
        case 0x04: return 0x03;
        case 0x06: return 0x05;
        case 0x0a: return 0x09;
        case 0x12: return 0x11;
        case 0x18: return 0x17;
        case 0x1a: return 0x19;
        case 0x26: return 0x25;
        case 0x28: return 0x27;
        case 0x2c: return 0x2b;
        case 0x32: return 0x31;
        case 0x34: return 0x33;
        case 0x36: return 0x35;
        case 0x3a: return 0x39;
        case 0x3c: return 0x3b;
        default:   return 0;
    }
}

size_t elementMinBytes(uint32_t arrayType) {
    switch (arrayType) {
        case 0x02: return 4;                    // u32 length + bytes
        case 0x04: case 0x32: case 0x34: return 8;
        case 0x06: case 0x26: case 0x28: return 4;
        case 0x0a: case 0x12: return 12;
        case 0x18: return 16;
        case 0x1a: return 32;
        case 0x2c: return 2;
        case 0x36: case 0x3a: case 0x3c: return 1;
        default:   return 1;
    }
}

// -------------------------------------------------------------- emitter --

struct Sink {
    std::string buf;
    size_t limit = 0;

    bool add(const char* s, size_t len) {
        if (buf.size() > limit || len > limit - buf.size()) return false;
        buf.append(s, len);
        return true;
    }
    bool add(const std::string& s) { return add(s.data(), s.size()); }
};

using EnumMap = std::vector<std::pair<uint32_t, std::string>>;

bool readScalarText(Reader& r, uint32_t type, const EnumMap* enumMap,
                    std::string& val, std::string& error,
                    const std::function<bool()>& canceled) {
    (void)canceled;
    switch (type) {
        case 0x01: {
            std::string s;
            if (!readStr(r, s)) { error = r.err; return false; }
            val = quoteString(s);
            return true;
        }
        case 0x03: {
            uint64_t bits = 0;
            if (!r.u64(bits)) { error = r.err; return false; }
            std::string tok;
            if (!decodeToken(bits, tok, error)) return false;
            val = tok.empty() ? std::string("\"\"") : tok;
            return true;
        }
        case 0x05: {
            float f = 0.0f;
            if (!r.f32(f)) { error = r.err; return false; }
            val = hexFloat(f);
            return true;
        }
        case 0x07: {
            float a = 0, b = 0;
            if (!r.f32(a) || !r.f32(b)) { error = r.err; return false; }
            val = "(" + decFloat(a) + ", " + decFloat(b) + ")";
            return true;
        }
        case 0x09: {
            float a = 0, b = 0, c = 0;
            if (!r.f32(a) || !r.f32(b) || !r.f32(c)) { error = r.err; return false; }
            val = "(" + decFloat(a) + ", " + decFloat(b) + ", " + decFloat(c) + ")";
            return true;
        }
        case 0x11: {
            int32_t a = 0, b = 0, c = 0;
            if (!readI32(r, a) || !readI32(r, b) || !readI32(r, c)) { error = r.err; return false; }
            val = "(" + std::to_string(a) + ", " + std::to_string(b) + ", " + std::to_string(c) + ")";
            return true;
        }
        case 0x17: {
            float a = 0, b = 0, c = 0, d = 0;
            if (!r.f32(a) || !r.f32(b) || !r.f32(c) || !r.f32(d)) { error = r.err; return false; }
            val = "(" + decFloat(a) + "; " + decFloat(b) + ", " + decFloat(c) + ", " + decFloat(d) + ")";
            return true;
        }
        case 0x19: {
            float x = 0, y = 0, z = 0, hb = 0, qw = 0, qx = 0, qy = 0, qz = 0;
            if (!r.f32(x) || !r.f32(y) || !r.f32(z) || !r.f32(hb) ||
                !r.f32(qw) || !r.f32(qx) || !r.f32(qy) || !r.f32(qz)) {
                error = r.err;
                return false;
            }
            if (!std::isfinite(hb)) { error = "placement hidden bias is not finite"; return false; }
            const double trunc = std::trunc((double)hb);
            if (!(trunc >= -2147483648.0 && trunc <= 2147483647.0)) {
                error = "placement hidden bias is out of range";
                return false;
            }
            const int32_t ib = (int32_t)trunc;
            const int32_t biasX = ((ib & 0xfff) - 2048) * 512;
            const int32_t biasZ = (((ib >> 12) & 0xfff) - 2048) * 512;
            const float nx = x + (float)biasX;  // rebase coordinates (hidden bias omitted)
            const float nz = z + (float)biasZ;
            val = "(" + decFloat(nx) + ", " + decFloat(y) + ", " + decFloat(nz) + ") (" +
                  decFloat(qw) + "; " + decFloat(qx) + ", " + decFloat(qy) + ", " + decFloat(qz) + ")";
            return true;
        }
        case 0x25: {
            int32_t v = 0;
            if (!readI32(r, v)) { error = r.err; return false; }
            val = std::to_string(v);
            return true;
        }
        case 0x27: case 0x2f: {
            uint32_t v = 0;
            if (!r.u32(v)) { error = r.err; return false; }
            val = std::to_string(v);
            return true;
        }
        case 0x2b: {
            uint16_t v = 0;
            if (!r.u16(v)) { error = r.err; return false; }
            val = std::to_string((unsigned)v);
            return true;
        }
        case 0x31: {
            int64_t v = 0;
            if (!readI64(r, v)) { error = r.err; return false; }
            val = std::to_string(v);
            return true;
        }
        case 0x33: {
            uint64_t v = 0;
            if (!r.u64(v)) { error = r.err; return false; }
            val = std::to_string(v);
            return true;
        }
        case 0x35: {
            uint8_t v = 0;
            if (!r.u8(v)) { error = r.err; return false; }
            if (v > 1) { error = "invalid boolean value"; return false; }
            val = v ? "true" : "false";
            return true;
        }
        case 0x37: {
            if (!enumMap) { error = "internal error: enum field without schema map"; return false; }
            uint32_t key = 0;
            if (!r.u32(key)) { error = r.err; return false; }
            for (const auto& kv : *enumMap) {
                if (kv.first == key) { val = kv.second; return true; }
            }
            error = "enum value has no matching key";
            return false;
        }
        case 0x39: case 0x3b: case 0x3d: {
            std::string id;
            bool isNull = false;
            if (!readComplexId(r, id, isNull, error)) return false;
            val = id;
            return true;
        }
        default:
            error = "unknown field type";
            return false;
    }
}

// ------------------------------------------------------------- schemas --

struct FieldDef {
    uint32_t type = 0;
    std::string name;
    EnumMap enumMap;
};

struct SchemaDef {
    uint32_t id = 0;
    std::string name;
    std::vector<FieldDef> fields;
};

bool parseSchema(Reader& r, SchemaDef& sd, std::string& error,
                 const std::function<bool()>& canceled) {
    uint32_t schemaId = 0;
    if (!r.u32(schemaId)) { error = r.err; return false; }
    if (schemaId == 0) { error = "schema id must be nonzero"; return false; }
    sd.id = schemaId;
    if (!readStr(r, sd.name)) { error = r.err; return false; }
    if (!safeMetadata(sd.name)) { error = "unsafe or empty schema name"; return false; }

    std::unordered_set<std::string> fieldNames;
    for (;;) {
        if (canceled && canceled()) { error = "conversion canceled"; return false; }
        uint32_t ftype = 0;
        if (!r.u32(ftype)) { error = r.err; return false; }
        if (ftype == 0) break;
        if (sd.fields.size() >= kMaxFieldsPerSchema) { error = "too many fields in schema"; return false; }

        FieldDef fd;
        fd.type = ftype;
        if (!readStr(r, fd.name)) { error = r.err; return false; }
        if (!safeMetadata(fd.name)) { error = "unsafe or empty field name"; return false; }
        if (!fieldNames.insert(fd.name).second) { error = "duplicate field name in schema"; return false; }

        if (ftype == 0x37) {
            uint32_t ec = 0;
            if (!r.u32(ec)) { error = r.err; return false; }
            if ((size_t)ec > kMaxEnumEntries) { error = "enum entry count exceeds limit"; return false; }
            if ((size_t)ec > r.remaining() / 8) { error = "enum entry count exceeds remaining input"; return false; }
            std::unordered_set<uint32_t> keys;
            fd.enumMap.reserve((size_t)ec);
            for (uint32_t i = 0; i < ec; ++i) {
                uint32_t key = 0;
                if (!r.u32(key)) { error = r.err; return false; }
                std::string token;
                if (!readStr(r, token)) { error = r.err; return false; }
                if (!safeMetadata(token)) { error = "unsafe or empty enum token"; return false; }
                if (!keys.insert(key).second) { error = "duplicate enum key"; return false; }
                fd.enumMap.emplace_back(key, std::move(token));
            }
        } else if (!isScalarType(ftype) && !isArrayType(ftype)) {
            error = isUndocumentedType(ftype)
                        ? "unsupported field type (undocumented in BSII v3)"
                        : "unknown field type";
            return false;
        }
        sd.fields.push_back(std::move(fd));
    }
    return true;
}

// ---------------------------------------------------------------- decode --

bool decodeImpl(const std::vector<uint8_t>& bytes, std::string& text, std::string& error,
                const std::function<bool()>& canceled, size_t maxOutputBytes, BsiiDecodeStats& stats) {
    if (bytes.size() < 8) { error = "file too small for BSII header"; return false; }
    if (!(bytes[0] == 'B' && bytes[1] == 'S' && bytes[2] == 'I' && bytes[3] == 'I')) {
        error = "not a BSII file (bad magic)";
        return false;
    }

    Reader r(bytes);
    r.p = 4;
    uint32_t version = 0;
    if (!r.u32(version)) { error = r.err; return false; }
    if (version != 3) { error = "unsupported BSII version: only v3 is supported"; return false; }
    stats.version = 3;

    Sink sink;
    sink.limit = maxOutputBytes;
    if (!sink.add(std::string("SiiNunit\n{\n"))) { error = "SII output exceeds size limit"; return false; }

    std::vector<SchemaDef> schemas;
    std::unordered_map<uint32_t, size_t> schemaById;
    std::unordered_set<std::string> unitIds;

    bool terminated = false;
    while (!terminated) {
        if (canceled && canceled()) { error = "conversion canceled"; return false; }
        if (r.remaining() == 0) { error = "premature end of BSII stream (missing terminator)"; return false; }

        uint32_t blockType = 0;
        if (!r.u32(blockType)) { error = r.err; return false; }

        if (blockType == 0) {
            uint8_t valid = 0;
            if (!r.u8(valid)) { error = r.err; return false; }
            if (valid == 0) { terminated = true; break; }
            if (valid != 1) { error = "invalid schema block validity flag"; return false; }
            if (schemas.size() >= kMaxSchemas) { error = "too many schemas"; return false; }
            SchemaDef sd;
            if (!parseSchema(r, sd, error, canceled)) return false;
            if (schemaById.count(sd.id)) { error = "duplicate schema id"; return false; }
            schemaById.emplace(sd.id, schemas.size());
            schemas.push_back(std::move(sd));
            continue;
        }

        auto it = schemaById.find(blockType);
        if (it == schemaById.end()) { error = "data block references undeclared schema"; return false; }
        if (stats.unitCount >= kMaxUnits) { error = "unit count exceeds limit"; return false; }
        const SchemaDef& schema = schemas[it->second];

        std::string unitId;
        bool isNull = false;
        if (!readComplexId(r, unitId, isNull, error)) return false;
        if (isNull) { error = "unit header id must not be null"; return false; }
        if (!unitIds.insert(unitId).second) { error = "duplicate unit id"; return false; }
        ++stats.unitCount;

        if (!sink.add(schema.name + " : " + unitId + " {\n")) {
            error = "SII output exceeds size limit";
            return false;
        }

        for (const FieldDef& fd : schema.fields) {
            if (canceled && canceled()) { error = "conversion canceled"; return false; }
            if (isArrayType(fd.type)) {
                uint32_t count = 0;
                if (!r.u32(count)) { error = r.err; return false; }
                if (count > kMaxArrayCount) { error = "array element count exceeds limit"; return false; }
                const size_t minBytes = elementMinBytes(fd.type);
                if (minBytes > 0 && (size_t)count > r.remaining() / minBytes) {
                    error = "array count exceeds remaining input";
                    return false;
                }
                if (!sink.add(fd.name + ": " + std::to_string(count) + "\n")) {
                    error = "SII output exceeds size limit";
                    return false;
                }
                const uint32_t elemType = arrayElementType(fd.type);
                for (uint32_t i = 0; i < count; ++i) {
                    if ((i & 0x3ffu) == 0 && canceled && canceled()) { error = "conversion canceled"; return false; }
                    std::string val;
                    if (!readScalarText(r, elemType, nullptr, val, error, canceled)) return false;
                    if (!sink.add(fd.name + "[" + std::to_string(i) + "]: " + val + "\n")) {
                        error = "SII output exceeds size limit";
                        return false;
                    }
                }
            } else {
                std::string val;
                if (!readScalarText(r, fd.type, &fd.enumMap, val, error, canceled)) return false;
                if (!sink.add(fd.name + ": " + val + "\n")) {
                    error = "SII output exceeds size limit";
                    return false;
                }
            }
        }
        if (!sink.add(std::string("}\n"))) { error = "SII output exceeds size limit"; return false; }
    }

    if (!sink.add(std::string("}\n"))) { error = "SII output exceeds size limit"; return false; }
    if (r.remaining() != 0) { error = "trailing bytes after BSII terminator"; return false; }

    stats.schemaCount = schemas.size();
    text = std::move(sink.buf);
    return true;
}

}  // namespace

bool decodeBsiiText(const std::vector<uint8_t>& bytes, std::string* out, std::string* error,
                    std::function<bool()> canceled, size_t maxOutputBytes, BsiiDecodeStats* stats) {
    if (stats) *stats = {};
    if (out) out->clear();          // never leak stale/partial output
    if (error) error->clear();
    if (!out) {
        if (error) *error = "null output buffer";
        return false;
    }
    if (maxOutputBytes < 16) {
        if (error) *error = "output size limit too small";
        return false;
    }

    BsiiDecodeStats local;
    std::string text;
    std::string err;
    const bool ok = decodeImpl(bytes, text, err, canceled, maxOutputBytes, local);
    if (ok) {
        *out = std::move(text);
        if (stats) *stats = local;
        return true;
    }
    out->clear();
    if (error) *error = err.empty() ? std::string("BSII decode failed") : err;
    return false;
}

}  // namespace ets2
