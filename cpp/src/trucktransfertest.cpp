#include "trucktransfertest.h"

#include "trucktransfer.h"
#include "trucktransferio.h"

#include <string>

namespace ets2 {
namespace {

const char* kTarget = R"SII(SiiNunit
{
economy : _nameless.4.1.1 {
 player: _nameless.5.1.1
}
player : _nameless.5.1.1 {
 trucks: 1
 trucks[0]: _nameless.1.1.1
 truck_profit_logs: 1
 truck_profit_logs[0]: _nameless.4.1.2
 drivers: 1
 drivers[0]: driver.11
 note: "_nameless.9.9.9"
 // _nameless.9.9.9 stays quoted or commented
}
garage : garage.koln {
 vehicles: 2
 vehicles[0]: _nameless.1.1.1
 vehicles[1]: null
 drivers: 2
 drivers[0]: driver.11
 drivers[1]: null
 trailers: 1
 trailers[0]: _nameless.7.7.7
 status: 3
 profit_log: _nameless.4.1.3
}
vehicle : _nameless.1.1.1 {
 accessories: 0
 fuel_relative: 1
}
profit_log : _nameless.4.1.2 {
 stats_data: 0
 acc_distance_free: 0
 acc_distance_on_job: 0
 history_age: 14
}
profit_log : _nameless.4.1.3 {
 stats_data: 0
 history_age: 0
}
trailer : _nameless.7.7.7 {
}
}
)SII";

const char* kSource = R"SII(SiiNunit
{
player : _nameless.5.9.9 {
 trucks: 1
 trucks[0]: _nameless.9.9.9
 truck_profit_logs: 0
 drivers: 0
}
vehicle : _nameless.9.9.9 {
 accessories: 3
 accessories[0]: _nameless.a.1
 accessories[1]: _nameless.a.1
 accessories[2]: _nameless.b.1
 fuel_relative: 0.5
 license_plate: "ABC-123"
}
vehicle_accessory : _nameless.a.1 {
 data_path: "/def/vehicle/truck/scania.s/data.sii"
 addon: _nameless.c.1
}
vehicle_addon_accessory : _nameless.c.1 {
 data_path: "/def/vehicle/addon/x/data.sii"
}
vehicle_wheel_accessory : _nameless.b.1 {
 data_path: "/def/vehicle/wheel/x/data.sii"
}
}
)SII";

std::string replaceAll(std::string value, const std::string& from, const std::string& to) {
    std::size_t pos = 0;
    while ((pos = value.find(from, pos)) != std::string::npos) {
        value.replace(pos, from.size(), to);
        pos += to.size();
    }
    return value;
}

std::size_t countOf(const std::string& value, const std::string& needle) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = value.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

TunerTestItem item(const char* name, bool ok, const std::string& detail = std::string()) {
    TunerTestItem result;
    result.name = name;
    result.ok = ok;
    result.detail = detail;
    return result;
}

}  // namespace

std::vector<TunerTestItem> runTruckTransferTests() {
    std::vector<TunerTestItem> results;
    const std::string target = kTarget;
    const std::string source = kSource;

    {
        TruckTransferRequest request;
        request.game=selectedGame()==GameId::Ets2?GameId::Ats:GameId::Ets2;
        auto refused=transferTruckToNewSave(request);
        results.push_back(item("truck-transfer-production-cross-game-refused",!refused.ok && refused.slotDir.empty(),refused.error));
        request.game=selectedGame();
        refused=transferTruckToNewSave(request);
        results.push_back(item("truck-transfer-production-unregistered-path-refused",!refused.ok && refused.slotDir.empty(),refused.error));
    }

    {
        std::string out,error;
        const auto empty=replaceAll(replaceAll(target,"trucks: 1\n trucks[0]: _nameless.1.1.1","trucks: 0"),
            "truck_profit_logs: 1\n truck_profit_logs[0]: _nameless.4.1.2","truck_profit_logs: 0");
        results.push_back(item("truck-transfer-empty-fleet",transferTruckText(source,empty,"_nameless.9.9.9","garage.koln",&out,&error) &&
            out.find("trucks[0]: _nameless.7fff.")!=std::string::npos,error));
        const auto sameId=replaceAll(target,"_nameless.1.1.1","_nameless.9.9.9");
        results.push_back(item("truck-transfer-source-id-exists-in-target",transferTruckText(source,sameId,"_nameless.9.9.9","garage.koln",&out,&error),error));
        const auto unowned=replaceAll(source,"trucks: 1\n trucks[0]: _nameless.9.9.9","trucks: 0");
        results.push_back(item("truck-transfer-unowned-refused",!transferTruckText(unowned,target,"_nameless.9.9.9","garage.koln",&out,&error) && out.empty(),error));
        const auto occupied=replaceAll(target,"drivers[1]: null","drivers[1]: driver.22");
        TransferInventory inventory;
        results.push_back(item("truck-transfer-preview-excludes-driver-occupied-slot",inspectTruckTransferText(occupied,&inventory,&error) && inventory.garages.empty(),error));
        const std::string meta="SiiNunit { save_container : a { version: 102 info_version: 1 dependencies: 1 dependencies[0]: \"dlc|test\" } }";
        results.push_back(item("truck-transfer-compatible-metadata",validateTruckTransferMetadata(meta,meta,&error),error));
        results.push_back(item("truck-transfer-missing-dependency",!validateTruckTransferMetadata(meta,replaceAll(meta,"dlc|test","dlc|other"),&error),error));
        results.push_back(item("truck-transfer-schema-version",!validateTruckTransferMetadata(meta,replaceAll(meta,"102","101"),&error),error));
    }

    {
        std::string out;
        std::string error;
        const std::string sourceBefore = source;
        const bool ok = transferTruckText(source, target, "_nameless.9.9.9", "garage.koln", &out, &error);
        const bool structure = ok && out.find("trucks: 2") != std::string::npos &&
            out.find("truck_profit_logs: 2") != std::string::npos &&
            out.find("vehicles[1]: _nameless.7fff.") != std::string::npos &&
            out.find("history_age: nil") != std::string::npos &&
            out.find("stats_data: 0") != std::string::npos &&
            source == sourceBefore;
        results.push_back(item("truck-transfer-valid-success", structure, ok ? error : error));
    }
    {
        std::string out;
        std::string error;
        const bool ok = transferTruckText(source, target, "_nameless.9.9.9", "garage.koln", &out, &error);
        const bool shared = ok && countOf(out, "vehicle_accessory : _nameless.7fff.") == 1 &&
            countOf(out, "vehicle_addon_accessory : _nameless.7fff.") == 1 &&
            countOf(out, "vehicle_wheel_accessory : _nameless.7fff.") == 1;
        results.push_back(item("truck-transfer-shared-accessories-remapped", shared, error));
    }
    {
        std::string collision = replaceAll(target, "trailer : _nameless.7.7.7 {",
                                            "trailer : _nameless.7fff.1 {");
        collision = replaceAll(collision, "trailers[0]: _nameless.7.7.7",
                               "trailers[0]: _nameless.7fff.1");
        std::string out;
        std::string error;
        const bool ok = transferTruckText(source, collision, "_nameless.9.9.9", "garage.koln", &out, &error);
        const bool safe = ok && out.find("vehicle : _nameless.7fff.0002") != std::string::npos &&
            out.find("trailer : _nameless.7fff.1") != std::string::npos;
        results.push_back(item("truck-transfer-fresh-id-collision-safe", safe, error));
    }
    {
        std::string out;
        std::string error;
        const bool ok = transferTruckText(source, target, "_nameless.9.9.9", "garage.koln", &out, &error);
        const bool preserved = ok && out.find("note: \"_nameless.9.9.9\"") != std::string::npos &&
            out.find("// _nameless.9.9.9 stays quoted or commented") != std::string::npos &&
            out.find("trucks[0]: _nameless.1.1.1") != std::string::npos &&
            out.find("drivers[0]: driver.11") != std::string::npos &&
            out.find("trailers[0]: _nameless.7.7.7") != std::string::npos;
        results.push_back(item("truck-transfer-preserves-unrelated-text", preserved, error));
    }
    {
        std::string broken = replaceAll(source, "accessories[2]: _nameless.b.1",
                                        "accessories[2]: _nameless.missing.1");
        std::string out = "stale";
        std::string error;
        const bool ok = transferTruckText(broken, target, "_nameless.9.9.9", "garage.koln", &out, &error);
        results.push_back(item("truck-transfer-missing-reference-rejected", !ok && out.empty() && !error.empty(), error));
    }
    {
        std::string foreign = replaceAll(source, "license_plate: \"ABC-123\"",
                                         "license_plate: \"ABC-123\"\n foreign: _nameless.f.1");
        foreign = replaceAll(foreign, "}\n}\n", "}\ntrailer : _nameless.f.1 {\n}\n}\n");
        std::string out = "stale";
        std::string error;
        const bool ok = transferTruckText(foreign, target, "_nameless.9.9.9", "garage.koln", &out, &error);
        results.push_back(item("truck-transfer-foreign-unit-rejected", !ok && out.empty() && !error.empty(), error));
    }
    {
        std::string duplicate=source;
        duplicate.insert(duplicate.rfind('}'),"\nvehicle : _nameless.9.9.9 { accessories: 0 }\n");
        std::string out;
        std::string error;
        const bool ok = transferTruckText(duplicate, target, "_nameless.9.9.9", "garage.koln", &out, &error);
        results.push_back(item("truck-transfer-duplicate-unit-id-rejected", !ok && out.empty(), error));
    }
    {
        std::string duplicate = replaceAll(target, "trucks[0]: _nameless.1.1.1",
                                           "trucks[0]: _nameless.1.1.1\n trucks[0]: _nameless.1.1.1");
        std::string out;
        std::string error;
        const bool ok = transferTruckText(source, duplicate, "_nameless.9.9.9", "garage.koln", &out, &error);
        results.push_back(item("truck-transfer-duplicate-array-index-rejected", !ok && out.empty(), error));
    }
    {
        std::string full = replaceAll(target, "vehicles[1]: null", "vehicles[1]: _nameless.1.1.1");
        full = replaceAll(full, "drivers[1]: null", "drivers[1]: driver.22");
        std::string out;
        std::string error;
        const bool ok = transferTruckText(source, full, "_nameless.9.9.9", "garage.koln", &out, &error);
        results.push_back(item("truck-transfer-no-free-garage-slot-rejected", !ok && out.empty(), error));
    }
    {
        std::string malformed = replaceAll(target, "trucks: 1", "trucks: 2");
        std::string out;
        std::string error;
        const bool ok = transferTruckText(source, malformed, "_nameless.9.9.9", "garage.koln", &out, &error);
        results.push_back(item("truck-transfer-malformed-count-rejected", !ok && out.empty(), error));
    }
    {
        TransferInventory inventory;
        std::string error;
        const bool ok = inspectTruckTransferText(source, &inventory, &error);
        results.push_back(item("truck-transfer-inspect-source", ok && inventory.trucks.size() == 1 &&
            inventory.trucks[0].accessories == 3 && inventory.trucks[0].label == "scania.s", error));
    }
    {
        TransferInventory inventory;
        std::string error;
        const bool ok = inspectTruckTransferText(target, &inventory, &error);
        bool found = false;
        for (const TransferGarage& g : inventory.garages) {
            if (g.id == "garage.koln" && g.freeSlots == 1) found = true;
        }
        results.push_back(item("truck-transfer-inspect-garage", ok && inventory.trucks.size() == 1 && found, error));
    }
    {
        std::string out = "stale";
        std::string error;
        const bool ok = transferTruckText(std::string("BSII") + std::string(40, '\0'), target,
                                          "_nameless.9.9.9", "garage.koln", &out, &error);
        results.push_back(item("truck-transfer-bsii-rejected", !ok && out.empty() && !error.empty(), error));
    }
    {
        const auto named=replaceAll(source,"_nameless.c.1","named.addon");
        std::string out,error;
        const bool ok=transferTruckText(named,target,"_nameless.9.9.9","garage.koln",&out,&error);
        results.push_back(item("truck-transfer-named-addon-cloned",ok && out.find("named.addon")==std::string::npos &&
            countOf(out,"vehicle_addon_accessory :")==1 && out.find("/def/vehicle/addon/x/data.sii")!=std::string::npos,error));
    }
    {
        const auto named=replaceAll(source,"_nameless.a.1","named.accessory");
        TransferInventory inventory;std::string out,error;
        const bool inspected=inspectTruckTransferText(named,&inventory,&error);
        const bool ok=transferTruckText(named,target,"_nameless.9.9.9","garage.koln",&out,&error);
        results.push_back(item("truck-transfer-named-accessory-array",inspected && ok && inventory.trucks[0].label=="scania.s" &&
            out.find("named.accessory")==std::string::npos,error));
    }
    {
        const auto named=replaceAll(source,"_nameless.c.1","named.addon");
        auto collision=target;
        const std::string untouched="trailer : named.addon { marker: \"target only\" }";
        collision.insert(collision.rfind('}'),untouched+"\n");
        std::string out,error;
        const bool ok=transferTruckText(named,collision,"_nameless.9.9.9","garage.koln",&out,&error);
        results.push_back(item("truck-transfer-named-collision-preserves-target",ok && out.find(untouched)!=std::string::npos &&
            out.find("addon: named.addon")==std::string::npos,error));
    }
    {
        auto foreign=replaceAll(source,"addon: _nameless.c.1","addon: named.trailer");
        foreign.insert(foreign.rfind('}'),"trailer : named.trailer {}\n");
        std::string out="stale",error;
        const bool ok=transferTruckText(foreign,target,"_nameless.9.9.9","garage.koln",&out,&error);
        results.push_back(item("truck-transfer-named-foreign-refused",!ok && out.empty() && error.find("unsupported")!=std::string::npos,error));
    }
    {
        auto cycle=replaceAll(source,"_nameless.c.1","named.addon");
        cycle=replaceAll(cycle,"/def/vehicle/addon/x/data.sii\"","/def/vehicle/addon/x/data.sii\"\n back: _nameless.a.1");
        std::string out="stale",error;
        const bool ok=transferTruckText(cycle,target,"_nameless.9.9.9","garage.koln",&out,&error);
        results.push_back(item("truck-transfer-named-cycle-refused",!ok && out.empty() && error.find("cycle")!=std::string::npos,error));
    }
    {
        std::string links="addon:";
        for(size_t i=0;i<65537;++i) links+=" _nameless.c.1";
        const auto excessive=replaceAll(source,"addon: _nameless.c.1",links);
        std::string out="stale",error;
        const bool ok=transferTruckText(excessive,target,"_nameless.9.9.9","garage.koln",&out,&error);
        results.push_back(item("truck-transfer-repeated-edge-limit",!ok && out.empty() && error.find("references")!=std::string::npos,error));
    }
    {
        std::string out="stale",error;
        const bool ok=transferTruckText(source,target,"_nameless.9.9.9","garage.koln",&out,&error,[]{return true;});
        TransferInventory inventory;inventory.trucks.push_back({"stale","stale",0});
        const bool inspected=inspectTruckTransferText(source,&inventory,&error,[]{return true;});
        results.push_back(item("truck-transfer-cancellation-clears-results",!ok && out.empty() && !inspected && inventory.trucks.empty(),error));
    }
    {
        const auto alias=replaceAll(target,"_nameless.7.7.7","_nameless.0.0000.7FFF.0001");
        std::string out,error;
        const bool ok=transferTruckText(source,alias,"_nameless.9.9.9","garage.koln",&out,&error);
        results.push_back(item("truck-transfer-numeric-id-alias-collision",ok &&
            out.find("vehicle : _nameless.7fff.0002")!=std::string::npos &&
            out.find("trailer : _nameless.0.0000.7FFF.0001")!=std::string::npos,error));
    }
    {
        auto duplicate=source;
        duplicate.insert(duplicate.rfind('}'),"vehicle : _nameless.0009.0009.0009 { accessories: 0 }\n");
        std::string out="stale",error;
        const bool ok=transferTruckText(duplicate,target,"_nameless.9.9.9","garage.koln",&out,&error);
        results.push_back(item("truck-transfer-numeric-duplicate-unit-refused",!ok && out.empty() &&
            error.find("duplicate")!=std::string::npos,error));
    }
    return results;
}

}  // namespace ets2
