#include "trailerswaptest.h"
#include "trucktransfer.h"
#include <stdexcept>

namespace ets2 {
namespace {

// 结构对齐真实解密存档（Promods 档案 autosave_drive_8）：任务挂车连在会话槽位上，
// 自有挂车停在车库列表里，my_vehicles[0] 是占用的车库槽。
const char* kJob = R"SII(SiiNunit
{
player : _nameless.3.35cf.d108 {
 my_vehicles: 1
 my_vehicles[0]: _nameless.3.3088.ee98
 assigned_vehicles: _nameless.3.3088.e498
 trailers: 2
 trailers[0]: _nameless.3.3b1f.5b08
 trailers[1]: _nameless.4.6c00.f108
 current_job: _nameless.11.5632.4fa8
 assigned_truck: null
 my_truck: null
 my_trailer: null
}
player_vehicles : _nameless.3.3088.ee98 {
 vehicle: _nameless.4.6692.52f8
 trailer: _nameless.4.6c00.f108
 stored_trailer_placements: 1
 stored_trailer_placements[0]: (1.0, 2.0, 3.0) (1.0; 0.0, 0.0, 0.0)
 stored_trailer_attached: false
}
player_vehicles : _nameless.3.3088.e498 {
 vehicle: _nameless.4.6692.52f8
 trailer: _nameless.11.42b5.9218
 stored_trailer_placements: 1
 stored_trailer_placements[0]: (-1020.0, 7.5, -16428.1) (0.69; 0.0, -0.72, 0.0)
 stored_trailer_attached: true
}
vehicle : _nameless.4.6692.52f8 {
 accessories: 1
 accessories[0]: _nameless.868.4238
 fuel_relative: 0.5
}
vehicle_accessory : _nameless.868.4238 {
 data_path: "/def/vehicle/truck/scania.s/data.sii"
}
trailer : _nameless.11.42b5.9218 {
 trailer_definition: trailer_def.scs.chemtank.single_3.paint
 slave_trailer: null
 is_private: false
 cargo_mass: &469ab000
 cargo_damage: &3c1ba96a
 license_plate: "<font xscale=1.15 yscale=1.15>GS 56 87</font>|denmark"
 odometer: 407
}
trailer : _nameless.3.3b1f.5b08 {
 trailer_definition: _nameless.bae.f458
 slave_trailer: null
 is_private: false
 cargo_mass: &00000000
 cargo_damage: &00000000
 license_plate: ""
 odometer: 0
}
trailer : _nameless.4.6c00.f108 {
 trailer_definition: _nameless.3.a086.8318
 slave_trailer: null
 is_private: false
 cargo_mass: &00000000
 cargo_damage: &00000000
 license_plate: "苏A·32906|china"
 odometer: 12
}
trailer_def : _nameless.bae.f458 {
 trailer: scs.box
 source_name: trailer_def.scs.box.single_4.dryvan
 body_type: dryvan
}
trailer_def : _nameless.3.a086.8318 {
 trailer: tmp.caravan
 source_name: trailer_def.tmp.caravan.default.default
 body_type: curtainside
}
player_job : _nameless.11.5632.4fa8 {
 company_trailer: _nameless.11.42b5.9218
 online_job_id: 0
 cargo: cargo.hchemicals
 is_trailer_loaded: true
}
driver : driver.11 {
 assigned_trailer: null
}
garage : garage.luxembourg {
 trailers: 2
 trailers[0]: _nameless.3.3b1f.5b08
 trailers[1]: _nameless.4.6c00.f108
}
}
)SII";

std::string changedAt(std::string text, const std::string& from, const std::string& to,
                      int occurrence = 1) {
    std::size_t pos = 0;
    for (int seen = 0; seen < occurrence; ++seen) {
        pos = text.find(from, pos);
        if (pos == std::string::npos) throw std::runtime_error("fixture marker missing: " + from);
        if (seen + 1 < occurrence) pos += from.size();
    }
    text.replace(pos, from.size(), to);
    return text;
}

std::size_t countOf(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    for (std::size_t pos = text.find(needle); pos != std::string::npos;
         pos = text.find(needle, pos + needle.size()))
        ++count;
    return count;
}

const TrailerSwapEntry* findEntry(const TrailerSwapInventory& inventory, const std::string& id) {
    for (const auto& entry : inventory.entries)
        if (entry.id == id) return &entry;
    return nullptr;
}

}  // namespace

std::vector<TunerTestItem> runTrailerSwapTests() {
    std::vector<TunerTestItem> results;
    auto check = [&](const char* name, bool ok, const std::string& detail = std::string()) {
        results.push_back({std::string("trailer-swap-") + name, ok, detail});
    };
    std::string error, out;

    {   // 分类：当前任务挂车、两辆自有挂车、车牌、载重、标签
        TrailerSwapInventory inv;
        const bool ok = inspectTrailerSwapText(kJob, &inv, &error);
        const TrailerSwapEntry* job = findEntry(inv, "_nameless.11.42b5.9218");
        const TrailerSwapEntry* box = findEntry(inv, "_nameless.3.3b1f.5b08");
        const TrailerSwapEntry* caravan = findEntry(inv, "_nameless.4.6c00.f108");
        check("job-inspection", ok && inv.entries.size() == 3 && inv.hasCurrent && inv.onJob &&
                                 !inv.onlineJob && inv.swapBlockReason.empty(), error);
        check("job-cargo-and-plate",
              inv.jobCargo == "cargo.hchemicals" &&
                  inv.currentPlate == "GS 56 87 · denmark" &&
                  job && job->current && job->job && !job->owned &&
                  job->cargoMass > 19799.f && job->cargoMass < 19801.f,
              error + " jobCargo=[" + inv.jobCargo + "] plate=[" + inv.currentPlate +
                  "] mass=" + (job ? std::to_string(job->cargoMass) : "null") +
                  " flags=" + (job ? std::string(job->current ? "C" : "-") +
                                           (job->job ? "J" : "-") +
                                           (job->owned ? "O" : "-")
                                : "null"));
        check("owned-entries", box && box->owned && !box->current && !box->blocked &&
                                 box->label == "scs.box.single_4.dryvan" &&
                                 caravan && caravan->owned && !caravan->blocked &&
                                 caravan->plate == "苏A·32906 · china" &&
                                 caravan->label == "tmp.caravan.default.default", error);
        check("truck-label", inv.truckLabel == "scania.s", inv.truckLabel);
        check("current-sorted-first", !inv.entries.empty() && inv.entries.front().current, "");
    }

    {   // 任务中换成自有干货车：槽位 + company_trailer 改写，货物随任务搬到新挂车
        const std::string expected = changedAt(
            changedAt(
                changedAt(
                    changedAt(changedAt(kJob, "trailer: _nameless.11.42b5.9218",
                                        "trailer: _nameless.3.3b1f.5b08"),
                              "company_trailer: _nameless.11.42b5.9218",
                              "company_trailer: _nameless.3.3b1f.5b08"),
                    "cargo_mass: &00000000", "cargo_mass: &469ab000"),
                "cargo_damage: &00000000", "cargo_damage: &3c1ba96a"),
            "license_plate: \"\"", "license_plate: \"\"");
        out = "stale";
        const bool ok = swapTrailerText(kJob, "_nameless.3.3b1f.5b08", &out, &error);
        check("job-swap-bytes", ok && out == expected, error);
        TrailerSwapInventory after;
        const bool reinspect = inspectTrailerSwapText(out, &after, &error);
        const TrailerSwapEntry* box = findEntry(after, "_nameless.3.3b1f.5b08");
        check("job-swap-reinspect",
              reinspect && after.hasCurrent && after.onJob &&
                  after.currentId == "_nameless.3.3b1f.5b08" &&
                  box && box->current && box->job &&
                  box->cargoMass > 19799.f && box->cargoMass < 19801.f,
              error + " reinspect=" + (reinspect ? "1" : "0") +
                  " cur=[" + after.currentId + "] onJob=" + (after.onJob ? "1" : "0") +
                  " box=" + (box ? std::string(box->current ? "C" : "-") +
                                        (box->job ? "J" : "-") + "/" +
                                        std::to_string(box->cargoMass)
                             : "null"));
        check("garage-lists-untouched",
              // player.trailers 与 garage.trailers 各列一次，交换后两处都原样保留
              countOf(out, "trailers[0]: _nameless.3.3b1f.5b08") == 2 &&
                  countOf(out, "trailers[1]: _nameless.4.6c00.f108") == 2,
              "counts=" + std::to_string(countOf(out, "trailers[0]: _nameless.3.3b1f.5b08")) +
                  "/" + std::to_string(countOf(out, "trailers[1]: _nameless.4.6c00.f108")));
    }

    {   // 换成另一槽位占用的挂车：目标槽位被腾空，挂车列表不动
        out = "stale";
        const bool ok = swapTrailerText(kJob, "_nameless.4.6c00.f108", &out, &error);
        const std::string expected = changedAt(
            changedAt(
                changedAt(
                    changedAt(
                        changedAt(
                            changedAt(changedAt(kJob, "trailer: _nameless.4.6c00.f108",
                                                "trailer: null", 1),
                                      "trailer: _nameless.11.42b5.9218",
                                      "trailer: _nameless.4.6c00.f108"),
                            "company_trailer: _nameless.11.42b5.9218",
                            "company_trailer: _nameless.4.6c00.f108"),
                        "cargo_mass: &00000000", "cargo_mass: &469ab000", 2),
                    "cargo_damage: &00000000", "cargo_damage: &3c1ba96a", 2),
                "license_plate: \"苏A·32906|china\"", "license_plate: \"苏A·32906|china\""),
            "odometer: 407", "odometer: 407");
        check("occupied-slot-swap", ok && out == expected, error);
        check("occupied-slot-freed",
              ok && countOf(out, "trailer: _nameless.4.6c00.f108") == 2,
              std::to_string(countOf(out, "trailer: _nameless.4.6c00.f108")));
    }

    {   // 无任务：换车不碰 company_trailer，货物照搬
        const std::string noJob = changedAt(
            changedAt(kJob, " current_job: _nameless.11.5632.4fa8", " current_job: null"),
            "company_trailer: _nameless.11.42b5.9218", "company_trailer: null");
        out = "stale";
        const bool ok = swapTrailerText(noJob, "_nameless.3.3b1f.5b08", &out, &error);
        TrailerSwapInventory after;
        const bool reinspect = ok && inspectTrailerSwapText(out, &after, &error);
        const TrailerSwapEntry* box = findEntry(after, "_nameless.3.3b1f.5b08");
        check("no-job-swap", ok && reinspect && !after.onJob &&
                             after.currentId == "_nameless.3.3b1f.5b08" &&
                             countOf(out, "company_trailer: null") == 1 &&
                             box && box->cargoMass > 19799.f,
              error + " ok=" + (ok ? "1" : "0") + " re=" + (reinspect ? "1" : "0") +
                  " cur=[" + after.currentId + "] onJob=" + (after.onJob ? "1" : "0") +
                  " boxMass=" + (box ? std::to_string(box->cargoMass) : "null"));
    }

    {   // 拒绝：相同目标 / 缺失目标 / 双挂目标 / AI 占用 / 其他任务占用 / 定义缺失
        out = "stale";
        check("same-target-refused",
              !swapTrailerText(kJob, "_nameless.11.42b5.9218", &out, &error) && out.empty(),
              error);
        out = "stale";
        check("missing-target-refused",
              !swapTrailerText(kJob, "_nameless.404.0", &out, &error) && out.empty(), error);
        const std::string doubleTarget =
            changedAt(kJob, "trailer : _nameless.4.6c00.f108 {\n trailer_definition: _nameless.3.a086.8318\n slave_trailer: null",
                      "trailer : _nameless.4.6c00.f108 {\n trailer_definition: _nameless.3.a086.8318\n slave_trailer: _nameless.9.9");
        TrailerSwapInventory inv;
        bool inspected = inspectTrailerSwapText(doubleTarget, &inv, &error);
        const TrailerSwapEntry* caravan = findEntry(inv, "_nameless.4.6c00.f108");
        out = "stale";
        check("double-target-refused",
              inspected && caravan && caravan->blocked &&
                  caravan->blockReason.find("双挂") != std::string::npos &&
                  !swapTrailerText(doubleTarget, "_nameless.4.6c00.f108", &out, &error) &&
                  out.empty(), error);
        const std::string driverBusy =
            changedAt(kJob, "assigned_trailer: null", "assigned_trailer: _nameless.4.6c00.f108");
        inspected = inspectTrailerSwapText(driverBusy, &inv, &error);
        caravan = findEntry(inv, "_nameless.4.6c00.f108");
        out = "stale";
        check("driver-occupied-refused",
              inspected && caravan && caravan->blocked &&
                  !swapTrailerText(driverBusy, "_nameless.4.6c00.f108", &out, &error) &&
                  out.empty(), error);
        const std::string otherJob = changedAt(
            changedAt(kJob, "driver : driver.11 {",
                      "player_job : _nameless.99.1 {\n company_trailer: _nameless.3.3b1f.5b08\n}\ndriver : driver.11 {"),
            "trailers[0]: _nameless.3.3b1f.5b08", "trailers[0]: _nameless.3.3b1f.5b08");
        inspected = inspectTrailerSwapText(otherJob, &inv, &error);
        const TrailerSwapEntry* box = findEntry(inv, "_nameless.3.3b1f.5b08");
        out = "stale";
        check("other-job-refused",
              inspected && box && box->blocked &&
                  !swapTrailerText(otherJob, "_nameless.3.3b1f.5b08", &out, &error) &&
                  out.empty(), error);
        const std::string missingDef = changedAt(
            kJob, "trailer_definition: _nameless.3.a086.8318",
            "trailer_definition: _nameless.404.1");
        inspected = inspectTrailerSwapText(missingDef, &inv, &error);
        caravan = findEntry(inv, "_nameless.4.6c00.f108");
        out = "stale";
        check("missing-def-refused",
              inspected && caravan && caravan->blocked &&
                  !swapTrailerText(missingDef, "_nameless.4.6c00.f108", &out, &error) &&
                  out.empty(), error);
    }

    {   // 没连挂车 / 当前双挂 / 在线任务标记 / 十进制载重 / 取消 / 坏输入
        const std::string noTrailer =
            changedAt(kJob, "trailer: _nameless.11.42b5.9218", "trailer: null", 1);
        TrailerSwapInventory inv;
        bool inspected = inspectTrailerSwapText(noTrailer, &inv, &error);
        out = "stale";
        check("no-current-refused",
              inspected && !inv.hasCurrent && !inv.swapBlockReason.empty() &&
                  !swapTrailerText(noTrailer, "_nameless.3.3b1f.5b08", &out, &error) &&
                  out.empty(), error);
        const std::string currentDouble = changedAt(
            kJob, "trailer : _nameless.11.42b5.9218 {\n trailer_definition: trailer_def.scs.chemtank.single_3.paint\n slave_trailer: null",
                  "trailer : _nameless.11.42b5.9218 {\n trailer_definition: trailer_def.scs.chemtank.single_3.paint\n slave_trailer: _nameless.9.9");
        inspected = inspectTrailerSwapText(currentDouble, &inv, &error);
        out = "stale";
        check("current-double-refused",
              inspected && !inv.swapBlockReason.empty() &&
                  !swapTrailerText(currentDouble, "_nameless.3.3b1f.5b08", &out, &error) &&
                  out.empty(), error);
        const std::string online =
            changedAt(kJob, "online_job_id: 0", "online_job_id: 149225837");
        inspected = inspectTrailerSwapText(online, &inv, &error);
        check("online-job-flag", inspected && inv.onlineJob, error);
        const std::string decimal =
            changedAt(kJob, "cargo_mass: &469ab000", "cargo_mass: 18750.0");
        inspected = inspectTrailerSwapText(decimal, &inv, &error);
        const TrailerSwapEntry* job = findEntry(inv, "_nameless.11.42b5.9218");
        check("decimal-cargo-mass",
              inspected && job && job->cargoMass > 18749.f && job->cargoMass < 18751.f, error);
        out = "stale";
        check("canceled-inspect",
              !inspectTrailerSwapText(kJob, &inv, &error, [] { return true; }) &&
                  !inspectTrailerSwapText(kJob, nullptr, &error) &&
                  !swapTrailerText(kJob, "_nameless.3.3b1f.5b08", &out, &error,
                                   [] { return true; }) &&
                  out.empty(), error);
        out = "stale";
        check("malformed-refused",
              !inspectTrailerSwapText("SiiNunit\n{\n}\n", &inv, &error) &&
                  !swapTrailerText("BSII\x03\x00\x00\x00", "_nameless.1", &out, &error) &&
                  out.empty(), error);
    }
    return results;
}
}  // namespace ets2
