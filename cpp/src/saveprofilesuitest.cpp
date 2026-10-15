#include "saveprofilesuitest.h"
#include "imgui.h"
#include <cstring>

namespace ets2 {
namespace {
bool switchChecks = true;
int checkedSwitches = 0;

void buildSyntheticSlots(AppState& app) {
    app.slots.clear();
    for(int profile=0;profile<3;++profile) {
        const int counts[]={2,3,1};
        for(int slot=0;slot<counts[profile];++slot) {
            SaveSlot s;
            s.profileDir=L"ui-fixture-not-a-save\\profile"+std::to_wstring(profile);
            s.profileName=L"同名测试档案";
            s.cloud=profile==2;
            s.slotName=std::to_wstring(slot+1);
            s.slotDir=s.profileDir+L"\\save\\"+s.slotName;
            s.gameSii=s.slotDir+L"\\game.sii";
            s.displayName="同名测试存档";s.format="scs";s.formatText="加密";
            app.slots.push_back(s);
        }
    }
    app.saveProfiles=groupSaveProfiles(app.slots);
    app.selectedSaveProfile=0;
    app.saveProfileUiProbe.capture=true;
}

void seedDraft(AppState& app,int slot) {
    selectSlot(app,slot);
    ::strcpy_s(app.saveMoney,"123456");
    ::strcpy_s(app.saveXp,"7890");
    app.slotValues.hasMoney=true;app.slotValues.hasExperience=true;
    app.copyRequest.source=app.slots[slot];
}

bool cleared(const AppState& app) {
    return app.selectedSlot==-1 && app.saveMoney[0]==0 && app.saveXp[0]==0 &&
           !app.slotValues.hasMoney && !app.slotValues.hasExperience &&
           app.copyRequest.source.slotDir.empty() && !app.showCopyDialog;
}
}

void saveProfilesSmokeRequest(AppState& app,int frame) {
    if(frame==0) {
        switchChecks=true;checkedSwitches=0;
        buildSyntheticSlots(app);seedDraft(app,0);
    }
    if(frame==2) switchChecks &= app.visibleSaveSlotCount==2;
    if(frame==5 || frame==9 || frame==13 || frame==17) {
        const int profile=frame==5 ? 1 : frame==13 ? 2 : 0;
        const int counts[]={2,3,1};
        switchChecks &= app.selectedSaveProfile==profile &&
                        app.visibleSaveSlotCount==counts[profile] && cleared(app);
        ++checkedSwitches;
        if(frame!=17) seedDraft(app,profile==1 ? 3 : profile==2 ? 5 : 0);
    }
    int target=-1;
    if(frame==2 || frame==3) target=1;
    if(frame==6 || frame==7 || frame==14 || frame==15) target=0;
    if(frame==10 || frame==11) target=2;
    if(target>=0) {
        auto& io=::ImGui::GetIO();
        io.AddFocusEvent(true);
        io.AddMousePosEvent(app.saveProfileUiProbe.rowX[target],app.saveProfileUiProbe.rowY[target]);
        io.AddMouseButtonEvent(0,frame%2==0);
    }
}

void saveProfilesSmokeReport(AppState& app,int& smokeResult) {
    const bool ok=switchChecks && checkedSwitches==4 && app.saveProfiles.size()==3 &&
                  app.selectedSaveProfile==0 && app.visibleSaveSlotCount==2 &&
                  cleared(app) && !app.worker.joinable();
    if(!ok) smokeResult=1;
    logLine(fmt("[ %s ] 存档按档案 UI：mouse_switches=%d groups=%d profile=%d visible=%d cleared=%d worker=%d；未写入存档",
                ok ? "PASS" : "FAIL",checkedSwitches,(int)app.saveProfiles.size(),
                app.selectedSaveProfile,app.visibleSaveSlotCount,cleared(app),app.worker.joinable()));
}
}
