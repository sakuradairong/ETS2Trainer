#include "fueltanktest.h"
#include "fueltank.h"
#include "fueltank_definitions.h"
#include <regex>
namespace ets2 {
std::vector<TunerTestItem> runFuelTankTests() {
    std::vector<TunerTestItem> tests;
    auto check=[&](const char* name,bool ok,const std::string& detail=""){tests.push_back({name,ok,detail});};
    const std::string path="/def/vehicle/truck/volvo.fh_2024/chassis/6x4.sii";
    FuelTankMod mod,again;std::string error;
    if(std::size(kFuelTankDefinitions)==0) {
        check("fuel-missing-local-catalog-refuses-generation",!buildFuelTankMod(path,5000,&mod,&error)&&mod.archive.empty()&&
            error.find("未包含本地原厂底盘定义")!=std::string::npos,error);
        check("fuel-missing-local-catalog-has-no-electric-support",!fuelTankIsElectric("/def/vehicle/truck/scania.s_2024e/chassis/4x2.sii"));
        return tests;
    }
    const bool built=buildFuelTankMod(path,3000,&mod,&error);
    check("fuel-builder-volvo-800-to-3000",built&&!mod.electric&&mod.originalCapacity==800&&mod.capacity==3000,error);
    check("fuel-builder-unique-same-folder",built&&mod.chassisPath!=path&&mod.chassisPath.substr(0,mod.chassisPath.find_last_of('/'))==path.substr(0,path.find_last_of('/')));
    check("fuel-builder-deterministic",buildFuelTankMod(path,3000,&again,&error)&&again.archive==mod.archive&&again.chassisPath==mod.chassisPath,error);
    check("fuel-builder-capacity-identity",buildFuelTankMod(path,5000,&again,&error)&&again.archive!=mod.archive&&again.chassisPath!=mod.chassisPath);
    bool all=true;int count=0;
    for(const auto& item:kFuelTankDefinitions) {
        FuelTankMod variant;
        if(!buildFuelTankMod(item.path,10000,&variant,&error)){all=false;break;}
        auto normalize=[](const std::string& s) {
            auto out=std::regex_replace(s,std::regex(R"(accessory_chassis_data[ \t]*:[ \t]*[a-zA-Z0-9_.]+)"),"accessory_chassis_data: ID");
            out=std::regex_replace(out,std::regex(R"(tank_size[ \t]*:[ \t]*[0-9]+(?:\.[0-9]+)?)"),"tank_size: N");
            return std::regex_replace(out,std::regex(R"( \[10000 (?:L|kWh)\])"),"");
        };
        if(variant.electric!=item.electric||normalize(item.text)!=normalize(variant.chassisText)){all=false;break;}
        std::smatch match;
        if(!std::regex_search(variant.chassisText,match,std::regex(R"(accessory_chassis_data\s*:\s*([a-z0-9_.]+))"))){all=false;break;}
        const auto id=match[1].str();std::size_t start=0;
        for(std::size_t pos=0;pos<=id.size();++pos)if(pos==id.size()||id[pos]=='.'){if(pos-start>12||pos==start)all=false;start=pos+1;}
        ++count;
    }
    check("fuel-builder-all-catalog-preserves-other-fields",all&&count==std::size(kFuelTankDefinitions),error);
    const std::string electricPath="/def/vehicle/truck/scania.s_2024e/chassis/4x2.sii";
    const std::string engine="/def/vehicle/truck/scania.s_2024e/engine/450kw.sii";
    const std::string diesel="/def/vehicle/truck/volvo.fh_2024/engine/d17a780.sii";
    FuelTankMod battery;
    check("battery-builder-scania-624-to-5000-kwh",buildFuelTankMod(electricPath,5000,&battery,&error)&&battery.electric&&
        battery.originalCapacity==624&&battery.capacity==5000,error);
    check("battery-preserves-kwh-comment-and-includes",battery.chassisText.find("tank_size: 5000\t#kwh")!=std::string::npos&&
        battery.chassisText.find("@include \"sound.sui\"")!=std::string::npos);
    check("battery-display-name-uses-kwh",battery.displayName.find("Battery 5000 kWh")!=std::string::npos&&
        battery.chassisText.find("[5000 kWh]")!=std::string::npos);
    check("battery-chassis-type-lookup",fuelTankIsElectric(electricPath)&&!fuelTankIsElectric(path)&&!fuelTankIsElectric("unknown"));
    check("battery-nonincrease-refused-with-kwh",!buildFuelTankMod(electricPath,624,&again,&error)&&error.find("624 kWh")!=std::string::npos);
    check("battery-invalid-capacity-has-correct-unit",!buildFuelTankMod(electricPath,10001,&again,&error)&&error.find("kWh")!=std::string::npos);
    check("battery-electric-engines-supported",validateFuelTankPowertrain(battery,{engine},&error)&&
        validateFuelTankPowertrain(battery,{"/def/vehicle/truck/scania.s_2024e/engine/400kw.sii"},&error),error);
    check("battery-diesel-engine-refused",!validateFuelTankPowertrain(battery,{diesel},&error),error);
    check("battery-unknown-electric-engine-refused",!validateFuelTankPowertrain(battery,{"/def/vehicle/truck/scania.s_2024e/engine/customkw.sii"},&error),error);
    check("fuel-electric-engine-refused",!validateFuelTankPowertrain(mod,{engine},&error),error);
    check("fuel-diesel-engine-supported",validateFuelTankPowertrain(mod,{diesel},&error),error);
    check("battery-missing-or-duplicate-engines-refused",!validateFuelTankPowertrain(battery,{},&error)&&
        !validateFuelTankPowertrain(battery,{engine,engine},&error),error);
    check("fuel-builder-unknown-refused",!buildFuelTankMod("/def/vehicle/truck/unknown/chassis/4x2.sii",3000,&again,&error)&&again.archive.empty());
    check("fuel-builder-traversal-refused",!buildFuelTankMod("/def/../chassis/test.sii",3000,&again,&error));
    check("fuel-builder-nonincrease-refused",!buildFuelTankMod(path,800,&again,&error)&&!buildFuelTankMod(path,700,&again,&error));
    check("fuel-builder-capacity-bounds",!buildFuelTankMod(path,99,&again,&error)&&!buildFuelTankMod(path,10001,&again,&error));
    check("fuel-builder-null-output",!buildFuelTankMod(path,3000,nullptr,&error));
    return tests;
}
}
