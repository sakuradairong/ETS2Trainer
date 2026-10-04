#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace ets2 {

struct TransferTruck {
    std::string id;
    std::string label;
    std::size_t accessories = 0;
};

struct TransferGarage {
    std::string id;
    std::size_t freeSlots = 0;
};

struct TransferInventory {
    std::vector<TransferTruck> trucks;
    std::vector<TransferGarage> garages;
};

// Read-only inspection of a decrypted SII text document.  Never touches disk.
bool inspectTruckTransferText(const std::string& text, TransferInventory* inventory,
                              std::string* error, std::function<bool()> canceled = {});

// Requires equal save schema versions and all source dependencies in the target.
bool validateTruckTransferMetadata(const std::string& source, const std::string& target,
                                   std::string* error);

// Pure in-memory transfer of one owned truck graph from source to target.
// Output is only written when the whole operation and final validation succeed.
bool transferTruckText(const std::string& source, const std::string& target,
                       const std::string& truckId, const std::string& garageId,
                       std::string* out, std::string* error, std::function<bool()> canceled = {});

}  // namespace ets2
