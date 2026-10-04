#include "trucktransfer.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
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

}  // namespace ets2
