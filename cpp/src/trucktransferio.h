#pragma once
#include "savecopy.h"
#include "trucktransfer.h"
#include "fleet.h"
namespace ets2 {
struct TruckTransferRequest {
    GameId game = GameId::Ets2;
    SaveSlot source, target;
    std::string truckId, garageId, displayName;
};
struct FleetRequest {
    GameId game = GameId::Ets2;
    SaveSlot source;
    FleetExpansion expansion;
    std::string displayName;
};
struct PendingFleetInspection {
    bool ready = false, ok = false;
    GameId game = GameId::Ets2;
    uint64_t generation = 0;
    FleetInventory inventory;
    std::string error;
};
bool inspectFleetSlot(const SaveSlot&, FleetInventory*, std::string* error,
                     std::function<bool()> canceled = {});
SaveCopyResult fleetToNewSave(const FleetRequest&, std::function<bool()> canceled = {});
struct HomeAccessoryRequest {
    GameId game = GameId::Ets2;
    SaveSlot source;
    std::string truckId, unitId, originalPath, newPath, displayName;
    bool scaniaTvPreset = false;
    int tankCapacity = 0; // Nonzero: litres (diesel) or kWh (electric), based on the selected stock chassis.
};
struct PendingHomeAccessoryInspection {
    bool ready = false, ok = false;
    GameId game = GameId::Ets2;
    uint64_t generation = 0;
    HomeAccessoryInventory inventory;
    std::string error;
};
struct TrailerSwapRequest {
    GameId game = GameId::Ets2;
    SaveSlot source;
    std::string targetTrailerId, displayName;
};
struct PendingTrailerSwapInspection {
    bool ready = false, ok = false;
    GameId game = GameId::Ets2;
    uint64_t generation = 0;
    TrailerSwapInventory inventory;
    std::string error;
};
bool inspectTruckTransferSlot(const SaveSlot&, TransferInventory*, std::string* error,
                              std::function<bool()> canceled = {});
bool inspectTruckTransferTarget(const SaveSlot& source, const SaveSlot& target,
                                TransferInventory*, std::string* error,
                                std::function<bool()> canceled = {});
SaveCopyResult transferTruckToNewSave(const TruckTransferRequest&, std::function<bool()> canceled = {});
bool inspectHomeAccessoriesSlot(const SaveSlot&, HomeAccessoryInventory*, std::string* error,
                               std::function<bool()> canceled = {});
SaveCopyResult homeAccessoryToNewSave(const HomeAccessoryRequest&, std::function<bool()> canceled = {});
bool inspectTrailerSwapSlot(const SaveSlot&, TrailerSwapInventory*, std::string* error,
                            std::function<bool()> canceled = {});
SaveCopyResult trailerSwapToNewSave(const TrailerSwapRequest&, std::function<bool()> canceled = {});
}
