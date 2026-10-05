// Shared cockpit palette and presentation helpers. No game state or I/O.
#pragma once
#include "imgui.h"
#include <algorithm>

namespace ets2::ui {
inline const ImVec4 background{0.067f, 0.078f, 0.086f, 1.0f};
inline const ImVec4 panel{0.102f, 0.118f, 0.129f, 1.0f};
inline const ImVec4 raised{0.126f, 0.145f, 0.161f, 1.0f};
inline const ImVec4 border{0.188f, 0.212f, 0.227f, 1.0f};
inline const ImVec4 foreground{0.933f, 0.918f, 0.898f, 1.0f};
inline const ImVec4 muted{0.643f, 0.667f, 0.682f, 1.0f};
inline const ImVec4 accent{0.929f, 0.667f, 0.400f, 1.0f};
inline const ImVec4 accentSoft{0.247f, 0.208f, 0.165f, 1.0f};
inline const ImVec4 ink{0.129f, 0.094f, 0.059f, 1.0f};
inline const ImVec4 success{0.545f, 0.741f, 0.600f, 1.0f};

inline void applyStyle() {
    ImGui::StyleColorsDark();
    auto& s = ImGui::GetStyle();
    s.WindowRounding = 0;
    s.ChildRounding = 12;
    s.FrameRounding = 7;
    s.PopupRounding = 10;
    s.ScrollbarRounding = 8;
    s.GrabRounding = 5;
    s.TabRounding = 7;
    s.WindowPadding = ImVec2(20, 16);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(12, 8);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.IndentSpacing = 20;
    s.ScrollbarSize = 12;
    s.FrameBorderSize = 0;
    s.ChildBorderSize = 1;
    auto* c = s.Colors;
    c[ImGuiCol_Text] = foreground;
    c[ImGuiCol_TextDisabled] = muted;
    c[ImGuiCol_WindowBg] = background;
    c[ImGuiCol_ChildBg] = panel;
    c[ImGuiCol_PopupBg] = panel;
    c[ImGuiCol_Border] = border;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = background;
    c[ImGuiCol_FrameBgHovered] = raised;
    c[ImGuiCol_FrameBgActive] = accentSoft;
    c[ImGuiCol_TitleBg] = background;
    c[ImGuiCol_TitleBgActive] = panel;
    c[ImGuiCol_TitleBgCollapsed] = background;
    c[ImGuiCol_MenuBarBg] = panel;
    c[ImGuiCol_ScrollbarBg] = background;
    c[ImGuiCol_ScrollbarGrab] = border;
    c[ImGuiCol_ScrollbarGrabHovered] = muted;
    c[ImGuiCol_ScrollbarGrabActive] = accent;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = foreground;
    c[ImGuiCol_Button] = raised;
    c[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.25f, 0.27f, 1);
    c[ImGuiCol_ButtonActive] = accentSoft;
    c[ImGuiCol_Header] = accentSoft;
    c[ImGuiCol_HeaderHovered] = ImVec4(0.29f, 0.25f, 0.20f, 1);
    c[ImGuiCol_HeaderActive] = ImVec4(0.36f, 0.28f, 0.20f, 1);
    c[ImGuiCol_Separator] = border;
    c[ImGuiCol_SeparatorHovered] = accent;
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = border;
    c[ImGuiCol_ResizeGripHovered] = muted;
    c[ImGuiCol_ResizeGripActive] = accent;
    c[ImGuiCol_Tab] = panel;
    c[ImGuiCol_TabHovered] = accentSoft;
    c[ImGuiCol_TabSelected] = accentSoft;
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = background;
    c[ImGuiCol_TabDimmedSelected] = raised;
    c[ImGuiCol_TabDimmedSelectedOverline] = muted;
    c[ImGuiCol_TableHeaderBg] = raised;
    c[ImGuiCol_TableBorderStrong] = border;
    c[ImGuiCol_TableBorderLight] = border;
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
    c[ImGuiCol_TextSelectedBg] = accentSoft;
    c[ImGuiCol_NavCursor] = accent;
    c[ImGuiCol_PlotHistogram] = accent;
    c[ImGuiCol_PlotHistogramHovered] = foreground;
}

inline void text(const ImVec4& color, const char* value) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", value);
    ImGui::PopStyleColor();
}

template <typename... Args>
inline void textf(const ImVec4& color, const char* format, Args... args) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped(format, args...);
    ImGui::PopStyleColor();
}

inline void heading(const char* value, float size = 22.0f) {
    ImGui::PushFont(nullptr, size);
    ImGui::TextUnformatted(value);
    ImGui::PopFont();
}

inline void beginCard(const char* id, const char* title = nullptr) {
    ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
    if (title) {
        heading(title, 18);
        ImGui::Spacing();
    }
}
inline void endCard() { ImGui::EndChild(); }

inline bool primaryButton(const char* label, ImVec2 size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 0.74f, 0.49f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.80f, 0.55f, 0.31f, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, ink);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return clicked;
}

inline bool choice(const char* label, bool selected, ImVec2 size = ImVec2(0, 0)) {
    if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Button, accentSoft);
        ImGui::PushStyleColor(ImGuiCol_Text, accent);
    }
    const bool clicked = ImGui::Button(label, size);
    if (selected) ImGui::PopStyleColor(2);
    return clicked;
}

// Wrap toolbars naturally instead of keeping legacy absolute SameLine offsets.
inline void next(float width) {
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <= right)
        ImGui::SameLine();
}
inline void nextButton(const char* label) {
    next(ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2);
}
}  // namespace ets2::ui
