#include "EditorGui.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{

float g_uiScale = 1.0f;

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

void ShowTooltip(const char *tooltip)
{
    if (tooltip == nullptr || tooltip[0] == '\0' || !ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
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

void DrawResetArrowIcon(ImDrawList *drawList, ImVec2 center, float radius, ImU32 color)
{
    constexpr float kPi = 3.14159265f;
    const float start = 0.25f * kPi;
    const float end = 1.75f * kPi;
    drawList->PathArcTo(center, radius, start, end, 14);
    drawList->PathStroke(color, ImDrawFlags_None, std::max(1.4f, 1.6f * g_uiScale));

    const float tipX = center.x + std::cos(end) * radius;
    const float tipY = center.y + std::sin(end) * radius;
    const float head = std::max(3.0f, 3.2f * g_uiScale);
    drawList->AddTriangleFilled(ImVec2(tipX - head * 0.2f, tipY - head * 0.6f),
                                ImVec2(tipX - head * 1.1f, tipY + head * 0.45f),
                                ImVec2(tipX + head * 0.7f, tipY + head * 0.45f), color);
}

void DrawCopyIcon(ImDrawList *drawList, ImVec2 p, float s, ImU32 color)
{
    const float pad = s * 0.22f;
    const float w = s * 0.42f;
    const float h = s * 0.50f;
    const float t = std::max(1.2f, 1.25f * g_uiScale);
    drawList->AddRect(ImVec2(p.x + pad + s * 0.14f, p.y + pad),
                      ImVec2(p.x + pad + s * 0.14f + w, p.y + pad + h), color, 1.0f, 0, t);
    drawList->AddRectFilled(ImVec2(p.x + pad, p.y + pad + s * 0.14f),
                            ImVec2(p.x + pad + w, p.y + pad + s * 0.14f + h),
                            ColorImU32(22, 27, 34, 240));
    drawList->AddRect(ImVec2(p.x + pad, p.y + pad + s * 0.14f),
                      ImVec2(p.x + pad + w, p.y + pad + s * 0.14f + h), color, 1.0f, 0, t);
}

void DrawPasteIcon(ImDrawList *drawList, ImVec2 p, float s, ImU32 color)
{
    const float t = std::max(1.2f, 1.25f * g_uiScale);
    drawList->AddRect(ImVec2(p.x + s * 0.26f, p.y + s * 0.28f),
                      ImVec2(p.x + s * 0.74f, p.y + s * 0.82f), color, 1.5f, 0, t);
    drawList->AddRectFilled(ImVec2(p.x + s * 0.37f, p.y + s * 0.18f),
                            ImVec2(p.x + s * 0.63f, p.y + s * 0.33f), color, 1.0f);
}

void DrawTrashIcon(ImDrawList *drawList, ImVec2 p, float s, ImU32 color)
{
    const float t = std::max(1.2f, 1.25f * g_uiScale);
    drawList->AddLine(ImVec2(p.x + s * 0.24f, p.y + s * 0.32f),
                      ImVec2(p.x + s * 0.76f, p.y + s * 0.32f), color, t);
    drawList->AddLine(ImVec2(p.x + s * 0.40f, p.y + s * 0.22f),
                      ImVec2(p.x + s * 0.60f, p.y + s * 0.22f), color, t);
    drawList->AddRect(ImVec2(p.x + s * 0.30f, p.y + s * 0.32f),
                      ImVec2(p.x + s * 0.70f, p.y + s * 0.78f), color, 1.0f, 0, t);
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
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        if (ImGui::Button("##TabResetTrash", ImVec2(trashSize, trashSize)) && outResetClicked != nullptr)
        {
            *outResetClicked = true;
        }
        DrawTrashIcon(ImGui::GetWindowDrawList(), pos, trashSize, ColorImU32(236, 178, 118));
        ShowTooltip(resetTooltip);
    }

    return changed;
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
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, ColorU8(46, 40, 26, 210));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ColorU8(76, 64, 34, 240));
    const bool clicked = ImGui::Button("##rst", ImVec2(size, size));
    ImGui::PopStyleColor(2);
    DrawResetArrowIcon(ImGui::GetWindowDrawList(),
                       ImVec2(pos.x + size * 0.5f, pos.y + size * 0.5f), size * 0.27f,
                       ColorImU32(246, 198, 92));
    ShowTooltip(tooltip != nullptr ? tooltip : "Reset to loaded value");
    ImGui::PopID();
    return clicked;
}

uint32_t EditorGuiBlockActions(const char *id, bool canCopy, bool canPaste, bool canRemove)
{
    uint32_t actions = kEditorGuiActionNone;
    const float s = Scaled(kEditorGuiActionSize);
    ImGui::PushID(id);

    ImGui::BeginDisabled(!canCopy);
    const ImVec2 cPos = ImGui::GetCursorScreenPos();
    if (ImGui::Button("##cp", ImVec2(s, s)))
    {
        actions |= kEditorGuiActionCopy;
    }
    DrawCopyIcon(ImGui::GetWindowDrawList(), cPos, s,
                 canCopy ? ColorImU32(215, 224, 236) : ColorImU32(110, 118, 128));
    ShowTooltip("Copy");
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!canPaste);
    const ImVec2 pPos = ImGui::GetCursorScreenPos();
    if (ImGui::Button("##ps", ImVec2(s, s)))
    {
        actions |= kEditorGuiActionPaste;
    }
    DrawPasteIcon(ImGui::GetWindowDrawList(), pPos, s,
                  canPaste ? ColorImU32(215, 224, 236) : ColorImU32(110, 118, 128));
    ShowTooltip("Paste");
    ImGui::EndDisabled();

    if (canRemove)
    {
        ImGui::SameLine();
        const ImVec2 rPos = ImGui::GetCursorScreenPos();
        if (ImGui::Button("##rm", ImVec2(s, s)))
        {
            actions |= kEditorGuiActionRemove;
        }
        DrawTrashIcon(ImGui::GetWindowDrawList(), rPos, s, ColorImU32(238, 118, 118));
        ShowTooltip("Remove");
    }

    ImGui::PopID();
    return actions;
}

bool EditorGuiCheckbox(const char *label, bool *value, const EditorGuiRowOpts &opts)
{
    ImGui::PushID(label);
    BeginTwoColumnRow(label, opts, 0.0f);
    ImGui::BeginDisabled(!opts.enabled);
    const bool edited = ImGui::Checkbox("##chk", value);
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
    const char *fmt = opts.format != nullptr ? opts.format : "%.3g";
    const bool edited = ImGui::SliderFloat("##slf", value, minVal, maxVal, fmt);
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
    const char *fmt = opts.format != nullptr ? opts.format : "%d";
    const bool edited = ImGui::SliderInt("##sli", value, minVal, maxVal, fmt);
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
    const char *preview = itemCount > 0 ? items[cur] : "";
    bool edited = false;

    if (ImGui::BeginCombo("##cmb", preview))
    {
        for (int32_t i = 0; i < itemCount; ++i)
        {
            const bool isSelected = (*selected == i);
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

bool EditorGuiTexturePath(const char *label, std::string &path, bool *browseClicked,
                          const EditorGuiRowOpts &opts)
{
    if (browseClicked != nullptr)
    {
        *browseClicked = false;
    }

    ImGui::PushID(label);
    const float browseW = Scaled(kEditorGuiBrowseWidth);
    BeginTwoColumnRow(label, opts, browseW + ImGui::GetStyle().ItemSpacing.x);

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
    if (edited)
    {
        path = buffer;
    }

    ImGui::SameLine();
    if (ImGui::Button("...", ImVec2(browseW, 0.0f)) && browseClicked != nullptr)
    {
        *browseClicked = true;
    }
    ImGui::EndDisabled();

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
    bool edited = ImGui::ColorEdit3("##swatch", rgb, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_Float);

    char hex[8] = {};
    ColorToHex(rgb, hex);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Scaled(84.0f));
    if (ImGui::InputText("##hex", hex, sizeof(hex),
                         ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase |
                             ImGuiInputTextFlags_EnterReturnsTrue))
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

bool EditorGuiVec3Input(const char *label, float vec[3], float step, const EditorGuiRowOpts &opts)
{
    static constexpr const char *kAxisNames[3] = { "X", "Y", "Z" };
    static constexpr ImU32 kAxisColors[3] = {
        IM_COL32(226, 94, 94, 255),
        IM_COL32(104, 212, 114, 255),
        IM_COL32(98, 162, 240, 255),
    };

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
        ImGui::PushID(axis);
        ImGui::PushStyleColor(ImGuiCol_Text, kAxisColors[axis]);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(kAxisNames[axis]);
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, Scaled(3.0f));
        ImGui::SetNextItemWidth(itemW);
        edited |= ImGui::DragFloat("##v", &vec[axis], step, 0.0f, 0.0f, "%.2f");
        ImGui::PopID();
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
