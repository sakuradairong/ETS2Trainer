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

// One editable Home accessory reachable from exactly one player truck.
struct HomeAccessoryEntry {
    std::string truckId;     // owning vehicle unit id
    std::string truckLabel;  // readable truck name derived from its data_path
    std::string unitId;      // accessory unit id
    std::string type;        // accessory unit type
    std::string subtype;     // optional `subtype` property when present
    std::string dataPath;    // current data_path token value (unquoted)
    bool        shared = false;  // referenced by more than one unit; never editable
};

struct HomeAccessoryInventory {
    std::vector<HomeAccessoryEntry> entries;
};

// Read-only enumeration of player-owned truck accessories.  Never touches disk.
bool inspectHomeAccessoriesText(const std::string& text, HomeAccessoryInventory* inventory,
                                std::string* error, std::function<bool()> canceled = {});

// Bounded /def/... .sii validation for a single Home accessory data_path.
bool validateHomeAccessoryPath(const std::string& path, std::string* error);

// Replaces only the selected accessory's data_path token; all other bytes and the
// truck graph stay identical.  Output is written only on success.
bool replaceHomeAccessoryPath(const std::string& text, const std::string& truckId,
                              const std::string& unitId, const std::string& newPath,
                              std::string* out, std::string* error,
                              std::function<bool()> canceled = {});

// Experimental local hookup preset. Appends to the economy's version-0 bus log;
// preserves other entries and refuses active/unknown log schemas or occupied slots.
bool installScaniaTvPresetText(const std::string& text, const std::string& truckId,
                               std::string* out, std::string* error,
                               std::function<bool()> canceled = {});
bool validateScaniaTvPresetInfo(const std::string& info,std::string* error);

// ---------- 更换挂车 ----------
// One trailer unit inside the current save, classified for the swap dialog.
struct TrailerSwapEntry {
    std::string id;           // trailer unit id
    std::string label;        // definition-derived name (scs.box.single_4.dryvan ...)
    std::string plate;        // cleaned license_plate text ("<font>GS 56 87</font>|denmark" -> "GS 56 87 · denmark")
    float cargoMass = 0.f;    // kg, from cargo_mass hexfloat/decimal
    bool owned = false;       // listed in player.trailers
    bool current = false;     // currently coupled to the active truck
    bool job = false;         // referenced by the active player_job.company_trailer
    bool blocked = false;     // never selectable
    std::string blockReason;
};

struct TrailerSwapInventory {
    bool hasCurrent = false;          // active player_vehicles.trailer non-null
    std::string currentId, currentLabel, currentPlate;
    std::string truckLabel;           // active truck label for the header line
    bool onJob = false, onlineJob = false;
    std::string jobCargo;
    std::string swapBlockReason;      // non-empty: the whole operation is impossible
    std::vector<TrailerSwapEntry> entries;
};

// Read-only classification of every trailer unit and the active coupling.
bool inspectTrailerSwapText(const std::string& text, TrailerSwapInventory* inventory,
                            std::string* error, std::function<bool()> canceled = {});

// Rewrites only the active coupling (plus cargo transfer and target slot cleanup);
// garage trailer lists and all unrelated bytes stay identical.  Output on success only.
bool swapTrailerText(const std::string& text, const std::string& targetTrailerId,
                     std::string* out, std::string* error,
                     std::function<bool()> canceled = {});

// Requires equal save schema versions and all source dependencies in the target.
bool validateTruckTransferMetadata(const std::string& source, const std::string& target,
                                   std::string* error);

// Pure in-memory transfer of one owned truck graph from source to target.
// Output is only written when the whole operation and final validation succeed.
bool transferTruckText(const std::string& source, const std::string& target,
                       const std::string& truckId, const std::string& garageId,
                       std::string* out, std::string* error, std::function<bool()> canceled = {});

}  // namespace ets2
