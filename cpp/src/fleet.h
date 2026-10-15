#pragma once
#include "trucktransfer.h"
#include <cstdint>

namespace ets2 {
struct FleetDriver {
    std::string id;
    int experience = 0;
    bool offered = false;
};
struct FleetProfit {
    std::int64_t revenue = 0, cost = 0, net = 0;
    std::size_t entries = 0;
};
struct FleetGarage {
    std::string id, city;
    std::size_t slots = 0, trucks = 0, drivers = 0, freeSlots = 0;
    bool purchased = false;
    std::size_t idleTrucks = 0;
    FleetProfit profit;
};
struct FleetTruckCache {
    std::string truckId;
    bool supported = false;
    int axles = 0;
    float mass = 0, efficiency = 0;
    std::size_t donors = 0;
    std::string note;
};
struct FleetEmployee {
    std::string id, garageId, truckId;
    int state = 0;
    FleetProfit profit;
    bool cacheVerified = false, cacheRepairable = false;
    std::string cacheNote;
};
struct FleetIdleTruck {
    std::string garageId, truckId;
    std::size_t slot = 0;
};
struct FleetInventory {
    std::string snapshot;
    std::vector<TransferTruck> trucks;
    std::vector<FleetDriver> availableDrivers; // validated pool/offers, highest XP first
    std::vector<FleetGarage> garages;
    std::size_t hiredDrivers = 0;
    std::vector<FleetTruckCache> truckCaches;
    std::vector<FleetEmployee> employees;
    std::vector<FleetIdleTruck> idleTrucks;
    FleetProfit profit;
};
struct FleetAllocation {
    std::string garageId;
    int count = 0;
};
struct FleetGarageChange {
    std::string garageId;
    int targetSlots = 0; // Purchase/upgrade to 3 or 5; no shrinking or implicit purchases.
};
struct FleetExpansion {
    std::string truckId, expectedSnapshot;
    std::vector<FleetAllocation> allocations;
    // Existing trucks receive drivers first, followed by clones in allocation order.
    std::vector<std::string> existingTruckIds;
    std::vector<std::string> repairDriverIds;
    std::vector<FleetGarageChange> garageChanges;
};
// No filesystem or process access. Unknown/inconsistent schemas fail closed.
bool inspectFleetText(const std::string&, FleetInventory*, std::string* error,
                      std::function<bool()> canceled = {});
bool expandFleetText(const std::string&, const FleetExpansion&, std::string* out,
                     std::string* error, std::function<bool()> canceled = {});
}
