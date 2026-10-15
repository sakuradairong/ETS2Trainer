#include "fleettest.h"
#include "fleet.h"
#include <algorithm>

namespace ets2 {
namespace {
std::string fixture() {
    std::string text = R"SII(SiiNunit {
economy : economy.main {
 player: player.main
 game_time: 100
 event_queue: _nameless.1.1.1
 garages: 3
 garages[0]: garage.berlin
 garages[1]: garage.koln
 garages[2]: garage.zurich
 driver_pool: 3
 driver_pool[0]: driver.42
 driver_pool[1]: driver.43
 driver_pool[2]: driver.44
 drivers_offer: 0
}
player : player.main {
 trucks: 3
 trucks[0]: _nameless.2.1.1
 trucks[1]: _nameless.5.1.100
 trucks[2]: _nameless.5.1.101
 truck_profit_logs: 3
 truck_profit_logs[0]: _nameless.2.1.2
 truck_profit_logs[1]: _nameless.5.3.100
 truck_profit_logs[2]: _nameless.5.3.101
 drivers: 3
 drivers[0]: driver.11
 drivers[1]: driver.100
 drivers[2]: driver.101
 driver_flags: 3
 driver_flags[0]: 7
 driver_flags[1]: 0
 driver_flags[2]: 0
 driver_readiness_timer: 3
 driver_readiness_timer[0]: 9
 driver_readiness_timer[1]: 0
 driver_readiness_timer[2]: 0
 driver_undrivable_truck_timers: 3
 driver_undrivable_truck_timers[0]: 11
 driver_undrivable_truck_timers[1]: 0
 driver_undrivable_truck_timers[2]: 0
 current_job: null
}
garage : garage.berlin {
 status: 2
 vehicles: 3
 vehicles[0]: _nameless.2.1.1
 vehicles[1]: null
 vehicles[2]: null
 drivers: 3
 drivers[0]: driver.11
 drivers[1]: null
 drivers[2]: null
 trailers: 0
}
garage : garage.koln {
 status: 2
 vehicles: 3
 vehicles[0]: null
 vehicles[1]: null
 vehicles[2]: null
 drivers: 3
 drivers[0]: null
 drivers[1]: null
 drivers[2]: null
 trailers: 0
}
garage : garage.zurich {
 status: 2
 vehicles: 3
 vehicles[0]: _nameless.5.1.100
 vehicles[1]: _nameless.5.1.101
 vehicles[2]: null
 drivers: 3
 drivers[0]: driver.100
 drivers[1]: driver.101
 drivers[2]: null
}
driver_player : driver.11 { profit_log: _nameless.2.1.3 }
vehicle : _nameless.2.1.1 {
 accessories: 1
 accessories[0]: _nameless.2.1.4
 fuel_relative: 1
 odometer: 1234
}
vehicle_accessory : _nameless.2.1.4 { data_path: "/def/vehicle/truck/volvo.fh_2024/data.sii" refund: 0 }
profit_log : _nameless.2.1.2 { stats_data: 0 acc_distance_free: 0 acc_distance_on_job: 0 history_age: nil }
profit_log : _nameless.2.1.3 { stats_data: 0 acc_distance_free: 0 acc_distance_on_job: 0 history_age: nil }
economy_event_queue : _nameless.1.1.1 {
 data: 2
 data[0]: _nameless.3.1.1
 data[1]: _nameless.3.1.2
}
economy_event : _nameless.3.1.1 { time: 100 unit_link: company.test param: 2 }
economy_event : _nameless.3.1.2 { time: 200 unit_link: company.test param: 3 }
// Preserve unrelated content and quoted ID-shaped strings.
unrelated : keep.me { note: "driver.42 _nameless.2.1.1" value: 987 }
)SII";
    for (int i=42;i<=44;++i) {
        const auto n=std::to_string(i);
        text += "driver_ai : driver."+n+" {\n adr: 1\n long_dist: 1\n heavy: 0\n fragile: 0\n urgent: 0\n mechanical: 0\n"
            " hometown: \"\"\n current_city: \"\"\n old_hometown: \"\"\n state: 1\n on_duty_timer: 0\n extra_maintenance: 0\n"
            " driver_job: _nameless.4.1."+n+"\n profit_log: _nameless.4.2."+n+"\n experience_points: "+n+"\n"
            " training_policy: 0\n adopted_truck: null\n assigned_truck: null\n adopted_trailer: null\n assigned_trailer: null\n"
            " slot_truck_axle_count: 2\n slot_truck_mass: &461c4000\n slot_truck_efficiency: &3f800000\n}\n";
        text += "job_info : _nameless.4.1."+n+" { cargo: null source_company: null target_company: null special: null car: null start_time: 0 planned_distance_km: 0 }\n";
        text += "profit_log : _nameless.4.2."+n+" { stats_data: 0 acc_distance_free: 0 acc_distance_on_job: 0 history_age: nil }\n";
    }
    for (int i=100;i<=101;++i) {
        const auto n=std::to_string(i);
        text += "vehicle : _nameless.5.1."+n+" { accessories: 1 accessories[0]: _nameless.5.2."+n+" odometer: 500 }\n";
        text += "vehicle_accessory : _nameless.5.2."+n+" { data_path: \"/def/vehicle/truck/volvo.fh_2024/data.sii\" refund: 99 }\n";
        text += "profit_log : _nameless.5.3."+n+" { stats_data: 0 }\n";
        text += "driver_ai : driver."+n+" { state: 2 profit_log: _nameless.5.4."+n+
            " slot_truck_axle_count: 4 slot_truck_mass: &4640cc00 slot_truck_efficiency: &3f8ab021 }\n";
        text += "profit_log : _nameless.5.4."+n+" { stats_data: 0 }\n";
    }
    return text+"}\n";
}
std::string replace(std::string s,const std::string& from,const std::string& to) {
    const auto p=s.find(from);if(p!=std::string::npos)s.replace(p,from.size(),to);return s;
}
std::string inUnit(std::string s,const std::string& id,const std::string& from,const std::string& to) {
    const auto begin=s.find(" : "+id+" {");if(begin==std::string::npos)return s;
    const auto end=s.find('}',begin),pos=s.find(from,begin);
    if(pos<end)s.replace(pos,from.size(),to);return s;
}
}
std::vector<TunerTestItem> runFleetTests() {
    std::vector<TunerTestItem> tests;
    auto check=[&](const char* name,bool ok,const std::string& error="") {tests.push_back({name,ok,error});};
    const auto text=fixture();FleetInventory before;std::string error,out;
    const bool inspected=inspectFleetText(text,&before,&error);
    check("fleet-inspect-pool-and-garages",inspected && before.hiredDrivers==2 && before.availableDrivers.size()==3 &&
          before.trucks.size()==3 && before.garages.size()==3 && before.garages[0].freeSlots==2 &&
          before.availableDrivers[0].id=="driver.44",error);
    FleetExpansion request{"_nameless.2.1.1",before.snapshot,{{"garage.berlin",1},{"garage.koln",2}}};
    const bool expanded=expandFleetText(text,request,&out,&error);
    FleetInventory after;
    const bool rechecked=expanded && inspectFleetText(out,&after,&error);
    check("fleet-batch-pairs-and-exhausts-pool",rechecked && after.hiredDrivers==5 && after.trucks.size()==6 &&
          after.availableDrivers.empty() && after.garages[0].freeSlots==1 && after.garages[1].freeSlots==1,error);
    check("fleet-preserves-player-truck-flags-and-unrelated-bytes",expanded &&
          out.find("driver_flags[0]: 7")!=std::string::npos && out.find("driver_readiness_timer[0]: 9")!=std::string::npos &&
          out.find("driver_undrivable_truck_timers[0]: 11")!=std::string::npos &&
          out.find("unrelated : keep.me { note: \"driver.42 _nameless.2.1.1\" value: 987 }")!=std::string::npos &&
          out.find("vehicle : _nameless.2.1.1 {\n accessories: 1\n accessories[0]: _nameless.2.1.4\n fuel_relative: 1\n odometer: 1234\n}")!=std::string::npos);
    std::size_t events=0,pos=0;
    while((pos=out.find("\n time: 101\n unit_link: driver.",pos))!=std::string::npos) {++events;++pos;}
    check("fleet-schedules-one-normal-update-per-driver",events==3 && out.find("data: 5")!=std::string::npos);
    auto refuse=[&](const char* name,const std::string& source,FleetExpansion r) {
        std::string result="stale",detail;
        check(name,!expandFleetText(source,r,&result,&detail) && result.empty() && !detail.empty(),detail);
    };
    refuse("fleet-refuses-stale-preview",text+"\n",request);
    auto invalid=request;invalid.allocations={{"garage.berlin",3}};refuse("fleet-refuses-overfilled-garage",text,invalid);
    invalid=request;invalid.allocations={{"garage.berlin",1},{"garage.berlin",1}};refuse("fleet-refuses-duplicate-target",text,invalid);
    invalid=request;invalid.allocations={{"garage.koln",-1}};refuse("fleet-refuses-negative-count",text,invalid);
    invalid=request;invalid.allocations={{"garage.koln",0}};refuse("fleet-refuses-zero-count",text,invalid);
    invalid=request;invalid.allocations={{"garage.berlin",2},{"garage.koln",2}};refuse("fleet-refuses-insufficient-drivers",text,invalid);
    invalid=request;invalid.truckId="_nameless.404";refuse("fleet-refuses-unowned-template",text,invalid);
    out="stale";check("fleet-cancel-does-not-return-partial-save",!expandFleetText(text,request,&out,&error,[]{return true;}) && out.empty(),error);
    for(const auto& corruption:std::vector<std::pair<std::string,std::string>>{
        {"driver_flags: 3","driver_flags: 2"},
        {"vehicles[1]: null","vehicles[1]: _nameless.2.1.1"},
        {"driver_pool[2]: driver.44","driver_pool[2]: driver.42"},
        {"time: 200","time: 99"},
        {"data[1]: _nameless.3.1.2","data[1]: _nameless.3.1.1"},
        {"status: 2","status: 9"},
        {"player: player.main","player: player.other"}}) {
        const auto bad=replace(text,corruption.first,corruption.second);FleetInventory inventory;
        check(("fleet-invalid-"+corruption.first).c_str(),!inspectFleetText(bad,&inventory,&error) && inventory.snapshot.empty(),error);
    }
    auto busy=replace(text,"hometown: \"\"","hometown: berlin");
    FleetInventory limited;check("fleet-excludes-nonidle-pool-driver",inspectFleetText(busy,&limited,&error) && limited.availableDrivers.size()==2,error);
    check("fleet-cache-two-donors-and-price-independent",inspected && before.truckCaches.size()==3 &&
          before.truckCaches[0].supported && before.truckCaches[0].donors==2 && before.truckCaches[0].axles==4,error);
    check("fleet-hires-initialize-cache",rechecked && after.employees.size()==5 &&
          std::all_of(after.employees.begin(),after.employees.end(),[](const FleetEmployee& d){return d.cacheVerified;}),error);
    // Keep the successful expansion; later refusal tests deliberately clear their output.
    std::string expandedText;
    const bool haveExpanded=expandFleetText(text,request,&expandedText,&error);
    auto damaged=inUnit(expandedText,"driver.44","slot_truck_axle_count: 4","slot_truck_axle_count: 2");
    damaged=inUnit(damaged,"driver.44","slot_truck_mass: &4640cc00","slot_truck_mass: &461c4000");
    damaged=inUnit(damaged,"driver.44","slot_truck_efficiency: &3f8ab021","slot_truck_efficiency: &3f800000");
    FleetInventory repairInv;const bool repairRead=haveExpanded && inspectFleetText(damaged,&repairInv,&error);
    FleetExpansion repair;repair.expectedSnapshot=repairInv.snapshot;repair.repairDriverIds={"driver.44"};
    check("fleet-default-cache-detected",repairRead && std::count_if(repairInv.employees.begin(),repairInv.employees.end(),
          [](const FleetEmployee& d){return d.cacheRepairable;})==1,error);
    check("fleet-repair-only-preserves-every-other-byte",repairRead && expandFleetText(damaged,repair,&out,&error) && out==expandedText,error);
    auto duplicateRepair=repair;duplicateRepair.repairDriverIds.push_back("driver.44");
    refuse("fleet-rejects-duplicate-repair",damaged,duplicateRepair);
    auto nonDefaultRepair=repair;nonDefaultRepair.expectedSnapshot=before.snapshot;nonDefaultRepair.repairDriverIds={"driver.100"};
    refuse("fleet-rejects-repair-of-verified-cache",text,nonDefaultRepair);
    auto conflict=inUnit(text,"driver.100","slot_truck_mass: &4640cc00","slot_truck_mass: &4640cc01");
    FleetInventory conflictInv;const bool conflictRead=inspectFleetText(conflict,&conflictInv,&error);
    auto conflictRequest=request;conflictRequest.expectedSnapshot=conflictInv.snapshot;
    check("fleet-conflicting-donors-never-majority-vote",conflictRead && !conflictInv.truckCaches[0].supported,error);
    refuse("fleet-conflicting-cache-blocks-hire",conflict,conflictRequest);
    auto single=inUnit(text,"_nameless.5.2.100","refund: 99","refund: 99 custom: 1");
    FleetInventory singleInv;const bool singleRead=inspectFleetText(single,&singleInv,&error);
    check("fleet-custom-accessory-value-prevents-false-match",singleRead && !singleInv.truckCaches[0].supported &&
          singleInv.truckCaches[0].donors==1,error);
    auto nan=inUnit(text,"driver.100","slot_truck_mass: &4640cc00","slot_truck_mass: &7fc00000");
    check("fleet-refuses-nan-cache",!inspectFleetText(nan,&limited,&error),error);
    auto offered=replace(text,"driver_pool: 3","driver_pool: 2");
    offered=replace(offered," driver_pool[2]: driver.44\n","");
    offered=replace(offered,"drivers_offer: 0","drivers_offer: 1\n drivers_offer[0]: driver.44");
    FleetInventory offerInv;const bool offerRead=inspectFleetText(offered,&offerInv,&error);
    auto offerRequest=request;offerRequest.expectedSnapshot=offerInv.snapshot;
    check("fleet-offer-is-eligible-and-xp-sorted",offerRead && offerInv.availableDrivers.size()==3 &&
          offerInv.availableDrivers[0].id=="driver.44" && offerInv.availableDrivers[0].offered,error);
    check("fleet-hiring-removes-offers-and-pool-refs",offerRead && expandFleetText(offered,offerRequest,&out,&error) &&
          out.find("drivers_offer: 0")!=std::string::npos && out.find("driver_pool: 0")!=std::string::npos,error);
    auto overlap=replace(text,"drivers_offer: 0","drivers_offer: 1\n drivers_offer[0]: driver.44");
    check("fleet-rejects-pool-offer-overlap",!inspectFleetText(overlap,&limited,&error),error);
    auto idle=inUnit(text,"garage.berlin","drivers[0]: driver.11","drivers[0]: null");
    idle=inUnit(idle,"garage.zurich","drivers[2]: null","drivers[2]: driver.11");
    FleetInventory idleInv;const bool idleRead=inspectFleetText(idle,&idleInv,&error);
    FleetExpansion idleRequest;idleRequest.expectedSnapshot=idleInv.snapshot;idleRequest.existingTruckIds={"_nameless.2.1.1"};
    FleetInventory assigned;
    check("fleet-assigns-existing-truck-without-cloning",idleRead && idleInv.idleTrucks.size()==1 &&
          expandFleetText(idle,idleRequest,&out,&error) && inspectFleetText(out,&assigned,&error) &&
          assigned.trucks.size()==idleInv.trucks.size() && assigned.hiredDrivers==idleInv.hiredDrivers+1 &&
          assigned.idleTrucks.empty(),error);
    auto duplicateIdle=idleRequest;duplicateIdle.existingTruckIds.push_back("_nameless.2.1.1");
    refuse("fleet-rejects-duplicate-existing-truck",idle,duplicateIdle);
    auto occupied=idleRequest;occupied.existingTruckIds={"_nameless.5.1.100"};
    refuse("fleet-rejects-occupied-truck",idle,occupied);
    auto combined=idleRequest;combined.allocations={{"garage.koln",1}};combined.truckId="_nameless.2.1.1";
    check("fleet-combines-existing-and-cloned-trucks",expandFleetText(idle,combined,&out,&error) && inspectFleetText(out,&assigned,&error) &&
          assigned.trucks.size()==idleInv.trucks.size()+1 && assigned.hiredDrivers==idleInv.hiredDrivers+2,error);
    auto money=inUnit(text,"_nameless.5.4.100","stats_data: 0","stats_data: 2 stats_data[0]: profit.a stats_data[1]: profit.b");
    money.insert(money.rfind('}'),
        "profit_log_entry : profit.a { revenue: 100 wage: 200 maintenance: 10 fuel: 2 }\n"
        "profit_log_entry : profit.b { revenue: 600 wage: 100 maintenance: 20 fuel: 8 }\n");
    FleetInventory financial;
    check("fleet-finance-sums-positive-and-negative-net",inspectFleetText(money,&financial,&error) &&
          financial.profit.entries==2 && financial.profit.revenue==700 && financial.profit.cost==340 &&
          financial.profit.net==360 && financial.garages.back().profit.net==360,error);
    const auto overflow=replace(money,"revenue: 100 wage: 200","revenue: 9223372036854775807 wage: -1");
    check("fleet-finance-rejects-overflow",!inspectFleetText(overflow,&limited,&error),error);
    const auto malformed=replace(money,"revenue: 100","revenue: 100oops");
    check("fleet-finance-rejects-malformed-amount",!inspectFleetText(malformed,&limited,&error),error);
    const auto sharedEntry=inUnit(money,"_nameless.5.4.101","stats_data: 0","stats_data: 1 stats_data[0]: profit.a");
    check("fleet-finance-refuses-shared-entry-across-drivers",!inspectFleetText(sharedEntry,&limited,&error),error);
    auto unowned=inUnit(text,"garage.koln","status: 2","status: 0");
    unowned=inUnit(unowned,"garage.koln","vehicles: 3\n vehicles[0]: null\n vehicles[1]: null\n vehicles[2]: null","vehicles: 0");
    unowned=inUnit(unowned,"garage.koln","drivers: 3\n drivers[0]: null\n drivers[1]: null\n drivers[2]: null","drivers: 0");
    FleetInventory unownedInv;const bool unownedRead=inspectFleetText(unowned,&unownedInv,&error);
    check("fleet-unowned-garage-shows-zero-slots",unownedRead && !unownedInv.garages[1].purchased && unownedInv.garages[1].slots==0,error);
    FleetExpansion purchase;purchase.expectedSnapshot=unownedInv.snapshot;purchase.garageChanges={{"garage.koln",3}};
    auto expectedPurchase=inUnit(unowned,"garage.koln","status: 0","status: 2");
    expectedPurchase=inUnit(expectedPurchase,"garage.koln","vehicles: 0","vehicles: 3\n vehicles[0]: null\n vehicles[1]: null\n vehicles[2]: null");
    expectedPurchase=inUnit(expectedPurchase,"garage.koln","drivers: 0","drivers: 3\n drivers[0]: null\n drivers[1]: null\n drivers[2]: null");
    check("fleet-purchase-only-changes-status-and-slot-arrays",unownedRead && expandFleetText(unowned,purchase,&out,&error) && out==expectedPurchase,error);
    auto purchaseFive=purchase;purchaseFive.garageChanges[0].targetSlots=5;
    FleetInventory bought;
    check("fleet-purchase-five-empty-slots",expandFleetText(unowned,purchaseFive,&out,&error) && inspectFleetText(out,&bought,&error) &&
          bought.garages[1].purchased && bought.garages[1].slots==5 && bought.garages[1].freeSlots==5 &&
          bought.hiredDrivers==unownedInv.hiredDrivers && bought.trucks.size()==unownedInv.trucks.size(),error);
    auto purchaseAndHire=purchaseFive;purchaseAndHire.truckId="_nameless.2.1.1";purchaseAndHire.allocations={{"garage.koln",3}};
    check("fleet-purchase-and-hire-in-one-save",expandFleetText(unowned,purchaseAndHire,&out,&error) && inspectFleetText(out,&bought,&error) &&
          bought.garages[1].slots==5 && bought.garages[1].trucks==3 && bought.garages[1].drivers==3 && bought.garages[1].freeSlots==2,error);
    auto undeclared=purchaseAndHire;undeclared.garageChanges.clear();
    refuse("fleet-refuses-implicit-garage-purchase",unowned,undeclared);
    auto upgrade=request;upgrade.allocations={{"garage.berlin",3}};upgrade.garageChanges={{"garage.berlin",5}};
    check("fleet-upgrade-fills-old-and-new-slots-without-moving-player",expandFleetText(text,upgrade,&out,&error) &&
          inspectFleetText(out,&bought,&error) && bought.garages[0].slots==5 && bought.garages[0].trucks==4 &&
          bought.garages[0].drivers==4 && bought.garages[0].freeSlots==1 &&
          out.find("vehicles[0]: _nameless.2.1.1")!=std::string::npos && out.find("drivers[0]: driver.11")!=std::string::npos,error);
    auto starter=inUnit(text,"garage.berlin","status: 2","status: 6");
    starter=inUnit(starter,"garage.berlin","vehicles: 3\n vehicles[0]: _nameless.2.1.1\n vehicles[1]: null\n vehicles[2]: null",
        "vehicles: 1\n vehicles[0]: _nameless.2.1.1");
    starter=inUnit(starter,"garage.berlin","drivers: 3\n drivers[0]: driver.11\n drivers[1]: null\n drivers[2]: null",
        "drivers: 1\n drivers[0]: driver.11");
    FleetInventory starterInv;const bool starterRead=inspectFleetText(starter,&starterInv,&error);
    FleetExpansion starterUpgrade;starterUpgrade.expectedSnapshot=starterInv.snapshot;starterUpgrade.garageChanges={{"garage.berlin",3}};
    check("fleet-upgrades-one-slot-starter-garage",starterRead && expandFleetText(starter,starterUpgrade,&out,&error) &&
          inspectFleetText(out,&bought,&error) && bought.garages[0].slots==3 && bought.garages[0].trucks==1 && bought.garages[0].freeSlots==2,error);
    auto combinedGarage=combined;combinedGarage.garageChanges={{"garage.berlin",5}};combinedGarage.allocations={{"garage.berlin",2}};
    check("fleet-upgrade-existing-hire-and-clones-share-array-edit",expandFleetText(idle,combinedGarage,&out,&error) &&
          inspectFleetText(out,&bought,&error) && bought.garages[0].slots==5 && bought.garages[0].trucks==3 &&
          bought.garages[0].drivers==3 && bought.idleTrucks.empty(),error);
    for(const int invalidCapacity:{-1,0,1,2,4,6}) {
        auto bad=purchase;bad.garageChanges[0].targetSlots=invalidCapacity;
        refuse(("fleet-refuses-garage-capacity-"+std::to_string(invalidCapacity)).c_str(),unowned,bad);
    }
    auto duplicateGarage=purchase;duplicateGarage.garageChanges.push_back({"garage.koln",5});
    refuse("fleet-refuses-duplicate-garage-change",unowned,duplicateGarage);
    auto unknownGarage=purchase;unknownGarage.garageChanges[0].garageId="garage.missing";
    refuse("fleet-refuses-unlisted-garage-purchase",unowned,unknownGarage);
    auto noGrowth=purchase;noGrowth.expectedSnapshot=before.snapshot;
    refuse("fleet-refuses-noop-or-downgrade",text,noGrowth);
    auto noDrivers=replace(unowned,"driver_pool: 3\n driver_pool[0]: driver.42\n driver_pool[1]: driver.43\n driver_pool[2]: driver.44","driver_pool: 0");
    FleetInventory noDriversInv;const bool noDriversRead=inspectFleetText(noDrivers,&noDriversInv,&error);
    auto noDriverPurchase=purchase;noDriverPurchase.expectedSnapshot=noDriversInv.snapshot;
    check("fleet-purchase-needs-no-driver-or-template",noDriversRead && noDriversInv.availableDrivers.empty() &&
          expandFleetText(noDrivers,noDriverPurchase,&out,&error),error);
    return tests;
}
}
