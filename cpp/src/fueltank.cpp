#include "fueltank.h"
#include "fueltank_definitions.h"
#include <algorithm>
#include <cstdio>
#include <regex>
#include <stdexcept>
namespace ets2 {
namespace {
std::uint32_t crcFuel(const std::string& text) {
    std::uint32_t crc=0xffffffffu;
    for(unsigned char c:text){crc^=c;for(int i=0;i<8;++i)crc=(crc>>1)^((crc&1)?0xedb88320u:0u);}
    return ~crc;
}
void word(std::vector<std::uint8_t>& out,std::uint32_t n,int bytes) {
    for(int i=0;i<bytes;++i)out.push_back(static_cast<std::uint8_t>(n>>(8*i)));
}
struct Member {std::string path,text;std::uint32_t offset=0;};
std::vector<std::uint8_t> zipFuel(std::vector<Member> files) {
    std::vector<std::uint8_t> out;
    for(auto& file:files) {
        file.offset=static_cast<std::uint32_t>(out.size());
        word(out,0x04034b50,4);word(out,20,2);word(out,0x800,2);word(out,0,2);
        word(out,0,2);word(out,0x21,2);word(out,crcFuel(file.text),4);
        word(out,static_cast<std::uint32_t>(file.text.size()),4);word(out,static_cast<std::uint32_t>(file.text.size()),4);
        word(out,static_cast<std::uint32_t>(file.path.size()),2);word(out,0,2);
        out.insert(out.end(),file.path.begin(),file.path.end());out.insert(out.end(),file.text.begin(),file.text.end());
    }
    const auto start=static_cast<std::uint32_t>(out.size());
    for(const auto& file:files) {
        word(out,0x02014b50,4);word(out,20,2);word(out,20,2);word(out,0x800,2);word(out,0,2);
        word(out,0,2);word(out,0x21,2);word(out,crcFuel(file.text),4);
        word(out,static_cast<std::uint32_t>(file.text.size()),4);word(out,static_cast<std::uint32_t>(file.text.size()),4);
        word(out,static_cast<std::uint32_t>(file.path.size()),2);word(out,0,2);word(out,0,2);word(out,0,2);
        word(out,0,2);word(out,0,4);word(out,file.offset,4);out.insert(out.end(),file.path.begin(),file.path.end());
    }
    const auto size=static_cast<std::uint32_t>(out.size())-start;
    word(out,0x06054b50,4);word(out,0,2);word(out,0,2);word(out,static_cast<std::uint32_t>(files.size()),2);
    word(out,static_cast<std::uint32_t>(files.size()),2);word(out,size,4);word(out,start,4);word(out,0,2);
    return out;
}
struct Edit {std::size_t at,size;std::string text;};
std::smatch uniqueField(const std::string& text,const std::regex& expression) {
    std::sregex_iterator begin(text.begin(),text.end(),expression),end;
    if(begin==end)throw std::runtime_error("原厂底盘定义缺少必要字段");
    const auto result=*begin;
    if(++begin!=end)throw std::runtime_error("原厂底盘定义含重复字段");
    return result;
}
}
bool fuelTankIsElectric(const std::string& originalPath) {
    for(const auto& item:kFuelTankDefinitions)if(originalPath==item.path)return item.electric;
    return false;
}
bool validateFuelTankPowertrain(const FuelTankMod& mod,const std::vector<std::string>& enginePaths,std::string* error) {
    if(error)error->clear();
    if(enginePaths.size()!=1) {
        if(error)*error="无法确认唯一发动机，请使用完整的原厂车辆配置。";
        return false;
    }
    const auto& path=enginePaths.front();
    if(mod.electric) {
        if(path=="/def/vehicle/truck/scania.s_2024e/engine/400kw.sii" ||
           path=="/def/vehicle/truck/scania.s_2024e/engine/450kw.sii")return true;
        if(error)*error="斯堪尼亚电池扩容需要原厂 400 kW 或 450 kW 电动发动机；当前配置无法确认。";
        return false;
    }
    if(path.find("kw.sii")!=std::string::npos || path.find("electric")!=std::string::npos || path.find("_2024e/")!=std::string::npos) {
        if(error)*error="所选柴油底盘搭配电动发动机，无法按升扩容；请选择动力类型匹配的车辆。";
        return false;
    }
    return true;
}
bool buildFuelTankMod(const std::string& originalPath,int capacity,FuelTankMod* out,std::string* error) {
    if(error)error->clear();if(out)*out={};
    try {
        if(!out)throw std::runtime_error("容量 Mod 输出为空");
        if(std::size(kFuelTankDefinitions)==0)throw std::runtime_error("此构建未包含本地原厂底盘定义，请按 README 生成容量目录后重新构建。");
        const FuelTankDefinition* definition=nullptr;
        for(const auto& item:kFuelTankDefinitions)if(originalPath==item.path){definition=&item;break;}
        if(!definition)throw std::runtime_error("此底盘未收录；支持内置原厂柴油底盘及斯堪尼亚 S 2024 电动 4×2 底盘。");
        const std::string unitName=definition->electric?"kWh":"升",unitSuffix=definition->electric?"kWh":"L";
        if(capacity<100||capacity>10000)throw std::runtime_error("容量应在 100–10000 "+unitName+"之间");
        const std::string source=definition->text;
        static const std::regex tankRe(R"((?:^|\n)[ \t]*tank_size[ \t]*:[ \t]*([0-9]+(?:\.[0-9]+)?)[ \t]*(?:#[^\r\n]*)?(?:\r?\n|$))");
        static const std::regex unitRe(R"(accessory_chassis_data[ \t]*:[ \t]*([a-zA-Z0-9_.]+))");
        static const std::regex nameRe("(?:^|\n)[ \t]*name[ \t]*:[ \t]*\"([^\"\r\n]*)\"");
        const auto tank=uniqueField(source,tankRe),unit=uniqueField(source,unitRe),name=uniqueField(source,nameRe);
        const auto stock=std::stod(tank[1].str());
        if(capacity<=stock)throw std::runtime_error("目标容量必须大于原厂容量（"+tank[1].str()+" "+unitName+"）");
        std::uint64_t hash=14695981039346656037ull;
        const auto key=originalPath+"\n"+source+"\n"+std::to_string(capacity);
        for(unsigned char c:key){hash^=c;hash*=1099511628211ull;}
        char hex[17]{};std::snprintf(hex,sizeof(hex),"%016llx",static_cast<unsigned long long>(hash));
        const std::string id=hex,amount=std::to_string(capacity);
        FuelTankMod result;result.originalPath=originalPath;result.originalCapacity=static_cast<int>(stock);result.capacity=capacity;
        result.electric=definition->electric;
        result.chassisPath=originalPath.substr(0,originalPath.find_last_of('/')+1)+"cdx_fuel_"+id+"_"+amount+".sii";
        result.fileName="ets2trainer_fuel_"+id+"_"+amount+".scs";
        result.displayName="ETS2Trainer "+std::string(result.electric?"Battery ":"Fuel ")+amount+" "+unitSuffix+" ["+id.substr(0,8)+"]";
        std::vector<Edit> edits={{static_cast<std::size_t>(tank.position(1)),static_cast<std::size_t>(tank.length(1)),amount},
            {static_cast<std::size_t>(unit.position(1)),static_cast<std::size_t>(unit.length(1)),"cdx_fuel."+id.substr(0,8)+"."+id.substr(8)},
            {static_cast<std::size_t>(name.position(1)),static_cast<std::size_t>(name.length(1)),name[1].str()+" ["+amount+" "+unitSuffix+"]"}};
        std::sort(edits.begin(),edits.end(),[](const Edit& a,const Edit& b){return a.at>b.at;});
        result.chassisText=source;
        for(const auto& edit:edits)result.chassisText.replace(edit.at,edit.size,edit.text);
        const std::string manifest="SiiNunit\n{\nmod_package : .package_name {\n package_version: \"1.0\"\n display_name: \""+
            result.displayName+"\"\n author: \"ETS2Trainer\"\n category[]: \"tuning_parts\"\n description_file: \"mod_description.txt\"\n compatible_versions[]: \"1.61.*\"\n}\n}\n";
        const std::string description=result.displayName+"\nOriginal: "+originalPath+"\nCapacity: "+amount+
            " "+unitSuffix+" (stock "+tank[1].str()+" "+unitSuffix+").\nSingle-player. Enable before loading the generated save.\nKeep enabled while using that save. To remove, load the original save first.\n";
        result.archive=zipFuel({{"manifest.sii",manifest},{"mod_description.txt",description},{result.chassisPath.substr(1),result.chassisText}});
        *out=std::move(result);return true;
    } catch(const std::exception& e){if(error)*error=e.what();return false;}
}
}
