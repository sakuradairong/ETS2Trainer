#pragma once
#include "savecopy.h"
#include "trucktransfer.h"
namespace ets2 {
struct TruckTransferRequest {
    GameId game = GameId::Ets2;
    SaveSlot source, target;
    std::string truckId, garageId, displayName;
};
bool inspectTruckTransferSlot(const SaveSlot&, TransferInventory*, std::string* error,
                              std::function<bool()> canceled = {});
bool inspectTruckTransferTarget(const SaveSlot& source, const SaveSlot& target,
                                TransferInventory*, std::string* error,
                                std::function<bool()> canceled = {});
SaveCopyResult transferTruckToNewSave(const TruckTransferRequest&, std::function<bool()> canceled = {});
}
