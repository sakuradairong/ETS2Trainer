#include "trucktransfer.h"
#include "fleet.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace ets2 {
namespace {

constexpr std::size_t kMaxText = 128u * 1024u * 1024u;
constexpr std::size_t kMaxUnits = 500000u;
constexpr std::size_t kMaxGraphNodes = 1024u;
constexpr std::size_t kMaxGraphReferences = 65536u;

enum class TokKind { Symbol, String, Atom, Comment };

struct Tok {
    TokKind kind = TokKind::Atom;
    std::string text;
    std::size_t begin = 0;
    std::size_t end = 0;
    int depth = 0;
};

struct Prop {
    std::string name;
    std::size_t begin = 0;
    int index = -1;
    std::vector<Tok> values;
};

struct UnitInfo {
    std::string type;
    std::string id;
    std::size_t headerBegin = 0;
    std::size_t headerIdBegin = 0;
    std::size_t headerIdEnd = 0;
    std::size_t bodyOpenTok = 0;
    std::size_t closeTok = 0;
    std::size_t end = 0;
    std::vector<Prop> props;
};

struct Doc {
    std::string text;
    std::vector<Tok> toks;
    std::vector<UnitInfo> units;
    std::unordered_map<std::string, std::size_t> byId;
    std::size_t outerCloseTok = 0;
};

void setError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

bool isSpaceByte(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

bool isAtomDelimiter(unsigned char c) {
    return isSpaceByte(c) || c == '{' || c == '}' || c == '[' || c == ']' || c == ':' ||
           c == '"' || c == '#';
}

bool startsNameless(const std::string& s) {
    return s.rfind("_nameless.", 0) == 0;
}

std::string anonymousId(std::uint64_t value) {
    if(!value) return "_nameless.0";
    std::string out="_nameless";bool first=true;
    for(int shift=48;shift>=0;shift-=16) {
        const auto part=static_cast<unsigned>((value>>shift)&0xffffu);
        if(first && !part) continue;
        char buf[8];std::snprintf(buf,sizeof(buf),first ? ".%x" : ".%04x",part);
        out+=buf;first=false;
    }
    return out;
}

// The game compares anonymous IDs numerically, regardless of case or padding.
std::string idKey(const std::string& id) {
    if(!startsNameless(id)) return id;
    std::uint64_t value=0;size_t pos=10,groups=0;
    while(pos<id.size()) {
        const auto end=id.find('.',pos);
        const auto len=(end==std::string::npos ? id.size() : end)-pos;
        if(!len || len>4 || ++groups>4) return id;
        unsigned part=0;
        for(size_t i=0;i<len;++i) {
            const auto c=static_cast<unsigned char>(id[pos+i]);
            unsigned digit=c>='0' && c<='9' ? c-'0' :
                           c>='a' && c<='f' ? c-'a'+10 :
                           c>='A' && c<='F' ? c-'A'+10 : 16;
            if(digit>=16) return id;
            part=(part<<4)|digit;
        }
        value=(value<<16)|part;
        if(end==std::string::npos) return anonymousId(value);
        pos=end+1;
    }
    return id;
}

bool tokenize(const std::string& text, std::vector<Tok>* out, std::string* error,
              const std::function<bool()>& canceled = {}) {
    if (text.size() > kMaxText) {
        setError(error, "SII text exceeds the 128 MiB limit");
        return false;
    }
    if (text.rfind("BSII", 0) == 0) {
        setError(error, "BSII binary save is not supported; provide decrypted SiiNunit text");
        return false;
    }
    std::vector<Tok> toks;
    const std::size_t n = text.size();
    std::size_t i = 0;
    int depth = 0;
    while (i < n) {
        if ((toks.size() & 0x3ffu) == 0 && canceled && canceled()) {
            setError(error, "读取车辆数据已取消"); return false;
        }
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (isSpaceByte(c)) {
            ++i;
            continue;
        }
        if (c == '#') {
            const std::size_t begin = i;
            while (i < n && text[i] != '\n') ++i;
            Tok t;
            t.kind = TokKind::Comment;
            t.text = text.substr(begin, i - begin);
            t.begin = begin;
            t.end = i;
            t.depth = depth;
            toks.push_back(t);
            continue;
        }
        if (c == '/' && i + 1 < n && text[i + 1] == '/') {
            const std::size_t begin = i;
            while (i < n && text[i] != '\n') ++i;
            Tok t;
            t.kind = TokKind::Comment;
            t.text = text.substr(begin, i - begin);
            t.begin = begin;
            t.end = i;
            t.depth = depth;
            toks.push_back(t);
            continue;
        }
        if (c == '"') {
            const std::size_t begin = i;
            ++i;
            std::string value;
            bool closed = false;
            while (i < n) {
                const char d = text[i];
                if (d == '\\' && i + 1 < n) {
                    value.push_back(d);
                    value.push_back(text[i + 1]);
                    i += 2;
                    continue;
                }
                if (d == '"') {
                    ++i;
                    closed = true;
                    break;
                }
                value.push_back(d);
                ++i;
            }
            if (!closed) {
                setError(error, "unterminated quoted string in SII text");
                return false;
            }
            Tok t;
            t.kind = TokKind::String;
            t.text = value;
            t.begin = begin;
            t.end = i;
            t.depth = depth;
            toks.push_back(t);
            continue;
        }
        if (c == '{' || c == '}' || c == '[' || c == ']' || c == ':') {
            if (c == '}') {
                --depth;
                if (depth < 0) {
                    setError(error, "unbalanced closing brace in SII text");
                    return false;
                }
            }
            Tok t;
            t.kind = TokKind::Symbol;
            t.text.assign(1, static_cast<char>(c));
            t.begin = i;
            t.end = i + 1;
            t.depth = depth;
            toks.push_back(t);
            if (c == '{') ++depth;
            ++i;
            continue;
        }
        const std::size_t begin = i;
        while (i < n && !isAtomDelimiter(static_cast<unsigned char>(text[i]))) ++i;
        if (i == begin) {
            ++i;
            continue;
        }
        Tok t;
        t.kind = TokKind::Atom;
        t.text = text.substr(begin, i - begin);
        t.begin = begin;
        t.end = i;
        t.depth = depth;
        toks.push_back(t);
    }
    if (depth != 0) {
        setError(error, "unbalanced braces in SII text");
        return false;
    }
    *out = std::move(toks);
    return true;
}

std::size_t nextNonComment(const std::vector<Tok>& toks, std::size_t pos) {
    while (pos < toks.size() && toks[pos].kind == TokKind::Comment) ++pos;
    return pos;
}

bool isSymbol(const Tok& t, char c) {
    return t.kind == TokKind::Symbol && t.text.size() == 1 && t.text[0] == c;
}

bool parseIndexToken(const std::string& s, int* value) {
    if (s.empty() || s.size() > 7) return false;
    std::size_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10u + static_cast<std::size_t>(c - '0');
        if (v > kMaxUnits) return false;
    }
    *value = static_cast<int>(v);
    return true;
}

bool parseCountToken(const std::string& s, std::size_t* value) {
    if (s.empty() || s.size() > 7) return false;
    std::size_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10u + static_cast<std::size_t>(c - '0');
        if (v > kMaxUnits) return false;
    }
    *value = v;
    return true;
}

bool isPropStart(const std::vector<Tok>& toks, std::size_t pos, std::size_t endPos) {
    if (pos >= endPos) return false;
    const Tok& name = toks[pos];
    if (name.kind != TokKind::Atom || name.depth != 2) return false;
    std::size_t q = nextNonComment(toks, pos + 1);
    if (q >= endPos) return false;
    if (isSymbol(toks[q], ':')) return true;
    if (!isSymbol(toks[q], '[')) return false;
    const std::size_t a = nextNonComment(toks, q + 1);
    if (a >= endPos || toks[a].kind != TokKind::Atom || toks[a].depth != 2) return false;
    int ignored = 0;
    if (!parseIndexToken(toks[a].text, &ignored)) return false;
    const std::size_t b = nextNonComment(toks, a + 1);
    if (b >= endPos || !isSymbol(toks[b], ']')) return false;
    const std::size_t c = nextNonComment(toks, b + 1);
    return c < endPos && isSymbol(toks[c], ':');
}

bool parseProps(const std::vector<Tok>& toks, UnitInfo* unit, std::string* error) {
    std::size_t idx = unit->bodyOpenTok + 1;
    while (idx < unit->closeTok) {
        if (!isPropStart(toks, idx, unit->closeTok)) {
            ++idx;
            continue;
        }
        const Tok& name = toks[idx];
        std::size_t q = nextNonComment(toks, idx + 1);
        int index = -1;
        if (isSymbol(toks[q], '[')) {
            const std::size_t a = nextNonComment(toks, q + 1);
            if (a >= unit->closeTok || toks[a].kind != TokKind::Atom ||
                !parseIndexToken(toks[a].text, &index)) {
                setError(error, "malformed array index in unit " + unit->type + " " + unit->id);
                return false;
            }
            const std::size_t b = nextNonComment(toks, a + 1);
            if (b >= unit->closeTok || !isSymbol(toks[b], ']')) {
                setError(error, "malformed array index in unit " + unit->type + " " + unit->id);
                return false;
            }
            q = nextNonComment(toks, b + 1);
        }
        if (q >= unit->closeTok || !isSymbol(toks[q], ':')) {
            setError(error, "malformed property in unit " + unit->type + " " + unit->id);
            return false;
        }
        Prop prop;
        prop.name = name.text;
        prop.begin = name.begin;
        prop.index = index;
        std::size_t v = nextNonComment(toks, q + 1);
        std::size_t e = v;
        while (e < unit->closeTok && !isPropStart(toks, e, unit->closeTok)) ++e;
        for (std::size_t k = v; k < e; ++k) {
            if (toks[k].kind != TokKind::Comment) prop.values.push_back(toks[k]);
        }
        unit->props.push_back(std::move(prop));
        idx = e;
    }
    return true;
}

bool parseUnits(Doc* doc, std::string* error) {
    const std::vector<Tok>& toks = doc->toks;
    std::size_t i = nextNonComment(toks, 0);
    if (i >= toks.size() || toks[i].kind != TokKind::Atom || toks[i].text != "SiiNunit") {
        setError(error, "only SiiNunit text saves are supported");
        return false;
    }
    const std::size_t open = nextNonComment(toks, i + 1);
    if (open >= toks.size() || !isSymbol(toks[open], '{')) {
        setError(error, "SiiNunit is missing its opening brace");
        return false;
    }
    std::unordered_set<std::string> seenIds;
    std::size_t j = nextNonComment(toks, open + 1);
    while (j < toks.size()) {
        const Tok& t = toks[j];
        if (t.depth == 0 && isSymbol(t, '}')) {
            doc->outerCloseTok = j;
            const std::size_t trailing = nextNonComment(toks, j + 1);
            if (trailing < toks.size()) {
                setError(error, "trailing tokens after the SiiNunit outer block");
                return false;
            }
            return true;
        }
        if (t.depth != 1 || t.kind != TokKind::Atom) {
            setError(error, "unexpected token inside SiiNunit");
            return false;
        }
        const std::size_t colon = nextNonComment(toks, j + 1);
        const std::size_t idTok = nextNonComment(toks, colon + 1);
        const std::size_t brace = nextNonComment(toks, idTok + 1);
        if (colon >= toks.size() || idTok >= toks.size() || brace >= toks.size() ||
            !isSymbol(toks[colon], ':') || toks[idTok].kind != TokKind::Atom ||
            toks[idTok].depth != 1 || !isSymbol(toks[brace], '{')) {
            setError(error, "malformed unit header inside SiiNunit");
            return false;
        }
        UnitInfo unit;
        unit.type = t.text;
        unit.id = toks[idTok].text;
        unit.headerBegin = t.begin;
        unit.headerIdBegin = toks[idTok].begin;
        unit.headerIdEnd = toks[idTok].end;
        unit.bodyOpenTok = brace;
        if (!seenIds.insert(idKey(unit.id)).second) {
            setError(error, "duplicate unit id: " + unit.id);
            return false;
        }
        std::size_t close = toks.size();
        for (std::size_t k = brace + 1; k < toks.size(); ++k) {
            if (toks[k].kind != TokKind::Symbol) continue;
            if (toks[k].text == "{" && toks[k].depth >= 2) {
                setError(error, "nested blocks are not supported in unit " + unit.type + " " + unit.id);
                return false;
            }
            if (toks[k].text == "}" && toks[k].depth == 1) {
                close = k;
                break;
            }
            if (toks[k].text == "}" && toks[k].depth == 0) break;
        }
        if (close >= toks.size()) {
            setError(error, "missing closing brace for unit " + unit.type + " " + unit.id);
            return false;
        }
        unit.closeTok = close;
        unit.end = toks[close].end;
        if (!parseProps(toks, &unit, error)) return false;
        if (doc->units.size() >= kMaxUnits) {
            setError(error, "SII document exceeds the 500000 unit limit");
            return false;
        }
        doc->byId[unit.id] = doc->units.size();
        doc->units.push_back(std::move(unit));
        j = nextNonComment(toks, close + 1);
    }
    setError(error, "missing closing brace for the SiiNunit outer block");
    return false;
}

bool loadDoc(const std::string& text, Doc* doc, std::string* error,
             const std::function<bool()>& canceled = {}) {
    Doc result;
    result.text = text;
    if (!tokenize(text, &result.toks, error, canceled)) return false;
    if (!parseUnits(&result, error)) return false;
    *doc = std::move(result);
    return true;
}

const Prop* findScalar(const UnitInfo& unit, const std::string& name) {
    const Prop* found = nullptr;
    for (const Prop& p : unit.props) {
        if (p.name != name || p.index >= 0) continue;
        if (found) return nullptr;
        found = &p;
    }
    return found;
}

bool readScalar(const UnitInfo& unit, const std::string& name, const Tok** value,
                std::string* error) {
    const Prop* p = findScalar(unit, name);
    if (!p) {
        setError(error, "missing or duplicate scalar property " + name + " in unit " + unit.type +
                            " " + unit.id);
        return false;
    }
    if (p->values.size() != 1) {
        setError(error, "malformed scalar property " + name + " in unit " + unit.type + " " +
                            unit.id);
        return false;
    }
    *value = &p->values[0];
    return true;
}

bool readArray(const UnitInfo& unit, const std::string& name, const Tok** count,
               std::vector<const Tok*>* values, std::string* error) {
    const Prop* countProp = nullptr;
    std::vector<const Prop*> elements;
    for (const Prop& p : unit.props) {
        if (p.name != name) continue;
        if (p.index < 0) {
            if (countProp) {
                setError(error, "duplicate count property " + name + " in unit " + unit.type + " " +
                                    unit.id);
                return false;
            }
            countProp = &p;
        } else {
            elements.push_back(&p);
        }
    }
    if (!countProp) {
        setError(error, "missing array " + name + " in unit " + unit.type + " " + unit.id);
        return false;
    }
    if (countProp->values.size() != 1) {
        setError(error, "malformed count property " + name + " in unit " + unit.type + " " +
                            unit.id);
        return false;
    }
    std::size_t n = 0;
    if (countProp->values[0].kind!=TokKind::Atom || !parseCountToken(countProp->values[0].text, &n)) {
        setError(error, "invalid count for " + name + " in unit " + unit.type + " " + unit.id);
        return false;
    }
    if (elements.size() != n) {
        setError(error, "array " + name + " count mismatch in unit " + unit.type + " " + unit.id);
        return false;
    }
    std::vector<const Prop*> ordered(n, nullptr);
    for (const Prop* p : elements) {
        if (p->index < 0 || static_cast<std::size_t>(p->index) >= n || ordered[p->index]) {
            setError(error, "duplicate or out-of-range index in " + name + " in unit " + unit.type +
                                " " + unit.id);
            return false;
        }
        ordered[p->index] = p;
    }
    values->clear();
    values->reserve(n);
    for (std::size_t k = 0; k < n; ++k) {
        if (!ordered[k] || ordered[k]->values.size() != 1) {
            setError(error, "malformed element in " + name + " in unit " + unit.type + " " +
                                unit.id);
            return false;
        }
        values->push_back(&ordered[k]->values[0]);
    }
    *count = &countProp->values[0];
    return true;
}

const UnitInfo* findById(const Doc& doc, const std::string& id) {
    const auto it = doc.byId.find(id);
    if (it == doc.byId.end()) return nullptr;
    return &doc.units[it->second];
}

bool readStatusPositive(const UnitInfo& unit, std::string* error) {
    const Tok* status = nullptr;
    if (!readScalar(unit, "status", &status, error)) return false;
    int value = 0;
    if (!parseIndexToken(status->text, &value) || value <= 0) {
        setError(error, "garage " + unit.id + " is not purchased");
        return false;
    }
    return true;
}

const UnitInfo* findPlayer(const Doc& doc, std::string* error) {
    const UnitInfo* player = nullptr;
    for (const UnitInfo& u : doc.units) {
        if (u.type != "player") continue;
        if (player) {
            setError(error, "multiple player units are not supported");
            return nullptr;
        }
        player = &u;
    }
    if (!player) {
        setError(error, "player unit not found");
        return nullptr;
    }
    return player;
}

bool isAllowedAccessoryType(const std::string& type) {
    return type == "vehicle_accessory" || type == "vehicle_paint_job_accessory" ||
           type == "vehicle_wheel_accessory" || type == "vehicle_addon_accessory" ||
           type == "vehicle_drv_plate_accessory";
}

std::string labelForVehicle(const Doc& doc, const UnitInfo& vehicle) {
    for (const Prop& p : vehicle.props) {
        if (p.name != "accessories" || p.index < 0) continue;
        for (const Tok& v : p.values) {
            if (v.kind != TokKind::Atom) continue;
            const UnitInfo* accessory = findById(doc, v.text);
            if (!accessory || accessory->type != "vehicle_accessory") continue;
            const Prop* path = findScalar(*accessory, "data_path");
            if (!path || path->values.size() != 1) continue;
            const std::string& value = path->values[0].text;
            const std::string marker = "/def/vehicle/truck/";
            const std::size_t p0 = value.find(marker);
            if (p0 == std::string::npos) continue;
            const std::size_t begin = p0 + marker.size();
            const std::size_t end = value.find('/', begin);
            if (begin < value.size()) {
                return value.substr(begin, end == std::string::npos ? std::string::npos
                                                                    : end - begin);
            }
        }
    }
    return vehicle.id;
}

bool collectRefs(const Doc& doc, const UnitInfo& unit,
                 std::vector<std::string>* refs, std::string* error) {
    for (const Prop& p : unit.props) {
        for (const Tok& v : p.values) {
            if (v.kind == TokKind::Atom &&
                (startsNameless(v.text) || doc.byId.count(v.text))) {
                if (refs->size() >= kMaxGraphReferences) {
                    setError(error, "truck graph exceeds 65536 references"); return false;
                }
                refs->push_back(v.text);
            }
        }
    }
    return true;
}

bool validateTruckGraph(const Doc& doc, const UnitInfo& vehicle, std::size_t* accessoryCount,
                        std::string* error, const std::function<bool()>& canceled = {},
                        std::vector<std::string>* order = nullptr) {
    const Tok* count = nullptr;
    std::vector<const Tok*> accessories;
    if (!readArray(vehicle, "accessories", &count, &accessories, error)) return false;
    (void)count;
    *accessoryCount = accessories.size();
    for(const auto* ref:accessories) {
        const UnitInfo* accessory=findById(doc,ref->text);
        if(ref->kind!=TokKind::Atom || !accessory ||
           !isAllowedAccessoryType(accessory->type)) {
            setError(error,"车辆配件数组包含无效引用"); return false;
        }
    }
    std::unordered_map<std::string, int> color;
    struct Frame { std::string id; std::vector<std::string> refs; std::size_t next = 0; };
    std::vector<Frame> work;
    Frame root{vehicle.id, {}};
    if (!collectRefs(doc, vehicle, &root.refs, error)) return false;
    std::size_t edges = root.refs.size();
    work.push_back(std::move(root));
    if (order) order->push_back(vehicle.id);
    color[vehicle.id] = 1;
    std::size_t visited = 1;
    while (!work.empty()) {
        if (canceled && canceled()) { setError(error, "车辆复制已取消"); return false; }
        Frame& frame = work.back();
        if (frame.next >= frame.refs.size()) {
            color[frame.id] = 2;
            work.pop_back();
            continue;
        }
        const std::string ref = frame.refs[frame.next++];
        const auto colorIt = color.find(ref);
        if (colorIt != color.end() && colorIt->second == 1) {
            setError(error, "cycle in truck graph at " + ref);
            return false;
        }
        if (colorIt != color.end() && colorIt->second == 2) continue;
        const UnitInfo* target = findById(doc, ref);
        if (!target) {
            setError(error, "missing referenced unit " + ref);
            return false;
        }
        if (!isAllowedAccessoryType(target->type)) {
            setError(error, "unsupported referenced unit type " + target->type + " at " + ref);
            return false;
        }
        if (visited >= kMaxGraphNodes) {
            setError(error, "truck graph exceeds 1024 units");
            return false;
        }
        ++visited;
        color[ref] = 1;
        Frame child{ref, {}};
        if (!collectRefs(doc, *target, &child.refs, error)) return false;
        if (child.refs.size() > kMaxGraphReferences - edges) {
            setError(error, "truck graph exceeds 65536 references"); return false;
        }
        edges += child.refs.size();
        if (order) order->push_back(ref);
        work.push_back(std::move(child));
    }
    return true;
}

bool collectAllIds(const Doc& doc, std::unordered_set<std::string>* ids) {
    for (const Tok& t : doc.toks) {
        if ((t.kind == TokKind::Atom || t.kind == TokKind::String) && startsNameless(t.text)) {
            ids->insert(idKey(t.text));
        }
    }
    return true;
}

std::string makeFreshId(std::unordered_set<std::string>* reserved, std::uint64_t* counter) {
    for (;;) {
        const std::string id = anonymousId(0x7fff0000ull + (*counter)++);
        if (reserved->insert(id).second) return id;
    }
}

struct Replacement {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::string text;
};

bool applyReplacements(const std::string& base, std::vector<Replacement> reps, std::string* out,
                       std::string* error) {
    std::sort(reps.begin(), reps.end(), [](const Replacement& a, const Replacement& b) {
        if (a.begin != b.begin) return a.begin > b.begin;
        return a.end > b.end;
    });
    std::string result = base;
    std::size_t lastBegin = result.size() + 1;
    for (const Replacement& r : reps) {
        if (r.begin > r.end || r.end > result.size() || r.end > lastBegin) {
            setError(error, "internal replacement overlap");
            return false;
        }
        result.replace(r.begin, r.end - r.begin, r.text);
        lastBegin = r.begin;
    }
    *out = std::move(result);
    return true;
}

bool cloneGraphUnit(const Doc& source, const UnitInfo& unit,
                    const std::unordered_map<std::string, std::string>& mapping,
                    std::string* out, std::string* error) {
    std::vector<Replacement> reps;
    Replacement header;
    header.begin = unit.headerIdBegin - unit.headerBegin;
    header.end = unit.headerIdEnd - unit.headerBegin;
    const auto rootMap = mapping.find(unit.id);
    if (rootMap == mapping.end()) {
        setError(error, "internal mapping error for " + unit.id);
        return false;
    }
    header.text = rootMap->second;
    reps.push_back(header);
    for (const Prop& p : unit.props) {
        for (const Tok& v : p.values) {
            if (v.kind != TokKind::Atom ||
                (!startsNameless(v.text) && !source.byId.count(v.text))) continue;
            const auto it = mapping.find(v.text);
            if (it == mapping.end()) {
                setError(error, "unsupported foreign reference " + v.text + " in " + unit.id);
                return false;
            }
            Replacement r;
            r.begin = v.begin - unit.headerBegin;
            r.end = v.end - unit.headerBegin;
            r.text = it->second;
            reps.push_back(r);
        }
    }
    const std::string original =
        source.text.substr(unit.headerBegin, unit.end - unit.headerBegin);
    return applyReplacements(original, std::move(reps), out, error);
}

}  // namespace

namespace {
using HomeReferences = std::unordered_map<std::string, std::vector<std::string>>;
bool collectHomeReferences(const Doc& doc, HomeReferences* incoming, std::string* error,
                           const std::function<bool()>& canceled) {
    for (const UnitInfo& u : doc.units) {
        if (canceled && canceled()) { setError(error, "配件读取已取消"); return false; }
        for (const Prop& p : u.props) {
            for (const Tok& v : p.values) {
                if (v.kind == TokKind::Atom && doc.byId.count(v.text))
                    (*incoming)[v.text].push_back(u.id);
            }
        }
    }
    return true;
}

bool homeAccessoryShared(const HomeReferences& incoming, const std::string& unitId,
                         const std::string& truckId) {
    std::unordered_set<std::string> visited;
    std::string current = unitId;
    // A shared ancestor also makes its child accessory shared. The vehicle itself
    // is normally referenced by both player and garage, so stop at the owner.
    while (current != truckId && visited.insert(current).second) {
        const auto it = incoming.find(current);
        if (it == incoming.end() || it->second.empty()) return true;
        if (it->second.size() != 1) return true;
        current = it->second.front();
    }
    return current != truckId;
}

bool collectOwnedTrucks(const Doc& doc, const UnitInfo& player,
                        std::vector<const UnitInfo*>* trucks, std::string* error) {
    const Tok* count = nullptr;
    std::vector<const Tok*> refs;
    if (!readArray(player, "trucks", &count, &refs, error)) return false;
    (void)count;
    for (const Tok* ref : refs) {
        if (ref->kind != TokKind::Atom || !startsNameless(ref->text)) {
            setError(error, "自有卡车引用不是单位 id");
            return false;
        }
        const UnitInfo* vehicle = findById(doc, ref->text);
        if (!vehicle || vehicle->type != "vehicle") {
            setError(error, "自有卡车引用没有指向 vehicle：" + ref->text);
            return false;
        }
        trucks->push_back(vehicle);
    }
    return true;
}

bool isEditableHomeAccessory(const Doc& doc, const UnitInfo& vehicle, const UnitInfo& unit,
                             const HomeReferences& incoming,
                             std::string* error, const std::function<bool()>& canceled) {
    if (!isAllowedAccessoryType(unit.type)) {
        setError(error, "所选单位不是可编辑的配件类型");
        return false;
    }
    if (homeAccessoryShared(incoming, unit.id, vehicle.id)) {
        setError(error, "该配件或其父配件被共享，修改可能影响其他车辆，已拒绝");
        return false;
    }
    std::vector<std::string> order;
    std::size_t accessoryCount = 0;
    if (!validateTruckGraph(doc, vehicle, &accessoryCount, error, canceled, &order)) return false;
    for (const std::string& id : order) {
        if (idKey(id) == idKey(unit.id)) return true;
    }
    setError(error, "所选配件不属于该卡车，已拒绝");
    return false;
}
}  // namespace

bool validateHomeAccessoryPath(const std::string& path, std::string* error) {
    if (error) error->clear();
    if (path.empty()) {
        setError(error, "配件路径不能为空");
        return false;
    }
    if (path.size() > 260u) {
        setError(error, "配件路径过长（最多 260 个字符）");
        return false;
    }
    for (unsigned char c : path) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/')) {
            setError(error, "路径只允许英文字母、数字、下划线、连字符、点和正斜杠");
            return false;
        }
    }
    if (path.find('"') != std::string::npos) {
        setError(error, "配件路径不能包含引号");
        return false;
    }
    if (path.find('\\') != std::string::npos) {
        setError(error, "配件路径必须使用正斜杠 /");
        return false;
    }
    if (path.compare(0, 5, "/def/") != 0) {
        setError(error, "配件路径必须以 /def/ 开头");
        return false;
    }
    if (path.size() < 8 || path.compare(path.size() - 4, 4, ".sii") != 0) {
        setError(error, "配件路径必须以 .sii 结尾");
        return false;
    }
    if (path.find("..") != std::string::npos) {
        setError(error, "配件路径不能包含 .. 目录跳转");
        return false;
    }
    if (path.find("//") != std::string::npos) {
        setError(error, "配件路径不能包含连续的 //");
        return false;
    }
    if (path.find("/./") != std::string::npos || path.substr(path.find_last_of('/') + 1) == ".sii") {
        setError(error, "配件路径不能包含空文件名或单点目录");
        return false;
    }
    return true;
}

bool inspectHomeAccessoriesText(const std::string& text, HomeAccessoryInventory* inventory,
                                std::string* error, std::function<bool()> canceled) {
    if (error) error->clear();
    if (inventory) inventory->entries.clear();
    if (!inventory) {
        setError(error, "inventory output is null");
        return false;
    }
    Doc doc;
    if (!loadDoc(text, &doc, error, canceled)) {
        inventory->entries.clear();
        return false;
    }
    const UnitInfo* player = findPlayer(doc, error);
    if (!player) {
        inventory->entries.clear();
        return false;
    }
    std::vector<const UnitInfo*> trucks;
    if (!collectOwnedTrucks(doc, *player, &trucks, error)) {
        inventory->entries.clear();
        return false;
    }
    HomeReferences incoming;
    if (!collectHomeReferences(doc, &incoming, error, canceled)) return false;
    std::vector<HomeAccessoryEntry> entries;
    for (const UnitInfo* vehicle : trucks) {
        std::vector<std::string> order;
        std::size_t accessoryCount = 0;
        if (!validateTruckGraph(doc, *vehicle, &accessoryCount, error, canceled, &order)) {
            inventory->entries.clear();
            return false;
        }
        const std::string label = labelForVehicle(doc, *vehicle);
        for (const std::string& id : order) {
            if (idKey(id) == idKey(vehicle->id)) continue;
            const UnitInfo* unit = findById(doc, id);
            if (!unit) continue;
            const Prop* path = findScalar(*unit, "data_path");
            if (!path || path->values.size() != 1 ||
                (path->values[0].kind != TokKind::String && path->values[0].kind != TokKind::Atom)) continue;
            HomeAccessoryEntry entry;
            entry.truckId = vehicle->id;
            entry.truckLabel = label;
            entry.unitId = unit->id;
            entry.type = unit->type;
            entry.dataPath = path->values[0].text;
            const Prop* subtype = findScalar(*unit, "subtype");
            if (subtype && subtype->values.size() == 1) entry.subtype = subtype->values[0].text;
            entry.shared = homeAccessoryShared(incoming, unit->id, vehicle->id);
            entries.push_back(std::move(entry));
        }
    }
    inventory->entries = std::move(entries);
    return true;
}

bool replaceHomeAccessoryPath(const std::string& text, const std::string& truckId,
                              const std::string& unitId, const std::string& newPath,
                              std::string* out, std::string* error,
                              std::function<bool()> canceled) {
    if (error) error->clear();
    if (out) out->clear();
    if (!out) {
        setError(error, "output is null");
        return false;
    }
    if (!validateHomeAccessoryPath(newPath, error)) return false;
    Doc doc;
    if (!loadDoc(text, &doc, error, canceled)) return false;
    const UnitInfo* player = findPlayer(doc, error);
    if (!player) return false;
    std::vector<const UnitInfo*> trucks;
    if (!collectOwnedTrucks(doc, *player, &trucks, error)) return false;
    const UnitInfo* vehicle = nullptr;
    for (const UnitInfo* v : trucks) {
        if (idKey(v->id) == idKey(truckId)) { vehicle = v; break; }
    }
    if (!vehicle) {
        setError(error, "所选卡车不属于该存档的自有车辆");
        return false;
    }
    const UnitInfo* unit = findById(doc, unitId);
    if (!unit) {
        setError(error, "所选配件不存在于该存档");
        return false;
    }
    HomeReferences incoming;
    if (!collectHomeReferences(doc, &incoming, error, canceled)) return false;
    if (!isEditableHomeAccessory(doc, *vehicle, *unit, incoming, error, canceled)) return false;
    const Prop* path = findScalar(*unit, "data_path");
    if (!path || path->values.size() != 1 ||
        (path->values[0].kind != TokKind::String && path->values[0].kind != TokKind::Atom)) {
        setError(error, "所选配件没有唯一的 data_path 属性");
        return false;
    }
    const Tok& value = path->values[0];
    Replacement rep;
    rep.begin = value.begin;
    rep.end = value.end;
    rep.text = "\"" + newPath + "\"";
    std::vector<Replacement> reps{rep};
    return applyReplacements(text, std::move(reps), out, error);
}

bool installScaniaTvPresetText(const std::string& text,const std::string& truckId,
                               std::string* out,std::string* error,std::function<bool()> canceled) {
    if(out) out->clear();
    if(error) error->clear();
    if(!out) {setError(error,"output is null");return false;}
    Doc doc;
    if(!loadDoc(text,&doc,error,canceled)) return false;
    const UnitInfo* player=findPlayer(doc,error);
    if(!player) return false;
    std::vector<const UnitInfo*> trucks;
    if(!collectOwnedTrucks(doc,*player,&trucks,error)) return false;
    const UnitInfo* vehicle=nullptr;
    for(const auto* truck:trucks) if(idKey(truck->id)==idKey(truckId)) {vehicle=truck;break;}
    if(!vehicle || labelForVehicle(doc,*vehicle)!="scania.s_2016") {
        setError(error,"三屏电视预设仅支持此存档的斯堪尼亚 S 2016 自有卡车");return false;
    }
    std::size_t graphCount=0;
    if(!validateTruckGraph(doc,*vehicle,&graphCount,error,canceled)) return false;
    const UnitInfo* economy=nullptr;
    for(const auto& unit:doc.units) if(unit.type=="economy") {
        if(economy){setError(error,"存档包含多个 economy，无法确认注册位置");return false;}
        economy=&unit;
    }
    const Tok* logRef=nullptr;
    if(!economy || !readScalar(*economy,"bus_job_log",&logRef,error)) {
        setError(error,"存档缺少 economy.bus_job_log，不能注册此实验预设");return false;
    }
    const UnitInfo* log=logRef->kind==TokKind::Atom ? findById(doc,logRef->text) : nullptr;
    const Tok* version=nullptr;
    if(!log || log->type!="bus_job_log" || !readScalar(*log,"version",&version,error) ||
       version->kind!=TokKind::Atom || version->text!="0") {
        setError(error,"巴士日志不是受支持的 version 0，已拒绝导入");return false;
    }
    const Tok* logCount=nullptr;
    std::vector<const Tok*> logEntries;
    if(!readArray(*log,"entries",&logCount,&logEntries,error)) return false;
    for(const auto* entry:logEntries) {
        const UnitInfo* hookup=findById(doc,entry->text);
        if(entry->kind!=TokKind::String || !hookup || hookup->type!="accessory_hookup_data") {
            setError(error,"巴士日志已有任务或未知条目，导入不会覆盖这些数据");return false;
        }
    }
    if(logEntries.size()>=256) {setError(error,"本地 hookup 注册条目过多");return false;}
    const Tok* accessoryCount=nullptr;
    std::vector<const Tok*> accessories;
    if(!readArray(*vehicle,"accessories",&accessoryCount,&accessories,error)) return false;
    if(accessories.size()>100) {setError(error,"卡车配件已超过 100 个");return false;}
    const std::string basePath="/def/vehicle/truck/scania.s_2016/accessory/set_glass/set1.dlc_toys.sii";
    const std::string model="/home/MoRan/scania/TV/model/MoRan_3TV.pmd";
    const UnitInfo* base=nullptr;
    for(const auto* ref:accessories) {
        const auto* unit=findById(doc,ref->text);
        const Prop* path=unit ? findScalar(*unit,"data_path") : nullptr;
        if(unit && unit->type=="vehicle_addon_accessory" && path && path->values.size()==1 &&
           path->values[0].text==basePath) {
            if(base){setError(error,"找到多个相同安装底座，请先整理该卡车配件");return false;}
            base=unit;
        }
    }
    HomeReferences incoming;
    if(!collectHomeReferences(doc,&incoming,error,canceled)) return false;
    if(base && homeAccessoryShared(incoming,base->id,vehicle->id)) {
        setError(error,"电视安装底座被共享，不能修改");return false;
    }
    std::unordered_set<std::string> used;
    for(const auto& unit:doc.units) used.insert(idKey(unit.id));
    std::string hookupId;
    for(std::size_t n=1;n<=doc.units.size()+1;++n) {
        const auto candidate="etzhome.tv"+std::to_string(n);
        if(!used.count(candidate)){hookupId=candidate;break;}
    }
    if(hookupId.empty()){setError(error,"无法分配唯一 hookup 名称");return false;}
    std::vector<Replacement> reps;
    auto replace=[&](const Tok* token,const std::string& value) {
        reps.push_back({token->begin,token->end,value});
    };
    auto appendArray=[&](const std::string& name,const Tok* count,const std::vector<const Tok*>& values,
                         const std::string& value) {
        replace(count,std::to_string(values.size()+1));
        const auto pos=values.empty()?count->end:values.back()->end;
        reps.push_back({pos,pos,"\n "+name+"["+std::to_string(values.size())+"]: "+value});
    };
    std::string appended;
    if(base) {
        const Tok* slotsCount=nullptr;const Tok* hooksCount=nullptr;
        std::vector<const Tok*> slots,hooks;
        if(!readArray(*base,"slot_name",&slotsCount,&slots,error) ||
           !readArray(*base,"slot_hookup",&hooksCount,&hooks,error)) return false;
        if(slots.size()!=hooks.size() || slots.size()>25) {
            setError(error,"底座的挂点数组不匹配或超过 25 项");return false;
        }
        int found=-1;
        for(std::size_t i=0;i<slots.size();++i) if(slots[i]->text=="slot_1") {
            if(found>=0){setError(error,"底座包含重复 slot_1");return false;}
            found=(int)i;
        }
        if(found>=0) {
            const auto* hook=findById(doc,hooks[found]->text);
            const auto* existingModel=hook ? findScalar(*hook,"model") : nullptr;
            if(existingModel && existingModel->values.size()==1 && existingModel->values[0].text==model) {
                setError(error,"所选卡车已经安装这份三屏电视预设，无需重复导入");return false;
            }
            if(!hooks[found]->text.empty()) {setError(error,"slot_1 已被其他配件占用，导入不会替换它");return false;}
            replace(hooks[found],"\""+hookupId+"\"");
        } else {
            if(slots.size()>=25){setError(error,"安装底座已有 25 个挂点");return false;}
            appendArray("slot_name",slotsCount,slots,"slot_1");
            appendArray("slot_hookup",hooksCount,hooks,"\""+hookupId+"\"");
        }
    } else {
        if(accessories.size()>=100){setError(error,"新增底座将超过卡车 100 个配件的上限");return false;}
        std::string baseId;
        for(std::size_t n=1;n<=doc.units.size()+1;++n) {
            const auto candidate="_nameless.7ffd."+std::to_string(n);
            if(!used.count(idKey(candidate))){baseId=candidate;break;}
        }
        if(baseId.empty()){setError(error,"无法分配唯一配件编号");return false;}
        appendArray("accessories",accessoryCount,accessories,baseId);
        appended="\nvehicle_addon_accessory : "+baseId+" {\n slot_name: 1\n slot_name[0]: slot_1\n"
                 " slot_hookup: 1\n slot_hookup[0]: \""+hookupId+"\"\n wear: 0\n data_path: \""+basePath+"\"\n refund: 0\n}\n";
    }
    appendArray("entries",logCount,logEntries,"\""+hookupId+"\"");
    appended+="\naccessory_hookup_data : "+hookupId+" {\n model: \""+model+"\"\n coll: \"\"\n look: MoRan\n variant: MoRan\n}\n";
    const auto insert=doc.toks[doc.outerCloseTok].begin;
    reps.push_back({insert,insert,appended});
    std::string result;
    if(!applyReplacements(text,std::move(reps),&result,error)) return false;
    HomeAccessoryInventory finalInventory;
    if(!inspectHomeAccessoriesText(result,&finalInventory,error,canceled)) return false;
    *out=std::move(result);return true;
}

bool validateScaniaTvPresetInfo(const std::string& info,std::string* error) {
    if(error) error->clear();
    Doc doc;
    if(!loadDoc(info,&doc,error)) return false;
    if(doc.units.size()!=1 || doc.units[0].type!="save_container") {
        setError(error,"存档信息结构无法识别，不能确认所需 DLC");return false;
    }
    const Tok* count=nullptr;std::vector<const Tok*> dependencies;
    if(!readArray(doc.units[0],"dependencies",&count,&dependencies,error)) return false;
    for(const auto* dep:dependencies) {
        if(dep->kind!=TokKind::String) continue;
        // 真实存档写作 "dlc|eut2_toys|DLC - Cabin Accessories" 或 "rdlc|eut2_toys|..."
        // （rdlc/dlc 前缀因 DLC 类型而异，显示名随语言版本变化），旧测试夹具用过裸 "dlc_toys"。
        if(dep->text=="dlc_toys" || dep->text.rfind("dlc|eut2_toys|",0)==0 ||
           dep->text.rfind("rdlc|eut2_toys|",0)==0)
            return true;
    }
    setError(error,"源存档未声明 Cabin Accessories（eut2_toys），请确认已启用该 DLC 后在游戏内重新保存");return false;
}

// =========================== 更换挂车 ===========================

namespace {

// "<font xscale=1.15>GS 56 87</font>|denmark" -> "GS 56 87 · denmark"
std::string cleanLicensePlate(const std::string& raw) {
    std::string text = raw;
    for (;;) {
        const std::size_t open = text.find("<font");
        if (open == std::string::npos) break;
        const std::size_t close = text.find('>', open);
        if (close == std::string::npos) break;
        text.erase(open, close - open + 1);
        const std::size_t end = text.find("</font>", open);
        if (end != std::string::npos) text.erase(end, 7);
    }
    std::string country;
    const std::size_t bar = text.find('|');
    if (bar != std::string::npos) {
        country = text.substr(bar + 1);
        text = text.substr(0, bar);
    }
    auto trim = [](std::string& s) {
        const auto first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) { s.clear(); return; }
        const auto last = s.find_last_not_of(" \t\r\n");
        s = s.substr(first, last - first + 1);
    };
    trim(text);
    trim(country);
    if (text.empty()) return std::string();
    return country.empty() ? text : text + " · " + country;
}

// cargo_mass 来样是 "&469ab000"（IEEE 位十六进制），手工文本可能是十进制。
bool parseFloatToken(const Tok& tok, float* out) {
    if (tok.kind != TokKind::Atom) return false;
    const std::string& s = tok.text;
    if (s.size() == 9 && s[0] == '&') {
        std::uint32_t bits = 0;
        for (int i = 0; i < 8; ++i) {
            const auto c = static_cast<unsigned char>(s[1 + i]);
            const unsigned digit = c >= '0' && c <= '9' ? c - '0'
                                 : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16u;
            if (digit >= 16) return false;
            bits = (bits << 4) | digit;
        }
        std::memcpy(out, &bits, sizeof(float));
        return true;
    }
    char* end = nullptr;
    const float value = std::strtof(s.c_str(), &end);
    if (!end || *end != '\0' || end == s.c_str()) return false;
    *out = value;
    return true;
}

std::string trailerDefLabel(const Doc& doc, const UnitInfo& trailer) {
    const Prop* def = findScalar(trailer, "trailer_definition");
    if (!def || def->values.size() != 1) return trailer.id;
    std::string raw = def->values[0].text;
    if (startsNameless(raw)) {
        const UnitInfo* unit = findById(doc, raw);
        if (!unit || unit->type != "trailer_def") return raw;
        const Prop* source = findScalar(*unit, "source_name");
        if (source && source->values.size() == 1 && !source->values[0].text.empty()) {
            raw = source->values[0].text;
        } else {
            const Prop* name = findScalar(*unit, "trailer");
            if (name && name->values.size() == 1 && !name->values[0].text.empty())
                raw = name->values[0].text;
        }
    }
    if (raw.rfind("trailer_def.", 0) == 0) raw = raw.substr(12);
    return raw.empty() ? trailer.id : raw;
}

// 解析当前耦合：活动 player_vehicles 槽位 + 可选的进行中任务。
struct TrailerCoupling {
    const UnitInfo* player = nullptr;
    const UnitInfo* slot = nullptr;              // assigned_vehicles 指向的会话槽位
    const UnitInfo* truck = nullptr;             // 槽位里的卡车（可为空）
    const UnitInfo* currentTrailer = nullptr;    // 当前连着的挂车（可为空）
    std::string currentTrailerId;
    const UnitInfo* job = nullptr;               // 进行中的 player_job（可为空）
    std::string jobTrailerId;                    // company_trailer（可为空）
};

const UnitInfo* referencedUnit(const Doc& doc, const UnitInfo& unit, const std::string& name,
                               std::string* idOut = nullptr) {
    const Prop* p = findScalar(unit, name);
    if (!p || p->values.size() != 1 || p->values[0].kind != TokKind::Atom ||
        p->values[0].text == "null")
        return nullptr;
    if (idOut) *idOut = p->values[0].text;
    return findById(doc, p->values[0].text);
}

bool resolveTrailerCoupling(const Doc& doc, TrailerCoupling* out, std::string* error) {
    const UnitInfo* player = findPlayer(doc, error);
    if (!player) return false;
    const std::string slotId = [&] {
        const Prop* p = findScalar(*player, "assigned_vehicles");
        return p && p->values.size() == 1 ? p->values[0].text : std::string();
    }();
    if (slotId.empty() || slotId == "null") {
        setError(error, "这份存档没有可用的当前车辆槽位（assigned_vehicles）");
        return false;
    }
    const UnitInfo* slot = findById(doc, slotId);
    if (!slot || slot->type != "player_vehicles") {
        setError(error, "当前车辆槽位指向无效单位：" + slotId);
        return false;
    }
    out->player = player;
    out->slot = slot;
    out->truck = referencedUnit(doc, *slot, "vehicle");
    referencedUnit(doc, *slot, "trailer", &out->currentTrailerId);
    if (!out->currentTrailerId.empty()) {
        out->currentTrailer = findById(doc, out->currentTrailerId);
        if (!out->currentTrailer) {
            setError(error, "当前挂车指向无效单位：" + out->currentTrailerId);
            return false;
        }
    }
    const UnitInfo* job = referencedUnit(doc, *player, "current_job");
    if (job) {
        if (job->type != "player_job") {
            setError(error, "当前任务指向无效单位");
            return false;
        }
        out->job = job;
        referencedUnit(doc, *job, "company_trailer", &out->jobTrailerId);
    }
    return true;
}

void collectArrayRefs(const UnitInfo& unit, const std::string& name,
                      std::unordered_set<std::string>* out) {
    for (const Prop& p : unit.props) {
        if (p.name != name || p.index < 0) continue;
        for (const Tok& v : p.values)
            if (v.kind == TokKind::Atom && v.text != "null") out->insert(idKey(v.text));
    }
}

}  // namespace

bool inspectTrailerSwapText(const std::string& text, TrailerSwapInventory* inventory,
                            std::string* error, std::function<bool()> canceled) {
    if (error) error->clear();
    if (inventory) *inventory = TrailerSwapInventory{};
    if (!inventory) {
        setError(error, "inventory output is null");
        return false;
    }
    Doc doc;
    if (!loadDoc(text, &doc, error, canceled)) return false;
    TrailerCoupling coupling;
    if (!resolveTrailerCoupling(doc, &coupling, error)) return false;

    TrailerSwapInventory result;
    result.hasCurrent = !coupling.currentTrailerId.empty();
    result.onJob = coupling.job != nullptr;
    result.truckLabel = coupling.truck ? labelForVehicle(doc, *coupling.truck)
                                       : std::string("未知卡车");
    if (coupling.currentTrailer) {
        result.currentId = coupling.currentTrailer->id;
        result.currentLabel = trailerDefLabel(doc, *coupling.currentTrailer);
        const Prop* plate = findScalar(*coupling.currentTrailer, "license_plate");
        if (plate && plate->values.size() == 1)
            result.currentPlate = cleanLicensePlate(plate->values[0].text);
    }
    if (coupling.job) {
        const Prop* cargo = findScalar(*coupling.job, "cargo");
        if (cargo && cargo->values.size() == 1 && !cargo->values[0].text.empty())
            result.jobCargo = cargo->values[0].text;
        const Prop* online = findScalar(*coupling.job, "online_job_id");
        result.onlineJob = online && online->values.size() == 1 &&
                           !online->values[0].text.empty() &&
                           online->values[0].text.find_first_not_of("0") !=
                               std::string::npos;
    }

    std::unordered_set<std::string> owned, driverAssigned, otherJobTrailer, slaveIds;
    collectArrayRefs(*coupling.player, "trailers", &owned);
    for (const UnitInfo& u : doc.units) {
        if (canceled && canceled()) {
            setError(error, "挂车读取已取消");
            return false;
        }
        if (u.type == "driver") {
            const std::string id = [&] {
                const Prop* p = findScalar(u, "assigned_trailer");
                return p && p->values.size() == 1 && p->values[0].text != "null"
                           ? p->values[0].text : std::string();
            }();
            if (!id.empty()) driverAssigned.insert(idKey(id));
        } else if (u.type == "player_job" && coupling.job && &u != coupling.job) {
            const std::string id = [&] {
                const Prop* p = findScalar(u, "company_trailer");
                return p && p->values.size() == 1 && p->values[0].text != "null"
                           ? p->values[0].text : std::string();
            }();
            if (!id.empty()) otherJobTrailer.insert(idKey(id));
        } else if (u.type == "trailer") {
            const std::string slave = [&] {
                const Prop* p = findScalar(u, "slave_trailer");
                return p && p->values.size() == 1 && p->values[0].text != "null"
                           ? p->values[0].text : std::string();
            }();
            if (!slave.empty()) slaveIds.insert(idKey(slave));
        }
    }

    bool currentIsDouble = false;
    for (const UnitInfo& u : doc.units) {
        if (u.type != "trailer") continue;
        TrailerSwapEntry entry;
        entry.id = u.id;
        entry.label = trailerDefLabel(doc, u);
        entry.owned = owned.count(idKey(u.id)) > 0;
        entry.current = coupling.currentTrailer &&
                        idKey(u.id) == idKey(coupling.currentTrailer->id);
        entry.job = coupling.job && !coupling.jobTrailerId.empty() &&
                    idKey(u.id) == idKey(coupling.jobTrailerId);
        const Prop* plate = findScalar(u, "license_plate");
        if (plate && plate->values.size() == 1)
            entry.plate = cleanLicensePlate(plate->values[0].text);
        const Prop* cargoMass = findScalar(u, "cargo_mass");
        float mass = 0.f;
        if (cargoMass && cargoMass->values.size() == 1)
            parseFloatToken(cargoMass->values[0], &mass);
        entry.cargoMass = mass;
        const std::string slave = [&] {
            const Prop* p = findScalar(u, "slave_trailer");
            return p && p->values.size() == 1 && p->values[0].text != "null"
                       ? p->values[0].text : std::string();
        }();
        const bool isSlave = slaveIds.count(idKey(u.id)) > 0;
        if (!slave.empty() || isSlave) {
            entry.blocked = true;
            entry.blockReason = "双挂车组（带副车），暂不支持整体切换";
            if (entry.current) currentIsDouble = true;
        } else if (driverAssigned.count(idKey(u.id))) {
            entry.blocked = true;
            entry.blockReason = "AI 司机正在使用这辆挂车";
        } else if (otherJobTrailer.count(idKey(u.id))) {
            entry.blocked = true;
            entry.blockReason = "被存档里的另一个任务占用";
        } else {
            const Prop* def = findScalar(u, "trailer_definition");
            if (def && def->values.size() == 1 && startsNameless(def->values[0].text) &&
                !findById(doc, def->values[0].text)) {
                entry.blocked = true;
                entry.blockReason = "挂车定义缺失（可能来自未启用的模组）";
            }
        }
        result.entries.push_back(entry);
    }
    std::stable_sort(result.entries.begin(), result.entries.end(),
                     [](const TrailerSwapEntry& a, const TrailerSwapEntry& b) {
                         if (a.current != b.current) return a.current;
                         if (a.owned != b.owned) return a.owned;
                         return false;
                     });
    if (!result.hasCurrent) {
        result.swapBlockReason =
            "这份存档里卡车当前没有连接挂车。请在游戏里连上挂车后保存，再选这份存档。";
    } else if (currentIsDouble) {
        result.swapBlockReason = "当前是双挂车组（带副车），暂不支持整体切换";
    }
    *inventory = std::move(result);
    return true;
}

bool swapTrailerText(const std::string& text, const std::string& targetTrailerId,
                     std::string* out, std::string* error, std::function<bool()> canceled) {
    if (error) error->clear();
    if (out) out->clear();
    if (!out) {
        setError(error, "output is null");
        return false;
    }
    Doc doc;
    if (!loadDoc(text, &doc, error, canceled)) return false;
    TrailerCoupling coupling;
    if (!resolveTrailerCoupling(doc, &coupling, error)) return false;
    if (coupling.currentTrailerId.empty() || !coupling.currentTrailer) {
        setError(error, "这份存档里卡车当前没有连接挂车，无法更换");
        return false;
    }
    const UnitInfo& current = *coupling.currentTrailer;
    const UnitInfo* target = findById(doc, targetTrailerId);
    if (!target || target->type != "trailer") {
        setError(error, "目标挂车不存在：" + targetTrailerId);
        return false;
    }
    if (idKey(target->id) == idKey(current.id)) {
        setError(error, "所选挂车就是当前挂车，无需更换");
        return false;
    }
    auto slaveOf = [](const UnitInfo& unit) -> std::string {
        const Prop* p = findScalar(unit, "slave_trailer");
        return p && p->values.size() == 1 && p->values[0].text != "null" ? p->values[0].text
                                                                         : std::string();
    };
    if (!slaveOf(current).empty()) {
        setError(error, "当前是双挂车组（带副车），暂不支持整体切换");
        return false;
    }
    if (!slaveOf(*target).empty()) {
        setError(error, "目标是双挂车组（带副车），暂不支持整体切换");
        return false;
    }
    const Prop* targetDef = findScalar(*target, "trailer_definition");
    if (targetDef && targetDef->values.size() == 1 &&
        startsNameless(targetDef->values[0].text) &&
        !findById(doc, targetDef->values[0].text)) {
        setError(error, "目标挂车的定义缺失（可能来自未启用的模组），已拒绝");
        return false;
    }
    std::unordered_set<std::string> slaveIds;
    for (const UnitInfo& u : doc.units) {
        if (u.type != "trailer") continue;
        const std::string slave = slaveOf(u);
        if (!slave.empty()) slaveIds.insert(idKey(slave));
    }
    if (slaveIds.count(idKey(target->id))) {
        setError(error, "目标是双挂车组的副车，暂不支持整体切换");
        return false;
    }
    for (const UnitInfo& u : doc.units) {
        if (canceled && canceled()) {
            setError(error, "更换挂车已取消");
            return false;
        }
        if (u.type == "driver") {
            const std::string id = [&] {
                const Prop* p = findScalar(u, "assigned_trailer");
                return p && p->values.size() == 1 && p->values[0].text != "null"
                           ? p->values[0].text : std::string();
            }();
            if (!id.empty() && idKey(id) == idKey(target->id)) {
                setError(error, "目标挂车正被 AI 司机使用，请先在游戏里换掉");
                return false;
            }
        } else if (u.type == "player_job" && coupling.job && &u != coupling.job) {
            const std::string id = [&] {
                const Prop* p = findScalar(u, "company_trailer");
                return p && p->values.size() == 1 && p->values[0].text != "null"
                           ? p->values[0].text : std::string();
            }();
            if (!id.empty() && idKey(id) == idKey(target->id)) {
                setError(error, "目标挂车被存档里的另一个任务占用，已拒绝");
                return false;
            }
        }
    }

    std::vector<Replacement> reps;
    const Prop* slotTrailer = findScalar(*coupling.slot, "trailer");
    if (!slotTrailer || slotTrailer->values.size() != 1) {
        setError(error, "当前槽位的挂车字段异常");
        return false;
    }
    reps.push_back({slotTrailer->values[0].begin, slotTrailer->values[0].end, target->id});
    if (coupling.job && !coupling.jobTrailerId.empty() &&
        idKey(coupling.jobTrailerId) == idKey(current.id)) {
        const Prop* p = findScalar(*coupling.job, "company_trailer");
        if (p && p->values.size() == 1)
            reps.push_back({p->values[0].begin, p->values[0].end, target->id});
    }
    // 货物随任务走：把旧挂车上的货物位原样搬到新挂车（无任务时两者通常都是零）。
    for (const char* field : {"cargo_mass", "cargo_damage"}) {
        const Prop* from = findScalar(current, field);
        const Prop* to = findScalar(*target, field);
        if (!from || !to || from->values.size() != 1 || to->values.size() != 1) continue;
        reps.push_back({to->values[0].begin, to->values[0].end, from->values[0].text});
    }
    // 目标若还停在别的车库槽位上，先腾出那个槽（列表本身不动，挂车位保留）。
    for (const UnitInfo& u : doc.units) {
        if (u.type != "player_vehicles" || &u == coupling.slot) continue;
        const Prop* p = findScalar(u, "trailer");
        if (!p || p->values.size() != 1 || p->values[0].kind != TokKind::Atom) continue;
        if (idKey(p->values[0].text) != idKey(target->id)) continue;
        reps.push_back({p->values[0].begin, p->values[0].end, std::string("null")});
        const Prop* attached = findScalar(u, "stored_trailer_attached");
        if (attached && attached->values.size() == 1)
            reps.push_back({attached->values[0].begin, attached->values[0].end,
                            std::string("false")});
    }
    return applyReplacements(text, std::move(reps), out, error);
}

bool inspectTruckTransferText(const std::string& text, TransferInventory* inventory,
                              std::string* error, std::function<bool()> canceled) {
    if (error) error->clear();
    if (inventory) inventory->trucks.clear(), inventory->garages.clear();
    if (!inventory) {
        setError(error, "inventory output is null");
        return false;
    }
    Doc doc;
    if (!loadDoc(text, &doc, error, canceled)) {
        inventory->trucks.clear();
        inventory->garages.clear();
        return false;
    }
    const UnitInfo* player = findPlayer(doc, error);
    if (!player) {
        inventory->trucks.clear();
        inventory->garages.clear();
        return false;
    }
    const Tok* trucksCount = nullptr;
    std::vector<const Tok*> trucks;
    if (!readArray(*player, "trucks", &trucksCount, &trucks, error)) {
        inventory->trucks.clear();
        inventory->garages.clear();
        return false;
    }
    (void)trucksCount;
    std::vector<TransferTruck> foundTrucks;
    foundTrucks.reserve(trucks.size());
    for (const Tok* ref : trucks) {
        if (ref->kind != TokKind::Atom || !startsNameless(ref->text)) {
            setError(error, "player truck reference is not a unit id");
            inventory->trucks.clear();
            inventory->garages.clear();
            return false;
        }
        const UnitInfo* vehicle = findById(doc, ref->text);
        if (!vehicle || vehicle->type != "vehicle") {
            setError(error, "player truck reference does not resolve to a vehicle: " + ref->text);
            inventory->trucks.clear();
            inventory->garages.clear();
            return false;
        }
        std::size_t accessoryCount = 0;
        if (!validateTruckGraph(doc, *vehicle, &accessoryCount, error, canceled)) {
            inventory->trucks.clear();
            inventory->garages.clear();
            return false;
        }
        TransferTruck truck;
        truck.id = vehicle->id;
        truck.accessories = accessoryCount;
        truck.label = labelForVehicle(doc, *vehicle);
        foundTrucks.push_back(std::move(truck));
    }
    std::vector<TransferGarage> foundGarages;
    for (const UnitInfo& u : doc.units) {
        if (u.type != "garage") continue;
        const Prop* status = findScalar(u, "status");
        const Prop* vehicles = findScalar(u, "vehicles");
        if (!status || !vehicles || status->values.size() != 1 || vehicles->values.size() != 1) {
            continue;
        }
        int statusValue = 0;
        if (!parseIndexToken(status->values[0].text, &statusValue) || statusValue <= 0) continue;
        std::size_t freeSlots = 0;
        const Tok* count = nullptr;
        std::vector<const Tok*> slots, drivers;
        std::string ignored;
        if (!readArray(u, "vehicles", &count, &slots, &ignored) ||
            !readArray(u, "drivers", &count, &drivers, &ignored) || slots.size()!=drivers.size()) continue;
        for (std::size_t i=0;i<slots.size();++i)
            if (slots[i]->kind==TokKind::Atom && slots[i]->text=="null" &&
                drivers[i]->kind==TokKind::Atom && drivers[i]->text=="null") ++freeSlots;
        if (!freeSlots) continue;
        TransferGarage garage;
        garage.id = u.id;
        garage.freeSlots = freeSlots;
        foundGarages.push_back(std::move(garage));
    }
    inventory->trucks = std::move(foundTrucks);
    inventory->garages = std::move(foundGarages);
    return true;
}

bool validateTruckTransferMetadata(const std::string& source, const std::string& target,
                                   std::string* error) {
    if(error) error->clear();
    Doc src,dst;
    if(!loadDoc(source,&src,error) || !loadDoc(target,&dst,error)) return false;
    if(src.units.size()!=1 || dst.units.size()!=1 || src.units[0].type!="save_container" ||
       dst.units[0].type!="save_container") {setError(error,"存档元信息结构不受支持");return false;}
    for(const char* field:{"version","info_version"}) {
        const Tok *a=nullptr,*b=nullptr;
        if(!readScalar(src.units[0],field,&a,error) || !readScalar(dst.units[0],field,&b,error)) return false;
        std::size_t va=0,vb=0;
        if(a->kind!=TokKind::Atom || b->kind!=TokKind::Atom || !parseCountToken(a->text,&va) ||
           !parseCountToken(b->text,&vb) || va!=vb) {
            setError(error,"源与目标存档版本不一致（"+std::string(field)+"：源 "+a->text+"，目标 "+b->text+"）");return false;
        }
    }
    const Tok* count=nullptr;
    std::vector<const Tok*> a,b;
    if(!readArray(src.units[0],"dependencies",&count,&a,error) ||
       !readArray(dst.units[0],"dependencies",&count,&b,error)) return false;
    std::unordered_set<std::string> dependencies;
    for(const auto* dep:b) {
        if(dep->kind!=TokKind::String) {setError(error,"目标依赖列表格式无效");return false;}
        dependencies.insert(dep->text);
    }
    for(const auto* dep:a) if(dep->kind!=TokKind::String || !dependencies.count(dep->text)) {
        setError(error,"目标存档缺少源存档的 DLC／模组依赖："+dep->text+
                       "；请先在目标档案启用相同依赖并重新保存");return false;
    }
    return true;
}

bool transferTruckText(const std::string& source, const std::string& target,
                       const std::string& truckId, const std::string& garageId,
                       std::string* out, std::string* error, std::function<bool()> canceled) {
    if (error) error->clear();
    if (out) out->clear();
    auto fail = [&](const std::string& message) {
        if (out) out->clear();
        setError(error, message);
        return false;
    };
    if (source.size() > kMaxText || target.size() > kMaxText) {
        return fail("SII text exceeds the 128 MiB limit");
    }
    Doc src;
    Doc dst;
    if (!loadDoc(source, &src, error, canceled)) return fail(error ? *error : std::string("source parse failed"));
    if (!loadDoc(target, &dst, error, canceled)) return fail(error ? *error : std::string("target parse failed"));

    const UnitInfo* sourcePlayer = findPlayer(src, error);
    const Tok* sourceCount = nullptr;
    std::vector<const Tok*> owned;
    if (!sourcePlayer || !readArray(*sourcePlayer, "trucks", &sourceCount, &owned, error))
        return fail("源存档的玩家车辆列表无效");
    std::size_t ownedMatches=0;
    for (const auto* ref:owned) if(ref->kind==TokKind::Atom && ref->text==truckId) ++ownedMatches;
    if (ownedMatches!=1) return fail("只能复制源玩家拥有且唯一登记的卡车");

    const UnitInfo* sourceVehicle = findById(src, truckId);
    if (!sourceVehicle || sourceVehicle->type != "vehicle") {
        return fail("truck id does not resolve to a vehicle unit");
    }
    std::size_t accessoryCount = 0;
    std::vector<std::string> order;
    if (!validateTruckGraph(src, *sourceVehicle, &accessoryCount, error, canceled, &order)) {
        return fail(error ? *error : std::string("source truck graph is invalid"));
    }
    (void)accessoryCount;

    const UnitInfo* player = findPlayer(dst, error);
    if (!player) return fail(error ? *error : std::string("target player unit is invalid"));

    const Tok* targetTruckCountTok = nullptr;
    std::vector<const Tok*> targetTrucks;
    if (!readArray(*player, "trucks", &targetTruckCountTok, &targetTrucks, error)) {
        return fail(error ? *error : std::string("target trucks array is invalid"));
    }
    const Tok* targetLogCountTok = nullptr;
    std::vector<const Tok*> targetLogs;
    if (!readArray(*player, "truck_profit_logs", &targetLogCountTok, &targetLogs, error)) {
        return fail(error ? *error : std::string("target truck_profit_logs array is invalid"));
    }
    if (targetTrucks.size() != targetLogs.size()) {
        return fail("target trucks and truck_profit_logs counts differ");
    }
    std::unordered_set<std::string> uniqueTrucks, uniqueLogs;
    for (std::size_t i = 0; i < targetTrucks.size(); ++i) {
        const Tok* ref = targetTrucks[i];
        if (ref->kind != TokKind::Atom || !startsNameless(ref->text)) {
            return fail("target player truck reference is not a unit id");
        }
        if (!uniqueTrucks.insert(ref->text).second) return fail("目标车辆列表含重复车辆引用");
        const UnitInfo* vehicle = findById(dst, ref->text);
        if (!vehicle || vehicle->type != "vehicle") {
            return fail("target player truck reference does not resolve to a vehicle");
        }
        const Tok* log = targetLogs[i];
        if (!uniqueLogs.insert(log->text).second) return fail("目标收益列表含重复引用");
        if (log->kind != TokKind::Atom || !startsNameless(log->text)) {
            return fail("target profit log reference is not a unit id");
        }
        const UnitInfo* logUnit = findById(dst, log->text);
        if (!logUnit || logUnit->type != "profit_log") {
            return fail("target profit log reference does not resolve to profit_log");
        }
    }

    const UnitInfo* garage = findById(dst, garageId);
    if (!garage || garage->type != "garage") return fail("target garage was not found");
    if (!readStatusPositive(*garage, error)) {
        return fail(error ? *error : std::string("target garage is not purchased"));
    }
    const Tok* garageVehiclesCount = nullptr;
    std::vector<const Tok*> garageVehicles;
    if (!readArray(*garage, "vehicles", &garageVehiclesCount, &garageVehicles, error)) {
        return fail(error ? *error : std::string("target garage vehicles array is invalid"));
    }
    const Tok* garageDriversCount = nullptr;
    std::vector<const Tok*> garageDrivers;
    if (!readArray(*garage, "drivers", &garageDriversCount, &garageDrivers, error)) {
        return fail(error ? *error : std::string("target garage drivers array is invalid"));
    }
    (void)garageVehiclesCount;
    (void)garageDriversCount;
    if (garageVehicles.size() != garageDrivers.size()) {
        return fail("target garage vehicles and drivers counts differ");
    }
    std::size_t freeIndex = garageVehicles.size();
    for (std::size_t i = 0; i < garageVehicles.size(); ++i) {
        if (garageVehicles[i]->kind == TokKind::Atom && garageVehicles[i]->text == "null" &&
            garageDrivers[i]->kind == TokKind::Atom && garageDrivers[i]->text == "null") {
            freeIndex = i;
            break;
        }
    }
    if (freeIndex >= garageVehicles.size()) return fail("target garage has no free slot");

    std::unordered_set<std::string> reserved;
    collectAllIds(src, &reserved);
    collectAllIds(dst, &reserved);
    std::uint64_t counter = 1;
    std::unordered_map<std::string, std::string> mapping;
    for (const auto& id : order) mapping[id] = makeFreshId(&reserved, &counter);

    std::string appended;
    for (const std::string& id : order) {
        if (canceled && canceled()) return fail("车辆复制已取消");
        const UnitInfo* unit = findById(src, id);
        if (!unit) return fail("internal missing cloned unit " + id);
        std::string cloned;
        if (!cloneGraphUnit(src, *unit, mapping, &cloned, error)) {
            return fail(error ? *error : std::string("failed to clone truck graph unit"));
        }
        appended += "\n";
        appended += cloned;
    }
    const std::string newLogId = makeFreshId(&reserved, &counter);
    appended += "\n\nprofit_log : ";
    appended += newLogId;
    appended += " {\n stats_data: 0\n acc_distance_free: 0\n acc_distance_on_job: 0\n history_age: nil\n}\n";

    const std::string rootId = mapping[truckId];
    std::vector<Replacement> reps;
    Replacement truckCount;
    truckCount.begin = targetTruckCountTok->begin;
    truckCount.end = targetTruckCountTok->end;
    truckCount.text = std::to_string(targetTrucks.size() + 1);
    if (targetTrucks.empty()) truckCount.text += "\n trucks[0]: " + rootId;
    reps.push_back(truckCount);
    Replacement logCount;
    logCount.begin = targetLogCountTok->begin;
    logCount.end = targetLogCountTok->end;
    logCount.text = std::to_string(targetLogs.size() + 1);
    if (targetLogs.empty()) logCount.text += "\n truck_profit_logs[0]: " + newLogId;
    reps.push_back(logCount);
    if (!targetTrucks.empty()) {
    const Tok* lastTruck = targetTrucks.back();
    Replacement appendTruck;
    appendTruck.begin = lastTruck->end;
    appendTruck.end = lastTruck->end;
    appendTruck.text = "\ntrucks[" + std::to_string(targetTrucks.size()) + "]: " + rootId;
    reps.push_back(appendTruck);
    const Tok* lastLog = targetLogs.back();
    Replacement appendLog;
    appendLog.begin = lastLog->end;
    appendLog.end = lastLog->end;
    appendLog.text = "\ntruck_profit_logs[" + std::to_string(targetLogs.size()) + "]: " + newLogId;
    reps.push_back(appendLog);
    }
    Replacement garageSlot;
    garageSlot.begin = garageVehicles[freeIndex]->begin;
    garageSlot.end = garageVehicles[freeIndex]->end;
    garageSlot.text = rootId;
    reps.push_back(garageSlot);
    Replacement insertUnits;
    insertUnits.begin = dst.toks[dst.outerCloseTok].begin;
    insertUnits.end = dst.toks[dst.outerCloseTok].begin;
    insertUnits.text = appended;
    reps.push_back(insertUnits);

    std::string result;
    if (!applyReplacements(dst.text, std::move(reps), &result, error)) {
        return fail(error ? *error : std::string("failed to build target document"));
    }

    Doc verify;
    if (!loadDoc(result, &verify, error, canceled)) return fail("final validation failed to parse output");
    const UnitInfo* verifyPlayer = findPlayer(verify, error);
    if (!verifyPlayer) return fail("final validation lost the player unit");
    const Tok* verifyTruckCount = nullptr;
    std::vector<const Tok*> verifyTrucks;
    if (!readArray(*verifyPlayer, "trucks", &verifyTruckCount, &verifyTrucks, error)) {
        return fail("final validation found an invalid trucks array");
    }
    const Tok* verifyLogCount = nullptr;
    std::vector<const Tok*> verifyLogs;
    if (!readArray(*verifyPlayer, "truck_profit_logs", &verifyLogCount, &verifyLogs, error)) {
        return fail("final validation found an invalid truck_profit_logs array");
    }
    (void)verifyTruckCount;
    (void)verifyLogCount;
    if (verifyTrucks.size() != targetTrucks.size() + 1 ||
        verifyLogs.size() != targetLogs.size() + 1) {
        return fail("final validation counts are not incremented");
    }
    if (verifyTrucks.back()->text != rootId) return fail("final validation lost the new root truck");
    const UnitInfo* verifyGarage = findById(verify, garageId);
    if (!verifyGarage) return fail("final validation lost the target garage");
    const Tok* vgCount = nullptr;
    std::vector<const Tok*> vgVehicles;
    if (!readArray(*verifyGarage, "vehicles", &vgCount, &vgVehicles, error)) {
        return fail("final validation found an invalid garage vehicles array");
    }
    (void)vgCount;
    if (freeIndex >= vgVehicles.size() || vgVehicles[freeIndex]->text != rootId) {
        return fail("final validation did not place the truck in the garage slot");
    }
    for (const std::string& id : order) {
        const UnitInfo* cloned = findById(verify, mapping[id]);
        if (!cloned) return fail("final validation lost cloned unit " + mapping[id]);
        std::vector<std::string> refs;
        if (!collectRefs(verify, *cloned, &refs, error)) return fail(error ? *error : "invalid references");
        for (const std::string& ref : refs) {
            if (!findById(verify, ref)) return fail("final validation has a missing reference " + ref);
        }
    }
    if (out) *out = std::move(result);
    return true;
}

#include "fleet_impl.inc"

}  // namespace ets2
