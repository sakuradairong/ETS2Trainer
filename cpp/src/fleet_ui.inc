// Included by ui.cpp, beside the other asynchronous save-tool dialogs.
void consumePendingFleet(AppState& app) {
    PendingFleetInspection result;
    {
        std::lock_guard<std::mutex> lock(app.resultMutex);
        if(!app.pendingFleet.ready) return;
        result=std::move(app.pendingFleet);app.pendingFleet={};
    }
    if(result.game!=selectedGame() || result.generation!=app.fleetGeneration) return;
    app.fleetInspectBusy=false;app.progressDone=1;
    app.fleetInventory=result.ok ? std::move(result.inventory) : FleetInventory{};
    app.fleetCounts.assign(app.fleetInventory.garages.size(),0);
    app.fleetGarageCapacity.assign(app.fleetInventory.garages.size(),0);app.fleetUnownedOnly=false;
    app.fleetIdleSelected.assign(app.fleetInventory.idleTrucks.size(),false);
    app.fleetRepairCaches=false;app.fleetTruckIndex=0;
    for(std::size_t i=0;i<app.fleetInventory.trucks.size();++i) {
        const auto& id=app.fleetInventory.trucks[i].id;
        if(std::any_of(app.fleetInventory.truckCaches.begin(),app.fleetInventory.truckCaches.end(),
            [&](const FleetTruckCache& c){return c.truckId==id && c.supported;})) {app.fleetTruckIndex=(int)i;break;}
    }
    app.fleetNote=result.ok ? "可购买/扩容车库、给闲置卡车配司机、复制卡车，或单独修复可核实的车辆参数。预览后生成新存档。" : "读取车队失败："+result.error;
}
void openFleet(AppState& app) {
    if(app.busy) {app.saveNote="请先完成或取消当前任务。";return;}
    if(selectedGame()!=GameId::Ets2) {app.saveNote="车队经济管理目前仅支持欧卡2。";return;}
    if(app.selectedSlot<0 || app.selectedSlot>=(int)app.slots.size()) {
        app.saveNote="请先在存档列表中选择要扩充车队的存档。";return;
    }
    joinWorker(app);consumePendingCopy(app);
    app.fleetRequest={};app.fleetRequest.game=selectedGame();
    app.fleetRequest.source=app.slots.at(app.selectedSlot);
    app.fleetInventory={};app.fleetCounts.clear();app.fleetIdleSelected.clear();app.fleetRepairCaches=false;app.fleetTruckIndex=0;
    app.fleetGarageCapacity.clear();app.fleetUnownedOnly=false;
    app.fleetPreview=false;app.fleetSearch[0]=0;
    const auto name=app.fleetRequest.source.label+" · 车队管理";
    std::snprintf(app.fleetName,sizeof(app.fleetName),"%s",name.c_str());
    app.showFleet=true;app.fleetInspectBusy=true;
    app.fleetNote="正在解密存档并读取车库、司机池和自有卡车……";
    const auto source=app.fleetRequest.source;
    const auto game=app.fleetRequest.game;
    const auto generation=++app.fleetGeneration;
    app.cancel=false;app.busy=true;app.progressDone=0;app.progressTotal=1;app.progressNote="读取车队经济系统";
    app.worker=std::thread([&app,source,game,generation] {
        PendingFleetInspection result;result.game=game;result.generation=generation;
        result.ok=inspectFleetSlot(source,&result.inventory,&result.error,[&app]{return app.cancel.load();});
        result.ready=true;
        {std::lock_guard<std::mutex> lock(app.resultMutex);app.pendingFleet=std::move(result);}
        app.busy=false;
    });
}
void startFleet(AppState& app) {
    if(app.busy || !app.fleetPreview) return;
    joinWorker(app);consumePendingCopy(app);
    auto request=app.fleetRequest;request.displayName=app.fleetName;
    app.busy=true;app.cancel=false;app.progressDone=0;app.progressTotal=1;app.progressNote="扩充车队并另存副本";
    app.saveNote="正在执行车队分配、参数修复并创建新存档……";
    app.worker=std::thread([&app,request] {
        auto result=fleetToNewSave(request,[&app]{return app.cancel.load();});
        {std::lock_guard<std::mutex> lock(app.resultMutex);
            app.pendingCopy=std::move(result);app.pendingCopyFleet=true;app.pendingCopyReady=true;}
        app.busy=false;
    });
}
const FleetTruckCache* fleetUiCache(const FleetInventory& inv,const std::string& id) {
    const auto it=std::find_if(inv.truckCaches.begin(),inv.truckCaches.end(),[&](const FleetTruckCache& c){return c.truckId==id;});
    return it==inv.truckCaches.end()?nullptr:&*it;
}
void fleetUiPoint(AppState& app,float& x,float& y) {
    if(!app.fleetUiProbe.capture)return;
    const auto lo=::ImGui::GetItemRectMin(),hi=::ImGui::GetItemRectMax();x=(lo.x+hi.x)/2;y=(lo.y+hi.y)/2;
}
void renderFleetHistory(const FleetInventory& inv) {
    ::ImGui::TextWrapped("仅统计本存档保留的雇佣司机历史记录；记录窗口可能不同，不代表每日收益或未来收入。费用包含工资、维护和燃油。");
    ::ImGui::Text("记录 %zu 条 | 营收 %lld | 费用 %lld | 净收益 %lld",inv.profit.entries,
        (long long)inv.profit.revenue,(long long)inv.profit.cost,(long long)inv.profit.net);
    if(::ImGui::BeginChild("fleet_history",ImVec2(0,(std::max)(100.f,::ImGui::GetContentRegionAvail().y-42)),ImGuiChildFlags_Borders)) {
        if(::ImGui::BeginTable("garage_history",6,ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_RowBg)) {
            for(const auto* label:{"车库","闲置卡车","记录数","营收","费用","净收益"})::ImGui::TableSetupColumn(label);
            ::ImGui::TableHeadersRow();
            for(const auto& g:inv.garages) if(g.purchased) {
                ::ImGui::TableNextRow();::ImGui::TableSetColumnIndex(0);::ImGui::TextUnformatted(g.city.c_str());
                ::ImGui::TableSetColumnIndex(1);::ImGui::Text("%zu",g.idleTrucks);
                ::ImGui::TableSetColumnIndex(2);::ImGui::Text("%zu",g.profit.entries);
                ::ImGui::TableSetColumnIndex(3);::ImGui::Text("%lld",(long long)g.profit.revenue);
                ::ImGui::TableSetColumnIndex(4);::ImGui::Text("%lld",(long long)g.profit.cost);
                ::ImGui::TableSetColumnIndex(5);::ImGui::Text("%lld",(long long)g.profit.net);
            }
            ::ImGui::EndTable();
        }
        if(::ImGui::CollapsingHeader("司机记录与车辆参数",ImGuiTreeNodeFlags_DefaultOpen) &&
           ::ImGui::BeginTable("driver_history",5,ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_RowBg)) {
            for(const auto* label:{"司机 / 车库","模拟状态","记录数","净收益","车辆参数"})::ImGui::TableSetupColumn(label);
            ::ImGui::TableHeadersRow();
            for(const auto& d:inv.employees) {
                ::ImGui::TableNextRow();::ImGui::TableSetColumnIndex(0);::ImGui::Text("%s / %s",d.id.c_str(),d.garageId.c_str());
                if(::ImGui::IsItemHovered())::ImGui::SetTooltip("卡车：%s",d.truckId.c_str());
                ::ImGui::TableSetColumnIndex(1);::ImGui::Text("%d",d.state);
                ::ImGui::TableSetColumnIndex(2);::ImGui::Text("%zu",d.profit.entries);
                ::ImGui::TableSetColumnIndex(3);::ImGui::Text("%lld",(long long)d.profit.net);
                ::ImGui::TableSetColumnIndex(4);::ImGui::TextWrapped("%s",d.cacheNote.c_str());
            }
            ::ImGui::EndTable();
        }
    }
    ::ImGui::EndChild();
}
void renderFleetDialog(AppState& app) {
    const char* title="车队经济管理##fleet";
    if(app.showFleet) {::ImGui::OpenPopup(title);app.showFleet=false;}
    ::ImGui::SetNextWindowSize(ImVec2(980,(std::min)(760.f,::ImGui::GetIO().DisplaySize.y-40.f)),ImGuiCond_Appearing);
    if(!::ImGui::BeginPopupModal(title,nullptr,ImGuiWindowFlags_None)) return;
    if(app.fleetUiProbe.capture) app.fleetUiProbe.dialog=true;
    if(app.fleetRequest.source.slotDir.empty() || app.fleetRequest.game!=selectedGame()) {
        ::ImGui::CloseCurrentPopup();::ImGui::EndPopup();return;
    }
    const auto& inv=app.fleetInventory;
    ::ImGui::TextWrapped("存档：%s",app.fleetRequest.source.label.c_str());
    ::ImGui::TextWrapped("可直接取得未购车库并扩容到 3 / 5 车位，再雇佣司机和分配卡车；不扣购买或升级费用。所有操作预览后另存新档，退出欧卡后才能生成。");
    const auto offered=std::count_if(inv.availableDrivers.begin(),inv.availableDrivers.end(),[](const FleetDriver& d){return d.offered;});
    ::ImGui::Text("雇佣司机 %zu | 可用 %zu（招聘 %zu）| 闲置卡车 %zu",inv.hiredDrivers,inv.availableDrivers.size(),(std::size_t)offered,inv.idleTrucks.size());
    if(::ImGui::BeginTabBar("fleet_tabs")) {
        const bool manage=::ImGui::BeginTabItem("分配与修复");
        fleetUiPoint(app,app.fleetUiProbe.manageX,app.fleetUiProbe.manageY);
        if(manage) {
            ::ImGui::BeginDisabled(app.busy || app.fleetInspectBusy || inv.snapshot.empty());
            bool changed=false;
            if(app.fleetTruckIndex<0 || app.fleetTruckIndex>=(int)inv.trucks.size())app.fleetTruckIndex=0;
            const auto* truck=inv.trucks.empty()?nullptr:&inv.trucks[app.fleetTruckIndex];
            const auto* cache=truck?fleetUiCache(inv,truck->id):nullptr;
            const char* templateLabel=truck?truck->label.c_str():"无受支持的自有卡车";
            ::ImGui::SetNextItemWidth(600);
            if(::ImGui::BeginCombo("复制卡车模板",templateLabel)) {
                for(int i=0;i<(int)inv.trucks.size();++i) {
                    const auto* c=fleetUiCache(inv,inv.trucks[i].id);
                    const auto label=inv.trucks[i].label+" / "+inv.trucks[i].id+(c&&c->supported?" [已核实]":" [参数未核实]");
                    if(::ImGui::Selectable(label.c_str(),i==app.fleetTruckIndex)) {app.fleetTruckIndex=i;changed=true;}
                }
                ::ImGui::EndCombo();
            }
            // Selection changes take effect within this frame as well.
            truck=inv.trucks.empty()?nullptr:&inv.trucks[app.fleetTruckIndex];cache=truck?fleetUiCache(inv,truck->id):nullptr;
            templateLabel=truck?truck->label.c_str():"无受支持的自有卡车";
            if(cache&&cache->supported)::ImGui::TextWrapped("模板：%d 轴 / %.0f kg / 效率 %.4f；%s",cache->axles,cache->mass,cache->efficiency,cache->note.c_str());
            else ::ImGui::TextWrapped("模板参数未核实：%s。可选择其他模板，或仅购买/扩容车库及修复参数。",cache?cache->note.c_str():"缺少参考记录");
            ::ImGui::SetNextItemWidth(240);
            changed|=::ImGui::InputTextWithHint("搜索车库","城市内部名，例如 koln",app.fleetSearch,sizeof(app.fleetSearch));
            ::ImGui::SameLine();changed|=::ImGui::Checkbox("只看未购买",&app.fleetUnownedOnly);
            fleetUiPoint(app,app.fleetUiProbe.unownedX,app.fleetUiProbe.unownedY);
            auto visible=[&](const std::string& id){
                if(id.find(app.fleetSearch)==std::string::npos)return false;
                return !app.fleetUnownedOnly || std::any_of(inv.garages.begin(),inv.garages.end(),
                    [&](const FleetGarage& g){return g.id==id&&!g.purchased;});
            };
            auto freeSlots=[&](std::size_t i){
                const auto& g=inv.garages[i];const int capacity=app.fleetGarageCapacity[i];
                return g.freeSlots+(capacity>0?(std::size_t)(std::max)(0,capacity-(int)g.slots):0);
            };
            auto capture=[&](float& x,float& y) {
                if(!app.fleetUiProbe.capture)return;
                const auto lo=::ImGui::GetItemRectMin(),hi=::ImGui::GetItemRectMax();x=(lo.x+hi.x)/2;y=(lo.y+hi.y)/2;
            };
            if(::ImGui::Button("筛选内未购车库设为5车位")) {
                for(std::size_t i=0;i<inv.garages.size();++i)
                    if(!inv.garages[i].purchased&&visible(inv.garages[i].id))app.fleetGarageCapacity[i]=5;
                changed=true;
            }
            fleetUiPoint(app,app.fleetUiProbe.purchaseX,app.fleetUiProbe.purchaseY);::ImGui::SameLine();
            if(::ImGui::Button("按空位自动分配")) {
                int remaining=(int)(std::min)(inv.availableDrivers.size(),std::size_t(400));
                for(std::size_t i=0;i<inv.idleTrucks.size();++i) {
                    const auto& t=inv.idleTrucks[i];const auto* c=fleetUiCache(inv,t.truckId);
                    const bool selected=remaining>0 && visible(t.garageId) && c && c->supported;
                    app.fleetIdleSelected[i]=selected;if(selected)--remaining;
                }
                for(std::size_t i=0;i<inv.garages.size();++i) {
                    const auto& g=inv.garages[i];const int n=visible(g.id)&&cache&&cache->supported?(std::min)(remaining,(int)freeSlots(i)):0;
                    app.fleetCounts[i]=n;remaining-=n;
                }
                changed=true;
            }
            capture(app.fleetUiProbe.autoX,app.fleetUiProbe.autoY);::ImGui::SameLine();
            if(::ImGui::Button("清空选择")) {
                std::fill(app.fleetCounts.begin(),app.fleetCounts.end(),0);
                std::fill(app.fleetGarageCapacity.begin(),app.fleetGarageCapacity.end(),0);
                std::fill(app.fleetIdleSelected.begin(),app.fleetIdleSelected.end(),false);app.fleetRepairCaches=false;changed=true;
            }
            capture(app.fleetUiProbe.clearX,app.fleetUiProbe.clearY);
            if(app.fleetPreview) {::ImGui::SameLine();if(::ImGui::Button("返回调整"))changed=true;}
            const auto repairable=std::count_if(inv.employees.begin(),inv.employees.end(),[](const FleetEmployee& d){return d.cacheRepairable;});
            ::ImGui::BeginDisabled(!repairable);
            changed|=::ImGui::Checkbox("修复已核实的默认车辆参数",&app.fleetRepairCaches);
            fleetUiPoint(app,app.fleetUiProbe.repairX,app.fleetUiProbe.repairY);
            ::ImGui::EndDisabled();::ImGui::SameLine();::ImGui::TextDisabled("可修复 %zu 名；参考至少两名同配置司机",(std::size_t)repairable);
            if(changed)app.fleetPreview=false;
            const float regionHeight=(std::max)(80.f,::ImGui::GetContentRegionAvail().y-170.f);
            if(::ImGui::BeginChild("fleet_selection",ImVec2(0,regionHeight),ImGuiChildFlags_Borders)) {
                if(app.fleetPreview) {
                    std::size_t next=0;const auto& plan=app.fleetRequest.expansion;
                    for(const auto& c:plan.garageChanges) {
                        const auto g=std::find_if(inv.garages.begin(),inv.garages.end(),[&](const FleetGarage& item){return item.id==c.garageId;});
                        ::ImGui::Text("%s %s：%zu → %d 车位（不扣费）",g->purchased?"扩容":"购入",c.garageId.c_str(),g->slots,c.targetSlots);
                    }
                    for(const auto& id:plan.existingTruckIds) {
                        const auto& d=inv.availableDrivers.at(next++);const auto* c=fleetUiCache(inv,id);
                        const auto t=std::find_if(inv.idleTrucks.begin(),inv.idleTrucks.end(),[&](const FleetIdleTruck& x){return x.truckId==id;});
                        ::ImGui::TextWrapped("%s（经验 %d%s）→ %s / 已有卡车 %s",d.id.c_str(),d.experience,d.offered?"，招聘":"",t->garageId.c_str(),id.c_str());
                        if(c)::ImGui::TextWrapped("  %d 轴 / %.0f kg / 效率 %.4f；%s",c->axles,c->mass,c->efficiency,c->note.c_str());
                    }
                    for(const auto& a:plan.allocations) {
                        ::ImGui::Text("%s：复制 %d 辆 %s",a.garageId.c_str(),a.count,templateLabel);
                        for(int i=0;i<a.count;++i) {const auto& d=inv.availableDrivers.at(next++);::ImGui::Text("  %s（经验 %d%s）",d.id.c_str(),d.experience,d.offered?"，招聘":"");}
                    }
                    for(const auto& id:plan.repairDriverIds) {
                        const auto d=std::find_if(inv.employees.begin(),inv.employees.end(),[&](const FleetEmployee& x){return x.id==id;});
                        const auto* c=fleetUiCache(inv,d->truckId);
                        ::ImGui::TextWrapped("修复 %s / %s → %d 轴 / %.0f kg / 效率 %.4f；%s",id.c_str(),d->garageId.c_str(),c->axles,c->mass,c->efficiency,c->note.c_str());
                    }
                } else {
                    if(!inv.idleTrucks.empty() && ::ImGui::CollapsingHeader("给闲置卡车配司机",ImGuiTreeNodeFlags_DefaultOpen)) {
                        for(std::size_t i=0;i<inv.idleTrucks.size();++i) {
                            const auto& t=inv.idleTrucks[i];if(!visible(t.garageId))continue;
                            const auto* c=fleetUiCache(inv,t.truckId);bool selected=app.fleetIdleSelected[i];
                            ::ImGui::PushID((int)i);::ImGui::BeginDisabled(!c||!c->supported);
                            if(::ImGui::Checkbox((t.garageId+" / "+t.truckId).c_str(),&selected)) {app.fleetIdleSelected[i]=selected;changed=true;}
                            if(i==0)fleetUiPoint(app,app.fleetUiProbe.idleX,app.fleetUiProbe.idleY);
                            ::ImGui::EndDisabled();::ImGui::PopID();
                            if(!c||!c->supported)::ImGui::TextDisabled("  参数未核实：%s",c?c->note.c_str():"结构不受支持");
                        }
                    }
                    ::ImGui::TextUnformatted("未购买车库可选 3 / 5 车位；已有车库可升级。复制卡车继承模板配置、磨损与里程。");
                    if(::ImGui::BeginTable("fleet_garages",7,ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_RowBg)) {
                        ::ImGui::TableSetupColumn("车库 / 城市",ImGuiTableColumnFlags_WidthStretch);
                        for(const auto* label:{"当前容量","目标容量","卡车","司机含玩家","可用空位","复制组数"})::ImGui::TableSetupColumn(label,ImGuiTableColumnFlags_WidthFixed,90);
                        ::ImGui::TableHeadersRow();
                        for(int i=0;i<(int)inv.garages.size();++i) {
                            const auto& g=inv.garages[i];if(!visible(g.id))continue;
                            ::ImGui::PushID(i);::ImGui::TableNextRow();::ImGui::TableSetColumnIndex(0);
                            ::ImGui::TextUnformatted(g.city.c_str());if(!g.purchased){::ImGui::SameLine();::ImGui::TextDisabled("未购买");}
                            ::ImGui::TableSetColumnIndex(1);::ImGui::Text("%zu",g.slots);
                            ::ImGui::TableSetColumnIndex(2);
                            ::ImGui::BeginDisabled(g.slots>=5);::ImGui::SetNextItemWidth(-1);
                            const auto targetLabel=app.fleetGarageCapacity[i]?std::to_string(app.fleetGarageCapacity[i])+" 车位":g.purchased?"保持":"未购买";
                            if(::ImGui::BeginCombo("##capacity",targetLabel.c_str())) {
                                if(::ImGui::Selectable("保持",!app.fleetGarageCapacity[i])){app.fleetGarageCapacity[i]=0;changed=true;}
                                for(const int capacity:{3,5})if((std::size_t)capacity>g.slots) {
                                    const auto label=std::to_string(capacity)+" 车位";
                                    if(::ImGui::Selectable(label.c_str(),app.fleetGarageCapacity[i]==capacity)){app.fleetGarageCapacity[i]=capacity;changed=true;}
                                }
                                ::ImGui::EndCombo();
                            }
                            ::ImGui::EndDisabled();
                            app.fleetCounts[i]=(std::min)(app.fleetCounts[i],(int)freeSlots(i));
                            ::ImGui::TableSetColumnIndex(3);::ImGui::Text("%zu",g.trucks);
                            ::ImGui::TableSetColumnIndex(4);::ImGui::Text("%zu",g.drivers);
                            ::ImGui::TableSetColumnIndex(5);::ImGui::Text("%zu",freeSlots(i));
                            ::ImGui::TableSetColumnIndex(6);::ImGui::BeginDisabled(!freeSlots(i)||!cache||!cache->supported);
                            ::ImGui::SetNextItemWidth(-1);
                            if(::ImGui::InputInt("##add",&app.fleetCounts[i],0,0)) {
                                app.fleetCounts[i]=(std::max)(0,(std::min)(app.fleetCounts[i],(int)freeSlots(i)));changed=true;
                            }
                            ::ImGui::EndDisabled();::ImGui::PopID();
                        }
                        ::ImGui::EndTable();
                    }
                }
            }
            ::ImGui::EndChild();
            ::ImGui::SetNextItemWidth(600);changed|=::ImGui::InputText("新存档名称",app.fleetName,sizeof(app.fleetName));
            if(changed)app.fleetPreview=false;
            int clones=0;for(int n:app.fleetCounts)clones+=n;
            const auto existing=(std::size_t)std::count(app.fleetIdleSelected.begin(),app.fleetIdleSelected.end(),true);
            const auto repairs=app.fleetRepairCaches?(std::size_t)repairable:0;const auto total=(std::size_t)clones+existing;
            const auto changes=(std::size_t)std::count_if(app.fleetGarageCapacity.begin(),app.fleetGarageCapacity.end(),[](int capacity){return capacity>0;});
            ::ImGui::Text("计划：雇佣 %zu | 配已有车 %zu | 复制 %d | 参数修复 %zu | 车库变更 %zu",total,existing,clones,repairs,changes);
            std::string escaped,error;
            bool valid=(total>0||repairs>0||changes>0) && total<=400 && total<=inv.availableDrivers.size() && repairs<=400 && changes<=400 &&
                (!clones||(cache&&cache->supported)) && siiEscapeName(app.fleetName,&escaped,&error);
            for(std::size_t i=0;i<inv.idleTrucks.size();++i) if(app.fleetIdleSelected[i]) {
                const auto* c=fleetUiCache(inv,inv.idleTrucks[i].truckId);if(!c||!c->supported)valid=false;
            }
            if(total>inv.availableDrivers.size())::ImGui::TextWrapped("可用司机不足，请减少分配数量。");
            if(total>400||repairs>400||changes>400)::ImGui::TextWrapped("单次最多雇佣 400 名、修复 400 名、购买或扩容 400 个车库。");
            if(!error.empty())::ImGui::TextWrapped("%s",error.c_str());
            ::ImGui::BeginDisabled(!valid);
            if(::ImGui::Button("预览分配")) {
                auto& plan=app.fleetRequest.expansion;plan={};plan.expectedSnapshot=inv.snapshot;if(truck)plan.truckId=truck->id;
                for(std::size_t i=0;i<inv.idleTrucks.size();++i)if(app.fleetIdleSelected[i])plan.existingTruckIds.push_back(inv.idleTrucks[i].truckId);
                for(std::size_t i=0;i<inv.garages.size();++i)if(app.fleetCounts[i]>0)plan.allocations.push_back({inv.garages[i].id,app.fleetCounts[i]});
                if(app.fleetRepairCaches)for(const auto& d:inv.employees)if(d.cacheRepairable)plan.repairDriverIds.push_back(d.id);
                for(std::size_t i=0;i<inv.garages.size();++i)if(app.fleetGarageCapacity[i])plan.garageChanges.push_back({inv.garages[i].id,app.fleetGarageCapacity[i]});
                app.fleetPreview=true;
            }
            capture(app.fleetUiProbe.previewX,app.fleetUiProbe.previewY);
            if(app.fleetUiProbe.capture) {app.fleetUiProbe.preview=app.fleetPreview;app.fleetUiProbe.apply=valid&&app.fleetPreview&&!app.busy;app.fleetUiProbe.total=(int)total;app.fleetUiProbe.repairs=(int)repairs;app.fleetUiProbe.garageChanges=(int)changes;}
            ::ImGui::SameLine();::ImGui::BeginDisabled(!app.fleetPreview);
            if(ui::primaryButton("生成车队新存档")) {startFleet(app);::ImGui::CloseCurrentPopup();}
            ::ImGui::EndDisabled();::ImGui::EndDisabled();::ImGui::EndDisabled();
            ::ImGui::TextWrapped("%s",app.fleetNote.c_str());
            ::ImGui::EndTabItem();
        }
        const bool history=::ImGui::BeginTabItem("经营记录");
        fleetUiPoint(app,app.fleetUiProbe.historyX,app.fleetUiProbe.historyY);
        if(history) {if(app.fleetUiProbe.capture)app.fleetUiProbe.historySeen=true;renderFleetHistory(inv);::ImGui::EndTabItem();}
        ::ImGui::EndTabBar();
    }
    if(::ImGui::Button(app.fleetInspectBusy?"取消读取并关闭":"关闭")) {
        if(app.fleetInspectBusy)app.cancel=true;
        ++app.fleetGeneration;app.fleetInspectBusy=false;app.fleetPreview=false;::ImGui::CloseCurrentPopup();
    }
    ::ImGui::EndPopup();
}
