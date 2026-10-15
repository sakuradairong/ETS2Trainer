// Compiled only by trucktransfer.cpp: shares its bounded SII parser and truck graph validator.
namespace {
void fleetNeed(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
const Tok* fleetValue(const UnitInfo& u, const std::string& key) {
    const Tok* value = nullptr; std::string error;
    if (!readScalar(u, key, &value, &error)) throw std::runtime_error(error);
    return value;
}
int fleetNumber(const UnitInfo& u, const std::string& key) {
    const Tok* t = fleetValue(u, key); int value = 0;
    fleetNeed(t->kind == TokKind::Atom && parseIndexToken(t->text, &value),
              "无效的车队数值：" + u.id + "." + key);
    return value;
}
std::vector<const Tok*> fleetArray(const UnitInfo& u, const std::string& key) {
    const Tok* count = nullptr; std::vector<const Tok*> values; std::string error;
    if (!readArray(u, key, &count, &values, &error)) throw std::runtime_error(error);
    return values;
}
const UnitInfo& fleetUnit(const Doc& d, const std::string& id, const std::string& type) {
    const UnitInfo* u = findById(d, id);
    fleetNeed(u && u->type == type, "缺失或不支持的车队对象：" + id + " / " + type);
    return *u;
}
std::vector<std::string> fleetTexts(const std::vector<const Tok*>& values) {
    std::vector<std::string> result;
    for (const auto* v : values) {
        fleetNeed(v->kind == TokKind::Atom, "车队数组引用必须是对象标识");
        result.push_back(v->text);
    }
    return result;
}
bool fleetNull(const Tok* t) { return t->kind == TokKind::Atom && t->text == "null"; }
bool fleetEmpty(const Tok* t) { return t->kind == TokKind::String && t->text.empty(); }
void fleetCancel(const std::function<bool()>& canceled) {
    fleetNeed(!canceled || !canceled(), "车队操作已取消");
}
std::string fleetSnapshot(const std::string& text) {
    // Change detector for preview/apply, not an authentication primitive.
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : text) { hash ^= c; hash *= 1099511628211ull; }
    char hex[48]; std::snprintf(hex, sizeof(hex), "%016llx:%llu",
        static_cast<unsigned long long>(hash), static_cast<unsigned long long>(text.size()));
    return hex;
}
struct FleetView {
    const UnitInfo* economy = nullptr;
    const UnitInfo* player = nullptr;
    const UnitInfo* queue = nullptr;
    FleetInventory inventory;
    std::vector<const Tok*> pool, offers, events;
    std::unordered_map<std::string, std::vector<std::size_t>> freeSlots;
    std::unordered_map<std::string, int> eventTimes;
    int gameTime = 0;
    struct Cache { int axles = 0; float mass = 0, efficiency = 0; std::string massText, efficiencyText; };
    std::unordered_map<std::string, Cache> resolvedCaches;
};

std::int64_t fleetAdd(std::int64_t a, std::int64_t b) {
    const auto lo = (std::numeric_limits<std::int64_t>::min)();
    const auto hi = (std::numeric_limits<std::int64_t>::max)();
    fleetNeed(!((b > 0 && a > hi - b) || (b < 0 && a < lo - b)), "收益记录数值溢出");
    return a + b;
}
std::int64_t fleetMoney(const UnitInfo& u, const char* key) {
    const auto* t = fleetValue(u, key); std::size_t used = 0;
    fleetNeed(t->kind == TokKind::Atom && !t->text.empty() &&
              t->text.find_first_not_of("-0123456789") == std::string::npos, "无效的收益数值");
    const auto n = std::stoll(t->text, &used);
    fleetNeed(used == t->text.size(), "无效的收益数值"); return n;
}
void fleetSum(FleetProfit& total, const FleetProfit& p) {
    total.revenue = fleetAdd(total.revenue, p.revenue);
    total.cost = fleetAdd(total.cost, p.cost); total.net = fleetAdd(total.net, p.net);
    fleetNeed(p.entries <= (std::numeric_limits<std::size_t>::max)() - total.entries, "收益记录数量溢出");
    total.entries += p.entries;
}
FleetProfit fleetProfit(const Doc& doc, const UnitInfo& driver, std::unordered_set<std::string>& ids,
                        const std::function<bool()>& canceled) {
    FleetProfit result;
    const auto& log = fleetUnit(doc, fleetValue(driver, "profit_log")->text, "profit_log");
    for (const auto* ref : fleetArray(log, "stats_data")) {
        fleetCancel(canceled);
        fleetNeed(ref->kind == TokKind::Atom && ids.insert(ref->text).second, "收益记录包含重复或无效引用");
        const auto& entry = fleetUnit(doc, ref->text, "profit_log_entry");
        FleetProfit p; p.entries = 1; p.revenue = fleetMoney(entry, "revenue");
        for (const auto* key : {"wage", "maintenance", "fuel"}) p.cost = fleetAdd(p.cost, fleetMoney(entry, key));
        // Subtract without negating INT64_MIN.
        fleetNeed(p.cost != (std::numeric_limits<std::int64_t>::min)(), "收益成本超出支持范围");
        p.net = fleetAdd(p.revenue, -p.cost); fleetSum(result, p);
    }
    return result;
}
FleetView::Cache fleetCache(const UnitInfo& driver) {
    FleetView::Cache c; c.axles = fleetNumber(driver, "slot_truck_axle_count");
    const auto* mass = fleetValue(driver, "slot_truck_mass");
    const auto* efficiency = fleetValue(driver, "slot_truck_efficiency");
    fleetNeed(parseFloatToken(*mass, &c.mass) && parseFloatToken(*efficiency, &c.efficiency) &&
              std::isfinite(c.mass) && std::isfinite(c.efficiency) && c.mass > 0 && c.efficiency > 0 &&
              c.axles >= 2 && c.axles <= 10, "司机车辆模拟参数无效：" + driver.id);
    c.massText = mass->text; c.efficiencyText = efficiency->text; return c;
}
bool fleetSameCache(const FleetView::Cache& a, const FleetView::Cache& b) {
    return a.axles == b.axles && a.mass == b.mass && a.efficiency == b.efficiency;
}
bool fleetDefaultCache(const FleetView::Cache& c) { return c.axles == 2 && c.mass == 10000 && c.efficiency == 1; }

// Compare complete accessory graphs, including custom values and hookup order, after
// normalizing object IDs. Refunds are historical purchase prices. Root driving/camera,
// wear and trip counters are not configuration; passenger mass and axle offset are.
std::string fleetConfiguration(const Doc& doc, const UnitInfo& truck,
                               const std::function<bool()>& canceled) {
    std::vector<std::string> graph; std::size_t count = 0; std::string error;
    fleetNeed(validateTruckGraph(doc, truck, &count, &error, canceled, &graph), error);
    std::unordered_map<std::string, std::string> local;
    for (std::size_t i = 0; i < graph.size(); ++i) local[graph[i]] = "@" + std::to_string(i);
    std::string signature;
    auto field = [&](const std::string& s) { signature += std::to_string(s.size()) + ":" + s; };
    for (std::size_t i = 0; i < graph.size(); ++i) {
        fleetCancel(canceled); const auto& u = *findById(doc, graph[i]); field(u.type);
        std::vector<std::string> properties;
        for (const auto& p : u.props) {
            if (p.name == "refund") continue;
            if (i == 0 && p.name != "accessories" && p.name != "passenger_mass" && p.name != "sliding_axle_offset") continue;
            std::string item = p.name + "[" + std::to_string(p.index) + "]";
            for (const auto& v : p.values) {
                const auto ref = local.find(v.text);
                const std::string value = v.kind == TokKind::Atom && ref != local.end() ? ref->second : v.text;
                item += ":" + std::to_string(static_cast<int>(v.kind)) + ":" + std::to_string(value.size()) + ":" + value;
            }
            properties.push_back(std::move(item));
        }
        std::sort(properties.begin(), properties.end()); field(std::to_string(properties.size()));
        for (const auto& p : properties) field(p);
        fleetNeed(signature.size() <= kMaxText, "车辆配置签名超过上限");
    }
    return signature;
}

FleetView readFleet(const Doc& doc, const std::function<bool()>& canceled) {
    FleetView view; std::string error;
    for (const auto& u : doc.units) if (u.type == "economy") {
        fleetNeed(!view.economy, "不支持多个经济系统对象"); view.economy = &u;
    }
    fleetNeed(view.economy != nullptr, "没有找到经济系统对象");
    view.player = findPlayer(doc, &error); fleetNeed(view.player != nullptr, error);
    fleetNeed(fleetValue(*view.economy, "player")->text == view.player->id, "经济系统与玩家引用不一致");
    view.gameTime = fleetNumber(*view.economy, "game_time");
    fleetNeed(view.gameTime < 2147483646, "存档游戏时间超出支持范围");
    view.queue = &fleetUnit(doc, fleetValue(*view.economy, "event_queue")->text, "economy_event_queue");
    view.events = fleetArray(*view.queue, "data");
    std::unordered_set<std::string> eventIds, scheduledDrivers;
    int lastTime = -1;
    for (const auto* event : view.events) {
        fleetCancel(canceled);
        fleetNeed(event->kind == TokKind::Atom && eventIds.insert(event->text).second, "经济事件队列包含重复或无效引用");
        const auto& u = fleetUnit(doc, event->text, "economy_event");
        const int time = fleetNumber(u, "time");
        fleetNeed(time >= lastTime, "经济事件队列顺序异常"); lastTime = time;
        view.eventTimes[u.id] = time;
        const auto* link = fleetValue(u, "unit_link");
        if (link->text.compare(0, 7, "driver.") == 0) scheduledDrivers.insert(link->text);
    }
    std::unordered_map<std::string, int> jobUsers, logUsers;
    for (const auto& u : doc.units) for (const auto& p : u.props) {
        if (p.index < 0 && p.values.size() == 1) {
            if (p.name == "driver_job") ++jobUsers[p.values[0].text];
            if (p.name == "profit_log") ++logUsers[p.values[0].text];
        }
    }
    const auto trucks = fleetTexts(fleetArray(*view.player, "trucks"));
    const auto logs = fleetTexts(fleetArray(*view.player, "truck_profit_logs"));
    const auto drivers = fleetTexts(fleetArray(*view.player, "drivers"));
    fleetNeed(trucks.size() == logs.size(), "卡车与收益记录数量不一致");
    for (const auto* key : {"driver_flags", "driver_readiness_timer", "driver_undrivable_truck_timers"})
        fleetNeed(fleetArray(*view.player, key).size() == drivers.size(), "司机并行数组数量不一致");
    for (const auto& p : view.player->props)
        if (p.index >= 0 && p.name.compare(0, 7, "driver_") == 0)
            fleetNeed(p.name == "driver_flags" || p.name == "driver_readiness_timer" ||
                      p.name == "driver_undrivable_truck_timers", "不支持的司机并行数组：" + p.name);
    std::unordered_set<std::string> ownedTrucks, ownedDrivers, uniqueLogs;
    std::unordered_map<std::string, std::string> configurations;
    std::size_t configurationBytes = 0;
    for (std::size_t i = 0; i < trucks.size(); ++i) {
        fleetCancel(canceled);
        fleetNeed(ownedTrucks.insert(trucks[i]).second && uniqueLogs.insert(logs[i]).second, "车辆或收益记录重复");
        const auto& truck = fleetUnit(doc, trucks[i], "vehicle"); fleetUnit(doc, logs[i], "profit_log");
        // An unsupported template is omitted; other owned trucks remain counted in garage validation.
        std::size_t accessories = 0;
        if (validateTruckGraph(doc, truck, &accessories, &error, canceled)) {
            view.inventory.trucks.push_back({truck.id, labelForVehicle(doc, truck), accessories});
            auto signature = fleetConfiguration(doc, truck, canceled);
            fleetNeed(signature.size() <= kMaxText - configurationBytes, "车辆配置总量超过支持上限");
            configurationBytes += signature.size(); configurations[truck.id] = std::move(signature);
        }
    }
    std::size_t playerDrivers = 0;
    for (const auto& id : drivers) {
        fleetNeed(ownedDrivers.insert(id).second, "雇佣司机列表含重复引用");
        const auto* u = findById(doc, id);
        fleetNeed(u && (u->type == "driver_ai" || u->type == "driver_player"), "司机对象不存在：" + id);
        if (u->type == "driver_ai") ++view.inventory.hiredDrivers; else ++playerDrivers;
    }
    fleetNeed(playerDrivers == 1, "无法唯一识别玩家本人，拒绝修改司机列表");
    std::unordered_set<std::string> placedTrucks, placedDrivers, garageIds, profitEntryIds;
    for (const auto* ref : fleetArray(*view.economy, "garages")) {
        fleetCancel(canceled);
        fleetNeed(ref->kind == TokKind::Atom && garageIds.insert(ref->text).second, "车库列表含重复或无效引用");
        const auto& garage = fleetUnit(doc, ref->text, "garage");
        FleetGarage g; g.id = garage.id;
        fleetNeed(g.id.compare(0, 7, "garage.") == 0 && g.id.size() > 7, "无法识别车库城市：" + g.id);
        g.city = g.id.substr(7);
        fleetNeed(g.city.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == std::string::npos,
                  "不支持的车库城市标识：" + g.city);
        const int status = fleetNumber(garage, "status");
        const auto vehicles = fleetArray(garage, "vehicles"), occupants = fleetArray(garage, "drivers");
        fleetNeed(vehicles.size() == occupants.size(), "车库车辆与司机槽位数量不一致：" + g.id);
        fleetNeed((status == 0 && vehicles.empty()) || (status == 2 && vehicles.size() == 3) ||
                  (status == 3 && vehicles.size() == 5) || (status == 6 && vehicles.size() == 1),
                  "不支持的车库等级或容量：" + g.id);
        g.purchased = status != 0; g.slots = vehicles.size();
        for (std::size_t i = 0; i < vehicles.size(); ++i) {
            fleetNeed(vehicles[i]->kind == TokKind::Atom && occupants[i]->kind == TokKind::Atom, "无效车库槽位");
            if (!fleetNull(vehicles[i])) {
                fleetNeed(ownedTrucks.count(vehicles[i]->text) && placedTrucks.insert(vehicles[i]->text).second,
                          "车库卡车不属于玩家或被重复分配：" + vehicles[i]->text); ++g.trucks;
            }
            if (!fleetNull(occupants[i])) {
                fleetNeed(ownedDrivers.count(occupants[i]->text) && placedDrivers.insert(occupants[i]->text).second,
                          "车库司机不属于玩家或被重复分配：" + occupants[i]->text); ++g.drivers;
                const auto& driver = *findById(doc, occupants[i]->text);
                if (driver.type == "driver_ai") {
                    FleetEmployee employee; employee.id = driver.id; employee.garageId = g.id;
                    if (!fleetNull(vehicles[i])) employee.truckId = vehicles[i]->text;
                    employee.state = fleetNumber(driver, "state");
                    fleetNeed(logUsers[fleetValue(driver, "profit_log")->text] == 1, "雇佣司机共用了收益记录");
                    employee.profit = fleetProfit(doc, driver, profitEntryIds, canceled); fleetSum(g.profit, employee.profit);
                    fleetSum(view.inventory.profit, employee.profit);
                    view.inventory.employees.push_back(std::move(employee));
                }
            }
            if (fleetNull(vehicles[i]) && fleetNull(occupants[i])) view.freeSlots[g.id].push_back(i), ++g.freeSlots;
            if (!fleetNull(vehicles[i]) && fleetNull(occupants[i])) {
                ++g.idleTrucks; view.inventory.idleTrucks.push_back({g.id, vehicles[i]->text, i});
            }
        }
        view.inventory.garages.push_back(std::move(g));
    }
    fleetNeed(placedTrucks == ownedTrucks && placedDrivers == ownedDrivers, "存在未登记车库的自有车辆或司机");
    struct Evidence { FleetView::Cache cache; std::size_t count = 0; bool conflict = false; std::string first; };
    std::unordered_map<std::string, Evidence> evidence;
    for (const auto& employee : view.inventory.employees) {
        fleetCancel(canceled); const auto signature = configurations.find(employee.truckId);
        if (signature == configurations.end()) continue;
        const auto cache = fleetCache(*findById(doc, employee.id));
        if (fleetDefaultCache(cache)) continue; // Known pool placeholder; never used as a donor.
        auto& group = evidence[signature->second];
        if (!group.count) { group.cache = cache; group.first = employee.id; }
        else if (!fleetSameCache(group.cache, cache)) group.conflict = true;
        ++group.count;
    }
    for (const auto& truck : view.inventory.trucks) {
        FleetTruckCache info; info.truckId = truck.id;
        const auto it = evidence.find(configurations.at(truck.id));
        if (it != evidence.end()) {
            const auto& group = it->second; info.donors = group.count;
            info.supported = group.count >= 2 && !group.conflict;
            if (info.supported) {
                info.axles = group.cache.axles; info.mass = group.cache.mass; info.efficiency = group.cache.efficiency;
                info.note = std::to_string(group.count) + " 名同配置在职司机参数一致，参考 " + group.first;
                view.resolvedCaches[truck.id] = group.cache;
            } else info.note = group.conflict ? "同配置在职司机参数冲突，无法核实" : "仅有一名同配置参考司机，需要至少两名";
        } else info.note = "缺少同配置在职司机的非默认参数，无法核实";
        view.inventory.truckCaches.push_back(std::move(info));
    }
    for (auto& employee : view.inventory.employees) {
        if (employee.truckId.empty()) { employee.cacheNote = "没有同槽位卡车"; continue; }
        const auto resolved = view.resolvedCaches.find(employee.truckId);
        if (resolved == view.resolvedCaches.end()) { employee.cacheNote = "车辆参数未核实"; continue; }
        const auto cache = fleetCache(*findById(doc, employee.id));
        employee.cacheVerified = fleetSameCache(cache, resolved->second);
        employee.cacheRepairable = !employee.cacheVerified && fleetDefaultCache(cache);
        employee.cacheNote = employee.cacheVerified ? "与同配置参考一致" :
                             employee.cacheRepairable ? "仍为司机池默认参数，可修复" : "车辆参数冲突";
    }
    view.pool = fleetArray(*view.economy, "driver_pool");
    view.offers = fleetArray(*view.economy, "drivers_offer");
    std::unordered_set<std::string> poolIds;
    std::vector<const Tok*> candidates = view.pool;
    candidates.insert(candidates.end(), view.offers.begin(), view.offers.end());
    const auto offers = fleetTexts(view.offers);
    const std::unordered_set<std::string> offered(offers.begin(), offers.end());
    for (const auto* ref : candidates) {
        fleetCancel(canceled);
        fleetNeed(ref->kind == TokKind::Atom && poolIds.insert(ref->text).second &&
                  !ownedDrivers.count(ref->text), "司机池、招聘列表或已雇佣司机重复");
        const auto& u = fleetUnit(doc, ref->text, "driver_ai");
        if (scheduledDrivers.count(u.id) || fleetNumber(u, "state") != 1 ||
            !fleetEmpty(fleetValue(u, "hometown")) || !fleetEmpty(fleetValue(u, "current_city")) ||
            !fleetEmpty(fleetValue(u, "old_hometown")) || fleetNumber(u, "on_duty_timer") != 0 ||
            fleetNumber(u, "extra_maintenance") != 0) continue;
        bool idle = true;
        for (const auto* key : {"adopted_truck", "assigned_truck", "adopted_trailer", "assigned_trailer"})
            idle = idle && fleetNull(fleetValue(u, key));
        const auto& job = fleetUnit(doc, fleetValue(u, "driver_job")->text, "job_info");
        const auto& log = fleetUnit(doc, fleetValue(u, "profit_log")->text, "profit_log");
        for (const auto* key : {"cargo", "source_company", "target_company", "special", "car"})
            idle = idle && fleetNull(fleetValue(job, key));
        idle = idle && fleetNumber(job, "start_time") == 0 && fleetNumber(job, "planned_distance_km") == 0 &&
               fleetArray(log, "stats_data").empty() && jobUsers[job.id] == 1 && logUsers[log.id] == 1;
        if (idle) {
            fleetCache(u); // Fields must exist before a hire can initialize them.
            view.inventory.availableDrivers.push_back({u.id, fleetNumber(u, "experience_points"), offered.count(u.id) != 0});
        }
    }
    std::sort(view.inventory.availableDrivers.begin(), view.inventory.availableDrivers.end(),
              [](const FleetDriver& a, const FleetDriver& b) {
                  return a.experience != b.experience ? a.experience > b.experience : a.id < b.id;
              });
    std::sort(view.inventory.garages.begin(), view.inventory.garages.end(),
              [](const FleetGarage& a, const FleetGarage& b) { return a.id < b.id; });
    return view;
}

void fleetSet(const UnitInfo& u, const std::string& key, const std::string& value, std::vector<Replacement>& reps) {
    const auto* t = fleetValue(u, key); reps.push_back({t->begin, t->end, value});
}
void fleetSetArray(const UnitInfo& u, const std::string& key, const std::vector<std::string>& values,
                   std::vector<Replacement>& reps) {
    const auto old = fleetArray(u, key);
    fleetSet(u, key, std::to_string(values.size()), reps);
    std::size_t anchor = fleetValue(u, key)->end;
    for (std::size_t i = 0; i < old.size(); ++i) {
        anchor = (std::max)(anchor, old[i]->end);
        if (i < values.size()) reps.push_back({old[i]->begin, old[i]->end, values[i]});
        else {
            const auto p = std::find_if(u.props.begin(), u.props.end(), [&](const Prop& p) {
                return p.name == key && p.index == static_cast<int>(i);
            });
            fleetNeed(p != u.props.end(), "内部数组索引错误");
            reps.push_back({p->begin, old[i]->end, ""});
        }
    }
    std::string added;
    for (std::size_t i = old.size(); i < values.size(); ++i)
        added += "\n " + key + "[" + std::to_string(i) + "]: " + values[i];
    if (!added.empty()) reps.push_back({anchor, anchor, added});
}
std::string fleetApply(const std::string& text, std::vector<Replacement> reps,
                       const std::function<bool()>& canceled) {
    // Linear output assembly matters when hundreds of trucks are added to a large save.
    std::sort(reps.begin(), reps.end(), [](const Replacement& a, const Replacement& b) {
        return a.begin != b.begin ? a.begin < b.begin : a.end < b.end;
    });
    std::size_t pos = 0; std::string result;
    for (const auto& r : reps) {
        fleetCancel(canceled);
        fleetNeed(r.begin >= pos && r.end >= r.begin && r.end <= text.size(), "内部车队编辑范围冲突");
        fleetNeed(result.size() + r.begin - pos + r.text.size() <= kMaxText, "车队结果超过 128 MiB 上限");
        result.append(text, pos, r.begin - pos); result += r.text; pos = r.end;
    }
    fleetNeed(result.size() + text.size() - pos <= kMaxText, "车队结果超过 128 MiB 上限");
    result.append(text, pos, std::string::npos); return result;
}
void fleetSetCache(const UnitInfo& driver, const FleetView::Cache& cache, std::vector<Replacement>& reps) {
    fleetSet(driver, "slot_truck_axle_count", std::to_string(cache.axles), reps);
    fleetSet(driver, "slot_truck_mass", cache.massText, reps);
    fleetSet(driver, "slot_truck_efficiency", cache.efficiencyText, reps);
}
} // namespace

bool inspectFleetText(const std::string& text, FleetInventory* inventory, std::string* error,
                      std::function<bool()> canceled) {
    if (inventory) *inventory = {}; if (error) error->clear();
    try {
        Doc doc; std::string detail;
        if (!loadDoc(text, &doc, &detail, canceled)) throw std::runtime_error(detail);
        auto view = readFleet(doc, canceled); fleetCancel(canceled);
        view.inventory.snapshot = fleetSnapshot(text);
        if (inventory) *inventory = std::move(view.inventory);
        return true;
    } catch (const std::exception& e) { setError(error, e.what()); return false; }
}

bool expandFleetText(const std::string& text, const FleetExpansion& request, std::string* out,
                     std::string* error, std::function<bool()> canceled) {
    if (out) out->clear(); if (error) error->clear();
    try {
        fleetCancel(canceled);
        fleetNeed(!request.expectedSnapshot.empty() && request.expectedSnapshot == fleetSnapshot(text),
                  "存档已在预览后改变，请重新读取车队");
        Doc doc; std::string detail;
        if (!loadDoc(text, &doc, &detail, canceled)) throw std::runtime_error(detail);
        auto view = readFleet(doc, canceled);
        struct GarageEdit { std::vector<std::string> vehicles, drivers; };
        std::unordered_map<std::string, GarageEdit> garageEdits;
        auto editGarage = [&](const UnitInfo& garage) -> GarageEdit& {
            const auto found = garageEdits.find(garage.id);
            if (found != garageEdits.end()) return found->second;
            GarageEdit edit{fleetTexts(fleetArray(garage, "vehicles")), fleetTexts(fleetArray(garage, "drivers"))};
            return garageEdits.emplace(garage.id, std::move(edit)).first->second;
        };
        fleetNeed(request.garageChanges.size() <= 400, "单次最多购买或扩容 400 个车库");
        std::unordered_set<std::string> changedGarages;
        for (const auto& change : request.garageChanges) {
            fleetCancel(canceled);
            const auto g = std::find_if(view.inventory.garages.begin(), view.inventory.garages.end(),
                [&](const FleetGarage& item) { return item.id == change.garageId; });
            fleetNeed(g != view.inventory.garages.end() && changedGarages.insert(change.garageId).second,
                      "购买或扩容的车库不存在或重复：" + change.garageId);
            fleetNeed((change.targetSlots == 3 || change.targetSlots == 5) &&
                      static_cast<std::size_t>(change.targetSlots) > g->slots, "车库目标容量必须增加到 3 或 5：" + change.garageId);
            auto& edit = editGarage(fleetUnit(doc, change.garageId, "garage"));
            edit.vehicles.resize(change.targetSlots, "null"); edit.drivers.resize(change.targetSlots, "null");
            for (std::size_t slot = g->slots; slot < edit.vehicles.size(); ++slot)
                view.freeSlots[change.garageId].push_back(slot);
        }
        std::unordered_set<std::string> chosenGarages;
        std::size_t clones = 0;
        for (const auto& a : request.allocations) {
            fleetNeed(a.count > 0 && chosenGarages.insert(a.garageId).second, "车库补充数量无效或车库重复");
            const auto slots = view.freeSlots.find(a.garageId);
            fleetNeed(slots != view.freeSlots.end() && static_cast<std::size_t>(a.count) <= slots->second.size(),
                      "车库没有足够的车辆和司机均为空的位置：" + a.garageId);
            clones += static_cast<std::size_t>(a.count);
            fleetNeed(clones <= 400, "单次最多雇佣 400 名司机");
        }
        fleetNeed(request.existingTruckIds.size() <= 400 && request.repairDriverIds.size() <= 400, "单次处理数量超过上限");
        const auto total = clones + request.existingTruckIds.size();
        fleetNeed(total <= 400 && total <= view.inventory.availableDrivers.size(), "雇佣数量超过上限或可用司机不足");
        fleetNeed(total > 0 || !request.repairDriverIds.empty() || !request.garageChanges.empty(), "没有选择车库、雇佣或参数修复操作");
        const UnitInfo* truck = nullptr;
        std::size_t accessories = 0; std::vector<std::string> graph;
        if (clones) {
            fleetNeed(view.resolvedCaches.count(request.truckId) != 0, "卡车模板缺少可靠的同配置车辆参数，请选择已核实车型");
            truck = &fleetUnit(doc, request.truckId, "vehicle");
            if (!validateTruckGraph(doc, *truck, &accessories, &detail, canceled, &graph)) throw std::runtime_error(detail);
        }
        fleetNeed(graph.size() * clones <= 100000, "复制的车辆对象总数超出上限，请减少数量");
        std::unordered_set<std::string> existing;
        for (const auto& id : request.existingTruckIds) {
            fleetNeed(existing.insert(id).second && std::any_of(view.inventory.idleTrucks.begin(), view.inventory.idleTrucks.end(),
                [&](const FleetIdleTruck& t) { return t.truckId == id; }), "所选卡车没有空闲司机位置或重复：" + id);
            fleetNeed(view.resolvedCaches.count(id) != 0, "闲置卡车的车辆参数未核实：" + id);
        }
        std::unordered_set<std::string> reserved; collectAllIds(doc, &reserved);
        std::uint64_t counter = 1;
        std::vector<Replacement> reps;
        auto trucks = fleetTexts(fleetArray(*view.player, "trucks"));
        auto logs = fleetTexts(fleetArray(*view.player, "truck_profit_logs"));
        auto drivers = fleetTexts(fleetArray(*view.player, "drivers"));
        auto events = fleetTexts(view.events);
        std::string appended; std::size_t next = 0;
        std::unordered_set<std::string> hired;
        auto hire = [&](const UnitInfo& garage, std::size_t slot, const FleetView::Cache& cache) {
            fleetCancel(canceled);
            const auto& driver = fleetUnit(doc, view.inventory.availableDrivers.at(next++).id, "driver_ai");
            hired.insert(driver.id); drivers.push_back(driver.id);
            const auto eventId = makeFreshId(&reserved, &counter); const int time = view.gameTime + 1;
            appended += "\neconomy_event : " + eventId + " {\n time: " + std::to_string(time) +
                "\n unit_link: " + driver.id + "\n param: 4\n}\n";
            events.push_back(eventId); view.eventTimes[eventId] = time;
            const auto city = garage.id.substr(7);
            fleetSet(driver, "hometown", city, reps); fleetSet(driver, "current_city", city, reps);
            fleetSetCache(driver, cache, reps);
            auto& occupants = editGarage(garage).drivers;
            fleetNeed(slot < occupants.size() && occupants[slot] == "null", "所选司机位置不为空");
            occupants[slot] = driver.id;
            // assigned_truck remains null, matching ordinary employed drivers; garage slots pair them.
        };
        for (const auto& id : request.existingTruckIds) {
            const auto slot = std::find_if(view.inventory.idleTrucks.begin(), view.inventory.idleTrucks.end(),
                [&](const FleetIdleTruck& t) { return t.truckId == id; });
            hire(fleetUnit(doc, slot->garageId, "garage"), slot->slot, view.resolvedCaches.at(id));
        }
        for (const auto& a : request.allocations) {
            const auto& garage = fleetUnit(doc, a.garageId, "garage");
            for (int i = 0; i < a.count; ++i) {
                fleetCancel(canceled);
                std::unordered_map<std::string, std::string> mapping;
                for (const auto& id : graph) mapping[id] = makeFreshId(&reserved, &counter);
                for (const auto& id : graph) {
                    fleetCancel(canceled); std::string cloned;
                    if (!cloneGraphUnit(doc, *findById(doc, id), mapping, &cloned, &detail)) throw std::runtime_error(detail);
                    fleetNeed(appended.size() + cloned.size() + 1024 < kMaxText, "车辆副本体积过大");
                    appended += "\n" + cloned;
                }
                const auto logId = makeFreshId(&reserved, &counter);
                appended += "\nprofit_log : " + logId +
                    " {\n stats_data: 0\n acc_distance_free: 0\n acc_distance_on_job: 0\n history_age: nil\n}\n";
                trucks.push_back(mapping.at(truck->id)); logs.push_back(logId);
                const auto slot = view.freeSlots.at(a.garageId).at(i);
                auto& vehicles = editGarage(garage).vehicles;
                fleetNeed(slot < vehicles.size() && vehicles[slot] == "null", "所选车辆位置不为空");
                vehicles[slot] = mapping.at(truck->id);
                hire(garage, slot, view.resolvedCaches.at(truck->id));
            }
        }
        for (const auto& edit : garageEdits) {
            const auto& garage = fleetUnit(doc, edit.first, "garage");
            fleetSetArray(garage, "vehicles", edit.second.vehicles, reps);
            fleetSetArray(garage, "drivers", edit.second.drivers, reps);
            if (changedGarages.count(edit.first)) fleetSet(garage, "status", edit.second.vehicles.size() == 3 ? "2" : "3", reps);
        }
        std::unordered_set<std::string> repaired;
        for (const auto& id : request.repairDriverIds) {
            fleetCancel(canceled);
            const auto employee = std::find_if(view.inventory.employees.begin(), view.inventory.employees.end(),
                [&](const FleetEmployee& d) { return d.id == id && d.cacheRepairable; });
            fleetNeed(repaired.insert(id).second && employee != view.inventory.employees.end(), "所选司机不能修复或重复：" + id);
            fleetSetCache(fleetUnit(doc, id, "driver_ai"), view.resolvedCaches.at(employee->truckId), reps);
        }
        fleetSetArray(*view.player, "trucks", trucks, reps);
        fleetSetArray(*view.player, "truck_profit_logs", logs, reps);
        fleetSetArray(*view.player, "drivers", drivers, reps);
        for (const auto* key : {"driver_flags", "driver_readiness_timer", "driver_undrivable_truck_timers"}) {
            auto values = fleetTexts(fleetArray(*view.player, key)); values.resize(drivers.size(), "0");
            fleetSetArray(*view.player, key, values, reps);
        }
        std::vector<std::string> pool;
        for (const auto* driver : view.pool) if (!hired.count(driver->text)) pool.push_back(driver->text);
        fleetSetArray(*view.economy, "driver_pool", pool, reps);
        std::vector<std::string> offers;
        for (const auto* driver : view.offers) if (!hired.count(driver->text)) offers.push_back(driver->text);
        fleetSetArray(*view.economy, "drivers_offer", offers, reps);
        std::stable_sort(events.begin(), events.end(), [&](const std::string& a, const std::string& b) {
            return view.eventTimes.at(a) < view.eventTimes.at(b);
        });
        fleetSetArray(*view.queue, "data", events, reps);
        reps.push_back({doc.toks[doc.outerCloseTok].begin, doc.toks[doc.outerCloseTok].begin, appended});
        auto result = fleetApply(text, std::move(reps), canceled);
        // Reparse the entire result and validate ownership, counts, garage uniqueness and event order.
        Doc verified; if (!loadDoc(result, &verified, &detail, canceled)) throw std::runtime_error(detail);
        const auto after = readFleet(verified, canceled);
        fleetNeed(after.inventory.hiredDrivers == view.inventory.hiredDrivers + total &&
                  after.inventory.trucks.size() == view.inventory.trucks.size() + clones &&
                  fleetArray(*after.player, "trucks").size() == trucks.size() &&
                  after.pool.size() + after.offers.size() + total == view.pool.size() + view.offers.size(), "车队最终数量校验失败");
        for (const auto& change : request.garageChanges) {
            const auto g = std::find_if(after.inventory.garages.begin(), after.inventory.garages.end(),
                [&](const FleetGarage& item) { return item.id == change.garageId; });
            fleetNeed(g != after.inventory.garages.end() && g->purchased && g->slots == static_cast<std::size_t>(change.targetSlots),
                      "购买或扩容车库最终校验失败");
        }
        for (const auto& id : hired) {
            const auto employee = std::find_if(after.inventory.employees.begin(), after.inventory.employees.end(),
                [&](const FleetEmployee& d) { return d.id == id; });
            fleetNeed(employee != after.inventory.employees.end() && employee->cacheVerified, "新增司机车辆参数校验失败");
        }
        for (const auto& id : repaired) {
            const auto employee = std::find_if(after.inventory.employees.begin(), after.inventory.employees.end(),
                [&](const FleetEmployee& d) { return d.id == id; });
            fleetNeed(employee != after.inventory.employees.end() && employee->cacheVerified, "司机参数修复校验失败");
        }
        fleetCancel(canceled);
        if (out) *out = std::move(result);
        return true;
    } catch (const std::exception& e) { setError(error, e.what()); return false; }
}
