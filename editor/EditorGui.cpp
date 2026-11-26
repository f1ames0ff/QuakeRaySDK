// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//

#include "EditorGui.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace
{

float g_uiScale = 1.0f;
bool g_iconFontLoaded = false;

constexpr const char *kFaRotateLeft = "\xEF\x8B\xAA";
constexpr const char *kFaCopy = "\xEF\x83\x85";
constexpr const char *kFaPaste = "\xEF\x83\xAA";
constexpr const char *kFaTrash = "\xEF\x87\xB8";
constexpr const char *kFaEyedropper = "\xEF\x87\xBB";
constexpr const char *kFaDrawPolygon = "\xEF\x97\xAE";
constexpr const char *kFaPaintbrush = "\xEF\x87\xBC";

float Scaled(float px)
{
    return std::round(px * g_uiScale);
}

ImVec4 ColorU8(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
{
    return ImVec4(float(r) / 255.0f, float(g) / 255.0f, float(b) / 255.0f, float(a) / 255.0f);
}

ImU32 ColorImU32(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
{
    return IM_COL32(r, g, b, a);
}

void ShowTooltip(const char *tooltip, bool allowWhenDisabled = false)
{
    const ImGuiHoveredFlags flags =
        ImGuiHoveredFlags_DelayShort |
        (allowWhenDisabled ? ImGuiHoveredFlags_AllowWhenDisabled : ImGuiHoveredFlags_None);
    if (tooltip == nullptr || tooltip[0] == '\0' || !ImGui::IsItemHovered(flags))
    {
        return;
    }

    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(Scaled(340.0f));
    ImGui::TextUnformatted(tooltip);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void ReportItemRect(const EditorGuiRowOpts &opts)
{
    if (opts.logRect && opts.rectName != nullptr && opts.rectName[0] != '\0')
    {
        opts.logRect(opts.rectName);
    }
}

std::string MakeIconButtonLabel(const char *faGlyph, const char *fallbackAscii, const char *idSuffix)
{
    std::string label = g_iconFontLoaded ? faGlyph : fallbackAscii;
    label += idSuffix;
    return label;
}

bool IconButton(const char *faGlyph, const char *fallbackAscii, const char *idSuffix, ImVec2 size,
                ImVec4 textColor, const char *tooltip, bool allowWhenDisabled = false)
{
    const std::string label = MakeIconButtonLabel(faGlyph, fallbackAscii, idSuffix);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, textColor);
    const bool clicked = ImGui::Button(label.c_str(), size);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ShowTooltip(tooltip, allowWhenDisabled);
    return clicked;
}

void BeginTwoColumnRow(const char *label, const EditorGuiRowOpts &opts, float rightReserve)
{
    const float labelWidth = Scaled(kEditorGuiLabelWidth);
    const float rowWidth = std::max(Scaled(80.0f), ImGui::GetContentRegionAvail().x);
    const float controlWidth =
        std::max(Scaled(48.0f), rowWidth - labelWidth - Scaled(kEditorGuiResetSize) - rightReserve -
                                    ImGui::GetStyle().ItemSpacing.x * 2.0f);

    ImGui::AlignTextToFramePadding();
    if (!opts.enabled)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ColorU8(130, 140, 152));
    }
    else if (opts.changed)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ColorU8(232, 200, 118));
    }

    ImGui::TextUnformatted(label);
    if (!opts.enabled || opts.changed)
    {
        ImGui::PopStyleColor();
    }
    ShowTooltip(opts.tooltip);

    ImGui::SameLine(labelWidth);
    ImGui::SetNextItemWidth(controlWidth);
}

void EndTwoColumnRow(const char *id, const EditorGuiRowOpts &opts)
{
    ReportItemRect(opts);
    ShowTooltip(opts.tooltip);

    if (opts.resetClicked != nullptr)
    {
        ImGui::SameLine();
        if (EditorGuiResetButton(id, opts.changed && opts.enabled))
        {
            *opts.resetClicked = true;
        }
    }
    else
    {
        ImGui::SameLine();
        const float size = Scaled(kEditorGuiResetSize);
        ImGui::Dummy(ImVec2(size, size));
    }
}

void ApplyPalette(ImGuiStyle &style)
{
    style.Colors[ImGuiCol_Text] = ColorU8(234, 238, 244);
    style.Colors[ImGuiCol_TextDisabled] = ColorU8(132, 140, 152);
    style.Colors[ImGuiCol_WindowBg] = ColorU8(17, 21, 27, 236);
    style.Colors[ImGuiCol_ChildBg] = ColorU8(15, 19, 25, 0);
    style.Colors[ImGuiCol_PopupBg] = ColorU8(22, 27, 35, 246);
    style.Colors[ImGuiCol_Border] = ColorU8(64, 76, 94, 185);
    style.Colors[ImGuiCol_FrameBg] = ColorU8(32, 39, 49, 225);
    style.Colors[ImGuiCol_FrameBgHovered] = ColorU8(45, 56, 72, 240);
    style.Colors[ImGuiCol_FrameBgActive] = ColorU8(56, 74, 98, 255);
    style.Colors[ImGuiCol_TitleBg] = ColorU8(20, 26, 34, 255);
    style.Colors[ImGuiCol_TitleBgActive] = ColorU8(26, 35, 48, 255);
    style.Colors[ImGuiCol_MenuBarBg] = ColorU8(20, 26, 34, 248);
    style.Colors[ImGuiCol_CheckMark] = ColorU8(102, 178, 255);
    style.Colors[ImGuiCol_SliderGrab] = ColorU8(78, 146, 224);
    style.Colors[ImGuiCol_SliderGrabActive] = ColorU8(112, 178, 250);
    style.Colors[ImGuiCol_Button] = ColorU8(40, 50, 64, 235);
    style.Colors[ImGuiCol_ButtonHovered] = ColorU8(56, 76, 102, 245);
    style.Colors[ImGuiCol_ButtonActive] = ColorU8(68, 98, 134, 255);
    style.Colors[ImGuiCol_Header] = ColorU8(50, 74, 104, 210);
    style.Colors[ImGuiCol_HeaderHovered] = ColorU8(64, 96, 134, 235);
    style.Colors[ImGuiCol_HeaderActive] = ColorU8(78, 118, 166, 255);
    style.Colors[ImGuiCol_Separator] = ColorU8(66, 78, 96, 160);
    style.Colors[ImGuiCol_ScrollbarBg] = ColorU8(18, 22, 28, 160);
    style.Colors[ImGuiCol_ScrollbarGrab] = ColorU8(64, 78, 96, 210);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = ColorU8(86, 104, 128, 235);
    style.Colors[ImGuiCol_ScrollbarGrabActive] = ColorU8(104, 130, 164, 255);
}

bool ContainsCaseInsensitive(const char *haystack, const char *needle)
{
    if (needle == nullptr || needle[0] == '\0')
    {
        return true;
    }

    if (haystack == nullptr)
    {
        return false;
    }

    const size_t nLen = std::strlen(needle);
    for (const char *p = haystack; *p != '\0'; ++p)
    {
        size_t i = 0;
        while (i < nLen && p[i] != '\0' &&
               std::tolower(static_cast<unsigned char>(p[i])) ==
                   std::tolower(static_cast<unsigned char>(needle[i])))
        {
            ++i;
        }
        if (i == nLen)
        {
            return true;
        }
    }

    return false;
}

void ColorToHex(const float rgb[3], char outHex[8])
{
    const auto byteOf = [](float v) {
        return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)) & 0xff;
    };
    std::snprintf(outHex, 8, "%02X%02X%02X", byteOf(rgb[0]), byteOf(rgb[1]), byteOf(rgb[2]));
}

bool HexToColor(const char *text, float outRgb[3])
{
    const char *src = text[0] == '#' ? text + 1 : text;
    if (std::strlen(src) != 6)
    {
        return false;
    }

    unsigned int r = 0;
    unsigned int g = 0;
    unsigned int b = 0;
    if (std::sscanf(src, "%02x%02x%02x", &r, &g, &b) != 3)
    {
        return false;
    }

    outRgb[0] = float(r) / 255.0f;
    outRgb[1] = float(g) / 255.0f;
    outRgb[2] = float(b) / 255.0f;
    return true;
}

} // namespace

void EditorGuiApplyStyle(float displayWidth, float displayHeight)
{
    if (displayHeight >= 1800.0f || displayWidth >= 3200.0f)
    {
        g_uiScale = 1.45f;
    }
    else if (displayHeight >= 1300.0f || displayWidth >= 2300.0f)
    {
        g_uiScale = 1.20f;
    }
    else
    {
        g_uiScale = 1.0f;
    }

    ImGui::GetIO().FontGlobalScale = g_uiScale * (19.0f / 17.0f);

    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = Scaled(5.0f);
    style.ChildRounding = Scaled(4.0f);
    style.FrameRounding = Scaled(3.0f);
    style.PopupRounding = Scaled(4.0f);
    style.ScrollbarRounding = Scaled(4.0f);
    style.GrabRounding = Scaled(2.0f);
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.WindowPadding = ImVec2(Scaled(12.0f), Scaled(10.0f));
    style.FramePadding = ImVec2(Scaled(7.0f), Scaled(4.0f));
    style.ItemSpacing = ImVec2(Scaled(7.0f), Scaled(5.0f));
    style.ItemInnerSpacing = ImVec2(Scaled(5.0f), Scaled(4.0f));
    style.ScrollbarSize = Scaled(12.0f);
    style.GrabMinSize = Scaled(10.0f);
    ApplyPalette(style);
}

float EditorGuiScale()
{
    return g_uiScale;
}

bool EditorGuiBeginPanel(const char *title, float displayWidth, float displayHeight)
{
    const float margin = Scaled(10.0f);
    const float topOffset = ImGui::GetFrameHeight() + margin;
    const float panelW = std::min(Scaled(kEditorGuiPanelWidth), std::max(280.0f, displayWidth - 2.0f * margin));
    const float panelH = std::max(200.0f, displayHeight - topOffset - margin);

    ImGui::SetNextWindowPos(ImVec2(displayWidth - panelW - margin, topOffset), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panelW, panelH), ImGuiCond_Always);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse;
    if (!ImGui::Begin("##QuakeRayEditorPanel", nullptr, flags))
    {
        return false;
    }

    ImGui::PushStyleColor(ImGuiCol_Text, ColorU8(130, 192, 255));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::Separator();
    return true;
}

void EditorGuiEndPanel()
{
    ImGui::End();
}

void EditorGuiBeginScroll(const char *id)
{
    ImGui::BeginChild(id, ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar);
}

void EditorGuiEndScroll()
{
    ImGui::EndChild();
}

bool EditorGuiTabs(const char *const *labels, int32_t count, int32_t *activeIndex,
                   bool showResetTrash, bool *outResetClicked, const char *resetTooltip)
{
    if (outResetClicked != nullptr)
    {
        *outResetClicked = false;
    }

    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float trashSize = showResetTrash ? ImGui::GetFrameHeight() : 0.0f;
    const float avail = ImGui::GetContentRegionAvail().x - (showResetTrash ? (trashSize + spacing) : 0.0f);
    const float tabW = count > 0 ? std::max(Scaled(50.0f), (avail - spacing * float(count - 1)) / float(count)) : avail;
    bool changed = false;

    for (int32_t i = 0; i < count; ++i)
    {
        if (i > 0)
        {
            ImGui::SameLine();
        }
        const bool active = *activeIndex == i;
        if (active)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ColorU8(60, 114, 178));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ColorU8(74, 132, 198));
        }
        if (ImGui::Button(labels[i], ImVec2(tabW, 0.0f)))
        {
            *activeIndex = i;
            changed = true;
        }
        if (active)
        {
            ImGui::PopStyleColor(2);
        }
    }

    if (showResetTrash)
    {
        ImGui::SameLine();
        if (IconButton(kFaTrash, "X", "##TabResetTrash", ImVec2(trashSize, trashSize),
                       ColorU8(236, 178, 118), resetTooltip) &&
            outResetClicked != nullptr)
        {
            *outResetClicked = true;
        }
    }

    return changed;
}

void EditorGuiSetIconFontLoaded(bool loaded)
{
    g_iconFontLoaded = loaded;
}

void EditorGuiSectionHeader(const char *label, const char *tooltip)
{
    ImGui::Spacing();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = ImGui::GetTextLineHeight() + Scaled(6.0f);
    ImGui::GetWindowDrawList()->AddRectFilled(
        pos, ImVec2(pos.x + width, pos.y + height), ColorImU32(34, 46, 62, 175), Scaled(2.0f));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Scaled(6.0f));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Scaled(3.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ColorU8(162, 206, 250));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ShowTooltip(tooltip);
    ImGui::Spacing();
}

bool EditorGuiResetButton(const char *id, bool changed, const char *tooltip)
{
    const float size = Scaled(kEditorGuiResetSize);
    if (!changed)
    {
        ImGui::Dummy(ImVec2(size, size));
        return false;
    }

    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_Button, ColorU8(46, 40, 26, 210));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ColorU8(76, 64, 34, 240));
    const bool clicked = IconButton(kFaRotateLeft, "R", "##rst", ImVec2(size, size),
                                    ColorU8(246, 198, 92),
                                    tooltip != nullptr ? tooltip : "Reset to loaded value");
    ImGui::PopStyleColor(2);
    ImGui::PopID();
    return clicked;
}

uint32_t EditorGuiBlockActions(const char *id, bool canCopy, bool canPaste, bool canRemove)
{
    uint32_t actions = kEditorGuiActionNone;
    const float s = Scaled(kEditorGuiActionSize);
    const ImVec4 activeCol = ColorU8(215, 224, 236);
    const ImVec4 disabledCol = ColorU8(110, 118, 128);
    ImGui::PushID(id);

    ImGui::BeginDisabled(!canCopy);
    if (IconButton(kFaCopy, "C", "##cp", ImVec2(s, s), canCopy ? activeCol : disabledCol, "Copy"))
    {
        actions |= kEditorGuiActionCopy;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!canPaste);
    if (IconButton(kFaPaste, "V", "##ps", ImVec2(s, s), canPaste ? activeCol : disabledCol,
                   "Paste"))
    {
        actions |= kEditorGuiActionPaste;
    }
    ImGui::EndDisabled();

    if (canRemove)
    {
        ImGui::SameLine();
        if (IconButton(kFaTrash, "X", "##rm", ImVec2(s, s), ColorU8(238, 118, 118), "Remove"))
        {
            actions |= kEditorGuiActionRemove;
        }
    }

    ImGui::PopID();
    return actions;
}

int32_t EditorGuiMaskAddButtons(const char *id, bool canAdd)
{
    int32_t clicked = -1;
    const float s = Scaled(26.0f);
    const ImVec4 col = canAdd ? ColorU8(215, 224, 236) : ColorU8(110, 118, 128);

    ImGui::PushID(id);
    ImGui::BeginDisabled(!canAdd);

    struct AddBtnSpec
    {
        const char *glyph;
        const char *fallback;
        const char *btnId;
        const char *tooltip;
        const char *disabledTooltip;
        int32_t result;
    };
    const AddBtnSpec specs[3] = {
        { kFaEyedropper, "C", "##add_color", "Add color", "Add color (block limit reached)", 0 },
        { kFaDrawPolygon, "M", "##add_mask", "Add mask", "Add mask (block limit reached)", 1 },
        { kFaPaintbrush, "B", "##add_brush", "Add brush", "Add brush (block limit reached)", 2 },
    };

    for (int32_t i = 0; i < 3; ++i)
    {
        if (i > 0)
        {
            ImGui::SameLine();
        }
        if (IconButton(specs[i].glyph, specs[i].fallback, specs[i].btnId, ImVec2(s, s), col,
                       canAdd ? specs[i].tooltip : specs[i].disabledTooltip, true))
        {
            clicked = specs[i].result;
        }
    }

    ImGui::EndDisabled();
    ImGui::PopID();
    return clicked;
}

bool EditorGuiCheckbox(const char *label, bool *value, const EditorGuiRowOpts &opts)
{
    ImGui::PushID(label);
    BeginTwoColumnRow(label, opts, 0.0f);
    ImGui::BeginDisabled(!opts.enabled);
    bool editVal = opts.mixed ? false : *value;
    const bool edited = ImGui::Checkbox("##chk", &editVal);
    if (opts.mixed && !edited)
    {
        const ImVec2 rMin = ImGui::GetItemRectMin();
        const ImVec2 rMax = ImGui::GetItemRectMax();
        const ImVec2 qSz = ImGui::CalcTextSize("?");
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(std::round(0.5f * (rMin.x + rMax.x - qSz.x)),
                   std::round(0.5f * (rMin.y + rMax.y - qSz.y))),
            ColorImU32(148, 156, 168, 255), "?");
    }
    if (edited)
    {
        *value = editVal;
    }
    ImGui::EndDisabled();
    EndTwoColumnRow("##rst_chk", opts);
    ImGui::PopID();
    return edited;
}

bool EditorGuiSliderFloat(const char *label, float *value, float minVal, float maxVal,
                          const EditorGuiRowOpts &opts)
{
    ImGui::PushID(label);
    BeginTwoColumnRow(label, opts, 0.0f);
    ImGui::BeginDisabled(!opts.enabled);
    const char *fmt = opts.mixed ? "?" : (opts.format != nullptr ? opts.format : "%.3g");
    float editVal = *value;
    if (opts.mixed)
    {
        const float span = std::max(1.0f, std::fabs(maxVal - minVal));
        editVal = minVal - span * 0.01f;
        ImGui::PushStyleColor(ImGuiCol_Text, ColorU8(148, 156, 168));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ColorU8(96, 104, 116, 180));
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ColorU8(120, 130, 145, 220));
    }
    const bool edited = ImGui::SliderFloat("##slf", &editVal, minVal, maxVal, fmt);
    if (opts.mixed)
    {
        ImGui::PopStyleColor(3);
    }
    if (edited)
    {
        *value = editVal;
    }
    ImGui::EndDisabled();
    EndTwoColumnRow("##rst_slf", opts);
    ImGui::PopID();
    return edited;
}

bool EditorGuiSliderInt(const char *label, int32_t *value, int32_t minVal, int32_t maxVal,
                        const EditorGuiRowOpts &opts)
{
    ImGui::PushID(label);
    BeginTwoColumnRow(label, opts, 0.0f);
    ImGui::BeginDisabled(!opts.enabled);
    const char *fmt = opts.mixed ? "?" : (opts.format != nullptr ? opts.format : "%d");
    const int32_t sentinel =
        (minVal > (std::numeric_limits<int32_t>::min)() / 2) ? (minVal - 1) : (maxVal + 1);
    int32_t editVal = opts.mixed ? sentinel : *value;
    if (opts.mixed)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ColorU8(148, 156, 168));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ColorU8(96, 104, 116, 180));
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ColorU8(120, 130, 145, 220));
    }
    const bool edited = ImGui::SliderInt("##sli", &editVal, minVal, maxVal, fmt);
    if (opts.mixed)
    {
        ImGui::PopStyleColor(3);
    }
    if (edited)
    {
        *value = editVal;
    }
    ImGui::EndDisabled();
    EndTwoColumnRow("##rst_sli", opts);
    ImGui::PopID();
    return edited;
}

bool EditorGuiCombo(const char *label, int32_t *selected, const char *const *items,
                    int32_t itemCount, const EditorGuiRowOpts &opts)
{
    ImGui::PushID(label);
    BeginTwoColumnRow(label, opts, 0.0f);
    ImGui::BeginDisabled(!opts.enabled);
    const int32_t cur = (*selected >= 0 && *selected < itemCount) ? *selected : 0;
    const char *preview = opts.mixed ? "?" : (itemCount > 0 ? items[cur] : "");
    bool edited = false;

    if (opts.mixed)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ColorU8(148, 156, 168));
    }
    const bool opened = ImGui::BeginCombo("##cmb", preview);
    if (opts.mixed)
    {
        ImGui::PopStyleColor();
    }

    if (opened)
    {
        for (int32_t i = 0; i < itemCount; ++i)
        {
            const bool isSelected = !opts.mixed && (*selected == i);
            if (ImGui::Selectable(items[i], isSelected))
            {
                *selected = i;
                edited = true;
            }
            if (isSelected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::EndDisabled();
    EndTwoColumnRow("##rst_cmb", opts);
    ImGui::PopID();
    return edited;
}

bool EditorGuiComboFiltered(const char *label, int32_t *selected, const EditorGuiComboModel &model,
                            const EditorGuiRowOpts &opts)
{
    ImGui::PushID(label);
    BeginTwoColumnRow(label, opts, 0.0f);
    ImGui::BeginDisabled(!opts.enabled);
    const char *preview = (*selected >= 0 && *selected < model.count && model.labelAt)
                              ? model.labelAt(*selected)
                              : "<none>";
    bool edited = false;

    static char s_filter[128] = {};
    if (ImGui::BeginCombo("##fcmb", preview, ImGuiComboFlags_HeightLarge))
    {
        if (ImGui::IsWindowAppearing())
        {
            s_filter[0] = '\0';
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##filter", "Filter...", s_filter, sizeof(s_filter));
        ImGui::Separator();

        ImGui::BeginChild("##flist", ImVec2(0.0f, Scaled(220.0f)));
        for (int32_t i = 0; i < model.count; ++i)
        {
            const char *entryLabel = model.labelAt ? model.labelAt(i) : "";
            if (!ContainsCaseInsensitive(entryLabel, s_filter))
            {
                continue;
            }
            const bool isSelected = (*selected == i);
            if (ImGui::Selectable(entryLabel, isSelected))
            {
                *selected = i;
                edited = true;
                ImGui::CloseCurrentPopup();
            }
            if (isSelected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndChild();
        ImGui::EndCombo();
    }

    ImGui::EndDisabled();
    EndTwoColumnRow("##rst_fcmb", opts);
    ImGui::PopID();
    return edited;
}

namespace
{

void DrawRobotIcon(ImDrawList *drawList, ImVec2 bMin, ImVec2 bMax, ImU32 color)
{
    const float w = bMax.x - bMin.x;
    const float h = bMax.y - bMin.y;
    const float s = std::min(w, h);
    const float cx = 0.5f * (bMin.x + bMax.x);
    const float cy = 0.5f * (bMin.y + bMax.y);
    const float u = s * 0.042f;
    const float t = std::max(1.2f, 1.35f * g_uiScale);

    // Antenna stem and ball tip.
    drawList->AddLine(ImVec2(cx, cy - 4.2f * u), ImVec2(cx, cy - 8.2f * u), color, t);
    drawList->AddCircleFilled(ImVec2(cx, cy - 8.8f * u), std::max(1.4f, 1.4f * u), color, 10);

    // Side ear tabs.
    drawList->AddRectFilled(ImVec2(cx - 9.0f * u, cy - 1.5f * u),
                            ImVec2(cx - 6.8f * u, cy + 3.0f * u), color, 1.0f);
    drawList->AddRectFilled(ImVec2(cx + 6.8f * u, cy - 1.5f * u),
                            ImVec2(cx + 9.0f * u, cy + 3.0f * u), color, 1.0f);

    // Rounded head outline, eyes, and mouth.
    drawList->AddRect(ImVec2(cx - 6.8f * u, cy - 4.2f * u),
                      ImVec2(cx + 6.8f * u, cy + 6.8f * u), color, 2.5f * u, 0, t);
    const float eyeRadius = std::max(1.3f, 1.35f * u);
    drawList->AddCircleFilled(ImVec2(cx - 2.6f * u, cy + 0.2f * u), eyeRadius, color, 10);
    drawList->AddCircleFilled(ImVec2(cx + 2.6f * u, cy + 0.2f * u), eyeRadius, color, 10);
    drawList->AddLine(ImVec2(cx - 3.0f * u, cy + 3.8f * u),
                      ImVec2(cx + 3.0f * u, cy + 3.8f * u), color, t);
}

bool DrawOptionalAiGenerateButton(bool enabled, float aiW, bool *generateAiClicked)
{
    if (generateAiClicked == nullptr)
    {
        return false;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!enabled);
    ImGui::PushStyleColor(ImGuiCol_Button, ColorU8(42, 74, 108, 240));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ColorU8(58, 98, 140, 250));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ColorU8(70, 118, 168, 255));
    const bool clicked = ImGui::Button("##ai_gen", ImVec2(aiW, 0.0f));
    const ImVec2 bMin = ImGui::GetItemRectMin();
    const ImVec2 bMax = ImGui::GetItemRectMax();
    DrawRobotIcon(ImGui::GetWindowDrawList(), bMin, bMax,
                  enabled ? ColorImU32(182, 226, 255) : ColorImU32(112, 120, 132));
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    ShowTooltip("Generate with AI", true);
    return clicked;
}

} // namespace

bool EditorGuiTexturePath(const char *label, std::string &path, bool *browseClicked,
                          const EditorGuiRowOpts &opts, bool *generateAiClicked)
{
    if (browseClicked != nullptr)
    {
        *browseClicked = false;
    }
    if (generateAiClicked != nullptr)
    {
        *generateAiClicked = false;
    }

    ImGui::PushID(label);
    const float spacingX = ImGui::GetStyle().ItemSpacing.x;
    const float browseW = Scaled(kEditorGuiBrowseWidth);
    const float aiW = generateAiClicked != nullptr ? Scaled(kEditorGuiBrowseWidth) : 0.0f;
    const float extraReserve = (generateAiClicked != nullptr) ? (aiW + spacingX) : 0.0f;
    BeginTwoColumnRow(label, opts, browseW + spacingX + extraReserve);

    char buffer[260] = {};
    std::snprintf(buffer, sizeof(buffer), "%s", path.c_str());

    const char *hint =
        (opts.format != nullptr && opts.format[0] != '\0') ? opts.format : "NONE";

    ImGui::BeginDisabled(!opts.enabled);
    if (opts.dimValue)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    }
    const bool edited = ImGui::InputTextWithHint("##texpath", hint, buffer, sizeof(buffer));
    if (opts.dimValue)
    {
        ImGui::PopStyleColor();
    }
    ShowTooltip(path.c_str());
    if (edited)
    {
        path = buffer;
    }

    ImGui::SameLine();
    if (ImGui::Button("...", ImVec2(browseW, 0.0f)) && browseClicked != nullptr)
    {
        *browseClicked = true;
    }
    ShowTooltip("Browse texture assets");
    ImGui::EndDisabled();

    if (DrawOptionalAiGenerateButton(opts.enabled, aiW, generateAiClicked))
    {
        *generateAiClicked = true;
    }

    EndTwoColumnRow("##rst_tex", opts);
    ImGui::PopID();
    return edited;
}

bool EditorGuiColorHex(const char *label, float rgb[3], const EditorGuiRowOpts &opts)
{
    ImGui::PushID(label);
    BeginTwoColumnRow(label, opts, 0.0f);
    const bool edited = EditorGuiColorRow("##clrrow", rgb, opts);
    EndTwoColumnRow("##rst_clr", opts);
    ImGui::PopID();
    return edited;
}

bool EditorGuiColorRow(const char *id, float rgb[3], const EditorGuiRowOpts &opts)
{
    ImGui::PushID(id);
    ImGui::BeginDisabled(!opts.enabled);
    float swatchRgb[3] = { rgb[0], rgb[1], rgb[2] };
    if (opts.mixed)
    {
        swatchRgb[0] = 0.24f;
        swatchRgb[1] = 0.26f;
        swatchRgb[2] = 0.30f;
    }
    bool edited = ImGui::ColorEdit3("##swatch", swatchRgb,
                                    ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_Float);
    if (opts.mixed && !edited)
    {
        const ImVec2 rMin = ImGui::GetItemRectMin();
        const ImVec2 rMax = ImGui::GetItemRectMax();
        const ImVec2 qSz = ImGui::CalcTextSize("?");
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(std::round(0.5f * (rMin.x + rMax.x - qSz.x)),
                   std::round(0.5f * (rMin.y + rMax.y - qSz.y))),
            ColorImU32(170, 178, 190, 255), "?");
    }
    if (edited)
    {
        rgb[0] = swatchRgb[0];
        rgb[1] = swatchRgb[1];
        rgb[2] = swatchRgb[2];
    }

    char hex[8] = {};
    if (!opts.mixed)
    {
        ColorToHex(rgb, hex);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Scaled(84.0f));
    bool hexEdited = false;
    if (opts.mixed)
    {
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, ColorU8(148, 156, 168));
        hexEdited = ImGui::InputTextWithHint(
            "##hex", "?", hex, sizeof(hex),
            ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase |
                ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopStyleColor();
    }
    else
    {
        hexEdited = ImGui::InputText("##hex", hex, sizeof(hex),
                                     ImGuiInputTextFlags_CharsHexadecimal |
                                         ImGuiInputTextFlags_CharsUppercase |
                                         ImGuiInputTextFlags_EnterReturnsTrue);
    }
    if (hexEdited)
    {
        float parsed[3] = {};
        if (HexToColor(hex, parsed))
        {
            rgb[0] = parsed[0];
            rgb[1] = parsed[1];
            rgb[2] = parsed[2];
            edited = true;
        }
    }
    ImGui::EndDisabled();
    ImGui::PopID();
    return edited;
}

namespace
{

void UpdateMixedAxisTempCommit(ImGuiID dragId, bool axisMixed, ImGuiID &tempDragId,
                               bool &axisEdited)
{
    const bool wasMixedTemp = (tempDragId == dragId);
    const bool isTempNow = ImGui::TempInputIsActive(dragId);
    if (axisMixed && isTempNow)
    {
        tempDragId = dragId;
        return;
    }
    if (wasMixedTemp && !isTempNow)
    {
        tempDragId = 0;
        const bool enterPressed = ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                                  ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
        if (axisMixed && !axisEdited && ImGui::IsItemDeactivated() && enterPressed)
        {
            axisEdited = true;
        }
    }
}

bool DrawVec3AxisInput(int axis, float &value, float step, float itemW,
                       const EditorGuiRowOpts &opts)
{
    static constexpr const char *kAxisNames[3] = { "X", "Y", "Z" };
    static constexpr ImU32 kAxisColors[3] = {
        IM_COL32(226, 94, 94, 255),
        IM_COL32(104, 212, 114, 255),
        IM_COL32(98, 162, 240, 255),
    };

    const bool axisMixed = opts.mixedAxisMask != 0
                               ? ((opts.mixedAxisMask & uint8_t(1u << axis)) != 0)
                               : opts.mixed;
    ImGui::PushID(axis);
    ImGui::PushStyleColor(ImGuiCol_Text, kAxisColors[axis]);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(kAxisNames[axis]);
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, Scaled(3.0f));
    ImGui::SetNextItemWidth(itemW);

    const ImGuiID dragId = ImGui::GetID("##v");
    static ImGuiID s_mixedTempDragId = 0;
    float editVal = value;
    if (axisMixed)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ColorU8(148, 156, 168));
    }
    bool axisEdited =
        ImGui::DragFloat("##v", &editVal, step, 0.0f, 0.0f, axisMixed ? "?" : "%.2f");
    if (axisMixed)
    {
        ImGui::PopStyleColor();
    }
    UpdateMixedAxisTempCommit(dragId, axisMixed, s_mixedTempDragId, axisEdited);
    if (axisEdited)
    {
        value = editVal;
        if (opts.editedAxisMask != nullptr)
        {
            *opts.editedAxisMask |= uint8_t(1u << axis);
        }
    }
    ImGui::PopID();
    return axisEdited;
}

} // namespace

bool EditorGuiVec3Input(const char *label, float vec[3], float step, const EditorGuiRowOpts &opts)
{
    if (opts.editedAxisMask != nullptr)
    {
        *opts.editedAxisMask = 0;
    }

    ImGui::PushID(label);
    BeginTwoColumnRow(label, opts, 0.0f);
    const float rowW =
        std::max(Scaled(120.0f), ImGui::GetContentRegionAvail().x - Scaled(kEditorGuiResetSize) -
                                     ImGui::GetStyle().ItemSpacing.x);
    const float itemW = std::max(Scaled(32.0f), (rowW - Scaled(48.0f)) / 3.0f);
    bool edited = false;

    ImGui::BeginDisabled(!opts.enabled);
    for (int axis = 0; axis < 3; ++axis)
    {
        if (axis > 0)
        {
            ImGui::SameLine();
        }
        edited |= DrawVec3AxisInput(axis, vec[axis], step, itemW, opts);
    }
    ImGui::EndDisabled();

    EndTwoColumnRow("##rst_v3", opts);
    ImGui::PopID();
    return edited;
}

void EditorGuiBackdrop(float displayWidth, float displayHeight, float alpha)
{
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        ImVec2(0.0f, 0.0f), ImVec2(displayWidth, displayHeight),
        ImGui::ColorConvertFloat4ToU32(ImVec4(0.02f, 0.03f, 0.05f, alpha)));
}

int32_t EditorGuiDialogCentered(const char *id, const char *title, const char *message,
                                const char *const *buttons, int32_t buttonCount)
{
    const ImGuiIO &io = ImGui::GetIO();
    const float width = Scaled(420.0f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);

    int32_t clicked = -1;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin(id, nullptr, flags))
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ColorU8(136, 196, 255));
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextWrapped("%s", message);
        ImGui::Spacing();
        ImGui::Spacing();

        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float btnW = buttonCount > 0 ? (ImGui::GetContentRegionAvail().x - spacing * float(buttonCount - 1)) /
                                                 float(buttonCount)
                                           : 100.0f;
        for (int32_t i = 0; i < buttonCount; ++i)
        {
            if (i > 0)
            {
                ImGui::SameLine();
            }
            if (ImGui::Button(buttons[i], ImVec2(btnW, Scaled(30.0f))))
            {
                clicked = i;
            }
        }
    }
    ImGui::End();
    return clicked;
}

int32_t EditorGuiDialogVertical(const char *id, const char *title, const char *subtitle,
                                const char *const *buttons, const char *const *descriptions,
                                int32_t buttonCount)
{
    const ImGuiIO &io = ImGui::GetIO();
    const float width = Scaled(460.0f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);

    int32_t clicked = -1;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin(id, nullptr, flags))
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ColorU8(136, 196, 255));
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
        if (subtitle != nullptr && subtitle[0] != '\0')
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ColorU8(178, 188, 200));
            ImGui::TextWrapped("%s", subtitle);
            ImGui::PopStyleColor();
        }
        ImGui::Separator();
        ImGui::Spacing();

        const float btnW = ImGui::GetContentRegionAvail().x;
        for (int32_t i = 0; i < buttonCount; ++i)
        {
            if (i == 0)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ColorU8(48, 86, 134));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ColorU8(64, 112, 170));
            }
            if (ImGui::Button(buttons[i], ImVec2(btnW, Scaled(38.0f))))
            {
                clicked = i;
            }
            if (i == 0)
            {
                ImGui::PopStyleColor(2);
            }
            if (descriptions != nullptr && descriptions[i] != nullptr && descriptions[i][0] != '\0')
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ColorU8(152, 162, 176));
                ImGui::TextWrapped("%s", descriptions[i]);
                ImGui::PopStyleColor();
            }
            ImGui::Spacing();
        }
    }
    ImGui::End();
    return clicked;
}
