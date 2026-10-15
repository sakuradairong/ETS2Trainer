#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace ets2 {
struct FuelTankMod {
    std::string originalPath,chassisPath,chassisText,displayName,fileName;
    int originalCapacity=0,capacity=0; // Litres for diesel, kWh for electric chassis.
    bool electric=false;
    std::vector<std::uint8_t> archive;
};
bool fuelTankIsElectric(const std::string& originalPath);
bool validateFuelTankPowertrain(const FuelTankMod& mod,const std::vector<std::string>& enginePaths,std::string* error);
bool buildFuelTankMod(const std::string& originalPath,int capacity,FuelTankMod* out,std::string* error);
}
