#include "Extension/Trainer/trainer_page.h"
#include "Extension/Trainer/trainer_presets.h"
#include "Extension/UI/Overlay/skate_menu_internal.h"
#include <imgui_internal.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
namespace physics=dingosdk::trainer;
using namespace dingosdk::overlay;
void require(bool good,const char *message){if(!good)throw std::runtime_error(message);}
// Optional CPU rendering of the actual native draw data, with no game/window/input automation.
void image(const std::filesystem::path &path, const ImDrawData &data, const unsigned char *atlas, int atlas_width, int atlas_height) {
    const int width = static_cast<int>(data.DisplaySize.x), height = static_cast<int>(data.DisplaySize.y);
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 3, 18);
    const auto edge = [](ImVec2 a, ImVec2 b, ImVec2 p) { return (p.x - a.x) * (b.y - a.y) - (p.y - a.y) * (b.x - a.x); };
    for (const auto *list : data.CmdLists) for (const auto &command : list->CmdBuffer) {
        if (command.UserCallback) continue;
        const int clip_left = std::max(0, static_cast<int>(command.ClipRect.x)), clip_top = std::max(0, static_cast<int>(command.ClipRect.y));
        const int clip_right = std::min(width, static_cast<int>(command.ClipRect.z)), clip_bottom = std::min(height, static_cast<int>(command.ClipRect.w));
        for (unsigned int i = 0; i + 2 < command.ElemCount; i += 3) {
            const auto &a = list->VtxBuffer[list->IdxBuffer[command.IdxOffset + i] + command.VtxOffset];
            const auto &b = list->VtxBuffer[list->IdxBuffer[command.IdxOffset + i + 1] + command.VtxOffset];
            const auto &c = list->VtxBuffer[list->IdxBuffer[command.IdxOffset + i + 2] + command.VtxOffset];
            const auto area = edge(a.pos, b.pos, c.pos);
            if (std::abs(area) < 1e-6f) continue;
            const int left = std::max(clip_left, static_cast<int>(std::floor(std::min({a.pos.x, b.pos.x, c.pos.x}))));
            const int right = std::min(clip_right, static_cast<int>(std::ceil(std::max({a.pos.x, b.pos.x, c.pos.x}))));
            const int top = std::max(clip_top, static_cast<int>(std::floor(std::min({a.pos.y, b.pos.y, c.pos.y}))));
            const int bottom = std::min(clip_bottom, static_cast<int>(std::ceil(std::max({a.pos.y, b.pos.y, c.pos.y}))));
            for (int y = top; y < bottom; ++y) for (int x = left; x < right; ++x) {
                const ImVec2 point(static_cast<float>(x) + .5f, static_cast<float>(y) + .5f);
                const float wa = edge(b.pos, c.pos, point) / area, wb = edge(c.pos, a.pos, point) / area, wc = 1 - wa - wb;
                if (wa < 0 || wb < 0 || wc < 0) continue;
                const auto u = wa * a.uv.x + wb * b.uv.x + wc * c.uv.x, v = wa * a.uv.y + wb * b.uv.y + wc * c.uv.y;
                const int tx = std::clamp(static_cast<int>(u * static_cast<float>(atlas_width)), 0, atlas_width - 1);
                const int ty = std::clamp(static_cast<int>(v * static_cast<float>(atlas_height)), 0, atlas_height - 1);
                const auto texture = static_cast<std::size_t>(ty * atlas_width + tx) * 4;
                const auto component = [&](unsigned shift) {
                    return wa * static_cast<float>((a.col >> shift) & 255u) + wb * static_cast<float>((b.col >> shift) & 255u) + wc * static_cast<float>((c.col >> shift) & 255u);
                };
                const float alpha = component(IM_COL32_A_SHIFT) / 255 * static_cast<float>(atlas[texture + 3]) / 255;
                const auto at = static_cast<std::size_t>(y * width + x) * 3;
                const unsigned shifts[]{IM_COL32_R_SHIFT, IM_COL32_G_SHIFT, IM_COL32_B_SHIFT};
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    const float color = component(shifts[channel]) * static_cast<float>(atlas[texture + channel]) / 255;
                    pixels[at + channel] = static_cast<unsigned char>(std::clamp(color * alpha + static_cast<float>(pixels[at + channel]) * (1 - alpha), 0.0f, 255.0f));
                }
            }
        }
    }
    std::ofstream output(path, std::ios::binary);
    output << "P6\n" << width << ' ' << height << "\n255\n";
    output.write(reinterpret_cast<const char *>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    require(static_cast<bool>(output), "Could not write native UI preview");
}

struct NativeItem { ImRect rectangle; ImGuiID id{}; ImGuiWindow *window{}; std::string label; int frame{}; int submissions{}; bool disabled{}; ImGuiItemStatusFlags status_flags{}; };
std::map<ImGuiID, NativeItem> native_items;
void ImGuiTestEngineHook_ItemAdd(ImGuiContext *context, ImGuiID id, const ImRect &rectangle, const ImGuiLastItemData *data) {
    auto &item = native_items[id];
    if (item.frame != context->FrameCount) item.submissions = 0;
    ++item.submissions; item.id = id; item.rectangle = rectangle; item.window = context->CurrentWindow; item.frame = context->FrameCount;
    item.disabled = data && (data->ItemFlags & ImGuiItemFlags_Disabled);
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext *, ImGuiID id, const char *label, ImGuiItemStatusFlags flags) { native_items[id].label = label ? label : ""; native_items[id].status_flags = flags; }
void ImGuiTestEngineHook_Log(ImGuiContext *, const char *, ...) {}
const char *ImGuiTestEngine_FindItemDebugLabel(ImGuiContext *, ImGuiID id) {
    const auto item = native_items.find(id); return item == native_items.end() ? "" : item->second.label.c_str();
}
NativeItem native_item(const char *label) {
    auto *context = ImGui::GetCurrentContext();
    for (const auto &[id, item] : native_items)
        if (item.frame == context->FrameCount && item.label == label && item.window && item.window->Active && item.window->ClipRect.Contains(item.rectangle.GetCenter())) return item;
    // BeginCombo has ItemAdd but no ItemInfo in this ImGui version. Plain controls
    // have stable root IDs in active workspace, modal and inspector windows.
    for (auto *window : context->Windows) if (window->Active) {
        const auto item = native_items.find(window->GetID(label));
        if (item != native_items.end() && item->second.frame == context->FrameCount) return item->second;
    }
    throw std::runtime_error(std::string("Native widget was not submitted: ") + label);
}
NativeItem native_scoped_item(const char *scope,const char *label) {
    for(auto *window:GImGui->Windows) if(window->Active) {
        const auto id=ImHashStr(label,0,window->GetID(scope));
        const auto found=native_items.find(id);
        if(found!=native_items.end() && found->second.frame==GImGui->FrameCount) return found->second;
    }
    throw std::runtime_error(std::string("Scoped native widget was not submitted: ")+scope+"/"+label);
}
struct FunPresetGroup { const char *id, *title; std::vector<const char *> names; };
const std::array<FunPresetGroup, 6> fun_preset_groups{{
    {"pop", "Pop and airtime", {"Super Ollie", "Mega Pop", "Grind Pop", "Gravity"}},
    {"rotations", "Body rotations", {"Fast Flips", "Fast Spins"}},
    {"board", "Speed and board feel", {"Fast", "Auto Push", "No Speed Wobble", "Smooth Surfaces", "Long Wheelbase"}},
    {"grinds", "Grinds and landings", {"Sticky Grinds", "Slick Grinds", "Soft Landings", "Hard To Bail", "Revert Friction"}},
    {"foot", "On foot and falling", {"Moon Jump", "Fast On Foot", "Super Glide", "Torpedo Boost"}},
    {"experimental", "Experimental tweaks", {"Board Mount", "Easy Body Flips", "Deep Landings"}},
}};
NativeItem fun_header(const char *id) { return native_item((std::string("###fun-")+id).c_str()); }
NativeItem fun_preset_item(const char *name) { return native_item((std::string("##fun-preset-")+name).c_str()); }
NativeItem native_nested_item(const char *parent,const char *scope,const char *label) {
    for (auto *window : GImGui->Windows) if (window->Active) {
        const auto scope_id=ImHashStr(scope,0,window->GetID(parent));
        const auto found=native_items.find(ImHashStr(label,0,scope_id));
        if (found!=native_items.end() && found->second.frame==GImGui->FrameCount) return found->second;
    }
    throw std::runtime_error(std::string("Nested native widget was not submitted: ")+parent+"/"+scope+"/"+label);
}
std::size_t fun_preset_submissions() {
    std::size_t count{};
    for (const auto &[id,item] : native_items)
        if (item.frame==GImGui->FrameCount && item.window && item.window->Active && item.label.starts_with("##fun-preset-")) ++count;
    return count;
}
void fun_group_items(const FunPresetGroup &category,float scale) {
    require(fun_preset_submissions()==category.names.size(),"Only the opened category submits its exact preset membership");
    std::vector<ImGuiID> submitted;
    for (const auto *name : category.names) {
        const auto item=fun_preset_item(name);
        require(item.submissions==1 && std::find(submitted.begin(),submitted.end(),item.id)==submitted.end(),
                "Each named native Fun switch is submitted once with a distinct ID");
        require(item.rectangle.GetWidth()>300*scale && item.rectangle.GetHeight()>=36*scale,
                "Fun switches retain spacious native hit targets");
        submitted.push_back(item.id);
    }
}


int main(int argc, char **argv) {
    try {
        ImGui::CreateContext();
        auto &io = ImGui::GetIO(); io.IniFilename = nullptr; io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        GImGui->TestEngineHookItems = true;
        SkateMenu skate; load_skate_fonts(skate); skate.page = 4;
        Model model; model.steam_offline = true;
        std::vector<std::string> queued;
        CallbacksV3 callbacks{}; callbacks.user = &queued;
        callbacks.queue_console_command = [](void *user, const char *text, char *out, std::size_t size) {
            static_cast<std::vector<std::string> *>(user)->emplace_back(text);
            if(size) out[0] = 0; return true;
        };
        auto view = std::make_shared<physics::View>();
        view->ready = view->editable = view->capture_changed = true; view->map = "test-map";
        view->flip_trick.fill(1);
        const auto add = [&](const char *id, const char *label, double stock) {
            physics::Row row; row.id = id; row.label = row.friendly = label; row.group = "Physics";
            row.used = true; row.stock = row.value = stock; row.help = "Known fixture control with a long description for readable layout checks.";
            view->rows.push_back(row);
        };
        add("physicsmode.grindlockdist", "Grind lock-on distance", .9f);
        add("physicsgrindsair.maxdistboardslide", "Board slide capture distance", .75f);
        add("physicsgrindsair.maxdisttipslide", "Nose and tail slide capture distance", .7f);
        add("physicsgrind.commonfrictionscalar", "Grind friction", .35f);
        add("physicsmode.jumpmaxheight", "Ollie height", 1.575f);
        add("onboard_powerslide.frictionscalar_revert", "Revert friction", 3.0f);
        add("physicsmode.grindjumpcommonmax", "Maximum pop out of a grind", 2.3f);
        add("physicsmode.deepcrouch", "Landing: leg length (lower crouches deeper; deliberately long label)", .65f);
        add("physicsmode.pumpeffectfactor", "Transition pump strength", 1);
        add("physicsairstates.maxspinspeed", "Body spin speed", 4.2);
        view->groups.push_back("Physics");
        for(const auto &source : physics::builtin_presets()) {
            physics::PresetRow row; row.name=source.name; row.note=source.note; row.builtin=true;
            const auto dial=physics::preset_dial(source.name); row.title=dial.title; row.dial=!dial.title.empty();
            row.factor=1; row.amount=source.rules.empty()?1:source.rules.front().amount; row.modes=dial.modes;
            view->presets.push_back(std::move(row));
        }
        view->presets.push_back({"Envi Hardcore - preserved", "Your previous values stay separate from the new Hardcore default.", false, false});
        view->presets.back().values = {{"physicsmode.grindlockdist", .4050000011920929}, {"trick.flip_speed", .8}, {"unknown.old-game-value", 2}};
        unsigned char *atlas{}; int atlas_width{}, atlas_height{};
        io.Fonts->GetTexDataAsRGBA32(&atlas,&atlas_width,&atlas_height);
        bool visible=true;
        float width=1000, height=850, scale=1;
        const auto frame = [&] {
            io.DisplaySize=ImVec2(width+40,(height+150)*scale); io.DeltaTime=1.0f/60;
            model.menu_scale=scale; physics::publish(view);
            ImGui::NewFrame();
            // Production first-use defaults replace pending SetNextWindowSize data.
            // Resize the existing named fixture window directly on later frames.
            ImGui::SetWindowSize("ReSkate###skate-menu",ImVec2(width,height*scale),ImGuiCond_Always);
            draw_skate_menu(skate,model,callbacks,visible); ImGui::Render();
        };
        const auto click_item = [&](NativeItem item) {
            require(!item.disabled,"Click target must be enabled");
            const auto centre=item.rectangle.GetCenter();
            if (!item.window->ClipRect.Contains(centre)) throw std::runtime_error(std::string("Click target must be visible: ")+item.label);
            io.AddMousePosEvent(centre.x,centre.y); io.AddMouseButtonEvent(0,true); frame();
            io.AddMouseButtonEvent(0,false); frame(); frame();
        };
        const auto click = [&](const char *label) { click_item(native_item(label)); };
        const auto type_item = [&](NativeItem item,const char *text,bool commit) {
            require(!item.disabled,"Typed control must be enabled");
            const auto centre=item.rectangle.GetCenter();
            require(item.window->ClipRect.Contains(centre),"Typed control must be inside the native viewport");
            io.AddMousePosEvent(centre.x,centre.y); io.AddKeyEvent(ImGuiMod_Ctrl,true); frame();
            io.AddMouseButtonEvent(0,true); frame(); io.AddMouseButtonEvent(0,false); frame();
            io.AddKeyEvent(ImGuiMod_Ctrl,false); frame();
            require(GImGui->InputTextState.ID == item.id && ImGui::TempInputIsActive(item.id),"Ctrl-click opens the precise editor for the intended native widget ID");
            io.AddInputCharactersUTF8(text); frame();
            const auto key=commit?ImGuiKey_Enter:ImGuiKey_Escape;
            io.AddKeyEvent(key,true); frame(); io.AddKeyEvent(key,false); frame(); frame();
        };
        const auto save = [&](const char *name) {
            if(argc<2)return;
            const auto directory=std::filesystem::path(argv[1]); std::filesystem::create_directories(directory);
            image(directory/name,*ImGui::GetDrawData(),atlas,atlas_width,atlas_height);
        };
        frame(); frame();
        require(native_item("FEEL").rectangle.GetWidth()>100,"Primary navigation has readable targets");
        for(const auto *label:{"Hardcore","Authentic","Stock","Accessible","Arcade"}) native_item(label);
        click("Hardcore"); require(queued.empty(),"Choosing Hardcore must only change the preview");
        click("Apply this feel"); require(queued.size()==1 && queued.back()=="trainer workshop -1","Explicit Apply queues the selected feel");
        queued.clear(); save("feel-wide.ppm");
        click("FUN");
        require(native_item("Tricklining and reverts").status_flags&ImGuiItemStatusFlags_Opened,"Tricklining leads Fun and opens by default");
        const auto jump_header=native_item("Jump heights");
        require(jump_header.rectangle.Min.y>native_item("Tricklining and reverts").rectangle.Min.y,"Jump heights follows tricklining");
        float category_y=jump_header.rectangle.Max.y;
        for (const auto &category : fun_preset_groups) {
            const auto header=fun_header(category.id);
            require(header.rectangle.Min.y>=category_y,"Preset categories appear below tricklining and heights in the intended order");
            require(!(header.status_flags&ImGuiItemStatusFlags_Opened),"Each shortcut category starts collapsed");
            require(header.label==std::string(category.title)+" (0 on)###fun-"+category.id,"Collapsed category label reports its active count");
            category_y=header.rectangle.Max.y;
        }
        require(fun_preset_submissions()==0,"Collapsed categories submit no preset switches");
        require(queued.empty(),"Navigating to Fun applies no shortcut"); save("fun-wide.ppm");
        // Submit both expanded cards together. A tall headless viewport keeps the
        // real widgets visible, so clipping cannot hide a duplicate submission.
        height=3600; frame(); frame();
        click("Jump heights");
        click("What counts as a bend, and what it is worth"); frame();
        for (const auto *label : {"Friction and forward force","Transition tuning","Pop tuning","Rotation tuning","Manuals and grind tuning"})
            require(!(native_item(label).status_flags&ImGuiItemStatusFlags_Opened),"Raw tuning groups start collapsed when supported rows exist");
        click("Friction and forward force");
        const auto widgets_once = [&](ImGuiWindow *window, const char *label, int expected) {
            int count=0;
            for (const auto &[id,item] : native_items)
                if (item.frame==GImGui->FrameCount && item.window==window && item.label==label) {
                    require(id!=0 && item.submissions==1,"Each real widget ID must be submitted exactly once per frame");
                    ++count;
                }
            if (count!=expected) throw std::runtime_error(std::string("Expanded card widget count: ")+label+" expected "+std::to_string(expected)+", got "+std::to_string(count));
        };
        auto *tricks=native_item("Reset tricks...").window;
        require(tricks->BeginCount==1,"Both FUN sections must render the TRICKS card only once");
        widgets_once(tricks,"##option",4); widgets_once(tricks,"Reset tricks...",1);
        auto *reverts=native_item("Reset these rules").window;
        require(reverts!=tricks && reverts->BeginCount==1,"Revert rules retain their own single card");
        widgets_once(reverts,"##option",10); widgets_once(reverts,"##value",1); widgets_once(reverts,"##typed",0);
        widgets_once(reverts,"##freeze",1); widgets_once(reverts,"Reset these rules",1);
        widgets_once(reverts,"##toggle",0); widgets_once(reverts,"Apply tricklining extras",1); widgets_once(reverts,"Restore stock extras",1);
        require(queued.empty(),"Expanding both FUN sections does not change any setting");
        type_item(native_nested_item("What counts as a bend, and what it is worth","revert spin","##option"),"22.5",false);
        require(queued.empty(),"Escape cancels a precise revert rule without live dispatch");
        type_item(native_nested_item("What counts as a bend, and what it is worth","revert spin","##option"),"22.5",true);
        require(queued.size()==1 && queued.back()=="trainer revert spin 22.5","Enter commits the exact fractional revert command once");
        queued.clear();
        click("Apply tricklining extras");
        require(queued.size()==1 && queued.back()=="trainer trickline extras on","Extras apply is an explicit exact command, independent of bending amount");
        queued.clear(); click("Restore stock extras");
        require(queued.size()==1 && queued.back()=="trainer trickline extras off","Extras restore queues one exact stock command");
        queued.clear(); view->boosts_blocked=true; ++view->revision; frame(); frame();
        for (const auto *label : {"Apply tricklining extras","Restore stock extras","Reset these rules"})
            require(native_item(label).disabled,"Host boost restrictions disable extras and rule reset consistently");
        view->boosts_blocked=false; view->session_enforced=true; view->editable=true; ++view->revision; frame(); frame();
        for (const auto *label : {"Apply tricklining extras","Restore stock extras","Reset these rules"})
            require(native_item(label).disabled,"Session enforcement independently guards extras and rule reset");
        require(native_nested_item("What counts as a bend, and what it is worth","revert spin","##option").disabled,
                "Session enforcement independently guards precise revert rule editing");
        view->session_enforced=false; ++view->revision; frame(); frame();
        require(queued.empty(),"Host policy changes never apply or restore extras");
        click("Reset these rules");
        require(queued.size()==1 && queued.back()=="trainer revert reset","The surviving revert reset queues its exact command once");
        queued.clear(); click("Reset tricks...");
        require(queued.empty(),"Trick reset waits for confirmation across all Feel/Fun options");
        click("Confirm trick reset");
        require(queued.size()==1 && queued.back()=="trainer reset tricks","The single TRICKS reset remains functional");
        queued.clear();
        click("Jump heights"); click("Tricklining and reverts");
        // Presets are a progressive disclosure after tricklining and jump heights.
        std::size_t named_switches{};
        std::vector<ImGuiID> all_switch_ids;
        for (const auto &category : fun_preset_groups) {
            click_item(fun_header(category.id));
            require(queued.empty(),"Opening a preset category changes no physics");
            fun_group_items(category,scale);
            for (const auto *name : category.names) {
                const auto item=fun_preset_item(name);
                require(std::find(all_switch_ids.begin(),all_switch_ids.end(),item.id)==all_switch_ids.end(),
                        "Preset groups never duplicate another group's native switch");
                all_switch_ids.push_back(item.id); ++named_switches;
            }
            click_item(fun_header(category.id));
            require(fun_preset_submissions()==0,"Closing a category removes its preset controls from submission");
        }
        require(named_switches==23 && queued.empty(),"All twenty-three community shortcuts survive grouping without mutations");
        click_item(fun_header("pop"));
        const auto pop_header_id=fun_header("pop").id;
        click_item(fun_preset_item("Super Ollie"));
        require(queued.size()==1 && queued.back()=="trainer preset apply Super Ollie","Inactive switch queues one exact apply command");
        queued.clear();
        const auto super_ollie_iterator=std::find_if(view->presets.begin(),view->presets.end(),[](const auto &preset){return preset.name=="Super Ollie";});
        require(super_ollie_iterator!=view->presets.end(),"Native preset fixture contains Super Ollie");
        auto &super_ollie=*super_ollie_iterator;
        super_ollie.active=true; ++view->revision; frame(); frame();
        require(fun_header("pop").id==pop_header_id && (fun_header("pop").status_flags&ImGuiItemStatusFlags_Opened),
                "Active-count changes retain the category's ID and open state");
        require(fun_header("pop").label=="Pop and airtime (1 on)###fun-pop","Category count reflects the authoritative active snapshot");
        click_item(fun_preset_item("Super Ollie"));
        require(queued.size()==1 && queued.back()=="trainer preset remove Super Ollie","Active switch queues one exact remove command");
        queued.clear(); super_ollie.active=false;
        view->session_enforced=true; view->editable=true; ++view->revision; frame(); frame();
        require(fun_preset_item("Super Ollie").disabled,"Host-enforced physics disables native preset switches");
        view->session_enforced=false; view->editable=true; view->boosts_blocked=true; ++view->revision; frame(); frame();
        require(!fun_preset_item("Super Ollie").disabled,"Native physics presets remain available when only script boosts are blocked");
        click_item(fun_header("pop")); click_item(fun_header("board"));
        require(fun_preset_item("Auto Push").disabled && !fun_preset_item("Fast").disabled,
                "Host boost policy disables Auto Push while preserving permitted physics presets");
        view->boosts_blocked=false; ++view->revision; frame(); frame();
        const auto dispatch=callbacks.queue_console_command; callbacks.queue_console_command=nullptr; frame(); frame();
        for (const auto *name : fun_preset_groups[2].names)
            require(fun_preset_item(name).disabled,"Unavailable command dispatcher disables every opened switch");
        callbacks.queue_console_command=dispatch; frame(); frame(); click_item(fun_header("board"));
        require(queued.empty(),"Host and dispatcher changes do not queue shortcut mutations");

        height=850; frame(); frame();
        // Every new option is a real full-width native control in Feel.
        click("FEEL"); height=3600; frame(); frame();
        click("Flips and catches"); click("Individual flip speeds"); click("Transition pumping");
        require(queued.empty(),"Opening new Feel groups applies no setting");
        auto board=native_scoped_item("flip_speed","##option");
        require(board.rectangle.GetWidth()>600 && !board.disabled,"Global board flip speed keeps a full-width editor while states prepare");
        for (const auto &trick : physics::flip_tricks) {
            const auto item=native_scoped_item(std::string(trick.key).c_str(),"##option");
            require(item.disabled && item.rectangle.GetWidth()>600,"Disabled per-trick editors retain their stored values and spacious layout");
        }
        click_item(native_scoped_item("Per-trick flip speeds","##toggle"));
        require(queued.size()==1 && queued.back()=="trainer option flip_advanced 1","Advanced flip toggle queues its exact option once");
        queued.clear(); view->flip_advanced=true; ++view->revision; frame(); frame();
        for (const auto &trick : physics::flip_tricks) {
            const auto item=native_scoped_item(std::string(trick.key).c_str(),"##option");
            require(!item.disabled,"All sixteen per-trick speed controls become editable");
            const auto id=item.window->GetID(std::string(trick.key).c_str());
            const auto actual=ImHashStr("##option",0,id);
            require(native_items.at(actual).submissions==1,"Each actual per-trick widget ID is submitted only once");
        }
        type_item(native_scoped_item("flip_speed","##option"),"0.375",false);
        require(queued.empty(),"Escape cancels a board flip edit without changing physics");
        type_item(native_scoped_item("flip_speed","##option"),"0.375",true);
        require(queued.size()==1 && queued.back()=="trainer option flip_speed 0.375","Board flip edit commits its exact command once");
        queued.clear(); type_item(native_scoped_item("flip.hardflip","##option"),"1.625",true);
        require(queued.size()==1 && queued.back()=="trainer option flip.hardflip 1.625","Individual trick edits retain the correct key and precision");
        queued.clear(); type_item(native_scoped_item("pump_power","##option"),"0.5",true);
        require(queued.size()==1 && queued.back()=="trainer option pump_power 0.5","Pump option commits the exact factor");
        queued.clear(); click_item(native_scoped_item("Automatic catch timing","##toggle"));
        require(queued.size()==1 && queued.back()=="trainer option catch_at 1","Automatic catch is a separate explicit choice");
        queued.clear(); view->catch_at=true; ++view->revision; frame(); frame();
        require(native_scoped_item("flip_speed","##option").disabled,"Timed catch takes priority over the global speed editor");
        require(native_scoped_item("flip.kickflip","##option").disabled,"Timed catch also takes priority over per-trick editors");
        type_item(native_scoped_item("catch_percent","##option"),"67.5",true);
        require(queued.size()==1 && queued.back()=="trainer option catch_percent 67.5","Catch timing keeps fractional precision");
        queued.clear(); view->catch_at=false; view->boosts_blocked=true; ++view->revision; frame(); frame();
        for(const auto *key:{"flip_speed","flip.kickflip","catch_percent","pump_power"})
            require(native_scoped_item(key,"##option").disabled,"Host boost restrictions disable the new effect controls");
        require(native_scoped_item("Automatic catch timing","##toggle").disabled,"Catch toggle explains the same host restriction");
        view->boosts_blocked=false; view->session_enforced=true; view->editable=true; ++view->revision; frame(); frame();
        require(native_scoped_item("pump_power","##option").disabled,"Enforced host physics blocks pump effects");
        require(queued.empty(),"Readiness and host-rule changes never submit mutations");
        view->session_enforced=false; view->editable=true; view->flip_live=view->pump_live=true; ++view->revision; frame(); frame();
        click("Flips and catches"); click("Transition pumping"); height=850; frame(); frame();
        click("SETTINGS");
        auto scalar=native_scoped_item("physicsmode.grindlockdist","##value");
        require(scalar.rectangle.GetWidth()>600,"Settings use the full content width below labels"); save("settings-wide.ppm");
        // The temporary text editor must keep exact precision and cancel without a command.
        const auto type_scalar = [&](const char *text,bool commit) { type_item(native_scoped_item("physicsmode.grindlockdist","##value"),text,commit); };
        type_scalar("0.4050000011920929",false);
        require(queued.empty(),"Escape cancels a typed scalar without applying any setting");
        type_scalar("0.4050000011920929",true);
        require(queued.size()==1 && queued.back()=="trainer set physicsmode.grindlockdist 0.4050000011920929","Enter commits the precise value exactly once");
        queued.clear();
        click("PRESETS"); native_item("Save current setup");
        click("Review preset"); require(queued.empty(),"Reviewing a saved preset applies no physics");
        native_item("Apply reviewed preset"); save("preset-review.ppm");
        click("Apply reviewed preset");
        require(queued.size()==1 && queued.back()=="trainer preset apply Envi Hardcore - preserved","Reviewed preset uses the exact saved name");
        queued.clear();
        view->capture_changed=false; ++view->revision; frame();
        require(native_item("Save current setup").disabled,"Unchanged setup cannot be saved");
        // A trick-only change enables Save after entering a name, without a touched physics row.
        view->capture_changed=true; view->touched=0; ++view->revision;
        click("##preset-name"); io.AddInputCharactersUTF8("Tricks only"); frame();
        require(!native_item("Save current setup").disabled,"Script-only edits can be saved");
        click("Save current setup"); require(queued.back()=="trainer preset save Tricks only","Save queues the exact name");
        queued.clear(); save("presets-wide.ppm");
        click("FEEL");
        width=750; frame(); frame(); save("feel-narrow.ppm");
        for(const auto *label:{"FEEL","SETTINGS","PRESETS","FUN","TOOLS"}) {
            const auto item=native_item(label);
            require(item.rectangle.Min.x>=item.window->InnerClipRect.Min.x && item.rectangle.Max.x<=item.window->InnerClipRect.Max.x+1,"Primary navigation stays inside narrow content");
        }
        click("SETTINGS"); frame(); scalar=native_scoped_item("physicsmode.grindlockdist","##value");
        require(scalar.rectangle.GetWidth()>400,"Narrow settings retain roomy full-width editors"); save("settings-narrow.ppm");
        click("FEEL"); scale=2; width=1500; frame(); frame(); save("feel-2x.ppm");
        click("TOOLS"); for(const auto *label:{"PRACTICE","MAP & HUD"}) native_item(label);
        require(queued.empty(),"Layout changes and navigation never queue physics mutations");
        // Visual captures use normal viewports and the real native window state.
        // Tall frames above are solely for full widget-inventory/input coverage.
        if (argc >= 2) {
            click("FEEL");
            view->flip_gate_found=true;
            const auto section = [&](const char *label, bool open) {
                const auto item=native_item(label);
                item.window->StateStorage.SetInt(item.id,open?1:0);
                frame(); frame();
            };
            const auto scroll_to = [&](NativeItem item) {
                ImGui::SetScrollFromPosY(item.window,item.rectangle.Min.y-item.window->Pos.y,0);
                frame(); frame();
            };
            struct Layout { const char *suffix; float width, scale; };
            for (const auto layout : {Layout{"wide",1000,1},Layout{"narrow",750,1},Layout{"2x",1500,2}}) {
                width=layout.width; scale=layout.scale; height=850; frame(); frame();
                click("FEEL");
                for (const auto *label : {"Pop and airtime","Speed and rotations","Landings and bails","Grinds and slides","Review affected values"}) section(label,false);
                section("Flips and catches",true); section("Individual flip speeds",false); section("Transition pumping",false);
                scroll_to(native_item("Flips and catches"));
                const auto capture_board=native_scoped_item("flip_speed","##option");
                require(!capture_board.disabled && capture_board.rectangle.GetWidth()>400*scale,"Captured flip editor remains full-width and active");
                save((std::string("flips-")+layout.suffix+".ppm").c_str());
                section("Individual flip speeds",true);
                scroll_to(native_item("Individual flip speeds"));
                save((std::string("flip-tricks-")+layout.suffix+".ppm").c_str());
                section("Individual flip speeds",false); section("Flips and catches",false); section("Transition pumping",true);
                scroll_to(native_item("Transition pumping"));
                require(!native_scoped_item("pump_power","##option").disabled,"Captured pump control is ready");
                save((std::string("pumping-")+layout.suffix+".ppm").c_str());
                click("FUN"); section("Tricklining and reverts",false); section("Jump heights",true);
                scroll_to(native_item("Jump heights"));
                require(!native_scoped_item("nocomply_height","##option").disabled,"Captured Fun controls are available");
                save((std::string("fun-boosts-")+layout.suffix+".ppm").c_str());
                section("Jump heights",false);
            }
            // One-column narrow/2x and two-column wide native switch layouts.
            super_ollie.active=true; view->stock=false; view->touched=1; ++view->revision;
            for (const auto layout : {Layout{"wide",1250,1},Layout{"narrow",750,1},Layout{"2x",1500,2}}) {
                width=layout.width; scale=layout.scale; height=850; frame(); frame(); click("FUN");
                section("Tricklining and reverts",false); section("Jump heights",false);
                for (const auto &category : fun_preset_groups) section((std::string("###fun-")+category.id).c_str(),false);
                ImGui::SetScrollY(fun_header("pop").window,0); frame(); frame();
                save((std::string("fun-overview-")+layout.suffix+".ppm").c_str());
                section("###fun-pop",true); scroll_to(fun_header("pop"));
                fun_group_items(fun_preset_groups[0],scale);
                std::vector<NativeItem> switches;
                for (const auto *name : fun_preset_groups[0].names) switches.push_back(fun_preset_item(name));
                bool beside=false;
                for (std::size_t i=0;i<switches.size();++i) for (std::size_t j=i+1;j<switches.size();++j) {
                    require(!switches[i].rectangle.Overlaps(switches[j].rectangle),"Responsive native switch hit targets never overlap");
                    if (std::abs(switches[i].rectangle.Min.y-switches[j].rectangle.Min.y)<1 &&
                        std::abs(switches[i].rectangle.Min.x-switches[j].rectangle.Min.x)>100*scale) beside=true;
                }
                require(beside==(layout.scale==1 && layout.width==1250),"Wide switches use two columns; narrow and 2x use one");
                save((std::string("fun-toggles-")+layout.suffix+".ppm").c_str());
                section("###fun-pop",false);
            }
            super_ollie.active=false; view->stock=true; view->touched=0; ++view->revision;
            for (const auto layout : {Layout{"wide",1000,1},Layout{"narrow",620,1},Layout{"2x",1240,2}}) {
                width=layout.width; scale=layout.scale; height=850; frame(); frame(); click("FUN");
                section("Tricklining and reverts",true); section("Jump heights",false);
                section("What counts as a bend, and what it is worth",false); section("Friction and forward force",false);
                scroll_to(native_item("Tricklining and reverts"));
                const auto apply_extras=native_item("Apply tricklining extras"), restore_extras=native_item("Restore stock extras");
                require(!apply_extras.disabled && !restore_extras.disabled,"Captured extras actions remain available");
                require(!apply_extras.rectangle.Overlaps(restore_extras.rectangle),"Responsive extras action hit targets never overlap");
                const bool extras_paired=std::abs(apply_extras.rectangle.Min.y-restore_extras.rectangle.Min.y)<1;
                require(extras_paired==(layout.scale==1 && layout.width==1000),"Extras pair at wide sizes and stack in compact or scaled viewports");
                require(apply_extras.rectangle.GetWidth()>250*scale && restore_extras.rectangle.GetWidth()>250*scale,
                        "Extras actions retain readable spacious targets");
                save((std::string("fun-tricklining-default-")+layout.suffix+".ppm").c_str());
                section("Tricklining and reverts",false);
            }

            width=1000; scale=1; frame(); frame(); click("FEEL");
            section("Transition pumping",false); section("Flips and catches",true); section("Individual flip speeds",false);
            view->catch_at=true; ++view->revision; frame(); frame();
            scroll_to(native_item("Flips and catches")); save("catch-timing-wide.ppm");
            view->catch_at=false; view->boosts_blocked=true; ++view->revision; frame(); frame();
            ImGui::SetScrollY(native_item("Flips and catches").window,0); frame(); frame(); save("host-boosts-wide.ppm");
            view->boosts_blocked=false; ++view->revision; frame(); frame();
            section("Flips and catches",false);
            require(queued.empty(),"Capture-only open/scroll/layout changes apply no physics");
        }
        click("FEEL"); view->editable=false; view->blocked="The host controls physics."; ++view->revision; frame();
        require(native_item("Apply this feel").disabled,"Guest cannot apply a feel under host control");
        require(ImGui::GetDrawData()->TotalVtxCount>0,"Real native widgets produce draw data");
        ImGui::DestroyContext();
        std::cout<<"Native workshop navigation, preview/apply, preset saving, widths and host guards passed\n";
    }catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}
