#include "bsiitest.h"

#include "bsii.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ets2 {
namespace {

struct Writer {
    std::vector<uint8_t> b;

    Writer& u8(uint8_t v) { b.push_back(v); return *this; }
    Writer& u32(uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i))); return *this; }
    Writer& u64(uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back((uint8_t)(v >> (8 * i))); return *this; }
    Writer& f32(float v) { uint32_t t = 0; std::memcpy(&t, &v, 4); return u32(t); }
    Writer& str(const std::string& s) {
        u32((uint32_t)s.size());
        b.insert(b.end(), s.begin(), s.end());
        return *this;
    }
    Writer& header() { b.insert(b.end(), {'B', 'S', 'I', 'I'}); return u32(3); }
    Writer& endFile() { return u32(0).u8(0); }  // blockType 0 + valid 0
};

using Fields = std::vector<std::pair<uint32_t, std::string>>;

template <class F>
std::vector<uint8_t> oneUnitFile(const std::string& schemaName, const Fields& fields,
                                 uint8_t idLen, const std::vector<uint64_t>& idParts, F&& writeValues) {
    Writer w;
    w.header();
    w.u32(0).u8(1).u32(1).str(schemaName);
    for (const auto& f : fields) w.u32(f.first).str(f.second);
    w.u32(0);            // end of schema field list
    w.u32(1);            // data block for schema id 1
    w.u8(idLen);
    for (uint64_t p : idParts) w.u64(p);
    writeValues(w);
    w.endFile();
    return std::move(w.b);
}

struct Case {
    bool ok = false;
    std::string text;
    std::string error;
};

Case runCase(const std::vector<uint8_t>& bytes, std::function<bool()> canceled = {},
             size_t maxOut = 128u * 1024u * 1024u) {
    Case c;
    c.text = "STALE";  // must not survive a failure
    c.ok = decodeBsiiText(bytes, &c.text, &c.error, std::move(canceled), maxOut, nullptr);
    return c;
}

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

bool failedClean(const Case& c, const std::string& needle) {
    return !c.ok && c.text.empty() && (needle.empty() || has(c.error, needle));
}

std::vector<uint8_t> threeFieldFile() {
    return oneUnitFile("u", {{0x01, "s"}, {0x27, "n"}, {0x35, "b"}}, 255, {0}, [](Writer& w) {
        w.str("x").u32(7).u8(1);
    });
}

}  // namespace

std::vector<TunerTestItem> runBsiiTests() {
    std::vector<TunerTestItem> items;
    auto add = [&](const std::string& name, bool ok, const std::string& detail = std::string()) {
        items.push_back(TunerTestItem{name, ok, detail});
    };

    // ---- happy path: utf8, quotes, control bytes, structure ----
    {
        std::string special = "he\"llo\\";
        const unsigned char utf8[] = {0xe4, 0xb8, 0x96};
        special.append((const char*)utf8, 3);
        special.push_back('\n');
        special.push_back((char)0x01);
        auto bytes = oneUnitFile("unit_a", {{0x01, "s"}}, 255, {0}, [&](Writer& w) { w.str(special); });
        Case c = runCase(bytes);
        std::string expected = "s: \"he\\\"llo\\\\";
        expected.append((const char*)utf8, 3);
        expected += "\\x0a\\x01\"\n";
        const bool ok = c.ok && has(c.text, "SiiNunit\n{\n") && has(c.text, "unit_a : _nameless.0 {\n") &&
                        has(c.text, expected) && c.text.size() >= 4 &&
                        c.text.compare(c.text.size() - 2, 2, "}\n") == 0;
        add("bsii/string utf8+quotes+control", ok, ok ? "" : (c.error.empty() ? c.text : c.error));
    }

    // ---- arrays incl. count 0, bools, strings ----
    {
        auto bytes = oneUnitFile("arr", {{0x02, "names"}, {0x26, "ints"}, {0x36, "flags"}}, 255, {5},
                                 [](Writer& w) {
                                     w.u32(2).str("a").str("b");
                                     w.u32(0);
                                     w.u32(3).u8(1).u8(0).u8(1);
                                 });
        Case c = runCase(bytes);
        const bool ok = c.ok &&
            has(c.text, "names: 2\nnames[0]: \"a\"\nnames[1]: \"b\"\n") &&
            has(c.text, "ints: 0\n") && !has(c.text, "ints[0]") &&
            has(c.text, "flags: 3\nflags[0]: true\nflags[1]: false\nflags[2]: true\n");
        add("bsii/arrays count0 bools strings", ok, ok ? "" : c.error);
    }

    // ---- empty token vs token ----
    {
        auto bytes = oneUnitFile("t", {{0x03, "empty"}, {0x03, "tok"}}, 255, {0}, [](Writer& w) {
            w.u64(0).u64(11);
        });
        Case c = runCase(bytes);
        const bool ok = c.ok && has(c.text, "empty: \"\"\n") && has(c.text, "tok: a\n");
        add("bsii/empty token emits quotes", ok, ok ? "" : c.error);
    }

    // Protocol literals, independent of the writer: garage=1224619291,
    // koln=1349667. Multi-character names must retain their actual order.
    {
        auto bytes = oneUnitFile("garage", {{0x03, "city"}}, 2,
                                 {1224619291ull, 1349667ull}, [](Writer& w) {
            w.u64(1349667ull);
        });
        Case c = runCase(bytes);
        const bool ok = c.ok && has(c.text, "garage : garage.koln {\n") &&
                        has(c.text, "city: koln\n");
        add("bsii/multichar token order", ok, c.text + c.error);
    }
    {
        auto bytes = oneUnitFile("u", {}, 255, {0x00000003448a1388ull}, [](Writer&) {});
        Case c = runCase(bytes);
        const bool ok = c.ok && has(c.text, "u : _nameless.3.448a.1388 {\n");
        add("bsii/canonical anonymous id", ok, c.text + c.error);
    }
    {
        auto bytes = oneUnitFile("u", {}, 2, {11, 0}, [](Writer&) {});
        Case c = runCase(bytes);
        add("bsii/empty unit id part rejected", failedClean(c, "empty token"), c.error);
    }

    // ---- i64/u64 full range ----
    {
        auto bytes = oneUnitFile("n", {{0x31, "i"}, {0x33, "u"}}, 255, {0}, [](Writer& w) {
            w.u64((uint64_t)INT64_MIN).u64(UINT64_MAX);
        });
        Case c = runCase(bytes);
        const bool ok = c.ok && has(c.text, "i: -9223372036854775808\n") &&
                        has(c.text, "u: 18446744073709551615\n");
        add("bsii/i64 u64 full range", ok, ok ? "" : c.error);
    }

    // ---- float bit preservation incl. NaN and -0 ----
    {
        auto bytes = oneUnitFile("f", {{0x05, "a"}, {0x05, "b"}, {0x05, "c"}}, 255, {0}, [](Writer& w) {
            w.u32(0x7fc00000u).u32(0x80000000u).f32(1.0f);
        });
        Case c = runCase(bytes);
        const bool ok = c.ok && has(c.text, "a: &7fc00000\n") && has(c.text, "b: &80000000\n") &&
                        has(c.text, "c: &3f800000\n");
        add("bsii/float exact ieee bits", ok, ok ? "" : c.error);
    }

    // ---- vector / quaternion syntax ----
    {
        auto bytes = oneUnitFile("v", {{0x07, "v2"}, {0x09, "v3"}, {0x17, "q"}}, 255, {0}, [](Writer& w) {
            w.f32(1.25f).f32(-2.5f);
            w.f32(1.0f).f32(2.0f).f32(3.0f);
            w.f32(1.0f).f32(0.0f).f32(0.0f).f32(0.0f);
        });
        Case c = runCase(bytes);
        const bool ok = c.ok && has(c.text, "v2: (1.25, -2.5)\n") && has(c.text, "v3: (1, 2, 3)\n") &&
                        has(c.text, "q: (1; 0, 0, 0)\n");
        add("bsii/vec2 vec3 quaternion syntax", ok, ok ? "" : c.error);
    }

    // ---- placement hidden bias rebasing ----
    {
        auto bytes = oneUnitFile("p", {{0x19, "pos"}}, 255, {0}, [](Writer& w) {
            w.f32(1.5f).f32(2.5f).f32(3.5f).f32(2050.0f);  // biasX=1024, biasZ=-1048576
            w.f32(1.0f).f32(0.0f).f32(0.0f).f32(0.0f);
        });
        Case c = runCase(bytes);
        const bool ok = c.ok && has(c.text, "pos: (1025.5, 2.5, -1048572.5) (1; 0, 0, 0)\n");
        add("bsii/placement rebases hidden bias", ok, ok ? "" : c.error);
    }

    // ---- named + anonymous unit ids ----
    {
        Writer w;
        w.header();
        w.u32(0).u8(1).u32(1).str("u");
        w.u32(0x27).str("n");
        w.u32(0);
        w.u32(1).u8(2).u64(11).u64(12).u32(11);   // unit id "a.b"
        w.u32(1).u8(255).u64(0).u32(12);          // unit id "_nameless.0"
        w.endFile();
        Case c = runCase(w.b);
        const bool ok = c.ok && has(c.text, "u : a.b {\n") && has(c.text, "u : _nameless.0 {\n");
        add("bsii/named and anonymous ids", ok, ok ? "" : c.error);
    }

    // ---- enum mapping ----
    {
        Writer w;
        w.header();
        w.u32(0).u8(1).u32(1).str("e");
        w.u32(0x37).str("kind").u32(2).u32(7).str("seven").u32(9).str("nine");
        w.u32(0);
        w.u32(1).u8(255).u64(1).u32(9);
        w.endFile();
        Case c = runCase(w.b);
        const bool ok = c.ok && has(c.text, "kind: nine\n");
        add("bsii/enum token mapping", ok, ok ? "" : c.error);

        Writer w2;
        w2.b = w.b;
        w2.b.erase(w2.b.end() - 9, w2.b.end());  // drop enum key + file terminator
        w2.u32(5);
        w2.endFile();
        Case c2 = runCase(w2.b);
        add("bsii/enum missing key rejected", failedClean(c2, "no matching key"), c2.error);
    }

    // ---- malformed value: invalid bool ----
    {
        auto bytes = oneUnitFile("b", {{0x35, "flag"}}, 255, {0}, [](Writer& w) { w.u8(2); });
        Case c = runCase(bytes);
        add("bsii/invalid bool rejected", failedClean(c, "boolean"), c.error);
    }

    // ---- truncated stream ----
    {
        auto bytes = threeFieldFile();
        bytes.erase(bytes.end() - 3, bytes.end());
        Case c = runCase(bytes);
        add("bsii/truncated stream rejected+cleared", failedClean(c, ""), c.error);
    }

    // ---- unknown version ----
    {
        std::vector<uint8_t> bytes = {'B', 'S', 'I', 'I', 2, 0, 0, 0, 0, 0, 0, 0, 0};
        Case c = runCase(bytes);
        add("bsii/unknown version rejected", failedClean(c, "version"), c.error);
    }

    // ---- unknown / undocumented field type ----
    {
        Writer w;
        w.header();
        w.u32(0).u8(1).u32(1).str("t").u32(0x50).str("x").u32(0);
        w.endFile();
        Case c = runCase(w.b);
        add("bsii/unknown field type rejected", failedClean(c, "unknown field type"), c.error);

        Writer w2;
        w2.header();
        w2.u32(0).u8(1).u32(1).str("t").u32(0x08).str("x").u32(0);
        w2.endFile();
        Case c2 = runCase(w2.b);
        add("bsii/undocumented type rejected", failedClean(c2, "undocumented"), c2.error);
    }

    // ---- trailing bytes after terminator ----
    {
        auto bytes = threeFieldFile();
        bytes.push_back(0);
        Case c = runCase(bytes);
        add("bsii/trailing bytes rejected", failedClean(c, "trailing"), c.error);
    }

    // ---- duplicate schema id ----
    {
        Writer w;
        w.header();
        w.u32(0).u8(1).u32(1).str("a").u32(0);
        w.u32(0).u8(1).u32(1).str("b").u32(0);
        w.endFile();
        Case c = runCase(w.b);
        add("bsii/duplicate schema id rejected", failedClean(c, "duplicate schema"), c.error);
    }

    // ---- duplicate unit id ----
    {
        Writer w;
        w.header();
        w.u32(0).u8(1).u32(1).str("u").u32(0x27).str("n").u32(0);
        w.u32(1).u8(255).u64(3).u32(1);
        w.u32(1).u8(255).u64(3).u32(2);
        w.endFile();
        Case c = runCase(w.b);
        add("bsii/duplicate unit id rejected", failedClean(c, "duplicate unit"), c.error);
    }

    // ---- duplicate field name ----
    {
        Writer w;
        w.header();
        w.u32(0).u8(1).u32(1).str("u");
        w.u32(0x27).str("n").u32(0x27).str("n").u32(0);
        w.endFile();
        Case c = runCase(w.b);
        add("bsii/duplicate field name rejected", failedClean(c, "duplicate field"), c.error);
    }

    // ---- undeclared data schema ----
    {
        Writer w;
        w.header();
        w.u32(42).u8(255).u64(1);
        w.endFile();
        Case c = runCase(w.b);
        add("bsii/undeclared schema rejected", failedClean(c, "undeclared"), c.error);
    }

    // ---- null unit header ----
    {
        Writer w;
        w.header();
        w.u32(0).u8(1).u32(1).str("u").u32(0x27).str("n").u32(0);
        w.u32(1).u8(0);  // null id, no field bytes
        w.endFile();
        Case c = runCase(w.b);
        add("bsii/null unit header rejected", failedClean(c, "must not be null"), c.error);
    }

    // ---- cancellation ----
    {
        auto bytes = threeFieldFile();
        Case c = runCase(bytes, [] { return true; });
        add("bsii/cancellation aborts+clears", failedClean(c, "cancel"), c.error);
    }

    // ---- output size refusal ----
    {
        std::string big(4096, 'z');
        auto bytes = oneUnitFile("big", {{0x01, "s"}}, 255, {0}, [&](Writer& w) { w.str(big); });
        Case c = runCase(bytes, {}, 64);
        add("bsii/oversize output refused+cleared", failedClean(c, "limit"), c.error);
    }

    return items;
}

}  // namespace ets2
