#include "EditorApp.h"
#include "EditorGui.h"
#include "EditorSession.h"
#include "EditorSpatial.h"
#include "EditorUi.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace
{

constexpr uint64_t kTorchUniqueId = uint64_t(UINT16_MAX) + 0x20000;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;

bool AlmostEq(float a, float b)
{
    return std::fabs(a - b) <= 1e-4f;
}

bool ColorEq(const float a[3], const float b[3])
{
    return AlmostEq(a[0], b[0]) && AlmostEq(a[1], b[1]) && AlmostEq(a[2], b[2]);
}

bool Vec3Eq(const float a[3], const float b[3])
{
    return AlmostEq(a[0], b[0]) && AlmostEq(a[1], b[1]) && AlmostEq(a[2], b[2]);
}

bool EqualsIgnoreCase(const std::string &a, const char *b)
{
    if (b == nullptr)
    {
        return false;
    }
    const size_t len = std::strlen(b);
    if (a.size() != len)
    {
        return false;
    }
    for (size_t i = 0; i < len; ++i)
    {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
        {
            return false;
        }
    }
    return true;
}

std::string *SlotTexturePtr(EditorMaterialParams &params, EditorApp::EditorTextureSlot slot)
{
    switch (slot)
    {
    case EditorApp::EditorTextureSlot::Base:
        return &params.baseTexture;
    case EditorApp::EditorTextureSlot::Normals:
        return &params.normalTexture;
    case EditorApp::EditorTextureSlot::Emissive:
        return &params.emissiveTexture;
    case EditorApp::EditorTextureSlot::Overlay:
        return &params.overlayTexture;
    case EditorApp::EditorTextureSlot::Gloss:
        return &params.glossTexture;
    case EditorApp::EditorTextureSlot::Subsurface:
        return &params.textureSss;
    }
    return &params.baseTexture;
}

const std::string &SlotTextureConst(const EditorMaterialParams &params,
                                    EditorApp::EditorTextureSlot slot)
{
    return *SlotTexturePtr(const_cast<EditorMaterialParams &>(params), slot);
}

bool NormalizeVec3(float v[3])
{
    const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len <= 1e-5f)
    {
        return false;
    }
    v[0] /= len;
    v[1] /= len;
    v[2] /= len;
    return true;
}

void BuildOrthoBasis(const float dir[3], float right[3], float up[3])
{
    const float ref[3] = {
        std::fabs(dir[2]) < 0.9f ? 0.0f : 1.0f,
        0.0f,
        std::fabs(dir[2]) < 0.9f ? 1.0f : 0.0f,
    };
    right[0] = dir[1] * ref[2] - dir[2] * ref[1];
    right[1] = dir[2] * ref[0] - dir[0] * ref[2];
    right[2] = dir[0] * ref[1] - dir[1] * ref[0];
    NormalizeVec3(right);
    up[0] = right[1] * dir[2] - right[2] * dir[1];
    up[1] = right[2] * dir[0] - right[0] * dir[2];
    up[2] = right[0] * dir[1] - right[1] * dir[0];
    NormalizeVec3(up);
}

void ClampSpotAngles(EditorLight &light)
{
    light.angleInner = std::clamp(light.angleInner, 0.0f, 90.0f);
    light.angleOuter = std::clamp(light.angleOuter, 0.0f, 90.0f);
    if (light.angleInner > light.angleOuter)
    {
        light.angleOuter = light.angleInner;
    }
}

template <typename T>
bool ResetIfClicked(T &dst, const T &base, bool *resetClicked)
{
    if (resetClicked != nullptr && *resetClicked)
    {
        dst = base;
        *resetClicked = false;
        return true;
    }
    return false;
}

bool ResetVec3IfClicked(float dst[3], const float base[3], bool *resetClicked)
{
    if (resetClicked != nullptr && *resetClicked)
    {
        dst[0] = base[0];
        dst[1] = base[1];
        dst[2] = base[2];
        *resetClicked = false;
        return true;
    }
    return false;
}

// Draws the perspective wireframe cone of a spotlight matching QRE_DrawSpotCone.
void DrawSpotCone(ImDrawList *drawList, const std::function<bool(const float[3], float &, float &)> &project,
                  const float origin[3], const float dirIn[3], float angleDeg, ImU32 color)
{
    float dir[3] = { dirIn[0], dirIn[1], dirIn[2] };
    if (!NormalizeVec3(dir))
    {
        return;
    }

    constexpr float kConeLen = 96.0f;
    constexpr int kSegments = 24;
    constexpr int kRays = 6;

    const float angle = std::clamp(angleDeg * kDegToRad, 0.0f, 89.5f * kDegToRad);
    const float radius = kConeLen * std::tan(angle);

    float right[3] = {};
    float up[3] = {};
    BuildOrthoBasis(dir, right, up);

    std::array<ImVec2, kSegments> rimPts = {};
    std::array<bool, kSegments> rimOk = {};
    for (int i = 0; i < kSegments; ++i)
    {
        const float theta = 2.0f * kPi * float(i) / float(kSegments);
        const float cs = std::cos(theta) * radius;
        const float sn = std::sin(theta) * radius;
        const float worldPos[3] = {
            origin[0] + dir[0] * kConeLen + right[0] * cs + up[0] * sn,
            origin[1] + dir[1] * kConeLen + right[1] * cs + up[1] * sn,
            origin[2] + dir[2] * kConeLen + right[2] * cs + up[2] * sn,
        };
        float sx = 0.0f;
        float sy = 0.0f;
        rimOk[size_t(i)] = project(worldPos, sx, sy);
        rimPts[size_t(i)] = ImVec2(sx, sy);
    }

    float ox = 0.0f;
    float oy = 0.0f;
    if (project(origin, ox, oy))
    {
        for (int r = 0; r < kRays; ++r)
        {
            const int idx = r * kSegments / kRays;
            if (rimOk[size_t(idx)])
            {
                drawList->AddLine(ImVec2(ox, oy), rimPts[size_t(idx)], color, 2.0f);
            }
        }
    }

    for (int i = 0; i < kSegments; ++i)
    {
        const int next = (i + 1) % kSegments;
        if (rimOk[size_t(i)] && rimOk[size_t(next)])
        {
            drawList->AddLine(rimPts[size_t(i)], rimPts[size_t(next)], color, 2.0f);
        }
    }
}

bool DrawMaterialTexturesSection(
    const std::function<void(EditorApp::EditorTextureSlot)> &onBrowse,
    EditorMaterialParams &params, const EditorMaterialParams &base,
    const EditorMaterialParams *baseModParams, bool inheritedFromBaseMod)
{
    EditorGuiSectionHeader("Textures", "Texture map overrides and UV transforms.");
    struct SlotRow
    {
        const char *label;
        EditorApp::EditorTextureSlot slot;
        const char *tip;
    };
    static constexpr SlotRow kSlots[] = {
        { "texture_base", EditorApp::EditorTextureSlot::Base, "Override base albedo texture." },
        { "texture_normals", EditorApp::EditorTextureSlot::Normals, "Override normal map texture." },
        { "texture_emissive", EditorApp::EditorTextureSlot::Emissive,
          "Override explicit emissive mask texture (disables color_emissive blocks)." },
        { "texture_overlay", EditorApp::EditorTextureSlot::Overlay, "Alpha-stamped overlay decal." },
        { "texture_gloss", EditorApp::EditorTextureSlot::Gloss, "Gloss / smoothness map." },
        { "texture_sss", EditorApp::EditorTextureSlot::Subsurface,
          "Greyscale/luminance subsurface scattering mask texture." },
    };

    bool edited = false;
    for (const SlotRow &row : kSlots)
    {
        std::string &dst = *SlotTexturePtr(params, row.slot);
        const std::string &ref = SlotTextureConst(base, row.slot);
        const std::string baseModSlot =
            baseModParams != nullptr ? SlotTextureConst(*baseModParams, row.slot) : std::string{};
        const std::string baseHint =
            !baseModSlot.empty() ? (baseModSlot + " (base mod)") : std::string{};
        bool reset = false;
        bool browse = false;
        EditorGuiRowOpts opts{ dst != ref, true, row.tip,
                               baseHint.empty() ? nullptr : baseHint.c_str(), &reset };
        opts.dimValue = inheritedFromBaseMod && !baseModSlot.empty() && dst == baseModSlot;
        if (EditorGuiTexturePath(row.label, dst, &browse, opts))
        {
            edited = true;
        }
        if (ResetIfClicked(dst, ref, &reset))
        {
            edited = true;
        }
        if (browse)
        {
            onBrowse(row.slot);
        }
    }

    struct FloatRow
    {
        const char *label;
        float *val;
        float baseVal;
        float lo;
        float hi;
        const char *tip;
    };
    const FloatRow floats[] = {
        { "texture_scale_u", &params.textureScaleU, base.textureScaleU, 0.05f, 8.0f,
          "Horizontal UV tiling multiplier." },
        { "texture_scale_v", &params.textureScaleV, base.textureScaleV, 0.05f, 8.0f,
          "Vertical UV tiling multiplier." },
        { "texture_offset_u", &params.textureOffsetU, base.textureOffsetU, -8.0f, 8.0f,
          "Horizontal UV shift." },
        { "texture_offset_v", &params.textureOffsetV, base.textureOffsetV, -8.0f, 8.0f,
          "Vertical UV shift." },
        { "bump_scale", &params.bumpScale, base.bumpScale, 0.0f, 4.0f,
          "Normal map xy strength factor." },
    };
    for (const FloatRow &f : floats)
    {
        bool reset = false;
        EditorGuiRowOpts opts{ !AlmostEq(*f.val, f.baseVal), true, f.tip, "%.2f", &reset };
        edited |= EditorGuiSliderFloat(f.label, f.val, f.lo, f.hi, opts);
        edited |= ResetIfClicked(*f.val, f.baseVal, &reset);
    }
    return edited;
}

bool DrawMaterialSurfaceSection(EditorMaterialParams &params, const EditorMaterialParams &base)
{
    EditorGuiSectionHeader("Surface", "Base tint, reflections and transmissive material type.");
    bool edited = false;

    bool baseReset = false;
    edited |= EditorGuiSliderFloat(
        "base_factor", &params.baseFactor, 0.0f, 4.0f,
        { !AlmostEq(params.baseFactor, base.baseFactor), true, nullptr, "%.2f", &baseReset });
    edited |= ResetIfClicked(params.baseFactor, base.baseFactor, &baseReset);

    bool mirrorReset = false;
    edited |= EditorGuiCheckbox(
        "mirror", &params.mirror,
        { params.mirror != base.mirror, true, "Mirror-smooth specular reflection.", nullptr, &mirrorReset });
    edited |= ResetIfClicked(params.mirror, base.mirror, &mirrorReset);

    static constexpr const char *kTypes[] = { "none", "glass", "water" };
    int32_t typeIdx = int32_t(params.type);
    bool typeReset = false;
    if (EditorGuiCombo("material", &typeIdx, kTypes, 3,
                       { params.type != base.type, true, nullptr, nullptr, &typeReset }))
    {
        params.type = static_cast<EditorMaterialType>(typeIdx);
        edited = true;
    }
    edited |= ResetIfClicked(params.type, base.type, &typeReset);

    if (params.type == EditorMaterialType::Glass)
    {
        bool trReset = false;
        edited |= EditorGuiSliderFloat(
            "transparency", &params.transparency, 0.0f, 1.0f,
            { !AlmostEq(params.transparency, base.transparency), true, nullptr, "%.2f", &trReset });
        edited |= ResetIfClicked(params.transparency, base.transparency, &trReset);

        bool iorReset = false;
        const char *iorFmt = params.glassIor <= 0.0f ? "default (1.52)" : "%.2f";
        edited |= EditorGuiSliderFloat(
            "glass_ior", &params.glassIor, 0.0f, 2.5f,
            { !AlmostEq(params.glassIor, base.glassIor), true, nullptr, iorFmt, &iorReset });
        edited |= ResetIfClicked(params.glassIor, base.glassIor, &iorReset);

        bool thReset = false;
        edited |= EditorGuiSliderFloat(
            "glass_thickness", &params.glassThickness, 0.0f, 32.0f,
            { !AlmostEq(params.glassThickness, base.glassThickness), true, nullptr, "%.2f", &thReset });
        edited |= ResetIfClicked(params.glassThickness, base.glassThickness, &thReset);

        bool gcReset = false;
        edited |= EditorGuiColorHex(
            "glass_color", params.glassColor,
            { !ColorEq(params.glassColor, base.glassColor), true, nullptr, nullptr, &gcReset });
        edited |= ResetVec3IfClicked(params.glassColor, base.glassColor, &gcReset);
    }

    EditorGuiSectionHeader("Model");
    bool exReset = false;
    edited |= EditorGuiCheckbox(
        "exact_normals", &params.exactNormals,
        { params.exactNormals != base.exactNormals, true, nullptr, nullptr, &exReset });
    edited |= ResetIfClicked(params.exactNormals, base.exactNormals, &exReset);

    bool frReset = false;
    edited |= EditorGuiCheckbox(
        "force_rasterize", &params.forceRasterize,
        { params.forceRasterize != base.forceRasterize, true,
          "Skip rasterized emitter on alias/sprite models.", nullptr, &frReset });
    edited |= ResetIfClicked(params.forceRasterize, base.forceRasterize, &frReset);
    return edited;
}

bool DrawMaterialLightShaftSliders(EditorMaterialParams &params, const EditorMaterialParams &base)
{
    bool edited = false;

    bool lspReset = false;
    edited |= EditorGuiSliderFloat(
        "light_shafts_power", &params.lightShaftsPower, 0.0f, 16.0f,
        { !AlmostEq(params.lightShaftsPower, base.lightShaftsPower), true,
          "Strength of volumetric shafts for this light material.", "%.2f", &lspReset });
    edited |= ResetIfClicked(params.lightShaftsPower, base.lightShaftsPower, &lspReset);

    bool lsdReset = false;
    edited |= EditorGuiSliderFloat(
        "light_shafts_distance", &params.lightShaftsDistance, 0.0f, 25.0f,
        { !AlmostEq(params.lightShaftsDistance, base.lightShaftsDistance), true,
          "How far the volumetric shafts reach in meters (0.5..25). Set 0 for automatic distance.",
          "%.2f", &lsdReset });
    edited |= ResetIfClicked(params.lightShaftsDistance, base.lightShaftsDistance, &lsdReset);

    bool lsfReset = false;
    edited |= EditorGuiSliderFloat(
        "light_shafts_falloff", &params.lightShaftsFalloff, 0.0f, 4.0f,
        { !AlmostEq(params.lightShaftsFalloff, base.lightShaftsFalloff), true,
          "How quickly the shafts fade with distance (0 = nearly steady until the end fade, 1 = "
          "smooth default fade, higher = stronger near the lamp and faster decay).",
          "%.2f", &lsfReset });
    edited |= ResetIfClicked(params.lightShaftsFalloff, base.lightShaftsFalloff, &lsfReset);

    return edited;
}

bool DrawMaterialLightSection(EditorMaterialParams &params, const EditorMaterialParams &base)
{
    EditorGuiSectionHeader("Light", "Area light emission parameters when surface casts light.");
    bool edited = false;

    bool enabledReset = false;
    edited |= EditorGuiCheckbox(
        "enabled", &params.enabled,
        { params.enabled != base.enabled, true,
          "Enable DTAL lighting and surface emission for this material.", nullptr, &enabledReset });
    edited |= ResetIfClicked(params.enabled, base.enabled, &enabledReset);

    bool isLightReset = false;
    edited |= EditorGuiCheckbox(
        "is_light", &params.luma,
        { params.luma != base.luma, true,
          "Treat surfaces using this material as RT area light sources.", nullptr, &isLightReset });
    edited |= ResetIfClicked(params.luma, base.luma, &isLightReset);

    if (!params.luma)
    {
        return edited;
    }

    bool stReset = false;
    edited |= EditorGuiCheckbox(
        "light_styles", &params.lightStyles,
        { params.lightStyles != base.lightStyles, true, nullptr, nullptr, &stReset });
    edited |= ResetIfClicked(params.lightStyles, base.lightStyles, &stReset);

    bool lcReset = false;
    const bool lcChanged =
        params.hasLightColor != base.hasLightColor || !ColorEq(params.lightColor, base.lightColor);
    if (EditorGuiColorHex("light_color", params.lightColor,
                          { lcChanged, true, "Override emitted area-light tint.", nullptr, &lcReset }))
    {
        params.hasLightColor = true;
        edited = true;
    }
    if (lcReset)
    {
        params.hasLightColor = base.hasLightColor;
        std::copy_n(base.lightColor, 3, params.lightColor);
        edited = true;
    }

    bool lbReset = false;
    edited |= EditorGuiSliderFloat(
        "light_brightness", &params.lightBrightness, 0.0f, 32.0f,
        { !AlmostEq(params.lightBrightness, base.lightBrightness), true, nullptr, "%.2f", &lbReset });
    edited |= ResetIfClicked(params.lightBrightness, base.lightBrightness, &lbReset);

    bool lfReset = false;
    float focusUi = params.lightFocus < 0.0f ? (params.lightProjector ? 45.0f : 0.0f) : params.lightFocus;
    const char *focusFmt = params.lightFocus < 0.0f ? "auto (%.0f)" : "%.1f";
    if (EditorGuiSliderFloat("light_focus", &focusUi, 0.0f, 180.0f,
                             { !AlmostEq(params.lightFocus, base.lightFocus), true,
                               "Focus cone angle (defaults to 45 for projector).", focusFmt, &lfReset }))
    {
        params.lightFocus = focusUi;
        edited = true;
    }
    edited |= ResetIfClicked(params.lightFocus, base.lightFocus, &lfReset);

    bool lfsReset = false;
    float softUi =
        params.lightFocusSoft < 0.0f ? (params.lightProjector ? 45.0f : 0.1f * focusUi) : params.lightFocusSoft;
    const char *softFmt = params.lightFocusSoft < 0.0f ? "auto (%.1f)" : "%.1f";
    if (EditorGuiSliderFloat("light_focus_soft", &softUi, 0.0f, 180.0f,
                             { !AlmostEq(params.lightFocusSoft, base.lightFocusSoft), true,
                               "Soft falloff margin in degrees.", softFmt, &lfsReset }))
    {
        params.lightFocusSoft = softUi;
        edited = true;
    }
    edited |= ResetIfClicked(params.lightFocusSoft, base.lightFocusSoft, &lfsReset);

    bool gpReset = false;
    edited |= EditorGuiCheckbox(
        "light_projector", &params.lightProjector,
        { params.lightProjector != base.lightProjector, true,
          "Gobo light projector mode using the surface texture.", nullptr, &gpReset });
    edited |= ResetIfClicked(params.lightProjector, base.lightProjector, &gpReset);

    bool lsReset = false;
    edited |= EditorGuiCheckbox(
        "light_shafts", &params.lightShafts,
        { params.lightShafts != base.lightShafts, true,
          "Enable volumetric light shafts from this material.", nullptr, &lsReset });
    edited |= ResetIfClicked(params.lightShafts, base.lightShafts, &lsReset);

    if (params.lightShafts)
    {
        edited |= DrawMaterialLightShaftSliders(params, base);
    }

    return edited;
}

static constexpr const char *kLumaBlendLabels[] = {
    "cvar", "off", "normal", "screen", "overlay", "hard light", "color dodge",
};

template <typename Block>
std::string FormatMaskBlockLabel(size_t index, const Block &block)
{
    char buf[48] = {};

    if (block.polyUv.size() >= 6)
    {
        std::snprintf(buf, sizeof(buf), "Mask %zu (%zu pts)", index + 1, block.polyUv.size() / 2);
    }
    else
    {
        std::snprintf(buf, sizeof(buf), "Color %zu (%02X%02X%02X)", index + 1,
                      int(std::lround(std::clamp(block.color[0], 0.0f, 1.0f) * 255.0f)),
                      int(std::lround(std::clamp(block.color[1], 0.0f, 1.0f) * 255.0f)),
                      int(std::lround(std::clamp(block.color[2], 0.0f, 1.0f) * 255.0f)));
    }

    return buf;
}

template <typename Block>
bool DrawMaskBlockHeader(const char *actionId, size_t index, Block &block, Block &clipboard,
                         bool &clipboardValid, bool &outRemove)
{
    const std::string header = FormatMaskBlockLabel(index, block);
    const bool isPoly = block.polyUv.size() >= 6;
    const bool canPaste = clipboardValid && ((clipboard.polyUv.size() >= 6) == isPoly);

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(header.c_str());
    ImGui::SameLine(
        std::max(160.0f, ImGui::GetWindowContentRegionMax().x - 108.0f * EditorGuiScale()));

    const uint32_t actions = EditorGuiBlockActions(actionId, true, canPaste, true);
    bool pasted = false;

    if ((actions & kEditorGuiActionCopy) != 0)
    {
        clipboard = block;
        clipboardValid = true;
    }

    if ((actions & kEditorGuiActionPaste) != 0 && canPaste)
    {
        block = clipboard;
        pasted = true;
    }

    outRemove = (actions & kEditorGuiActionRemove) != 0;
    return pasted;
}

bool PickTextureColorPixel(const char *pickId, uint64_t previewHandle,
                           const EditorTexture *previewTex, float outColor[3])
{
    if (previewHandle == 0 || previewTex == nullptr || previewTex->width == 0 ||
        previewTex->height == 0)
    {
        ImGui::TextDisabled("Texture preview unavailable.");
        return false;
    }

    float u = 0.5f;
    float v = 0.5f;
    if (!EditorImagePick(pickId, previewHandle, previewTex->width, previewTex->height, u, v))
    {
        return false;
    }

    const size_t pixels = size_t(previewTex->width) * size_t(previewTex->height) * 4;
    if (previewTex->rgba.size() < pixels)
    {
        return false;
    }

    const int32_t px =
        std::clamp(int32_t(u * float(previewTex->width)), 0, int32_t(previewTex->width) - 1);
    const int32_t py =
        std::clamp(int32_t(v * float(previewTex->height)), 0, int32_t(previewTex->height) - 1);
    const uint8_t *pixel =
        previewTex->rgba.data() + (size_t(py) * size_t(previewTex->width) + size_t(px)) * 4;

    outColor[0] = float(pixel[0]) / 255.0f;
    outColor[1] = float(pixel[1]) / 255.0f;
    outColor[2] = float(pixel[2]) / 255.0f;
    return true;
}

bool DrawUvPolygonMaskPreview(const char *maskId, std::vector<float> &polyUv,
                              uint64_t previewHandle, const EditorTexture *previewTex)
{
    bool edited = false;

    if (previewHandle != 0 && previewTex != nullptr && previewTex->width > 0 &&
        previewTex->height > 0)
    {
        edited |=
            EditorPolygonEdit(maskId, previewHandle, previewTex->width, previewTex->height, polyUv, 16);
    }
    else
    {
        ImGui::TextDisabled("Texture preview unavailable.");
    }

    if (ImGui::Button("Reset mask"))
    {
        polyUv = { 0.25f, 0.25f, 0.75f, 0.25f, 0.75f, 0.75f, 0.25f, 0.75f };
        edited = true;
    }

    return edited;
}

struct BlockColorUiLabels
{
    const char *colorId;
    const char *pickId;
    const char *thresholdLabel;
    const char *colorTip;
    const char *thresholdTip;
};

template <typename Block>
bool DrawColorBlockPickerAndThreshold(const BlockColorUiLabels &ui, Block &block,
                                      const Block *baseBlock, uint64_t previewHandle,
                                      const EditorTexture *previewTex)
{
    bool edited = false;
    const bool clrChanged = baseBlock != nullptr && !ColorEq(block.color, baseBlock->color);
    bool clrReset = false;

    edited |= EditorGuiColorRow(
        ui.colorId, block.color,
        { clrChanged, true, ui.colorTip, nullptr, baseBlock ? &clrReset : nullptr });

    if (baseBlock != nullptr)
    {
        edited |= ResetVec3IfClicked(block.color, baseBlock->color, &clrReset);
    }

    edited |= PickTextureColorPixel(ui.pickId, previewHandle, previewTex, block.color);

    const bool thrChanged = baseBlock != nullptr && !AlmostEq(block.threshold, baseBlock->threshold);
    bool thrReset = false;
    edited |= EditorGuiSliderFloat(
        ui.thresholdLabel, &block.threshold, 0.0f, 1.0f,
        { thrChanged, true, ui.thresholdTip, "%.3f", baseBlock ? &thrReset : nullptr });

    if (baseBlock != nullptr)
    {
        edited |= ResetIfClicked(block.threshold, baseBlock->threshold, &thrReset);
    }

    return edited;
}

template <typename Block>
bool DrawMaskBlockFeatherSlider(const char *label, Block &block, const Block *baseBlock)
{
    bool edited = false;
    const bool ftChanged = baseBlock != nullptr && !AlmostEq(block.feather, baseBlock->feather);
    bool ftReset = false;
    int32_t feather = int32_t(std::lround(block.feather));

    if (EditorGuiSliderInt(label, &feather, 0, 16,
                           { ftChanged, true, "Outward mask edge blur radius in pixels.", "%d",
                             baseBlock ? &ftReset : nullptr }))
    {
        block.feather = float(feather);
        edited = true;
    }

    if (baseBlock != nullptr)
    {
        edited |= ResetIfClicked(block.feather, baseBlock->feather, &ftReset);
    }

    return edited;
}

bool DrawBlockUnitFloatSlider(const char *label, const char *tip, float &value, const float *baseVal)
{
    const bool changed = baseVal != nullptr && !AlmostEq(value, *baseVal);
    bool reset = false;
    bool edited = EditorGuiSliderFloat(
        label, &value, 0.0f, 1.0f,
        { changed, true, tip, "%.2f", baseVal != nullptr ? &reset : nullptr });
    if (baseVal != nullptr)
    {
        edited |= ResetIfClicked(value, *baseVal, &reset);
    }
    return edited;
}

bool DrawLumaColorPreview(EditorEmissiveBlock &block, const EditorEmissiveBlock *baseBlock,
                          uint64_t previewHandle, const EditorTexture *previewTex)
{
    static constexpr BlockColorUiLabels kUi{
        "##color",
        "##color_pick",
        "luma_threshold",
        "The color this block matches, up to the threshold distance.",
        "Color cube distance within which pixels emit.",
    };
    return DrawColorBlockPickerAndThreshold(kUi, block, baseBlock, previewHandle, previewTex);
}

bool DrawGlossColorPreview(EditorGlossBlock &block, const EditorGlossBlock *baseBlock,
                           uint64_t previewHandle, const EditorTexture *previewTex)
{
    static constexpr BlockColorUiLabels kUi{
        "##gloss_color",
        "##gloss_color_pick",
        "gloss_threshold",
        "The color this gloss block matches, up to the threshold distance.",
        "Color cube distance within which pixels apply gloss.",
    };
    return DrawColorBlockPickerAndThreshold(kUi, block, baseBlock, previewHandle, previewTex);
}

bool DrawSssColorPreview(EditorSssBlock &block, const EditorSssBlock *baseBlock,
                         uint64_t previewHandle, const EditorTexture *previewTex)
{
    static constexpr BlockColorUiLabels kUi{
        "##sss_color",
        "##sss_color_pick",
        "sss_threshold",
        "The color this subsurface block matches, up to the threshold distance.",
        "Color cube distance within which pixels receive subsurface scattering.",
    };
    return DrawColorBlockPickerAndThreshold(kUi, block, baseBlock, previewHandle, previewTex);
}

bool DrawGlossBlockShadingControls(EditorGlossBlock &block, const EditorGlossBlock *baseBlock)
{
    bool edited = DrawMaskBlockFeatherSlider("gloss_feather", block, baseBlock);
    edited |= DrawBlockUnitFloatSlider(
        "roughness", "Target roughness for matched pixels (0 = mirror, 1 = matte).",
        block.roughness, baseBlock != nullptr ? &baseBlock->roughness : nullptr);
    edited |= DrawBlockUnitFloatSlider(
        "metalness", "Target metalness for matched pixels (0 = dielectric, 1 = metal).",
        block.metalness, baseBlock != nullptr ? &baseBlock->metalness : nullptr);
    return edited;
}

bool DrawSssBlockControls(EditorSssBlock &block, const EditorSssBlock *baseBlock)
{
    bool edited = DrawMaskBlockFeatherSlider("sss_feather", block, baseBlock);
    edited |= DrawBlockUnitFloatSlider(
        "factor", "Subsurface mask multiplier for this block.",
        block.factor, baseBlock != nullptr ? &baseBlock->factor : nullptr);
    return edited;
}

bool DrawLumaBlockToneControls(EditorEmissiveBlock &block, const EditorEmissiveBlock *baseBlock)
{
    bool edited = DrawMaskBlockFeatherSlider("luma_feather", block, baseBlock);

    const bool fcChanged = baseBlock != nullptr && !AlmostEq(block.factor, baseBlock->factor);
    bool fcReset = false;
    edited |= EditorGuiSliderFloat(
        "emissive_factor", &block.factor, 0.0f, 5.0f,
        { fcChanged, true, "Emission multiplier for this block.", "%.2f",
          baseBlock ? &fcReset : nullptr });

    if (baseBlock != nullptr)
    {
        edited |= ResetIfClicked(block.factor, baseBlock->factor, &fcReset);
    }

    int32_t blendIdx = block.blend < 0 ? 0 : std::min<int32_t>(block.blend + 1, 6);
    bool blReset = false;
    const bool blDiff = baseBlock != nullptr && block.blend != baseBlock->blend;

    if (EditorGuiCombo("luma_blend", &blendIdx, kLumaBlendLabels, 7,
                       { blDiff, true, "How this block's glow composites into the frame.",
                         nullptr, baseBlock ? &blReset : nullptr }))
    {
        block.blend = blendIdx - 1;
        edited = true;
    }

    if (baseBlock != nullptr)
    {
        edited |= ResetIfClicked(block.blend, baseBlock->blend, &blReset);
    }

    return edited;
}

bool DrawLightOriginAndOffset(EditorLight &light, const EditorLight *baseLight)
{
    bool edited = false;

    const bool posDiff = baseLight != nullptr && !Vec3Eq(light.position, baseLight->position);
    bool posReset = false;
    edited |= EditorGuiVec3Input("origin", light.position, 2.0f,
                                 { posDiff, true, nullptr, nullptr, baseLight ? &posReset : nullptr });
    if (baseLight != nullptr)
    {
        edited |= ResetVec3IfClicked(light.position, baseLight->position, &posReset);
    }

    const bool hoDiff = baseLight != nullptr && light.hasOffset != baseLight->hasOffset;
    bool hoReset = false;
    edited |= EditorGuiCheckbox("has_offset", &light.hasOffset,
                                { hoDiff, true, nullptr, nullptr, baseLight ? &hoReset : nullptr });
    if (baseLight != nullptr)
    {
        edited |= ResetIfClicked(light.hasOffset, baseLight->hasOffset, &hoReset);
    }

    if (light.hasOffset)
    {
        const bool offDiff = baseLight != nullptr && !Vec3Eq(light.offset, baseLight->offset);
        bool offReset = false;
        edited |= EditorGuiVec3Input("offset", light.offset, 1.0f,
                                     { offDiff, true, nullptr, nullptr, baseLight ? &offReset : nullptr });
        if (baseLight != nullptr)
        {
            edited |= ResetVec3IfClicked(light.offset, baseLight->offset, &offReset);
        }
    }
    return edited;
}

bool DrawLightRadiusControl(EditorLight &light, const EditorLight *baseLight)
{
    bool edited = false;
    float radiusM = light.radius / kEditorUnitsPerMeter;
    const bool radDiff = baseLight != nullptr && !AlmostEq(light.radius, baseLight->radius);
    bool radReset = false;
    if (EditorGuiSliderFloat("radius (m)", &radiusM, 0.05f, 10.0f,
                             { radDiff, true, nullptr, "%.2f", baseLight ? &radReset : nullptr }))
    {
        light.radius = radiusM * kEditorUnitsPerMeter;
        edited = true;
    }
    if (baseLight != nullptr && ResetIfClicked(light.radius, baseLight->radius, &radReset))
    {
        edited = true;
    }
    return edited;
}

bool DrawLightRadiometry(EditorLight &light, const EditorLight *baseLight, bool isCustom)
{
    bool edited = DrawLightRadiusControl(light, baseLight);
    const float scale = isCustom ? kEditorCustomLightIntensityUnits : 1.0f;
    float intUi = light.intensity / scale;
    const bool intDiff = baseLight != nullptr && !AlmostEq(light.intensity, baseLight->intensity);
    bool intReset = false;
    const char *label = isCustom ? "intensity" : "light";
    const char *fmt = isCustom ? "%.2f" : "%.0f";
    if (EditorGuiSliderFloat(label, &intUi, 0.0f, isCustom ? 100.0f : 2000.0f,
                             { intDiff, true, nullptr, fmt, baseLight ? &intReset : nullptr }))
    {
        SetEditorLightIntensity(light, intUi * scale);
        edited = true;
    }
    if (baseLight != nullptr && intReset)
    {
        SetEditorLightIntensity(light, baseLight->intensity);
        edited = true;
    }
    return edited;
}

bool DrawLightEnabledField(EditorLight &light, const EditorLight *baseLight)
{
    const bool baseEnabled = baseLight != nullptr ? baseLight->enabled : true;
    const bool enDiff = baseLight != nullptr && light.enabled != baseEnabled;
    bool enReset = false;
    bool edited = EditorGuiCheckbox(
        "enabled", &light.enabled,
        { enDiff, true, "Include this light in scene rendering and clusters.", nullptr,
          baseLight ? &enReset : nullptr });
    if (baseLight != nullptr)
    {
        edited |= ResetIfClicked(light.enabled, baseEnabled, &enReset);
    }
    return edited;
}

bool DrawLightCommonFields(EditorLight &light, const EditorLight *baseLight, bool isCustom)
{
    bool edited = DrawLightEnabledField(light, baseLight);
    edited |= DrawLightOriginAndOffset(light, baseLight);
    edited |= DrawLightRadiometry(light, baseLight, isCustom);

    float tint[3] = { light.tint[0], light.tint[1], light.tint[2] };
    const bool clrDiff = baseLight != nullptr && !ColorEq(light.tint, baseLight->tint);
    bool clrReset = false;
    if (EditorGuiColorHex("color", tint,
                          { clrDiff, true, nullptr, nullptr, baseLight ? &clrReset : nullptr }))
    {
        SetEditorLightTint(light, tint);
        light.hasColor = true;
        edited = true;
    }
    if (baseLight != nullptr && clrReset)
    {
        SetEditorLightTint(light, baseLight->tint);
        edited = true;
    }

    int32_t style = std::clamp(light.style, 0, kEditorStyleCount - 1);
    const bool styDiff = baseLight != nullptr && light.style != baseLight->style;
    bool styReset = false;
    if (EditorGuiCombo("style", &style, kEditorStyleNames, kEditorStyleCount,
                       { styDiff, true, nullptr, nullptr, baseLight ? &styReset : nullptr }))
    {
        light.style = style;
        edited = true;
    }
    if (baseLight != nullptr)
    {
        edited |= ResetIfClicked(light.style, baseLight->style, &styReset);
    }
    return edited;
}

void DrawCustomLightShaftFloatRow(const char *label, float &value, float baseValue,
                                   bool hasBase, float maxValue, const char *tooltip)
{
    const bool diff = hasBase && !AlmostEq(value, baseValue);
    bool reset = false;
    EditorGuiSliderFloat(label, &value, 0.0f, maxValue,
                         { diff, true, tooltip, "%.2f", hasBase ? &reset : nullptr });
    if (hasBase)
    {
        ResetIfClicked(value, baseValue, &reset);
    }
}

void DrawCustomSpotShafts(EditorLight &light, const EditorLight *baseLight)
{
    const bool hasBase = baseLight != nullptr;
    const bool lsDiff = hasBase && light.lightShafts != baseLight->lightShafts;
    bool lsReset = false;
    EditorGuiCheckbox("light_shafts", &light.lightShafts,
                      { lsDiff, true, "Volumetric shafts for this custom spotlight.", nullptr,
                        hasBase ? &lsReset : nullptr });
    if (hasBase)
    {
        ResetIfClicked(light.lightShafts, baseLight->lightShafts, &lsReset);
    }
    if (!light.lightShafts)
    {
        return;
    }

    const float basePower = hasBase ? baseLight->lightShaftsPower : 1.0f;
    const float baseDist = hasBase ? baseLight->lightShaftsDistance : 0.0f;
    const float baseFalloff = hasBase ? baseLight->lightShaftsFalloff : 1.0f;
    DrawCustomLightShaftFloatRow("light_shafts_power", light.lightShaftsPower, basePower, hasBase,
                                 16.0f, nullptr);
    DrawCustomLightShaftFloatRow(
        "light_shafts_distance", light.lightShaftsDistance, baseDist, hasBase, 25.0f,
        "How far the volumetric shafts reach in meters (0.5..25). Set 0 for automatic distance.");
    DrawCustomLightShaftFloatRow(
        "light_shafts_falloff", light.lightShaftsFalloff, baseFalloff, hasBase, 4.0f,
        "How quickly the shafts fade with distance (0 = nearly steady until the end fade, 1 = "
        "smooth default fade, higher = steeper near-lamp decay).");
}

void DrawSpotConeAnglesAndShafts(EditorLight &light, const EditorLight *baseLight, bool isCustom)
{
    const bool inDiff = baseLight != nullptr && !AlmostEq(light.angleInner, baseLight->angleInner);
    bool inReset = false;
    if (EditorGuiSliderFloat("angle_inner", &light.angleInner, 0.0f, 90.0f,
                             { inDiff, true, nullptr, "%.1f", baseLight ? &inReset : nullptr }))
    {
        ClampSpotAngles(light);
    }
    if (baseLight != nullptr)
    {
        ResetIfClicked(light.angleInner, baseLight->angleInner, &inReset);
    }

    const bool outDiff = baseLight != nullptr && !AlmostEq(light.angleOuter, baseLight->angleOuter);
    bool outReset = false;
    if (EditorGuiSliderFloat("angle_outer", &light.angleOuter, 0.0f, 90.0f,
                             { outDiff, true, nullptr, "%.1f", baseLight ? &outReset : nullptr }))
    {
        ClampSpotAngles(light);
    }
    if (baseLight != nullptr)
    {
        ResetIfClicked(light.angleOuter, baseLight->angleOuter, &outReset);
    }

    if (isCustom)
    {
        DrawCustomSpotShafts(light, baseLight);
    }
}

void DrawSpotLightFields(EditorLight &light, const EditorLight *baseLight, bool isCustom,
                         const float camYaw, const float camPitch)
{
    const bool spDiff = baseLight != nullptr && light.spot != baseLight->spot;
    bool spReset = false;
    EditorGuiCheckbox("spot", &light.spot,
                      { spDiff, true, "Convert light to directional cone spotlight.", nullptr,
                        baseLight ? &spReset : nullptr });
    if (baseLight != nullptr)
    {
        ResetIfClicked(light.spot, baseLight->spot, &spReset);
    }

    if (!light.spot)
    {
        return;
    }

    const bool dirDiff = baseLight != nullptr && !Vec3Eq(light.direction, baseLight->direction);
    bool dirReset = false;
    if (EditorGuiVec3Input("dir", light.direction, 0.05f,
                           { dirDiff, true, nullptr, nullptr, baseLight ? &dirReset : nullptr }))
    {
        NormalizeVec3(light.direction);
    }
    if (baseLight != nullptr)
    {
        ResetVec3IfClicked(light.direction, baseLight->direction, &dirReset);
    }

    if (ImGui::Button("Aim along camera view"))
    {
        const float cp = std::cos(camPitch);
        light.direction[0] = cp * std::cos(camYaw);
        light.direction[1] = cp * std::sin(camYaw);
        light.direction[2] = std::sin(camPitch);
    }

    DrawSpotConeAnglesAndShafts(light, baseLight, isCustom);
}

void DrawSystemDtalSection(EditorRenderSettings &s, const EditorRenderSettings &b)
{
    EditorGuiSectionHeader("System / DTAL", "Direct Triangular Area Light & geometry emission limits.");

    bool pwReset = false;
    EditorGuiCheckbox("pause world", &s.pauseWorld,
                      { s.pauseWorld != b.pauseWorld, true, nullptr, nullptr, &pwReset });
    ResetIfClicked(s.pauseWorld, b.pauseWorld, &pwReset);

    static constexpr const char *kDebugModes[] = { "off", "world", "models", "all" };
    bool dbgReset = false;
    EditorGuiCombo("rt_dtal_debug", &s.dtalDebug, kDebugModes, 4,
                   { s.dtalDebug != b.dtalDebug, true, nullptr, nullptr, &dbgReset });
    ResetIfClicked(s.dtalDebug, b.dtalDebug, &dbgReset);

    bool prReset = false;
    EditorGuiCheckbox("rt_dtal_proxy", &s.dtalProxy,
                      { s.dtalProxy != b.dtalProxy, true, nullptr, nullptr, &prReset });
    ResetIfClicked(s.dtalProxy, b.dtalProxy, &prReset);

    struct FloatCvar
    {
        const char *label;
        float *val;
        float baseVal;
        float lo;
        float hi;
    };
    const FloatCvar floatCvars[] = {
        { "rt_dtal_clearance", &s.dtalClearance, b.dtalClearance, 0.0f, 16.0f },
        { "rt_dtal_minarea", &s.dtalMinArea, b.dtalMinArea, 0.0f, 1024.0f },
        { "rt_dtal_proxy_minarea", &s.dtalProxyMinArea, b.dtalProxyMinArea, 0.0f, 1024.0f },
        { "rt_dtal_proxy_model_minarea", &s.dtalProxyModelMinArea, b.dtalProxyModelMinArea, 0.0f, 1024.0f },
        { "rt_dtal_spacing", &s.dtalSpacing, b.dtalSpacing, 1.0f, 1024.0f },
        { "rt_dtal_model_minarea", &s.dtalModelMinArea, b.dtalModelMinArea, 0.0f, 1024.0f },
    };
    for (const FloatCvar &c : floatCvars)
    {
        bool r = false;
        EditorGuiSliderFloat(c.label, c.val, c.lo, c.hi, { !AlmostEq(*c.val, c.baseVal), true, nullptr, "%.1f", &r });
        ResetIfClicked(*c.val, c.baseVal, &r);
    }

    struct IntCvar
    {
        const char *label;
        int32_t *val;
        int32_t baseVal;
        int32_t lo;
        int32_t hi;
    };
    const IntCvar intCvars[] = {
        { "rt_dtal_maxpolys", &s.dtalMaxPolys, b.dtalMaxPolys, 0, 64 },
        { "rt_dtal_groups", &s.dtalGroups, b.dtalGroups, 0, 2 },
        { "rt_dtal_maxgroups", &s.dtalMaxGroups, b.dtalMaxGroups, 1, 4096 },
        { "rt_level_light_maxpolys", &s.levelLightMaxPolys, b.levelLightMaxPolys, 0, 256 },
        { "rt_dtal_model_maxpolys", &s.dtalModelMaxPolys, b.dtalModelMaxPolys, 0, 64 },
        { "rt_dtal_model_budget", &s.dtalModelBudget, b.dtalModelBudget, 0, 2048 },
    };
    for (const IntCvar &c : intCvars)
    {
        bool r = false;
        EditorGuiSliderInt(c.label, c.val, c.lo, c.hi, { *c.val != c.baseVal, true, nullptr, "%d", &r });
        ResetIfClicked(*c.val, c.baseVal, &r);
    }
}

void DrawSystemRenderSection(EditorRenderSettings &s, const EditorRenderSettings &b)
{
    EditorGuiSectionHeader("Light Shafts & Rendering");

    bool lsReset = false;
    EditorGuiCheckbox("rt_light_shafts", &s.lightShafts,
                      { s.lightShafts != b.lightShafts, true, nullptr, nullptr, &lsReset });
    ResetIfClicked(s.lightShafts, b.lightShafts, &lsReset);

    bool lsiReset = false;
    EditorGuiSliderFloat("rt_light_shafts_intensity", &s.lightShaftsIntensity, 0.0f, 8.0f,
                         { !AlmostEq(s.lightShaftsIntensity, b.lightShaftsIntensity), true, nullptr, "%.2f", &lsiReset });
    ResetIfClicked(s.lightShaftsIntensity, b.lightShaftsIntensity, &lsiReset);

    static constexpr const char *kResItems[] = { "quarter", "half", "full" };
    bool lsrReset = false;
    EditorGuiCombo("rt_light_shafts_resolution", &s.lightShaftsResolution, kResItems, 3,
                   { s.lightShaftsResolution != b.lightShaftsResolution, true, nullptr, nullptr, &lsrReset });
    ResetIfClicked(s.lightShaftsResolution, b.lightShaftsResolution, &lsrReset);

    static constexpr const char *kFsrItems[] = { "off", "native", "quality", "balanced", "performance", "ultra perf" };
    bool fsrReset = false;
    EditorGuiCombo("rt_upscale_fsr31", &s.upscaleFsr31, kFsrItems, 6,
                   { s.upscaleFsr31 != b.upscaleFsr31, true, nullptr, nullptr, &fsrReset });
    ResetIfClicked(s.upscaleFsr31, b.upscaleFsr31, &fsrReset);

    static constexpr const char *kSharpenItems[] = { "off", "CAS", "FSR RCAS" };
    bool shReset = false;
    EditorGuiCombo("rt_sharpen", &s.sharpenMode, kSharpenItems, 3,
                   { s.sharpenMode != b.sharpenMode, true, nullptr, nullptr, &shReset });
    ResetIfClicked(s.sharpenMode, b.sharpenMode, &shReset);

    bool rsReset = false;
    EditorGuiSliderFloat("rt_renderscale", &s.renderScale, 25.0f, 100.0f,
                         { !AlmostEq(s.renderScale, b.renderScale), true, nullptr, "%.0f%%", &rsReset });
    ResetIfClicked(s.renderScale, b.renderScale, &rsReset);

    bool den = s.denoiser > 0.0f;
    bool denReset = false;
    if (EditorGuiCheckbox("rt_denoiser", &den,
                          { (s.denoiser > 0.0f) != (b.denoiser > 0.0f), true, nullptr, nullptr, &denReset }))
    {
        s.denoiser = den ? 1.0f : 0.0f;
    }
    ResetIfClicked(s.denoiser, b.denoiser, &denReset);

    int32_t sr = int32_t(s.shadowRays);
    bool srReset = false;
    if (EditorGuiSliderInt("rt_shadowrays", &sr, 0, 8,
                           { s.shadowRays != b.shadowRays, true, nullptr, "%d", &srReset }))
    {
        s.shadowRays = uint32_t(sr);
    }
    ResetIfClicked(s.shadowRays, b.shadowRays, &srReset);

    bool neeReset = false;
    EditorGuiSliderFloat("rt_nee_samples", &s.neeSamples, 1.0f, 2.0f,
                         { !AlmostEq(s.neeSamples, b.neeSamples), true, nullptr, "%.0f", &neeReset });
    ResetIfClicked(s.neeSamples, b.neeSamples, &neeReset);

    bool sensReset = false;
    EditorGuiSliderFloat("sensitivity", &s.mouseSensitivity, 0.5f, 15.0f,
                         { !AlmostEq(s.mouseSensitivity, b.mouseSensitivity), true, nullptr, "%.2f", &sensReset });
    ResetIfClicked(s.mouseSensitivity, b.mouseSensitivity, &sensReset);
}

struct FloatSettingRow
{
    const char *label;
    float *value;
    float baseValue;
    float lo;
    float hi;
    const char *fmt;
    const char *tip;
};

void DrawFloatSettingRows(const FloatSettingRow *rows, size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        const FloatSettingRow &row = rows[i];
        bool reset = false;
        EditorGuiSliderFloat(row.label, row.value, row.lo, row.hi,
                             { !AlmostEq(*row.value, row.baseValue), true, row.tip, row.fmt, &reset });
        ResetIfClicked(*row.value, row.baseValue, &reset);
    }
}

void DrawBoolSettingRow(const char *label, bool *value, bool baseValue, const char *tip)
{
    bool reset = false;
    EditorGuiCheckbox(label, value, { *value != baseValue, true, tip, nullptr, &reset });
    ResetIfClicked(*value, baseValue, &reset);
}

void DrawColorSettingRow(const char *label, float value[3], const float baseValue[3],
                         const char *tip)
{
    bool reset = false;
    EditorGuiColorHex(label, value, { !ColorEq(value, baseValue), true, tip, nullptr, &reset });
    ResetVec3IfClicked(value, baseValue, &reset);
}

void DrawIntSettingRow(const char *label, int32_t *value, int32_t baseValue, int32_t lo,
                       int32_t hi, const char *tip)
{
    bool reset = false;
    EditorGuiSliderInt(label, value, lo, hi,
                       { *value != baseValue, true, tip, "%d", &reset });
    ResetIfClicked(*value, baseValue, &reset);
}

void DrawComboSettingRow(const char *label, int32_t *value, int32_t baseValue,
                         const char *const *items, int32_t count, const char *tip)
{
    int32_t clamped = std::clamp(*value, 0, count - 1);
    bool reset = false;
    if (EditorGuiCombo(label, &clamped, items, count,
                       { clamped != std::clamp(baseValue, 0, count - 1), true, tip, nullptr, &reset }))
    {
        *value = clamped;
    }
    ResetIfClicked(*value, baseValue, &reset);
}

void DrawGlobalLightingSection(EditorRenderSettings &s, const EditorRenderSettings &b)
{
    EditorGuiSectionHeader("Lighting");
    static constexpr const char *kTrueItems[] = { "0 entity lights", "1 materials", "2 combined" };
    int32_t tl = std::clamp(int32_t(std::lround(s.truelight)), 0, 2);
    bool tlReset = false;
    if (EditorGuiCombo("rt_truelight", &tl, kTrueItems, 3,
                       { !AlmostEq(s.truelight, b.truelight), true, nullptr, nullptr, &tlReset }))
    {
        s.truelight = float(tl);
    }
    ResetIfClicked(s.truelight, b.truelight, &tlReset);

    DrawBoolSettingRow("DTAL models", &s.modelLights, b.modelLights,
                       "rt_model_lights: light character models from emissive geometry (DTAL).");
    DrawBoolSettingRow("DTAL level lights", &s.levelLights, b.levelLights,
                       "rt_level_lights: light level flame/torch models from emissive geometry.");

    const FloatSettingRow rows[] = {
        { "rt_brightness", &s.brightness, b.brightness, 0.1f, 4.0f, "%.2f", nullptr },
        { "rt_globallight_mult", &s.globalLightMult, b.globalLightMult, 0.0f, 32.0f, "%.2f", nullptr },
        { "rt_emis_mapboost", &s.emissionMapBoost, b.emissionMapBoost, 0.0f, 100.0f, "%.1f", nullptr },
        { "rt_emis_light_intensity", &s.emissionLightIntensity, b.emissionLightIntensity, 0.0f, 8.0f, "%.2f", nullptr },
    };
    DrawFloatSettingRows(rows, std::size(rows));
}

void DrawGlobalSkySection(EditorRenderSettings &s, const EditorRenderSettings &b)
{
    EditorGuiSectionHeader("Sky & Sun");
    static constexpr const char *kSkyItems[] = { "classic", "skybox", "procedural" };
    int32_t skyMode = 0;
    if (s.skyType == QR_SKY_TYPE_CUBEMAP)
    {
        skyMode = 1;
    }
    else if (s.skyType == QR_SKY_TYPE_PROCEDURAL)
    {
        skyMode = 2;
    }

    const bool skyDiff = s.skyType != b.skyType;
    bool stReset = false;
    if (EditorGuiCombo("rt_physical_sky", &skyMode, kSkyItems, 3,
                       { skyDiff, true, "0 classic Quake sky texture, 1 6-sided mod skybox, 2 procedural sky with clouds and sun.", nullptr, &stReset }))
    {
        if (skyMode == 2)
        {
            s.skyType = QR_SKY_TYPE_PROCEDURAL;
            if (s.skyBrightness <= 0.001f)
            {
                s.skyBrightness = 1.0f;
            }
        }
        else if (skyMode == 1)
        {
            s.skyType = QR_SKY_TYPE_CUBEMAP;
        }
        else
        {
            s.skyType = QR_SKY_TYPE_RASTERIZED_GEOMETRY;
        }
    }
    if (ResetIfClicked(s.skyType, b.skyType, &stReset) && s.skyType == QR_SKY_TYPE_PROCEDURAL &&
        s.skyBrightness <= 0.001f)
    {
        s.skyBrightness = 1.0f;
    }

    const FloatSettingRow rows[] = {
        { "rt_sky", &s.skyScale, b.skyScale, 0.0f, 8.0f, "%.2f",
          "Overall sky intensity multiplier (visible sky and emitted sky light)." },
        { "rt_sky_brightness", &s.skyBrightness, b.skyBrightness, 0.0f, 10.0f, "%.2f",
          "Visible brightness of the sky / skybox (and procedural sky)." },
        { "rt_sky_light_mult", &s.skyLightMultiplier, b.skyLightMultiplier, 0.0f, 8.0f, "%.2f",
          "How much light the sky casts on the level, separate from visible sky brightness." },
    };
    DrawFloatSettingRows(rows, std::size(rows));
    DrawColorSettingRow("rt_sky_color", s.skyColor, b.skyColor, nullptr);
    if (s.skyType == QR_SKY_TYPE_PROCEDURAL)
    {
        const FloatSettingRow atmoRows[] = {
            { "rt_sky_atmosphere_density", &s.atmosphereDensity, b.atmosphereDensity, 0.0f, 4.0f, "%.2f",
              "Rayleigh/ozone atmosphere optical thickness (0 = space void)." },
            { "rt_sky_mie", &s.atmosphereMie, b.atmosphereMie, 0.0f, 4.0f, "%.2f",
              "Aerosol haze and solar aureole strength." },
            { "rt_sky_ozone", &s.atmosphereOzone, b.atmosphereOzone, 0.0f, 4.0f, "%.2f",
              "Chappuis ozone band separating twilight crimson-orange from blue zenith." },
        };
        DrawFloatSettingRows(atmoRows, std::size(atmoRows));
    }
    DrawIntSettingRow("rt_sky_ambient_lod", &s.skyAmbientLod, b.skyAmbientLod, 0, 10, nullptr);
    DrawBoolSettingRow("rt_sky_nee", &s.skyNee, b.skyNee, nullptr);
}

void DrawGlobalTimeAndNightSection(EditorRenderSettings &s, const EditorRenderSettings &b)
{
    if (s.skyType != QR_SKY_TYPE_PROCEDURAL)
    {
        return;
    }

    EditorGuiSectionHeader("Time of Day");
    DrawBoolSettingRow("rt_sky_time_enable", &s.timeOfDayEnable, b.timeOfDayEnable,
                       "Compute sun, moon and star rotation from 24-hour time of day and enable automatic sky sunlight.");

    const FloatSettingRow todRows[] = {
        { "rt_sky_time", &s.timeOfDay, b.timeOfDay, 0.0f, 24.0f, "%.2f",
          "Time of day in hours (0 midnight, 6 dawn, 12 noon, 18 sunset)." },
        { "rt_sky_time_speed", &s.timeOfDaySpeed, b.timeOfDaySpeed, 0.0f, 60.0f, "%.2f",
          "Time-of-day speed in hours per real minute (0 = paused)." },
        { "rt_sky_latitude", &s.skyLatitude, b.skyLatitude, -80.0f, 80.0f, "%.1f",
          "Observer latitude in degrees (tilts the solar arc)." },
        { "rt_sky_azimuth", &s.skyAzimuth, b.skyAzimuth, -180.0f, 180.0f, "%.1f",
          "Compass azimuth offset for sunrise/sunset direction in degrees." },
    };
    DrawFloatSettingRows(todRows, std::size(todRows));

    struct PresetTime { const char *label; float hour; };
    static constexpr PresetTime kPresets[] = {
        { "Dawn", 6.15f }, { "Noon", 12.0f }, { "Sunset", 17.85f }, { "Twilight", 18.25f }, { "Night", 0.0f },
    };
    for (size_t i = 0; i < std::size(kPresets); i++)
    {
        if (i > 0)
        {
            ImGui::SameLine();
        }
        if (ImGui::SmallButton(kPresets[i].label))
        {
            s.timeOfDayEnable = true;
            s.timeOfDay = kPresets[i].hour;
        }
    }

    EditorGuiSectionHeader("Moon & Stars");
    DrawBoolSettingRow("rt_sky_stars_enable", &s.starsEnabled, b.starsEnabled,
                       "Render the procedural 3-tier night starfield.");
    const FloatSettingRow starRows[] = {
        { "rt_sky_stars_density", &s.starDensity, b.starDensity, 0.0f, 4.0f, "%.2f",
          "Count and spatial density of stars across all three magnitude tiers." },
        { "rt_sky_stars", &s.starIntensity, b.starIntensity, 0.0f, 5.0f, "%.2f",
          "Overall star brightness and bright-star glow." },
        { "rt_sky_moon", &s.moonIntensity, b.moonIntensity, 0.0f, 4.0f, "%.2f",
          "Moon disc brightness and night cloud illumination." },
    };
    DrawFloatSettingRows(starRows, std::size(starRows));
    DrawColorSettingRow("rt_sky_moon_color", s.moonColor, b.moonColor, "Tint color of the moon and moonlight.");
    const FloatSettingRow moonRows[] = {
        { "rt_sky_moon_size", &s.moonSize, b.moonSize, 0.0f, 10.0f, "%.2f",
          "Moon disc angular size multiplier." },
        { "rt_sky_moon_phase", &s.moonPhase, b.moonPhase, -1.0f, 1.0f, "%.2f",
          "Lunar illumination phase (0 new, 0.5 half, 1 full, < 0 auto from sun)." },
    };
    DrawFloatSettingRows(moonRows, std::size(moonRows));
}

void DrawGlobalLightingAndSky(EditorApp &app, EditorRenderSettings &s, const EditorRenderSettings &b,
                              EditorSunState &sun, const EditorSunState &bsun, bool &outPlaceSun)
{
    DrawGlobalLightingSection(s, b);
    DrawGlobalSkySection(s, b);
    DrawGlobalTimeAndNightSection(s, b);

    DrawBoolSettingRow("rt_physical_sun", &sun.enabled, bsun.enabled, nullptr);

    float sunGain = sun.radiance / kEditorSkySunRadiance;
    bool sgReset = false;
    if (EditorGuiSliderFloat("rt_sky_sun", &sunGain, 0.0f, 20.0f,
                             { !AlmostEq(sun.radiance, bsun.radiance), true, nullptr, "%.2f", &sgReset }))
    {
        sun.radiance = sunGain * kEditorSkySunRadiance;
    }
    ResetIfClicked(sun.radiance, bsun.radiance, &sgReset);

    const FloatSettingRow sunRows[] = {
        { "rt_sky_sun_disc", &s.sunDiscScale, b.sunDiscScale, 0.0f, 20.0f, "%.2f", nullptr },
    };
    DrawFloatSettingRows(sunRows, std::size(sunRows));
    DrawColorSettingRow("rt_sky_sun_color", s.sunColor, b.sunColor, nullptr);

    const FloatSettingRow angleRows[] = {
        { "rt_sky_sun_size", &s.sunDiscSize, b.sunDiscSize, 0.0f, 10.0f, "%.2f", nullptr },
        { "rt_sky_sun_pitch", &sun.pitch, bsun.pitch, -180.0f, 180.0f, "%.1f", nullptr },
        { "rt_sky_sun_yaw", &sun.yaw, bsun.yaw, 0.0f, 360.0f, "%.1f", nullptr },
    };
    DrawFloatSettingRows(angleRows, std::size(angleRows));

    if (ImGui::Button("Set sun position"))
    {
        outPlaceSun = true;
    }
    (void)app;
}

void DrawGlobalCloudsControls(EditorRenderSettings &s, const EditorRenderSettings &b)
{
    DrawBoolSettingRow("rt_sky_clouds", &s.clouds, b.clouds, "Draw volumetric clouds.");
    DrawColorSettingRow("rt_sky_clouds_color", s.cloudsColor, b.cloudsColor, "Tint color of the clouds.");

    const FloatSettingRow rowsA[] = {
        { "rt_sky_clouds_alpha", &s.cloudsAlpha, b.cloudsAlpha, 0.0f, 1.0f, "%.2f", "Cloud layer opacity." },
        { "rt_sky_clouds_coverage", &s.cloudsCoverage, b.cloudsCoverage, 0.0f, 1.0f, "%.2f", "Fraction of sky covered by clouds." },
        { "rt_sky_clouds_density", &s.cloudsDensity, b.cloudsDensity, 0.0f, 10.0f, "%.2f", "Optical density of the cloud layer." },
        { "rt_sky_clouds_speed", &s.cloudsSpeed, b.cloudsSpeed, 0.0f, 4.0f, "%.2f", "Cloud drift speed." },
    };
    DrawFloatSettingRows(rowsA, std::size(rowsA));

    static constexpr const char *kCloudQuality[] = { "low", "medium", "high", "ultra" };
    DrawComboSettingRow("rt_sky_clouds_quality", &s.cloudsQuality, b.cloudsQuality, kCloudQuality, 4,
                        "Volumetric cloud sampling quality.");

    const FloatSettingRow rowsB[] = {
        { "rt_sky_clouds_height", &s.cloudsHeight, b.cloudsHeight, 1.0f, 300000.0f, "%.0f", "Cloud altitude in world units." },
        { "rt_sky_clouds_thickness", &s.cloudsThickness, b.cloudsThickness, 1.0f, 300000.0f, "%.0f", "Cloud layer thickness in world units." },
    };
    DrawFloatSettingRows(rowsB, std::size(rowsB));
}

void DrawGlobalTonemapSection(EditorRenderSettings &s, const EditorRenderSettings &b)
{
    EditorGuiSectionHeader("Tonemap");
    const FloatSettingRow rows[] = {
        { "rt_tonemap_white", &s.tonemapWhite, b.tonemapWhite, 1.0f, 40.0f, "%.1f", nullptr },
        { "rt_tonemap_chroma", &s.tonemapChroma, b.tonemapChroma, 0.0f, 1.0f, "%.2f", nullptr },
        { "rt_tonemap_desat", &s.tonemapDesat, b.tonemapDesat, 0.0f, 1.0f, "%.2f", nullptr },
        { "rt_tonemap_max_log", &s.tonemapMaxLog, b.tonemapMaxLog, -4.0f, 4.0f, "%.2f", nullptr },
        { "rt_tonemap_min_log", &s.tonemapMinLog, b.tonemapMinLog, -12.0f, 0.0f, "%.2f", nullptr },
    };
    DrawFloatSettingRows(rows, std::size(rows));
}

void DrawGlobalAtmosphereSection(EditorRenderSettings &s, const EditorRenderSettings &b)
{
    EditorGuiSectionHeader("Clouds, Volumetrics & Fog");

    if (s.skyType == QR_SKY_TYPE_PROCEDURAL)
    {
        DrawGlobalCloudsControls(s, b);
    }

    DrawBoolSettingRow("rt_sky_godrays", &s.godRays, b.godRays, nullptr);
    const FloatSettingRow rayRows[] = {
        { "rt_sky_godrays_intensity", &s.godRaysIntensity, b.godRaysIntensity, 0.0f, 4.0f, "%.2f", nullptr },
        { "rt_sky_godrays_sky_threshold", &s.godRaysSkyThreshold, b.godRaysSkyThreshold, 0.0f, 1.0f, "%.2f", nullptr },
    };
    DrawFloatSettingRows(rayRows, std::size(rayRows));

    static constexpr const char *kGodrayQuality[] = { "low", "medium", "high", "ultra", "extreme" };
    DrawComboSettingRow("rt_sky_godrays_quality", &s.godRaysQuality, b.godRaysQuality,
                        kGodrayQuality, 5, nullptr);

    static constexpr const char *kVolItems[] = { "off", "simple", "volumetric" };
    DrawComboSettingRow("rt_volume_type", &s.volumeType, b.volumeType, kVolItems, 3, nullptr);

    const FloatSettingRow volRows[] = {
        { "rt_volume_scatter", &s.volumeScatter, b.volumeScatter, 0.0f, 2.0f, "%.2f", nullptr },
        { "rt_volume_ambient", &s.volumeAmbient, b.volumeAmbient, 0.0f, 8.0f, "%.2f", nullptr },
        { "rt_volume_far", &s.volumeFar, b.volumeFar, 0.0f, 4000.0f, "%.0f", nullptr },
    };
    DrawFloatSettingRows(volRows, std::size(volRows));

    DrawColorSettingRow("fog_color", s.fogColor, b.fogColor, nullptr);
    const FloatSettingRow fogRows[] = {
        { "fog_density", &s.fogDensity, b.fogDensity, 0.0f, 4.0f, "%.3f", nullptr },
    };
    DrawFloatSettingRows(fogRows, std::size(fogRows));

    DrawGlobalTonemapSection(s, b);
}

} // namespace

void EditorApp::ApplyBaselineCapture()
{
    m_baselineMaterials = m_materials;
    m_baselineSettings = m_settings;
    m_baselineSun = m_sun;
    m_baselineCustomLights = m_customLights;
    m_baselineMapLights = m_scene.lights;
    m_baselineEmitterOverrides = m_emitterLightOverrides;
}

bool EditorApp::HasUnsavedEditorChanges() const
{
    return m_sceneLoaded &&
           (m_sessionDirty || !m_touchedMaterials.empty() || m_levelTouched ||
            (!m_levelFingerprint.empty() && LevelFingerprint() != m_levelFingerprint));
}

void EditorApp::InitEditorSessionMode()
{
    ApplyBaselineCapture();
    if (!m_initialTab.empty() || m_maxFrames > 0)
    {
        m_choosingMode = false;
        if (EqualsIgnoreCase(m_initialTab, "Entity"))
        {
            m_editorMode = EditorMode::Light;
            m_lightTab = 0;
        }
        else if (EqualsIgnoreCase(m_initialTab, "Custom"))
        {
            m_editorMode = EditorMode::Light;
            m_lightTab = 1;
        }
        else if (EqualsIgnoreCase(m_initialTab, "Global"))
        {
            m_editorMode = EditorMode::Light;
            m_lightTab = 2;
        }
        else if (EqualsIgnoreCase(m_initialTab, "Models") || EqualsIgnoreCase(m_initialTab, "MDL"))
        {
            m_editorMode = EditorMode::Material;
            m_materialTab = 1;
        }
        else if (EqualsIgnoreCase(m_initialTab, "Entities"))
        {
            m_editorMode = EditorMode::Material;
            m_materialTab = 2;
        }
        else if (EqualsIgnoreCase(m_initialTab, "System") || EqualsIgnoreCase(m_initialTab, "Render"))
        {
            m_editorMode = EditorMode::Material;
            m_materialTab = 3;
        }
        else
        {
            m_editorMode = EditorMode::Material;
            m_materialTab = 0;
        }
        SetCursorMode(true);
        return;
    }

    m_choosingMode = true;
    SetCursorMode(true);
}

void EditorApp::StartEditorMode(EditorMode mode)
{
    m_editorMode = mode;
    m_choosingMode = false;
    m_customPlacing = false;
    m_customCloning = -1;
    m_placingSun = false;
    if (m_sceneLoaded && m_materialsChanged)
    {
        m_materialsChanged = false;
        if (m_instance != nullptr)
        {
            UploadStaticGeometry();
        }
        else
        {
            BuildEmissiveLights();
        }
    }
    ApplyBaselineCapture();
    SetCursorMode(true);
}

void EditorApp::BackToChooser()
{
    m_customPlacing = false;
    m_customCloning = -1;
    m_placingSun = false;
    m_choosingMode = true;
    SetCursorMode(true);
}

bool EditorApp::HandleEditorModeKeyDown(int key)
{
    (void)key;
    if (m_openLevelDialog)
    {
        m_openLevelDialog = false;
        return true;
    }
    if (m_openModDialog)
    {
        m_openModDialog = false;
        return true;
    }
    if (m_choosingMode)
    {
        RequestEditorExit();
        return true;
    }
    if (m_placingSun || m_customPlacing || m_customCloning >= 0)
    {
        if (m_placingSun)
        {
            m_sun = m_placingSunBackup;
        }
        m_placingSun = false;
        m_customPlacing = false;
        m_customCloning = -1;
        SetCursorMode(true);
        return true;
    }
    if (m_gizmoDragging || m_grabDragging)
    {
        FinishGizmoDrag(false);
        FinishLightGrab(false);
    }
    else if (!m_textureBrowserMaterial.empty())
    {
        m_textureBrowserMaterial.clear();
    }
    else if (m_sessionPrompt != 0 || m_resetPrompt != 0 || m_regenPrompt != 0)
    {
        m_sessionPrompt = 0;
        m_pendingAction = PendingSessionAction::None;
        m_pendingActionPath.clear();
        m_pendingActionFlag = false;
        m_resetPrompt = 0;
        m_regenPrompt = 0;
    }
    else if (m_showPanel)
    {
        SetCursorMode(false);
    }
    return true;
}

void EditorApp::HandleLightModePick(const float direction[3], float surfaceDistance, bool hitSurface)
{
    if (m_placingSun)
    {
        ApplySunDirectionFromRay(direction, m_sun);
        m_placingSun = false;
        SetCursorMode(true);
        PrintMessage("sun: direction updated from viewport pick", this);
        return;
    }

    if (m_customPlacing || m_customCloning >= 0)
    {
        const float dist = hitSurface ? std::max(16.0f, surfaceDistance) : 128.0f;
        m_hasPickPoint = true;
        m_pickCluster = 0;
        for (int axis = 0; axis < 3; axis++)
        {
            m_pickPoint[axis] = m_cameraPosition[axis] + direction[axis] * dist;
        }
        const bool hasCloneSrc =
            m_customCloning >= 0 && size_t(m_customCloning) < m_scene.lights.size();
        const EditorLight *source = hasCloneSrc ? &m_scene.lights[size_t(m_customCloning)] : nullptr;
        AddCustomLight(source, LightSpawn::Clone);
        m_customPlacing = false;
        m_customCloning = -1;
        m_lightTab = 1;
        SetCursorMode(true);
        return;
    }

    float lightDistance = 0.0f;
    bool lightCustom = false;
    const int32_t light = PickLight(m_cameraPosition, direction, lightDistance, lightCustom);
    m_selectedSurface = -1;
    m_selectedFace = -1;
    m_hasPickPoint = hitSurface;
    m_selectedLight = -1;
    m_selectedCustomLight = -1;
    if (light >= 0)
    {
        m_selectedLight = lightCustom ? -1 : light;
        m_selectedCustomLight = lightCustom ? light : -1;
        m_lightTab = lightCustom ? 1 : 0;
    }
    if (hitSurface)
    {
        for (int axis = 0; axis < 3; axis++)
        {
            m_pickPoint[axis] = m_cameraPosition[axis] + direction[axis] * surfaceDistance;
        }
    }
}

void EditorApp::OpenTextureBrowserForSlot(const std::string &materialName, EditorTextureSlot slot)
{
    m_textureBrowserSlot = slot;
    m_textureBrowserMaterial = materialName;
    m_textureBrowserRequested = true;
}

void EditorApp::MarkMaterialEdited(int32_t materialIndex)
{
    m_materialsChanged = true;
    if (materialIndex >= 0 && size_t(materialIndex) < m_materials.Count())
    {
        TouchMaterial(m_materials.NameAt(size_t(materialIndex)));
    }
}

void EditorApp::MarkMaterialsEdited()
{
    MarkMaterialEdited(m_selectedMaterial);
}

bool EditorApp::SaveSessionToDisk()
{
    const bool ok = ApplySession();
    if (ok)
    {
        ApplyBaselineCapture();
    }
    return ok;
}

void EditorApp::CancelSessionChanges()
{
    DiscardSession();
    ApplyBaselineCapture();
}

void EditorApp::RequestEditorExit()
{
    RequestSessionAction(PendingSessionAction::Exit);
}

void EditorApp::UploadTorchLight(QrResult &lastResult)
{
    EditorLight torch;
    std::copy_n(m_cameraPosition, 3, torch.position);
    torch.radius = 8.0f;
    const float warm[3] = { 1.0f, 0.92f, 0.78f };
    SetEditorLightTint(torch, warm);
    SetEditorLightIntensity(torch, 350.0f);
    UploadLight(torch, kTorchUniqueId, lastResult);
}

namespace
{
void EffectiveLightOrigin(const EditorLight &light, float outPos[3])
{
    outPos[0] = light.position[0] + (light.hasOffset ? light.offset[0] : 0.0f);
    outPos[1] = light.position[1] + (light.hasOffset ? light.offset[1] : 0.0f);
    outPos[2] = light.position[2] + (light.hasOffset ? light.offset[2] : 0.0f);
}

float ComputePixelsPerUnit(const EditorCameraBasis &cam, const float pos[3], float &outDist)
{
    const float dx = pos[0] - cam.origin[0];
    const float dy = pos[1] - cam.origin[1];
    const float dz = pos[2] - cam.origin[2];
    const float depth = dx * cam.forward[0] + dy * cam.forward[1] + dz * cam.forward[2];
    outDist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (depth <= 0.0f || cam.tanHalfFovY <= 0.0f)
    {
        return 0.0f;
    }
    return (0.5f * cam.viewportHeight) / (depth * cam.tanHalfFovY);
}

void DrawCustomLightGlyph(ImDrawList *draw, float cx, float cy, float dist, float scale,
                          bool centerHovered)
{
    constexpr int kGlyphSegs = 10;
    constexpr size_t kGlyphPointCount = size_t(kGlyphSegs) + 1;
    const float radius = std::clamp(dist * 0.005f, 1.0f, 24.0f) * scale;
    std::array<ImVec2, kGlyphPointCount> pts = {};
    for (size_t g = 0; g < kGlyphPointCount; ++g)
    {
        const float angle = (45.0f + 270.0f * float(g) / float(kGlyphSegs)) * kDegToRad;
        pts[g] = ImVec2(cx + std::cos(angle) * radius, cy - std::sin(angle) * radius);
    }
    const ImU32 glyphColor =
        centerHovered ? IM_COL32(255, 214, 64, 255) : IM_COL32(200, 235, 255, 255);
    draw->AddPolyline(pts.data(), int(kGlyphPointCount), glyphColor, ImDrawFlags_None,
                      centerHovered ? 2.5f : 2.0f);
}

struct ViewportLightDrawState
{
    const EditorCameraBasis &cam;
    int32_t selectedIdx = -1;
    int32_t hoverIdx = -1;
    bool custom = false;
    const std::function<bool(const float[3], float &, float &)> &project;
    bool centerHovered = false;
};

void DrawViewportLightList(ImDrawList *draw, const std::vector<EditorLight> &lights,
                           const ViewportLightDrawState &state)
{
    for (size_t i = 0; i < lights.size(); ++i)
    {
        float pos[3] = {};
        EffectiveLightOrigin(lights[i], pos);

        float sx = 0.0f;
        float sy = 0.0f;
        if (!state.project(pos, sx, sy))
        {
            continue;
        }

        float dist = 0.0f;
        const float scale = ComputePixelsPerUnit(state.cam, pos, dist);
        const bool isSel = state.selectedIdx == int32_t(i);
        const bool isHov = state.hoverIdx == int32_t(i);
        const ImU32 idleColor =
            lights[i].enabled ? IM_COL32(0, 255, 255, 200) : IM_COL32(128, 128, 128, 140);
        const ImU32 color =
            isSel ? IM_COL32(255, 255, 255, 255) : (isHov ? IM_COL32(255, 214, 64, 255) : idleColor);

        draw->AddCircle(ImVec2(sx, sy), std::max(1.0f, lights[i].radius * scale), color, 0, 2.0f);

        if (state.custom)
        {
            DrawCustomLightGlyph(draw, sx, sy, dist, scale, isSel && state.centerHovered);
        }

        const float dirLenSq = lights[i].direction[0] * lights[i].direction[0] +
                               lights[i].direction[1] * lights[i].direction[1] +
                               lights[i].direction[2] * lights[i].direction[2];
        if (lights[i].spot && dirLenSq > 0.0f)
        {
            DrawSpotCone(draw, state.project, pos, lights[i].direction, lights[i].angleOuter, color);
            DrawSpotCone(draw, state.project, pos, lights[i].direction, lights[i].angleInner,
                         IM_COL32(200, 235, 255, 255));
        }
    }
}
} // namespace

void EditorApp::DrawLightEditorViewportExtras()
{
    if (m_choosingMode || m_editorMode != EditorMode::Light || m_clientWidth == 0 || m_clientHeight == 0)
    {
        return;
    }

    ImDrawList *draw = ImGui::GetBackgroundDrawList();
    if (draw == nullptr)
    {
        return;
    }

    const EditorCameraBasis cam = CameraBasis();
    const auto project = [this](const float p[3], float &x, float &y) {
        return ProjectPoint(p, x, y);
    };

    DrawViewportLightList(draw, m_scene.lights,
                          { cam, m_selectedLight, m_hoverLight, false, project, false });
    const bool centerActive = (m_hoverGizmoPart == GizmoPart::Center || m_grabDragging);
    DrawViewportLightList(draw, m_customLights,
                          { cam, m_selectedCustomLight, m_hoverCustomLight, true, project, centerActive });

    if (m_placingSun || m_customPlacing || m_customCloning >= 0)
    {
        const char *msg = m_placingSun
                              ? "Click in viewport to set sun direction (Esc cancels)"
                              : "Click surface in viewport to place light (Esc cancels)";
        const ImVec2 sz = ImGui::CalcTextSize(msg);
        const ImVec2 pos(float(m_clientWidth) * 0.5f - sz.x * 0.5f,
                         ImGui::GetFrameHeight() + 16.0f * EditorGuiScale());
        draw->AddText(ImVec2(pos.x + 1.0f, pos.y + 1.0f), IM_COL32(0, 0, 0, 200), msg);
        draw->AddText(pos, IM_COL32(255, 224, 112, 255), msg);
    }
}

namespace
{

struct BrowserDirEntry
{
    std::string name;
    std::string fullPath;
    bool isDir = false;
};

bool ContainsTextIgnoreCase(const std::string &haystack, const char *needle)
{
    if (needle == nullptr || needle[0] == '\0')
    {
        return true;
    }

    const size_t nLen = std::strlen(needle);
    if (nLen > haystack.size())
    {
        return false;
    }

    for (size_t i = 0; i + nLen <= haystack.size(); i++)
    {
        size_t j = 0;
        while (j < nLen &&
               std::tolower(static_cast<unsigned char>(haystack[i + j])) ==
                   std::tolower(static_cast<unsigned char>(needle[j])))
        {
            j++;
        }
        if (j == nLen)
        {
            return true;
        }
    }

    return false;
}

bool IsBrowserMapFile(const std::filesystem::path &path)
{
    std::string ext = path.extension().string();
    for (char &ch : ext)
    {
        ch = char(std::tolower(static_cast<unsigned char>(ch)));
    }
    return ext == ".bsp" || ext == ".obj";
}

std::vector<BrowserDirEntry> ListBrowserDirectory(const std::string &dir, bool includeMaps)
{
    std::vector<BrowserDirEntry> result;
    std::error_code ec;

    if (dir.empty() || !std::filesystem::is_directory(dir, ec))
    {
        return result;
    }

    for (const std::filesystem::directory_entry &entry :
         std::filesystem::directory_iterator(dir, ec))
    {
        const bool isDir = entry.is_directory(ec);
        const bool isFile = includeMaps && entry.is_regular_file(ec) && IsBrowserMapFile(entry.path());
        if (!isDir && !isFile)
        {
            continue;
        }

        result.push_back(BrowserDirEntry{
            entry.path().filename().string(),
            entry.path().lexically_normal().string(),
            isDir,
        });
    }

    std::sort(result.begin(), result.end(), [](const BrowserDirEntry &a, const BrowserDirEntry &b) {
        if (a.isDir != b.isDir)
        {
            return a.isDir;
        }
        return a.name < b.name;
    });

    return result;
}

void SetBrowserDir(std::string &dir, char *buf, size_t bufSize, const std::string &target)
{
    std::error_code ec;
    const std::filesystem::path absPath = std::filesystem::absolute(target, ec).lexically_normal();
    dir = ec ? target : absPath.string();
    std::snprintf(buf, bufSize, "%s", dir.c_str());
}

void DrawDirectoryPathBar(std::string &dir, char *buf, size_t bufSize, const char *idSuffix)
{
    const float scale = EditorGuiScale();
    const float btnW = 54.0f * scale;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float inputW = std::max(120.0f, ImGui::GetContentRegionAvail().x - (btnW + spacing) * 2.0f);

    ImGui::SetNextItemWidth(inputW);
    const std::string inputId = std::string("##dir_") + idSuffix;
    const bool submitted =
        ImGui::InputText(inputId.c_str(), buf, bufSize, ImGuiInputTextFlags_EnterReturnsTrue);

    ImGui::SameLine();
    if (ImGui::Button((std::string("Go##go_") + idSuffix).c_str(), ImVec2(btnW, 0.0f)) || submitted)
    {
        std::error_code ec;
        if (std::filesystem::is_directory(buf, ec))
        {
            SetBrowserDir(dir, buf, bufSize, buf);
        }
    }

    ImGui::SameLine();
    if (ImGui::Button((std::string("Up##up_") + idSuffix).c_str(), ImVec2(btnW, 0.0f)))
    {
        const std::filesystem::path parent = std::filesystem::path(dir).parent_path();
        if (!parent.empty())
        {
            SetBrowserDir(dir, buf, bufSize, parent.string());
        }
    }
}

} // namespace

void EditorApp::DrawLevelsInModSubmenu()
{
    if (!ImGui::BeginMenu("Levels in Mod", !m_mapCatalog.empty()))
    {
        return;
    }

    int32_t shown = 0;
    for (const MapCatalogItem &item : m_mapCatalog)
    {
        if (item.isBrushSubmodel && !m_showBrushSubmodels)
        {
            continue;
        }
        const bool active = m_sceneLoaded && item.levelKey == m_sessionPaths.level;
        const std::string label =
            item.hasSavedSettings ? (item.levelKey + "  [saved]") : item.levelKey;
        if (ImGui::MenuItem(label.c_str(), nullptr, active))
        {
            RequestSessionAction(PendingSessionAction::OpenScene, item.path);
        }
        shown++;
    }

    if (shown == 0)
    {
        ImGui::MenuItem("No playable maps found", nullptr, false, false);
    }
    ImGui::EndMenu();
}

void EditorApp::DrawSwitchModSubmenu()
{
    if (!ImGui::BeginMenu("Switch Mod", !m_discoveredMods.empty()))
    {
        return;
    }

    for (const std::string &modDir : m_discoveredMods)
    {
        const std::string modName = std::filesystem::path(modDir).filename().string();
        const bool active = !m_gamePath.empty() && modDir == m_gamePath;
        if (ImGui::MenuItem(modName.c_str(), nullptr, active))
        {
            RequestSessionAction(PendingSessionAction::OpenMod, modDir, m_modUseOwnYaml);
        }
    }
    ImGui::EndMenu();
}

void EditorApp::DrawFileMenu()
{
    if (!ImGui::BeginMenu("File"))
    {
        return;
    }

    if (ImGui::MenuItem("Open Level...", "Ctrl+O"))
    {
        OpenLevelBrowserDialog();
    }
    if (ImGui::MenuItem("Open Mod / Game Directory...", "Ctrl+M"))
    {
        OpenModBrowserDialog();
    }
    DrawLevelsInModSubmenu();
    DrawSwitchModSubmenu();
    ImGui::Separator();

    if (ImGui::MenuItem("Reload Level", "Ctrl+R", false, m_sceneLoaded))
    {
        RequestSessionAction(PendingSessionAction::ReloadScene);
    }
    if (ImGui::MenuItem("Close Level", "Ctrl+W", false, m_sceneLoaded))
    {
        RequestSessionAction(PendingSessionAction::CloseScene);
    }
    if (ImGui::MenuItem("Close / Unload Mod", nullptr, false, !m_gamePath.empty()))
    {
        RequestSessionAction(PendingSessionAction::UnloadMod);
    }
    ImGui::Separator();

    if (ImGui::MenuItem("Multiplayer Mode (Deathmatch)", nullptr, m_multiplayerMode))
    {
        m_multiplayerMode = !m_multiplayerMode;
        if (m_sceneLoaded)
        {
            RequestSessionAction(PendingSessionAction::ReloadScene);
        }
    }
    ImGui::Separator();

    const bool canSave = m_sceneLoaded && HasUnsavedEditorChanges();
    if (ImGui::MenuItem("Save Session", "Ctrl+S", false, canSave))
    {
        SaveSessionToDisk();
    }
    if (ImGui::MenuItem("Revert Unsaved Changes", nullptr, false, canSave))
    {
        CancelSessionChanges();
    }
    if (ImGui::MenuItem("Reset Session Files...", nullptr, false, m_sceneLoaded))
    {
        m_resetPrompt = 1;
        m_loggedResetLayout = 0;
    }
    ImGui::Separator();

    if (ImGui::MenuItem("Exit"))
    {
        RequestEditorExit();
    }

    ImGui::EndMenu();
}

void EditorApp::DrawModeMenu()
{
    if (!ImGui::BeginMenu("Mode"))
    {
        return;
    }

    if (ImGui::MenuItem("Material Editor", nullptr,
                        m_sceneLoaded && !m_choosingMode && m_editorMode == EditorMode::Material,
                        m_sceneLoaded))
    {
        StartEditorMode(EditorMode::Material);
    }

    if (ImGui::MenuItem("Light Editor", nullptr,
                        m_sceneLoaded && !m_choosingMode && m_editorMode == EditorMode::Light,
                        m_sceneLoaded))
    {
        StartEditorMode(EditorMode::Light);
    }

    if (ImGui::MenuItem("Mode Chooser...", nullptr, m_sceneLoaded && m_choosingMode, m_sceneLoaded))
    {
        BackToChooser();
    }

    ImGui::Separator();

    if (ImGui::MenuItem("Regenerate Light Models...", nullptr, false, m_sceneLoaded))
    {
        m_regenPrompt = 1;
        m_loggedRegenLayout = 0;
    }

    ImGui::EndMenu();
}

void EditorApp::DrawViewMenu()
{
    if (!ImGui::BeginMenu("View"))
    {
        return;
    }

    if (ImGui::MenuItem("Inspector Panel (Cursor Mode)", "Tab", m_sceneLoaded && m_showPanel,
                        m_sceneLoaded))
    {
        SetCursorMode(!m_showPanel);
    }

    if (ImGui::MenuItem("Torch Mode (Headlight)", nullptr, m_sceneLoaded && m_torchMode,
                        m_sceneLoaded))
    {
        m_torchMode = !m_torchMode;
    }

    if (ImGui::MenuItem("Reset Camera to Spawn", "Home", false, m_sceneLoaded))
    {
        ResetCameraToScene();
    }

    ImGui::EndMenu();
}

void EditorApp::DrawMenuBarStatus()
{
    const std::string modName =
        m_gamePath.empty() ? std::string("none")
                           : std::filesystem::path(m_gamePath).filename().string();
    const std::string levelName =
        !m_sceneLoaded ? std::string("none")
                       : (m_sessionPaths.level.empty() ? std::string("scene") : m_sessionPaths.level);
    const bool dirty = HasUnsavedEditorChanges();
    const std::string status = "Mod: " + modName + "  |  Level: " + levelName +
                               (m_sceneLoaded ? (m_multiplayerMode ? " [MP]" : " [SP]") : "") +
                               (dirty ? "  [Unsaved]" : "");
    const float pad = 12.0f * EditorGuiScale();
    const float textW = ImGui::CalcTextSize(status.c_str()).x;
    const float targetX = std::max(ImGui::GetCursorPosX() + pad, ImGui::GetWindowWidth() - textW - pad);

    ImGui::SetCursorPosX(targetX);
    if (dirty)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.78f, 0.38f, 1.0f));
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.66f, 0.72f, 0.80f, 1.0f));
    }
    ImGui::TextUnformatted(status.c_str());
    ImGui::PopStyleColor();
}

void EditorApp::DrawMainMenuBar()
{
    if (!ImGui::BeginMainMenuBar())
    {
        return;
    }

    DrawFileMenu();
    DrawModeMenu();
    DrawViewMenu();
    DrawMenuBarStatus();
    ImGui::EndMainMenuBar();
}

void EditorApp::DrawOpenLevelModTab(std::string &chosenPath)
{
    const ImGuiTabItemFlags flags = m_openLevelTab == 0 ? ImGuiTabItemFlags_SetSelected : 0;
    if (!ImGui::BeginTabItem("Mod Levels", nullptr, flags))
    {
        return;
    }

    const float scale = EditorGuiScale();
    m_openLevelTab = 0;
    ImGui::SetNextItemWidth(260.0f * scale);
    ImGui::InputTextWithHint("##level_search", "Filter maps...", m_openLevelSearch,
                             sizeof(m_openLevelSearch));
    ImGui::SameLine();
    ImGui::Checkbox("Show brush models (b_*.bsp)", &m_showBrushSubmodels);

    const float listH = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 2.8f;
    if (ImGui::BeginChild("##mod_levels", ImVec2(0.0f, std::max(120.0f, listH)), true))
    {
        if (m_mapCatalog.empty())
        {
            ImGui::TextDisabled("No maps mounted. Load a mod or switch to Filesystem.");
        }
        for (size_t i = 0; i < m_mapCatalog.size(); i++)
        {
            const MapCatalogItem &item = m_mapCatalog[i];
            if ((item.isBrushSubmodel && !m_showBrushSubmodels) ||
                (!ContainsTextIgnoreCase(item.levelKey, m_openLevelSearch) &&
                 !ContainsTextIgnoreCase(item.path, m_openLevelSearch)))
            {
                continue;
            }

            const bool selected = m_selectedCatalogMap == int32_t(i);
            std::string row = item.levelKey + "   (" + item.path + ")   [" + item.source + "]";
            if (item.hasSavedSettings)
            {
                row += " [configured]";
            }
            if (ImGui::Selectable(row.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick))
            {
                m_selectedCatalogMap = int32_t(i);
                if (ImGui::IsMouseDoubleClicked(0))
                {
                    chosenPath = item.path;
                }
            }
        }
    }
    ImGui::EndChild();
    ImGui::EndTabItem();
}

namespace
{
void DrawFsLevelQuickButtons(std::string &dir, char *buf, size_t bufSize,
                             const std::string &gamePath)
{
    std::error_code ec;
    const bool hasScenes = std::filesystem::is_directory("build/linux-editor/scenes", ec);
    if (hasScenes && ImGui::SmallButton("Scenes"))
    {
        SetBrowserDir(dir, buf, bufSize, "build/linux-editor/scenes");
    }
    if (hasScenes && !gamePath.empty())
    {
        ImGui::SameLine();
    }
    if (!gamePath.empty() && ImGui::SmallButton("Game Dir"))
    {
        SetBrowserDir(dir, buf, bufSize, gamePath);
    }
}

void DrawFsLevelEntryRow(const BrowserDirEntry &entry, std::string &dir, char *buf, size_t bufSize,
                         std::string &selectedFile, std::string &chosenPath)
{
    const std::string label = entry.isDir ? ("[DIR] " + entry.name) : entry.name;
    const bool selected = !entry.isDir && selectedFile == entry.fullPath;
    const bool clicked = ImGui::Selectable(label.c_str(), selected,
                                           ImGuiSelectableFlags_AllowDoubleClick);
    const bool dblClick = clicked && ImGui::IsMouseDoubleClicked(0);

    if (entry.isDir && dblClick)
    {
        SetBrowserDir(dir, buf, bufSize, entry.fullPath);
        selectedFile.clear();
    }
    else if (!entry.isDir && clicked)
    {
        selectedFile = entry.fullPath;
        if (dblClick)
        {
            chosenPath = entry.fullPath;
        }
    }
}
} // namespace

void EditorApp::DrawOpenLevelFsTab(std::string &chosenPath)
{
    const ImGuiTabItemFlags flags = m_openLevelTab == 1 ? ImGuiTabItemFlags_SetSelected : 0;
    if (!ImGui::BeginTabItem("Filesystem", nullptr, flags))
    {
        return;
    }

    m_openLevelTab = 1;
    DrawDirectoryPathBar(m_fileBrowserDir, m_fileBrowserPathInput,
                         sizeof(m_fileBrowserPathInput), "lvl");
    DrawFsLevelQuickButtons(m_fileBrowserDir, m_fileBrowserPathInput,
                            sizeof(m_fileBrowserPathInput), m_gamePath);

    const float listH = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 2.8f;
    if (ImGui::BeginChild("##fs_levels", ImVec2(0.0f, std::max(120.0f, listH)), true))
    {
        for (const BrowserDirEntry &entry : ListBrowserDirectory(m_fileBrowserDir, true))
        {
            DrawFsLevelEntryRow(entry, m_fileBrowserDir, m_fileBrowserPathInput,
                                sizeof(m_fileBrowserPathInput), m_selectedBrowserFile, chosenPath);
        }
    }
    ImGui::EndChild();
    ImGui::EndTabItem();
}

void EditorApp::DrawOpenLevelDialog()
{
    if (!m_openLevelDialog)
    {
        return;
    }

    const float scale = EditorGuiScale();
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                   viewport->Pos.y + viewport->Size.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(680.0f * scale, 480.0f * scale), ImGuiCond_Appearing);

    bool open = true;
    std::string chosenPath;

    if (ImGui::Begin("Open Level", &open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings))
    {
        if (ImGui::BeginTabBar("##OpenLevelTabs"))
        {
            DrawOpenLevelModTab(chosenPath);
            DrawOpenLevelFsTab(chosenPath);
            ImGui::EndTabBar();
        }

        const bool hasSelection =
            m_openLevelTab == 0
                ? (m_selectedCatalogMap >= 0 && size_t(m_selectedCatalogMap) < m_mapCatalog.size())
                : !m_selectedBrowserFile.empty();
        ImGui::Checkbox("Multiplayer mode (Deathmatch)", &m_multiplayerMode);
        ImGui::BeginDisabled(!hasSelection);
        if (ImGui::Button("Open", ImVec2(110.0f * scale, 0.0f)))
        {
            chosenPath = m_openLevelTab == 0
                             ? m_mapCatalog[size_t(m_selectedCatalogMap)].path
                             : m_selectedBrowserFile;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110.0f * scale, 0.0f)))
        {
            open = false;
        }
    }

    ImGui::End();

    if (!open || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        m_openLevelDialog = false;
        return;
    }

    if (!chosenPath.empty())
    {
        m_openLevelDialog = false;
        RequestSessionAction(PendingSessionAction::OpenScene, chosenPath);
    }
}

void EditorApp::DrawDetectedModsList(std::string &modToLoad)
{
    if (m_discoveredMods.empty())
    {
        return;
    }

    const float scale = EditorGuiScale();
    ImGui::TextUnformatted("Detected Mods:");
    if (ImGui::BeginChild("##detected_mods", ImVec2(0.0f, 88.0f * scale), true))
    {
        for (const std::string &modDir : m_discoveredMods)
        {
            const bool selected = (m_selectedModSubdir == modDir) ||
                                  (m_selectedModSubdir.empty() && modDir == m_modBrowserDir);
            const std::string label =
                std::filesystem::path(modDir).filename().string() + "  (" + modDir + ")";
            if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick))
            {
                m_selectedModSubdir = modDir;
                SetBrowserDir(m_modBrowserDir, m_modBrowserPathInput,
                              sizeof(m_modBrowserPathInput), modDir);
                if (ImGui::IsMouseDoubleClicked(0))
                {
                    modToLoad = modDir;
                }
            }
        }
    }
    ImGui::EndChild();
}

void EditorApp::DrawModDirectoryList()
{
    const float listH = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 2.8f;
    if (ImGui::BeginChild("##mod_dirs", ImVec2(0.0f, std::max(100.0f, listH)), true))
    {
        for (const BrowserDirEntry &entry : ListBrowserDirectory(m_modBrowserDir, false))
        {
            const bool selected = (m_selectedModSubdir == entry.fullPath);
            const std::string label = "[DIR] " + entry.name;
            if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick))
            {
                m_selectedModSubdir = entry.fullPath;
                if (ImGui::IsMouseDoubleClicked(0))
                {
                    SetBrowserDir(m_modBrowserDir, m_modBrowserPathInput,
                                  sizeof(m_modBrowserPathInput), entry.fullPath);
                }
            }
        }
    }
    ImGui::EndChild();
}

void EditorApp::DrawOpenModDialog()
{
    if (!m_openModDialog)
    {
        return;
    }

    const float scale = EditorGuiScale();
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                   viewport->Pos.y + viewport->Size.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(640.0f * scale, 460.0f * scale), ImGuiCond_Appearing);

    bool open = true;
    std::string modToLoad;
    bool unloadRequested = false;

    if (ImGui::Begin("Open Mod / Game Directory", &open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::TextWrapped("Active Mod: %s", m_gamePath.empty() ? "(none)" : m_gamePath.c_str());
        ImGui::TextDisabled("Materials YAML: %s",
                            m_sessionPaths.materials.empty() ? "-" : m_sessionPaths.materials.c_str());
        ImGui::TextDisabled("Lights YAML:    %s",
                            m_sessionPaths.lights.empty() ? "-" : m_sessionPaths.lights.c_str());
        ImGui::Separator();

        DrawDetectedModsList(modToLoad);
        DrawDirectoryPathBar(m_modBrowserDir, m_modBrowserPathInput, sizeof(m_modBrowserPathInput),
                             "mod");
        DrawModDirectoryList();

        ImGui::Checkbox("Use mod folder for qray.materials.yaml and qray.lights.yaml",
                        &m_modUseOwnYaml);

        const std::string targetMod =
            !m_selectedModSubdir.empty() ? m_selectedModSubdir : m_modBrowserDir;
        std::error_code ec;
        const bool validDir = !targetMod.empty() && std::filesystem::is_directory(targetMod, ec);
        ImGui::BeginDisabled(!validDir);
        if (ImGui::Button("Load Mod", ImVec2(120.0f * scale, 0.0f)))
        {
            modToLoad = targetMod;
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(m_gamePath.empty());
        if (ImGui::Button("Unload Mod", ImVec2(120.0f * scale, 0.0f)))
        {
            unloadRequested = true;
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100.0f * scale, 0.0f)))
        {
            open = false;
        }
    }

    ImGui::End();

    if (!open || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        m_openModDialog = false;
        return;
    }

    if (unloadRequested)
    {
        m_openModDialog = false;
        RequestSessionAction(PendingSessionAction::UnloadMod);
        return;
    }

    if (!modToLoad.empty())
    {
        m_openModDialog = false;
        RequestSessionAction(PendingSessionAction::OpenMod, modToLoad, m_modUseOwnYaml);
    }
}

void EditorApp::DrawEmptySceneOverlay()
{
    if (m_openLevelDialog || m_openModDialog || m_sessionPrompt != 0)
    {
        return;
    }

    static constexpr const char *kButtons[] = {
        "Open Level...",
        "Open Mod / Game Directory...",
        "Exit",
    };
    static constexpr const char *kDescs[] = {
        "Browse maps inside the active mod or open a .bsp / .obj file from disk.",
        "Mount a Quake game or mod folder (id1, hipnotic, rogue, custom mods).",
        "Close QuakeRay Editor.",
    };
    const std::string sub = m_gamePath.empty()
                                ? "No level or mod is currently loaded."
                                : ("Mod loaded (" +
                                   std::filesystem::path(m_gamePath).filename().string() +
                                   "), no level open.");
    const int32_t choice = EditorGuiDialogVertical("##EmptySceneChooser", "QuakeRay Editor",
                                                   sub.c_str(), kButtons, kDescs, 3);
    if (choice == 0)
    {
        OpenLevelBrowserDialog();
    }
    else if (choice == 1)
    {
        OpenModBrowserDialog();
    }
    else if (choice == 2)
    {
        RequestEditorExit();
    }
}

void EditorApp::DrawEditorOverlay(float deltaSeconds)
{
    (void)deltaSeconds;
    EditorGuiApplyStyle(float(m_clientWidth), float(m_clientHeight));
    DrawMainMenuBar();
    DrawOpenLevelDialog();
    DrawOpenModDialog();

    if (!m_sceneLoaded)
    {
        m_panelVisible = false;
        DrawEmptySceneOverlay();
        return;
    }

    if (m_choosingMode)
    {
        m_panelVisible = false;
        DrawModeChooser();
        return;
    }

    if (m_showPanel)
    {
        if (m_editorMode == EditorMode::Material)
        {
            DrawMaterialEditorPanel();
        }
        else
        {
            DrawLightEditorPanel();
        }
    }
    else
    {
        m_panelVisible = false;
    }

    DrawTextureBrowser();
}

void EditorApp::DrawModeChooser()
{
    EditorGuiBackdrop(float(m_clientWidth), float(m_clientHeight), 0.52f);
    static constexpr const char *kButtons[] = { "Material Editor", "Light Editor", "Exit" };
    static constexpr const char *kDescs[] = {
        "Edit texture overrides, PBR parameters, is_light emission and DTAL system cvars.",
        "Inspect entity lights, place custom spotlights/lights and adjust sky & sun.",
        "Quit QuakeRay Editor (prompts if there are unsaved session changes).",
    };
    const int32_t choice = EditorGuiDialogVertical(
        "##ModeChooser", "QuakeRay Editor", "Choose the editor to run on this level (Esc closes).",
        kButtons, kDescs, 3);
    if (choice == 0)
    {
        StartEditorMode(EditorMode::Material);
    }
    else if (choice == 1)
    {
        StartEditorMode(EditorMode::Light);
    }
    else if (choice == 2)
    {
        RequestEditorExit();
    }
}

void EditorApp::DrawPanelActionBar(bool isLightEditor)
{
    (void)isLightEditor;
    if (!m_levelTouched && LevelFingerprint() != m_levelFingerprint)
    {
        m_levelTouched = true;
        m_sessionDirty = true;
    }

    EditorGuiCheckbox("torch mode", &m_torchMode,
                      { m_torchMode, true, "Attach a camera headlight while exploring dark areas." });

    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float btnW = (ImGui::GetContentRegionAvail().x - spacing * 3.0f) * 0.25f;
    const bool dirty = HasUnsavedEditorChanges();

    ImGui::BeginDisabled(!dirty);
    if (ImGui::Button("Save", ImVec2(btnW, 0.0f)))
    {
        SaveSessionToDisk();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(btnW, 0.0f)))
    {
        CancelSessionChanges();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Modes", ImVec2(btnW, 0.0f)))
    {
        BackToChooser();
    }
    ImGui::SameLine();
    if (ImGui::Button("Exit", ImVec2(btnW, 0.0f)))
    {
        RequestEditorExit();
    }
    ImGui::Separator();
}

namespace
{

const EditorSurface *FindAnimatedSurfaceForMaterial(const EditorScene &scene,
                                                    const std::string &matName)
{
    for (const EditorSurface &surface : scene.surfaces)
    {
        if (surface.animationFrames.size() <= 1)
        {
            continue;
        }

        for (int32_t texIdx : surface.animationFrames)
        {
            if (texIdx >= 0 && size_t(texIdx) < scene.textures.size() &&
                EqualsIgnoreCase(scene.textures[size_t(texIdx)].name, matName.c_str()))
            {
                return &surface;
            }
        }
    }

    return nullptr;
}

void AppendLumaBlock(EditorMaterialParams &params, bool isMask, int32_t &selectedBlock)
{
    EditorEmissiveBlock blk;
    blk.color[0] = 1.0f;
    blk.color[1] = 0.0f;
    blk.color[2] = 0.0f;
    blk.threshold = params.lumaThreshold > 0.0f ? params.lumaThreshold : 0.02f;
    blk.feather = params.lumaFeather;
    blk.factor = params.emissiveFactor;
    blk.blend = params.lumaBlend;

    if (isMask)
    {
        blk.polyUv = { 0.25f, 0.25f, 0.75f, 0.25f, 0.75f, 0.75f, 0.25f, 0.75f };
    }

    params.emissiveBlocks.push_back(std::move(blk));
    params.hasColorEmissive = true;
    selectedBlock = int32_t(params.emissiveBlocks.size()) - 1;
}

bool DrawMaterialAnimFrameRow(const EditorScene &scene, EditorMaterialTable &materials,
                              int32_t &selectedMaterial)
{
    const std::string &matName = materials.NameAt(size_t(selectedMaterial));
    const EditorSurface *surface = FindAnimatedSurfaceForMaterial(scene, matName);

    if (surface == nullptr)
    {
        return false;
    }

    std::vector<int32_t> frameMats;
    frameMats.reserve(surface->animationFrames.size());

    for (int32_t texIdx : surface->animationFrames)
    {
        if (texIdx >= 0 && size_t(texIdx) < scene.textures.size())
        {
            const int32_t mIdx = materials.IndexOf(scene.textures[size_t(texIdx)].name);

            if (mIdx >= 0)
            {
                frameMats.push_back(mIdx);
            }
        }
    }

    if (frameMats.size() <= 1)
    {
        return false;
    }

    int32_t framePos = 0;

    for (size_t i = 0; i < frameMats.size(); i++)
    {
        if (frameMats[i] == selectedMaterial)
        {
            framePos = int32_t(i);
            break;
        }
    }

    ImGui::TextDisabled("the frames of this texture: the one below is shown and edited");
    bool switched = false;

    if (EditorGuiSliderInt("frame", &framePos, 0, int32_t(frameMats.size()) - 1,
                           { false, true, "The texture frame the panel edits and previews." }))
    {
        selectedMaterial = frameMats[size_t(framePos)];
        materials.BindSurfaceAlias(surface->textureName, selectedMaterial);
        switched = true;
    }

    ImGui::TextDisabled("Frames:");

    for (size_t i = 0; i < frameMats.size(); i++)
    {
        ImGui::SameLine();
        const std::string &fname = materials.NameAt(size_t(frameMats[i]));
        const std::string tag = fname.size() >= 2 ? fname.substr(0, 2) : fname;
        const bool active = frameMats[i] == selectedMaterial;

        if (active)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.45f, 0.70f, 1.0f));
        }

        if (ImGui::SmallButton(tag.c_str()))
        {
            selectedMaterial = frameMats[i];
            materials.BindSurfaceAlias(surface->textureName, selectedMaterial);
            switched = true;
        }

        if (active)
        {
            ImGui::PopStyleColor();
        }
    }

    return switched;
}

void DrawMaterialTextureInfoHeader(const EditorScene &scene, const EditorMaterialParams &params,
                                 const std::string &matName, int32_t selectedSurface,
                                 const std::function<int32_t(const std::string &)> &resolveTex)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.66f, 0.72f, 1.0f));
    ImGui::PushTextWrapPos(0.0f);

    if (selectedSurface >= 0 && size_t(selectedSurface) < scene.surfaces.size())
    {
        ImGui::Text("face: %s", scene.surfaces[size_t(selectedSurface)].textureName.c_str());
    }

    int32_t texIdx = resolveTex(params.baseTexture);

    if (texIdx < 0)
    {
        texIdx = resolveTex(matName);
    }

    if (texIdx >= 0 && size_t(texIdx) < scene.textures.size())
    {
        const EditorTexture &tex = scene.textures[size_t(texIdx)];
        ImGui::Text("material texture: %s -- %u x %u px", matName.c_str(), tex.width, tex.height);
    }
    else
    {
        ImGui::Text("material texture: %s", matName.c_str());
    }

    const auto slotStr = [](const std::string &s) { return s.empty() ? "-" : s.c_str(); };
    ImGui::Text("maps: base %s | normals %s | emissive %s | overlay %s | gloss %s | sss %s",
                slotStr(params.baseTexture), slotStr(params.normalTexture),
                slotStr(params.emissiveTexture), slotStr(params.overlayTexture),
                slotStr(params.glossTexture), slotStr(params.textureSss));
    ImGui::Text("uv: scale %.2f x %.2f  offset %.2f, %.2f", double(params.textureScaleU),
                double(params.textureScaleV), double(params.textureOffsetU),
                double(params.textureOffsetV));

    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Spacing();
}

bool DrawMaterialLumaToggleSection(EditorMaterialParams &params, const EditorMaterialParams &base,
                                   int32_t &selectedBlock)
{
    EditorGuiSectionHeader("Luma", "Emissive mask blocks & blend controls.");
    const bool hasTexEmis = !params.emissiveTexture.empty();
    bool edited = false;

    if (hasTexEmis)
    {
        bool efReset = false;
        edited |= EditorGuiSliderFloat(
            "emissive_factor", &params.emissiveFactor, 0.0f, 5.0f,
            { !AlmostEq(params.emissiveFactor, base.emissiveFactor), true, nullptr, "%.2f", &efReset });
        edited |= ResetIfClicked(params.emissiveFactor, base.emissiveFactor, &efReset);

        int32_t blIdx = params.lumaBlend < 0 ? 0 : std::min<int32_t>(params.lumaBlend + 1, 6);
        bool blReset = false;

        if (EditorGuiCombo("luma_blend", &blIdx, kLumaBlendLabels, 7,
                           { params.lumaBlend != base.lumaBlend, true, nullptr, nullptr, &blReset }))
        {
            params.lumaBlend = blIdx - 1;
            edited = true;
        }

        edited |= ResetIfClicked(params.lumaBlend, base.lumaBlend, &blReset);
    }

    const bool prevLuma = params.hasColorEmissive;
    bool lumaReset = false;
    edited |= EditorGuiCheckbox(
        "luma", &params.hasColorEmissive,
        { params.hasColorEmissive != base.hasColorEmissive, !hasTexEmis,
          hasTexEmis ? "Disabled while texture_emissive is assigned."
                     : "Glow by color or mask: click pixels or paint UV masks below.",
          nullptr, &lumaReset });

    if (lumaReset)
    {
        params.hasColorEmissive = base.hasColorEmissive;
        params.emissiveBlocks = base.emissiveBlocks;
        selectedBlock = 0;
        edited = true;
    }

    if (!prevLuma && params.hasColorEmissive && params.emissiveBlocks.empty() && !hasTexEmis)
    {
        AppendLumaBlock(params, false, selectedBlock);
        edited = true;
    }

    return edited;
}

struct BlockPreviewTarget
{
    uint64_t handle = 0;
    const EditorTexture *tex = nullptr;
};

struct LumaClipboardState
{
    EditorEmissiveBlock &block;
    bool &valid;
};

struct GlossClipboardState
{
    EditorGlossBlock &block;
    bool &valid;
};

struct SssClipboardState
{
    EditorSssBlock &block;
    bool &valid;
};

BlockPreviewTarget MakeBlockPreviewTarget(const EditorScene &scene,
                                          const std::vector<QrMaterial> &materials,
                                          int32_t texIndex)
{
    const EditorTexture *previewTex =
        (texIndex >= 0 && size_t(texIndex) < scene.textures.size())
            ? &scene.textures[size_t(texIndex)]
            : nullptr;
    const uint64_t previewHandle =
        (previewTex != nullptr && size_t(texIndex) < materials.size() &&
         materials[size_t(texIndex)] != QR_NO_MATERIAL)
            ? uint64_t(materials[size_t(texIndex)])
            : 0;
    return BlockPreviewTarget{ previewHandle, previewTex };
}

bool TryReadOpaqueTexelRgb(const EditorTexture &tex, uint32_t x, uint32_t y,
                           bool requireNonBlack, float outColor[3])
{
    const size_t offset = (size_t(y) * tex.width + x) * 4;
    if (tex.rgba[offset + 3] == 0)
    {
        return false;
    }

    const uint8_t r = tex.rgba[offset + 0];
    const uint8_t g = tex.rgba[offset + 1];
    const uint8_t b = tex.rgba[offset + 2];
    if (requireNonBlack && r == 0 && g == 0 && b == 0)
    {
        return false;
    }

    outColor[0] = r / 255.0f;
    outColor[1] = g / 255.0f;
    outColor[2] = b / 255.0f;
    return true;
}

void SeedGlossBlockColor(const EditorTexture *previewTex, float outColor[3])
{
    outColor[0] = 0.7f;
    outColor[1] = 0.7f;
    outColor[2] = 0.7f;

    if (previewTex == nullptr || previewTex->width == 0 || previewTex->height == 0 ||
        previewTex->rgba.size() != size_t(previewTex->width) * size_t(previewTex->height) * 4)
    {
        return;
    }

    const uint32_t cx = previewTex->width / 2;
    const uint32_t cy = previewTex->height / 2;
    if (TryReadOpaqueTexelRgb(*previewTex, cx, cy, true, outColor))
    {
        return;
    }

    for (uint32_t y = 0; y < previewTex->height; y++)
    {
        for (uint32_t x = 0; x < previewTex->width; x++)
        {
            if (TryReadOpaqueTexelRgb(*previewTex, x, y, true, outColor))
            {
                return;
            }
        }
    }

    TryReadOpaqueTexelRgb(*previewTex, cx, cy, false, outColor);
}

void AppendGlossBlock(EditorMaterialParams &params, bool isMask, int32_t &selectedBlock,
                      const EditorTexture *previewTex = nullptr)
{
    EditorGlossBlock blk;
    SeedGlossBlockColor(previewTex, blk.color);
    blk.threshold = params.glossThreshold > 0.02f ? params.glossThreshold : 0.25f;
    blk.feather = params.glossFeather;
    blk.roughness = params.roughnessOverride >= 0.0f
                        ? std::clamp(params.roughnessOverride, 0.0f, 1.0f)
                        : 0.08f;
    blk.metalness = params.metalnessFactor > 0.0f
                        ? std::clamp(params.metalnessFactor, 0.0f, 1.0f)
                        : 0.70f;

    if (isMask)
    {
        blk.polyUv = { 0.25f, 0.25f, 0.75f, 0.25f, 0.75f, 0.75f, 0.25f, 0.75f };
    }

    params.glossBlocks.push_back(std::move(blk));
    params.hasColorGloss = true;
    selectedBlock = int32_t(params.glossBlocks.size()) - 1;
}

void AppendSssBlock(EditorMaterialParams &params, bool isMask, int32_t &selectedBlock,
                    const EditorTexture *previewTex = nullptr)
{
    EditorSssBlock blk;
    SeedGlossBlockColor(previewTex, blk.color);
    blk.threshold = params.sssThreshold > 0.0f ? params.sssThreshold : 0.02f;
    blk.feather = params.sssFeather;
    blk.factor = 1.0f;

    if (isMask)
    {
        blk.polyUv = { 0.25f, 0.25f, 0.75f, 0.25f, 0.75f, 0.75f, 0.25f, 0.75f };
    }

    params.sssBlocks.push_back(std::move(blk));
    selectedBlock = int32_t(params.sssBlocks.size()) - 1;
    if (params.sssStrength <= 0.0f)
    {
        params.sssStrength = 1.0f;
    }
}

bool DrawMaterialGlossSection(EditorMaterialParams &params, const EditorMaterialParams &base,
                              int32_t &selectedBlock, const EditorTexture *previewTex)
{
    EditorGuiSectionHeader("Gloss", "Roughness, metalness and procedural gloss mask blocks.");
    bool edited = false;

    const bool hasTexGloss = !params.glossTexture.empty();
    const bool proceduralGlossActive = params.hasColorGloss && !hasTexGloss;

    if (!proceduralGlossActive)
    {
        bool roughReset = false;
        float roughUi = params.roughnessOverride < 0.0f ? -0.02f : params.roughnessOverride;
        const char *roughFmt = roughUi < 0.0f ? "default" : "%.2f";
        EditorGuiRowOpts roughOpts{ !AlmostEq(params.roughnessOverride, base.roughnessOverride),
                                    true,
                                    "Override surface roughness (-0.02 resets to default).",
                                    roughFmt, &roughReset };
        if (EditorGuiSliderFloat("roughness_override", &roughUi, -0.02f, 1.0f, roughOpts))
        {
            params.roughnessOverride = roughUi < 0.0f ? -1.0f : roughUi;
            edited = true;
        }
        edited |= ResetIfClicked(params.roughnessOverride, base.roughnessOverride, &roughReset);

        bool metalReset = false;
        edited |= EditorGuiSliderFloat(
            "metalness_factor", &params.metalnessFactor, 0.0f, 1.0f,
            { !AlmostEq(params.metalnessFactor, base.metalnessFactor), true, nullptr, "%.2f",
              &metalReset });
        edited |= ResetIfClicked(params.metalnessFactor, base.metalnessFactor, &metalReset);
    }

    bool mnReset = false;
    edited |= EditorGuiCheckbox(
        "metalness_from_normal_alpha", &params.metalnessFromNormalAlpha,
        { params.metalnessFromNormalAlpha != base.metalnessFromNormalAlpha, true, nullptr, nullptr,
          &mnReset });
    edited |= ResetIfClicked(params.metalnessFromNormalAlpha, base.metalnessFromNormalAlpha,
                             &mnReset);

    const bool prevGloss = params.hasColorGloss;
    bool glossReset = false;
    edited |= EditorGuiCheckbox(
        "gloss", &params.hasColorGloss,
        { params.hasColorGloss != base.hasColorGloss, !hasTexGloss,
          hasTexGloss ? "Disabled while texture_gloss is assigned."
                      : "Gloss by color or mask: click pixels or paint UV masks below.",
          nullptr, &glossReset });

    if (glossReset)
    {
        params.hasColorGloss = base.hasColorGloss;
        params.glossBlocks = base.glossBlocks;
        selectedBlock = 0;
        edited = true;
    }

    if (!prevGloss && params.hasColorGloss && params.glossBlocks.empty() && !hasTexGloss)
    {
        AppendGlossBlock(params, false, selectedBlock, previewTex);
        edited = true;
    }

    return edited;
}

bool IsSssLockedByMaterial(const EditorMaterialParams &params)
{
    return params.type == EditorMaterialType::Glass ||
           params.type == EditorMaterialType::Water || params.mirror;
}

bool DrawSssScalarRow(const char *label, float &value, float baseValue, bool enabled,
                      const char *fmt, const char *tip)
{
    bool reset = false;
    bool edited = EditorGuiSliderFloat(
        label, &value, 0.0f, 1.0f,
        { !AlmostEq(value, baseValue), enabled, tip, fmt, &reset });
    edited |= ResetIfClicked(value, baseValue, &reset);
    return edited;
}

bool DrawMaterialSssSection(EditorMaterialParams &params, const EditorMaterialParams &base)
{
    EditorGuiSectionHeader(
        "Subsurface", "Subsurface scattering tint, radius, strength, and procedural mask blocks.");
    const bool enabled = !IsSssLockedByMaterial(params);
    if (!enabled)
    {
        ImGui::TextDisabled("Disabled on glass, water, and mirror materials.");
    }

    bool edited = false;
    edited |= DrawSssScalarRow("sss_strength", params.sssStrength, base.sssStrength, enabled, "%.2f",
                               "Global subsurface scattering intensity (0 disables).");
    edited |= DrawSssScalarRow("sss_radius", params.sssRadius, base.sssRadius, enabled, "%.3f",
                               "Subsurface scatter blur radius in meters.");

    bool colorReset = false;
    edited |= EditorGuiColorHex("sss_color", params.sssColor,
                                { !ColorEq(params.sssColor, base.sssColor), enabled,
                                  "Subsurface scattering tint color.", nullptr, &colorReset });
    edited |= ResetVec3IfClicked(params.sssColor, base.sssColor, &colorReset);

    edited |= DrawSssScalarRow("sss_threshold", params.sssThreshold, base.sssThreshold, enabled,
                               "%.3f", "Default color distance threshold for new subsurface blocks.");
    edited |= DrawSssScalarRow("sss_feather", params.sssFeather, base.sssFeather, enabled, "%.3f",
                               "Default edge feathering for new subsurface blocks.");
    return edited;
}

template <typename Block>
bool DrawMaskBlockSelector(const char *kindTooltip, std::vector<Block> &blocks,
                           int32_t &selectedBlock, bool &outIsPoly)
{
    if (blocks.empty())
    {
        return false;
    }

    selectedBlock = std::clamp(selectedBlock, 0, int32_t(blocks.size()) - 1);
    outIsPoly = blocks[size_t(selectedBlock)].polyUv.size() >= 6;

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.66f, 0.72f, 1.0f));
    ImGui::TextWrapped("%s", outIsPoly
                                 ? "the mask below: drag a point, an edge to slide it; "
                                   "Ctrl+click adds a point; right-click removes; Ctrl+wheel zooms"
                                 : "the preview below: click a pixel to set the color; "
                                   "Ctrl+wheel zooms");
    ImGui::PopStyleColor();

    EditorGuiComboModel blkModel{
        int32_t(blocks.size()),
        [&blocks](int32_t idx) {
            static std::string buf;
            buf = FormatMaskBlockLabel(size_t(idx), blocks[size_t(idx)]);
            return buf.c_str();
        },
    };
    EditorGuiComboFiltered("colors & masks", &selectedBlock, blkModel,
                           { false, true, kindTooltip });
    return true;
}

bool DrawAddColorOrMaskButtons(bool canAdd, bool &outAddColor, bool &outAddMask)
{
    outAddColor = false;
    outAddMask = false;
    ImGui::BeginDisabled(!canAdd);

    if (ImGui::Button("Add color"))
    {
        outAddColor = true;
    }

    ImGui::SameLine();

    if (ImGui::Button("Add mask"))
    {
        outAddMask = true;
    }

    ImGui::EndDisabled();
    return outAddColor || outAddMask;
}

template <typename DrawColorFn, typename DrawControlsFn>
struct MaskBlockPanelUi
{
    const char *selectorTip;
    const char *actionId;
    const char *maskPickId;
    bool *hasColorFlag;
    DrawColorFn drawColor;
    DrawControlsFn drawControls;
};

template <typename Block, typename ClipboardState, typename DrawColorFn, typename DrawControlsFn>
bool DrawActiveMaskBlock(const MaskBlockPanelUi<DrawColorFn, DrawControlsFn> &ui,
                         std::vector<Block> &blocks, const std::vector<Block> &baseBlocks,
                         ClipboardState clipboard, const BlockPreviewTarget &preview,
                         int32_t &selectedBlock)
{
    bool isPoly = false;
    if (!DrawMaskBlockSelector(ui.selectorTip, blocks, selectedBlock, isPoly))
    {
        return false;
    }

    const size_t blkIndex = size_t(selectedBlock);
    Block &block = blocks[blkIndex];
    const Block *baseBlock = blkIndex < baseBlocks.size() ? &baseBlocks[blkIndex] : nullptr;

    ImGui::PushID(selectedBlock);
    bool removeThis = false;
    bool edited = DrawMaskBlockHeader(ui.actionId, blkIndex, block, clipboard.block,
                                      clipboard.valid, removeThis);

    if (removeThis)
    {
        blocks.erase(blocks.begin() + ptrdiff_t(blkIndex));
        selectedBlock = std::min(selectedBlock, int32_t(blocks.size()) - 1);
        if (blocks.empty() && ui.hasColorFlag != nullptr)
        {
            *ui.hasColorFlag = false;
        }
        edited = true;
    }
    else
    {
        edited |= isPoly ? DrawUvPolygonMaskPreview(ui.maskPickId, block.polyUv, preview.handle,
                                                    preview.tex)
                         : ui.drawColor(block, baseBlock, preview.handle, preview.tex);
        edited |= ui.drawControls(block, baseBlock);
    }

    ImGui::PopID();
    return edited;
}

bool DrawActiveLumaBlock(EditorMaterialParams &params, const EditorMaterialParams &base,
                         LumaClipboardState clipboard, const BlockPreviewTarget &preview,
                         int32_t &selectedBlock)
{
    const MaskBlockPanelUi ui{
        "The luma block the form below edits.",
        "##blkact",
        "##mask_pick",
        &params.hasColorEmissive,
        DrawLumaColorPreview,
        DrawLumaBlockToneControls,
    };
    return DrawActiveMaskBlock(ui, params.emissiveBlocks, base.emissiveBlocks, clipboard, preview,
                               selectedBlock);
}

bool DrawActiveGlossBlock(EditorMaterialParams &params, const EditorMaterialParams &base,
                          GlossClipboardState clipboard, const BlockPreviewTarget &preview,
                          int32_t &selectedBlock)
{
    const MaskBlockPanelUi ui{
        "The gloss block the form below edits.",
        "##glossact",
        "##gloss_mask_pick",
        &params.hasColorGloss,
        DrawGlossColorPreview,
        DrawGlossBlockShadingControls,
    };
    return DrawActiveMaskBlock(ui, params.glossBlocks, base.glossBlocks, clipboard, preview,
                               selectedBlock);
}

bool DrawActiveSssBlock(EditorMaterialParams &params, const EditorMaterialParams &base,
                        SssClipboardState clipboard, const BlockPreviewTarget &preview,
                        int32_t &selectedBlock)
{
    const MaskBlockPanelUi ui{
        "The subsurface block the form below edits.",
        "##sssact",
        "##sss_mask_pick",
        static_cast<bool *>(nullptr),
        DrawSssColorPreview,
        DrawSssBlockControls,
    };
    return DrawActiveMaskBlock(ui, params.sssBlocks, base.sssBlocks, clipboard, preview,
                               selectedBlock);
}

int32_t FindDefaultPreviewModelIndex(const std::vector<EditorAssetCatalogEntry> &catalog)
{
    for (size_t i = 0; i < catalog.size(); i++)
    {
        if (EqualsIgnoreCase(catalog[i].path, "progs/soldier.mdl"))
        {
            return int32_t(i);
        }
    }

    return catalog.empty() ? -1 : 0;
}

std::string LowercaseCopy(const std::string &text)
{
    std::string out = text;
    for (char &c : out)
    {
        c = char(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

struct MaskBlockContext
{
    EditorMaterialParams base;
    BlockPreviewTarget preview;
};

template <typename ResolveTexFn>
MaskBlockContext BuildMaskBlockContext(const EditorMaterialTable &baseline, const EditorScene &scene,
                                       const std::vector<QrMaterial> &textureMaterials,
                                       const std::string &name, const EditorMaterialParams &params,
                                       ResolveTexFn resolveTex)
{
    const int32_t baseIdx = baseline.IndexOf(name);
    EditorMaterialParams base =
        baseIdx >= 0 ? baseline.At(size_t(baseIdx)) : EditorMaterialParams{};
    int32_t texIdx = resolveTex(params.baseTexture);
    if (texIdx < 0)
    {
        texIdx = resolveTex(name);
    }
    return MaskBlockContext{ std::move(base),
                             MakeBlockPreviewTarget(scene, textureMaterials, texIdx) };
}

template <typename DrawFn, typename AppendFn>
bool RunMaskBlockPanel(const char *pushId, bool canAdd, DrawFn drawActive, AppendFn appendBlock)
{
    if (pushId != nullptr)
    {
        ImGui::PushID(pushId);
    }
    bool edited = drawActive();
    bool addColor = false;
    bool addMask = false;
    if (DrawAddColorOrMaskButtons(canAdd, addColor, addMask))
    {
        appendBlock(addMask);
        edited = true;
    }
    if (pushId != nullptr)
    {
        ImGui::PopID();
    }
    return edited;
}

} // namespace

void EditorApp::DrawMaterialEditorLumaBlocks(const std::string &name, EditorMaterialParams &params,
                                             int32_t materialIndex)
{
    const MaskBlockContext ctx = BuildMaskBlockContext(
        m_baselineMaterials, m_scene, m_textureMaterials, name, params,
        [this](const std::string &n) { return ResolveOrLoadTexture(n); });
    const bool edited = RunMaskBlockPanel(
        nullptr, params.emissiveBlocks.size() < kEditorEmissiveBlockMax,
        [&] {
            return DrawActiveLumaBlock(
                params, ctx.base, LumaClipboardState{ m_lumaClipboard, m_lumaClipboardValid },
                ctx.preview, m_selectedLumaBlock);
        },
        [&](bool addMask) { AppendLumaBlock(params, addMask, m_selectedLumaBlock); });

    if (edited)
    {
        MarkMaterialEdited(materialIndex);
    }
}

void EditorApp::DrawMaterialEditorGlossBlocks(const std::string &name, EditorMaterialParams &params,
                                              int32_t materialIndex)
{
    const MaskBlockContext ctx = BuildMaskBlockContext(
        m_baselineMaterials, m_scene, m_textureMaterials, name, params,
        [this](const std::string &n) { return ResolveOrLoadTexture(n); });
    const bool edited = RunMaskBlockPanel(
        "gloss_blocks", params.glossBlocks.size() < kEditorGlossBlockMax,
        [&] {
            return DrawActiveGlossBlock(
                params, ctx.base, GlossClipboardState{ m_glossClipboard, m_glossClipboardValid },
                ctx.preview, m_selectedGlossBlock);
        },
        [&](bool addMask) {
            AppendGlossBlock(params, addMask, m_selectedGlossBlock, ctx.preview.tex);
        });

    if (edited)
    {
        MarkMaterialEdited(materialIndex);
    }
}

void EditorApp::DrawMaterialEditorSssBlocks(const std::string &name, EditorMaterialParams &params,
                                            int32_t materialIndex)
{
    const MaskBlockContext ctx = BuildMaskBlockContext(
        m_baselineMaterials, m_scene, m_textureMaterials, name, params,
        [this](const std::string &n) { return ResolveOrLoadTexture(n); });
    const bool edited = RunMaskBlockPanel(
        "sss_blocks", params.sssBlocks.size() < kEditorSssBlockMax,
        [&] {
            return DrawActiveSssBlock(
                params, ctx.base, SssClipboardState{ m_sssClipboard, m_sssClipboardValid },
                ctx.preview, m_selectedSssBlock);
        },
        [&](bool addMask) {
            AppendSssBlock(params, addMask, m_selectedSssBlock, ctx.preview.tex);
        });

    if (edited)
    {
        MarkMaterialEdited(materialIndex);
    }
}

static bool DrawMaterialClipboardActions(const std::string &matName, EditorMaterialParams &params,
                                         const EditorMaterialParams &base,
                                         EditorMaterialParams &clipboard, bool &hasClipboard)
{
    ImGui::TextDisabled("Material: %s", matName.c_str());
    ImGui::SameLine(
        std::max(180.0f, ImGui::GetWindowContentRegionMax().x - 108.0f * EditorGuiScale()));
    const uint32_t actions = EditorGuiBlockActions("##matcp", true, hasClipboard, params != base);

    if ((actions & kEditorGuiActionCopy) != 0)
    {
        clipboard = params;
        hasClipboard = true;
    }

    if ((actions & kEditorGuiActionPaste) != 0 && hasClipboard)
    {
        params = clipboard;
        return true;
    }

    if ((actions & kEditorGuiActionRemove) != 0)
    {
        params = base;
        return true;
    }

    return false;
}

static const EditorTexture *GetSceneTexturePtr(const EditorScene &scene, int32_t texIdx)
{
    if (texIdx < 0 || size_t(texIdx) >= scene.textures.size())
    {
        return nullptr;
    }
    return &scene.textures[size_t(texIdx)];
}

static bool UsesProceduralLuma(const EditorMaterialParams &params)
{
    return params.emissiveTexture.empty() && params.hasColorEmissive;
}

static bool UsesProceduralGloss(const EditorMaterialParams &params)
{
    return params.glossTexture.empty() && params.hasColorGloss;
}

void EditorApp::DrawMaterialEditorBody(int32_t materialIndex, bool showFaceHeader)
{
    if (materialIndex < 0 || size_t(materialIndex) >= m_materials.Count())
    {
        return;
    }

    if (materialIndex != m_lastInspectedMaterial)
    {
        m_lastInspectedMaterial = materialIndex;
        m_selectedLumaBlock = 0;
        m_selectedGlossBlock = 0;
        m_selectedSssBlock = 0;
    }

    const std::string &matName = m_materials.NameAt(size_t(materialIndex));
    EditorMaterialParams &params = m_materials.At(size_t(materialIndex));
    const int32_t baseIdx = m_baselineMaterials.IndexOf(matName);
    const EditorMaterialParams base =
        baseIdx >= 0 ? m_baselineMaterials.At(size_t(baseIdx)) : EditorMaterialParams{};
    EditorMaterialParams baseModParams;
    const bool hasBaseModEntry = m_baseMaterialsDocument.ReadMaterial(matName, baseModParams);
    const bool inheritedFromBaseMod = IsMaterialFromBaseMod(matName);

    DrawMaterialTextureInfoHeader(m_scene, params, matName, showFaceHeader ? m_selectedSurface : -1,
                                  [this](const std::string &n) { return TextureIndexByName(n); });

    if (inheritedFromBaseMod)
    {
        ImGui::TextDisabled(
            "Inherited from base mod (id1) -- edit any field to override in this mod.");
    }

    bool edited = false;
    edited |= DrawMaterialClipboardActions(matName, params, base, m_materialClipboard,
                                           m_hasMaterialClipboard);
    edited |= DrawMaterialTexturesSection(
        [this, &matName](EditorTextureSlot slot) { OpenTextureBrowserForSlot(matName, slot); },
        params, base, hasBaseModEntry ? &baseModParams : nullptr, inheritedFromBaseMod);
    edited |= DrawMaterialSurfaceSection(params, base);
    edited |= DrawMaterialLightSection(params, base);
    edited |= DrawMaterialLumaToggleSection(params, base, m_selectedLumaBlock);

    if (edited)
    {
        MarkMaterialEdited(materialIndex);
    }

    if (UsesProceduralLuma(params))
    {
        DrawMaterialEditorLumaBlocks(matName, params, materialIndex);
    }

    int32_t glossTexIdx = ResolveOrLoadTexture(params.baseTexture);
    if (glossTexIdx < 0)
    {
        glossTexIdx = ResolveOrLoadTexture(matName);
    }
    const EditorTexture *glossPreviewTex = GetSceneTexturePtr(m_scene, glossTexIdx);

    if (DrawMaterialGlossSection(params, base, m_selectedGlossBlock, glossPreviewTex))
    {
        MarkMaterialEdited(materialIndex);
    }

    if (UsesProceduralGloss(params))
    {
        DrawMaterialEditorGlossBlocks(matName, params, materialIndex);
    }

    if (DrawMaterialSssSection(params, base))
    {
        MarkMaterialEdited(materialIndex);
    }

    if (!IsSssLockedByMaterial(params))
    {
        DrawMaterialEditorSssBlocks(matName, params, materialIndex);
    }
}

void EditorApp::DrawMaterialEditorMaterialsTab()
{
    if (m_materials.Count() == 0)
    {
        ImGui::TextDisabled("No scene materials loaded.");
        return;
    }

    if (m_selectedMaterial < 0 || size_t(m_selectedMaterial) >= m_materials.Count())
    {
        m_selectedMaterial = 0;
    }

    EditorGuiComboModel matModel{
        int32_t(m_materials.Count()),
        [this](int32_t idx) {
            static std::string s_label;
            s_label = FormatMaterialListLabel(m_materials.NameAt(size_t(idx)));
            return s_label.c_str();
        },
    };
    EditorGuiComboFiltered("Material", &m_selectedMaterial, matModel,
                           { false, true, "Pick material to inspect or edit." });

    if (DrawMaterialAnimFrameRow(m_scene, m_materials, m_selectedMaterial))
    {
        m_selectedLumaBlock = 0;
        m_selectedGlossBlock = 0;
        m_selectedSssBlock = 0;
        m_lastInspectedMaterial = m_selectedMaterial;
        m_materialsChanged = true;
    }

    DrawMaterialEditorBody(m_selectedMaterial, true);
}

namespace
{

std::string FormatModelEntryComboLabel(const std::string &path, bool inActiveMod, bool inBaseMod)
{
    std::string label = path;
    if (inActiveMod)
    {
        label += "  *";
    }
    else if (inBaseMod)
    {
        label += "  [base]";
    }
    return label;
}

std::string FormatModelSkinComboLabel(const std::string &formattedSkinMaterialName)
{
    const size_t colon = formattedSkinMaterialName.rfind(":frame");
    return colon != std::string::npos ? formattedSkinMaterialName.substr(colon + 1)
                                      : formattedSkinMaterialName;
}

void DrawPreviewModelPoseControl(const EditorPreviewModel &model, int32_t &frameIndex)
{
    if (model.frames.size() <= 1)
    {
        return;
    }

    const size_t fIdx =
        std::clamp<size_t>(size_t(std::max(0, frameIndex)), 0, model.frames.size() - 1);
    const std::string &fName = model.frames[fIdx].name;
    char poseFmt[64] = "%d";
    if (!fName.empty())
    {
        std::snprintf(poseFmt, sizeof(poseFmt), "%%d (%s)", fName.c_str());
    }
    EditorGuiSliderInt("Pose", &frameIndex, 0, int32_t(model.frames.size()) - 1,
                       { false, true, "Preview animation frame index.", poseFmt });
}

} // namespace

void EditorApp::DrawMaterialEditorModelsTab()
{
    if (m_modelCatalog.empty())
    {
        ImGui::TextDisabled("No .mdl models found in mounted game assets.");
        return;
    }

    if (!m_previewModelLoaded)
    {
        const int32_t initialIdx =
            (m_selectedModelCatalog >= 0 &&
             size_t(m_selectedModelCatalog) < m_modelCatalog.size())
                ? m_selectedModelCatalog
                : FindDefaultPreviewModelIndex(m_modelCatalog);
        if (initialIdx >= 0)
        {
            SelectPreviewModel(initialIdx);
        }
    }

    int32_t chosenModel = m_selectedModelCatalog;
    EditorGuiComboModel mdlModel{
        int32_t(m_modelCatalog.size()),
        [this](int32_t idx) {
            static std::string s_label;
            const std::string &path = m_modelCatalog[size_t(idx)].path;
            const std::string skin0 = LowercaseCopy(path) + ":frame0";
            s_label = FormatModelEntryComboLabel(path, IsMaterialInActiveMod(skin0),
                                                 IsMaterialFromBaseMod(skin0));
            return s_label.c_str();
        },
    };
    if (EditorGuiComboFiltered("Model", &chosenModel, mdlModel,
                               { false, true, "Pick .mdl model to preview and edit." }) &&
        chosenModel != m_selectedModelCatalog)
    {
        SelectPreviewModel(chosenModel);
    }

    if (!m_previewModelLoaded)
    {
        return;
    }

    if (m_previewModel.skins.size() > 1)
    {
        int32_t chosenSkin = m_previewModelSkin;
        EditorGuiComboModel skinModel{
            int32_t(m_previewModel.skins.size()),
            [this](int32_t idx) {
                static std::string s_skinLabel;
                const std::string &skinName = m_previewModel.skins[size_t(idx)].name;
                s_skinLabel = FormatModelSkinComboLabel(FormatMaterialListLabel(skinName));
                return s_skinLabel.c_str();
            },
        };
        if (EditorGuiComboFiltered("Skin", &chosenSkin, skinModel,
                                   { false, true, "Select which MDL skin slot to edit." }) &&
            chosenSkin != m_previewModelSkin)
        {
            SelectPreviewModelSkin(chosenSkin);
        }
    }

    DrawPreviewModelPoseControl(m_previewModel, m_previewModelFrame);

    EditorGuiCheckbox("Preview light", &m_previewLight,
                      { false, true, "Illuminate the previewed model with a front key light." });
    EditorGuiCheckbox("Auto rotate", &m_previewAutoRotate,
                      { false, true, "Slowly spin the previewed model on the turntable." });
    if (ImGui::Button("Reset view"))
    {
        ResetPreviewView();
    }

    ImGui::Separator();

    if (m_previewMaterialIndex >= 0 && size_t(m_previewMaterialIndex) < m_materials.Count())
    {
        DrawMaterialEditorBody(m_previewMaterialIndex, false);
    }
}

void EditorApp::DrawMaterialEditorEntitiesTab()
{
    if (m_selectedMaterial >= 0 && size_t(m_selectedMaterial) < m_materials.Count())
    {
        const std::string &matName = m_materials.NameAt(size_t(m_selectedMaterial));
        const EditorMaterialParams &params = m_materials.At(size_t(m_selectedMaterial));
        DrawMaterialTextureInfoHeader(m_scene, params, matName, m_selectedSurface,
                                      [this](const std::string &n) { return TextureIndexByName(n); });
    }

    EditorGuiSectionHeader("Scene Summary");
    ImGui::Text("Vertices: %zu  Triangles: %zu", m_scene.vertices.size(), m_scene.indices.size() / 3);
    ImGui::Text("Surfaces: %zu  Materials: %zu  Lights: %zu", m_scene.surfaces.size(),
                m_materials.Count(), m_scene.lights.size());

    EditorGuiSectionHeader("Selected Surface");
    if (m_selectedSurface >= 0 && size_t(m_selectedSurface) < m_scene.surfaces.size())
    {
        const EditorSurface &surf = m_scene.surfaces[size_t(m_selectedSurface)];
        ImGui::Text("Surface #%d  Face #%d", m_selectedSurface, m_selectedFace);
        ImGui::Text("Texture: %s", surf.textureName.c_str());
        ImGui::Text("Faces: %zu  Triangles: %zu", surf.faces.size(), surf.indices.size() / 3);
        if (ImGui::Button("Edit material"))
        {
            const int32_t matIdx = m_materials.IndexOf(surf.textureName);
            if (matIdx >= 0)
            {
                m_selectedMaterial = matIdx;
                m_materialTab = 0;
            }
        }
    }
    else
    {
        ImGui::TextDisabled("Nothing selected. Press Tab for fly mode and LMB a face.");
    }

    EditorGuiSectionHeader("Emissive Tools");
    if (ImGui::Button("Regenerate from map"))
    {
        m_regenPrompt = 1;
    }
}

void EditorApp::DrawMaterialEditorSystemTab()
{
    DrawSystemDtalSection(m_settings, m_baselineSettings);
    DrawSystemRenderSection(m_settings, m_baselineSettings);
}

void EditorApp::ResetActiveMaterialTab()
{
    if (m_materialTab == 0 && m_selectedMaterial >= 0)
    {
        const std::string &name = m_materials.NameAt(size_t(m_selectedMaterial));
        const int32_t b = m_baselineMaterials.IndexOf(name);
        m_materials.At(size_t(m_selectedMaterial)) =
            b >= 0 ? m_baselineMaterials.At(size_t(b)) : EditorMaterialParams{};
        MarkMaterialEdited(m_selectedMaterial);
    }
    else if (m_materialTab == 1 && m_previewMaterialIndex >= 0)
    {
        const std::string &name = m_materials.NameAt(size_t(m_previewMaterialIndex));
        const int32_t b = m_baselineMaterials.IndexOf(name);
        m_materials.At(size_t(m_previewMaterialIndex)) =
            b >= 0 ? m_baselineMaterials.At(size_t(b)) : EditorMaterialParams{};
        MarkMaterialEdited(m_previewMaterialIndex);
    }
    else if (m_materialTab == 3)
    {
        m_settings = m_baselineSettings;
    }
}

void EditorApp::DrawMaterialEditorPanel()
{
    if (!EditorGuiBeginPanel("MATERIAL EDITOR", float(m_clientWidth), float(m_clientHeight)))
    {
        EditorGuiEndPanel();
        return;
    }
    m_panelVisible = true;
    m_panelLeft = ImGui::GetWindowPos().x;

    DrawPanelActionBar(false);
    static constexpr const char *kTabs[] = { "Materials", "Models", "Entities", "System" };
    bool resetClicked = false;
    EditorGuiTabs(kTabs, 4, &m_materialTab, true, &resetClicked,
                  "Reset current tab parameters to baseline");
    if (m_materialTab != 1)
    {
        m_previewModelDragging = false;
    }
    if (resetClicked)
    {
        ResetActiveMaterialTab();
    }

    EditorGuiBeginScroll("##MaterialScroll");
    if (m_materialTab == 0)
    {
        DrawMaterialEditorMaterialsTab();
    }
    else if (m_materialTab == 1)
    {
        DrawMaterialEditorModelsTab();
    }
    else if (m_materialTab == 2)
    {
        DrawMaterialEditorEntitiesTab();
    }
    else
    {
        DrawMaterialEditorSystemTab();
    }
    EditorGuiEndScroll();
    EditorGuiEndPanel();
}

namespace
{
void DrawEntityLightSummary(const std::vector<EditorLight> &lights, int32_t selectedLight,
                            bool hasEntityLights, bool ownOverride)
{
    if (hasEntityLights && selectedLight >= 0 && size_t(selectedLight) < lights.size())
    {
        const EditorLight &selected = lights[size_t(selectedLight)];
        ImGui::TextDisabled("light: %s%s",
                            selected.className.empty() ? "(no emitter name)"
                                                       : selected.className.c_str(),
                            ownOverride ? "  (own)" : "");
        ImGui::TextDisabled("at %.0f %.0f %.0f   radius %.1f", double(selected.position[0]),
                            double(selected.position[1]), double(selected.position[2]),
                            double(selected.radius));
        const char *kindLabel = "a map light entity";
        if (selected.emitterUniqueID != 0)
        {
            kindLabel = selected.modelSizeMeters > 0.0f ? "casts a dlight (light_color)"
                                                       : "emissive proxy light";
        }
        ImGui::TextDisabled("%s", kindLabel);
    }
    else
    {
        ImGui::TextDisabled("aim at a light wireframe and press the fire button");
    }
    ImGui::Spacing();
    const int32_t liveCount = hasEntityLights ? int32_t(lights.size()) : 0;
    ImGui::TextDisabled("%d generated light%s in the frame", liveCount, liveCount == 1 ? "" : "s");
    ImGui::Spacing();
}

bool DrawEntityLightCombo(const std::vector<EditorLight> &lights, int32_t &selectedLight)
{
    EditorGuiComboModel model{
        int32_t(lights.size()),
        [&lights](int32_t idx) {
            static char buf[96] = {};
            const EditorLight &l = lights[size_t(idx)];
            std::snprintf(buf, sizeof(buf), "#%d %s%s (%.0f %.0f %.0f)", idx, l.className.c_str(),
                          l.enabled ? "" : " [off]", double(l.position[0]), double(l.position[1]),
                          double(l.position[2]));
            return buf;
        },
    };
    const bool picked = EditorGuiComboFiltered(
        "light", &selectedLight, model,
        { false, true, "The lights this frame generated; pick one and the form below edits it." });
    ImGui::Spacing();
    return picked;
}

void DrawEntityLightFields(EditorLight &light, const EditorLight *base, float camYaw, float camPitch)
{
    DrawLightCommonFields(light, base, false);
    DrawSpotLightFields(light, base, false, camYaw, camPitch);

    bool frReset = false;
    EditorGuiCheckbox("force_rasterize", &light.forceRasterize,
                      { base && light.forceRasterize != base->forceRasterize, true, nullptr, nullptr,
                        base ? &frReset : nullptr });
    if (base != nullptr)
    {
        ResetIfClicked(light.forceRasterize, base->forceRasterize, &frReset);
    }
}

void DrawEmitterRadiusField(const EditorLight &light, EditorEmitterLightOverride &ov,
                            const EditorEmitterLightOverride *baseOv, bool &changed)
{
    const bool modified = baseOv != nullptr && ov.hasRadius != baseOv->hasRadius;
    bool radReset = false;
    if (EditorGuiSliderFloat("light_radius", &ov.radius, 0.0f, 10.0f,
                             { modified, true,
                               "Size fraction of the model's size, or absolute meters for a proxy.",
                               "%.2f", baseOv != nullptr ? &radReset : nullptr }))
    {
        ov.hasRadius = true;
        changed = true;
    }
    if (baseOv != nullptr && radReset)
    {
        ov.hasRadius = baseOv->hasRadius;
        ov.radius = baseOv->hasRadius ? baseOv->radius : ov.radius;
        changed = true;
    }

    if (light.modelSizeMeters > 0.0f)
    {
        ImGui::TextDisabled("%.2f x the model's own size", double(ov.radius));
    }
    else
    {
        ImGui::TextDisabled("radius %.1f game units", double(light.radius));
    }
}

void DrawEmitterIntensityField(EditorEmitterLightOverride &ov,
                               const EditorEmitterLightOverride *baseOv, bool &changed)
{
    const float baseInt = (baseOv != nullptr && baseOv->hasIntensity) ? baseOv->intensity : 1.0f;
    const bool modified = baseOv != nullptr && !AlmostEq(ov.intensity, baseInt);
    bool intReset = false;
    if (EditorGuiSliderFloat("light_intensity", &ov.intensity, 0.0f, 100.0f,
                             { modified, true, "Brightness multiplier of the emitter light.",
                               "%.2f", baseOv != nullptr ? &intReset : nullptr }))
    {
        ov.hasIntensity = true;
        changed = true;
    }
    if (baseOv != nullptr && intReset)
    {
        ov.hasIntensity = baseOv->hasIntensity;
        ov.intensity = baseInt;
        changed = true;
    }
}

void DrawEmitterOffsetField(EditorEmitterLightOverride &ov,
                            const EditorEmitterLightOverride *baseOv, bool &changed)
{
    const bool modified =
        baseOv != nullptr && (ov.hasOffset != baseOv->hasOffset || !Vec3Eq(ov.offset, baseOv->offset));
    bool offReset = false;
    if (EditorGuiVec3Input("light_offset", ov.offset, 1.0f,
                           { modified, true, "Offset from the emitter's pivot in Quake units.",
                             nullptr, baseOv != nullptr ? &offReset : nullptr }))
    {
        ov.hasOffset = true;
        changed = true;
    }
    if (baseOv != nullptr && offReset)
    {
        ov.hasOffset = baseOv->hasOffset;
        std::copy_n(baseOv->offset, 3, ov.offset);
        changed = true;
    }
}

void DrawEmitterColorField(EditorEmitterLightOverride &ov,
                           const EditorEmitterLightOverride *baseOv, bool &changed)
{
    const bool modified =
        baseOv != nullptr && (ov.hasColor != baseOv->hasColor || !ColorEq(ov.color, baseOv->color));
    bool clrReset = false;
    if (EditorGuiColorHex("light_color", ov.color,
                          { modified, true, "Explicit color for the emitter light.", nullptr,
                            baseOv != nullptr ? &clrReset : nullptr }))
    {
        ov.hasColor = true;
        changed = true;
    }
    if (baseOv != nullptr && clrReset)
    {
        ov.hasColor = baseOv->hasColor;
        std::copy_n(baseOv->color, 3, ov.color);
        changed = true;
    }
}

void DrawEmitterStyleField(EditorEmitterLightOverride &ov,
                           const EditorEmitterLightOverride *baseOv, bool &changed)
{
    const char *styleItems[kEditorStyleCount + 1] = {};
    styleItems[0] = "NONE";
    for (int32_t i = 0; i < kEditorStyleCount; i++)
    {
        styleItems[i + 1] = kEditorStyleNames[i];
    }
    int32_t styleIdx = ov.hasStyle ? std::clamp(ov.style, 0, kEditorStyleCount - 1) + 1 : 0;
    const int32_t baseStyleIdx = (baseOv != nullptr && baseOv->hasStyle)
                                     ? std::clamp(baseOv->style, 0, kEditorStyleCount - 1) + 1
                                     : 0;
    bool styReset = false;
    if (EditorGuiCombo("light_style", &styleIdx, styleItems, kEditorStyleCount + 1,
                       { baseOv != nullptr && styleIdx != baseStyleIdx, true, nullptr, nullptr,
                         baseOv != nullptr ? &styReset : nullptr }))
    {
        ov.hasStyle = (styleIdx > 0);
        ov.style = (styleIdx > 0) ? (styleIdx - 1) : 0;
        changed = true;
    }
    if (baseOv != nullptr && styReset)
    {
        ov.hasStyle = baseOv->hasStyle;
        ov.style = baseOv->style;
        changed = true;
    }
}

const EditorEmitterLightOverride *FindBaselineOverrideForKey(
    const std::vector<EditorEmitterLightOverride> &baseline, const std::string &key)
{
    for (const EditorEmitterLightOverride &candidate : baseline)
    {
        if (EqualsIgnoreCase(candidate.name, key.c_str()))
        {
            return &candidate;
        }
    }
    return nullptr;
}

void DrawEmitterSpotAndFlags(EditorEmitterLightOverride &ov, const EditorEmitterLightOverride *baseOv,
                             float camYaw, float camPitch, bool &changed)
{
    bool spReset = false;
    if (EditorGuiCheckbox("spot", &ov.spot,
                          { baseOv && ov.spot != baseOv->spot, true,
                            "Convert the emitter to a directional cone spotlight.", nullptr,
                            baseOv ? &spReset : nullptr }))
    {
        ov.hasSpot = true;
        changed = true;
    }
    if (baseOv != nullptr && spReset)
    {
        ov.hasSpot = baseOv->hasSpot;
        ov.spot = baseOv->spot;
        changed = true;
    }

    if (ov.spot)
    {
        if (!ov.groupEdit)
        {
            if (EditorGuiVec3Input("dir", ov.dir, 0.05f, { false, true, nullptr, nullptr }))
            {
                NormalizeVec3(ov.dir);
                ov.hasDir = true;
                changed = true;
            }
            if (ImGui::Button("Aim along camera view"))
            {
                const float cp = std::cos(camPitch);
                ov.dir[0] = cp * std::cos(camYaw);
                ov.dir[1] = cp * std::sin(camYaw);
                ov.dir[2] = std::sin(camPitch);
                ov.hasDir = true;
                changed = true;
            }
        }
        else
        {
            ImGui::TextDisabled("direction: geometry faces it; group sets angles only");
        }

        if (EditorGuiSliderFloat("angle_inner", &ov.angleInner, 0.0f, 90.0f,
                                 { false, true, nullptr, "%.1f" }))
        {
            ov.angleOuter = std::max(ov.angleOuter, ov.angleInner);
            ov.hasAngles = true;
            changed = true;
        }
        if (EditorGuiSliderFloat("angle_outer", &ov.angleOuter, 0.0f, 90.0f,
                                 { false, true, nullptr, "%.1f" }))
        {
            ov.angleInner = std::min(ov.angleInner, ov.angleOuter);
            ov.hasAngles = true;
            changed = true;
        }
    }

    if (EditorGuiCheckbox("force_rasterize", &ov.forceRasterize,
                          { baseOv && ov.forceRasterize != baseOv->forceRasterize, true,
                            "Draw the emitter in the rasterized path.", nullptr }))
    {
        changed = true;
    }
}

void DrawEmitterEnabledField(EditorEmitterLightOverride &ov,
                             const EditorEmitterLightOverride *baseOv, bool &changed)
{
    const bool baseEnabled = baseOv != nullptr ? baseOv->enabled : true;
    const bool modified = baseOv != nullptr && ov.enabled != baseEnabled;
    bool enReset = false;
    if (EditorGuiCheckbox("enabled", &ov.enabled,
                          { modified, true, "Include this generated light in rendering.", nullptr,
                            baseOv != nullptr ? &enReset : nullptr }))
    {
        changed = true;
    }
    if (baseOv != nullptr && ResetIfClicked(ov.enabled, baseEnabled, &enReset))
    {
        changed = true;
    }
}
} // namespace

void EditorApp::DrawEmitterControlsForSelected()
{
    const EditorLight &light = m_scene.lights[size_t(m_selectedLight)];
    EditorEmitterLightOverride ov = EffectiveEmitterOverride(m_selectedLight);
    bool groupEdit = ov.groupEdit;
    if (EditorGuiCheckbox("group_edit", &groupEdit,
                          { false, true,
                            "Edit all lights sharing this emitter model at once; uncheck for "
                            "instance-only edits." }))
    {
        SetEntityLightGroupEdit(m_selectedLight, groupEdit);
        return;
    }

    const std::string baselineKey =
        groupEdit ? ov.name : (ov.name + "#" + std::to_string(light.emitterUniqueID));
    const EditorEmitterLightOverride *baseOv =
        FindBaselineOverrideForKey(m_baselineEmitterOverrides, baselineKey);

    bool changed = false;
    DrawEmitterEnabledField(ov, baseOv, changed);
    DrawEmitterRadiusField(light, ov, baseOv, changed);
    DrawEmitterIntensityField(ov, baseOv, changed);
    DrawEmitterOffsetField(ov, baseOv, changed);
    DrawEmitterColorField(ov, baseOv, changed);
    DrawEmitterStyleField(ov, baseOv, changed);
    DrawEmitterSpotAndFlags(ov, baseOv, m_cameraYaw, m_cameraPitch, changed);
    if (changed)
    {
        ApplyEntityEmitterEdit(m_selectedLight, ov);
    }
}

void EditorApp::DrawLightEditorEntityTab()
{
    const bool hasEntityLights = !m_scene.lights.empty();
    if (!hasEntityLights)
    {
        m_selectedLight = -1;
        m_hoverLight = -1;
    }

    const bool ownOverride = hasEntityLights && HasOwnEmitterInstanceOverride(m_selectedLight);
    DrawEntityLightSummary(m_scene.lights, m_selectedLight, hasEntityLights, ownOverride);

    if (hasEntityLights && DrawEntityLightCombo(m_scene.lights, m_selectedLight))
    {
        SelectLight(m_selectedLight, false);
    }

    if (m_selectedLight < 0 || size_t(m_selectedLight) >= m_scene.lights.size())
    {
        ImGui::TextDisabled("%s", m_selectedCustomLight >= 0
                                      ? "a custom light is selected (edit it in the Custom tab)"
                                      : "nothing selected");
        return;
    }

    EditorLight &light = m_scene.lights[size_t(m_selectedLight)];
    const EditorLight *base = size_t(m_selectedLight) < m_baselineMapLights.size()
                                  ? &m_baselineMapLights[size_t(m_selectedLight)]
                                  : nullptr;

    if (ImGui::Button("Teleport"))
    {
        m_cameraPosition[0] = light.position[0] - 64.0f;
        m_cameraPosition[1] = light.position[1] - 64.0f;
        m_cameraPosition[2] = light.position[2] + 32.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clone in world"))
    {
        m_customCloning = m_selectedLight;
        m_customPlacing = false;
        m_placingSun = false;
        SetCursorMode(false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clone here"))
    {
        AddCustomLight(&light, LightSpawn::Duplicate);
        m_lightTab = 1;
    }

    if (light.emitterUniqueID == 0)
    {
        DrawEntityLightFields(light, base, m_cameraYaw, m_cameraPitch);
        return;
    }

    DrawEmitterControlsForSelected();
}

void EditorApp::DrawLightEditorCustomTab()
{
    if (ImGui::Button("Add light"))
    {
        m_customPlacing = true;
        m_customCloning = -1;
        m_placingSun = false;
        SetCursorMode(false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Add in front"))
    {
        AddCustomLight(nullptr, LightSpawn::Camera);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(m_selectedCustomLight < 0);
    if (ImGui::Button("Duplicate"))
    {
        DuplicateSelectedLight();
    }
    ImGui::SameLine();
    if (ImGui::Button("Remove"))
    {
        RemoveSelectedCustomLight();
    }
    ImGui::EndDisabled();

    if (m_customLights.empty())
    {
        ImGui::TextDisabled("No custom lights authored. Click 'Add light' to place one.");
        return;
    }

    if (m_selectedCustomLight < 0 || size_t(m_selectedCustomLight) >= m_customLights.size())
    {
        m_selectedCustomLight = 0;
    }

    EditorGuiComboModel model{
        int32_t(m_customLights.size()),
        [this](int32_t idx) {
            static char buf[96] = {};
            const EditorLight &l = m_customLights[size_t(idx)];
            std::snprintf(buf, sizeof(buf), "custom #%d%s (%.0f %.0f %.0f)", idx,
                          l.enabled ? "" : " [off]", double(l.position[0]), double(l.position[1]),
                          double(l.position[2]));
            return buf;
        },
    };
    if (EditorGuiComboFiltered("Custom Light", &m_selectedCustomLight, model))
    {
        SelectLight(m_selectedCustomLight, true);
    }

    EditorLight &light = m_customLights[size_t(m_selectedCustomLight)];
    const EditorLight *base = size_t(m_selectedCustomLight) < m_baselineCustomLights.size()
                                  ? &m_baselineCustomLights[size_t(m_selectedCustomLight)]
                                  : nullptr;

    if (DrawLightCommonFields(light, base, true))
    {
        light.cluster = ResolveEditorLightCluster(m_scene, light.position);
    }
    DrawSpotLightFields(light, base, true, m_cameraYaw, m_cameraPitch);
}

void EditorApp::DrawLightEditorGlobalTab()
{
    bool levelScope = !m_skyGlobal;
    if (EditorGuiCheckbox("level sky overrides", &levelScope,
                          { false, true, "Write sky keys into this level's qray.lights.yaml block." }))
    {
        SetSkyScope(!levelScope);
    }

    bool placeSun = false;
    DrawGlobalLightingAndSky(*this, m_settings, m_baselineSettings, m_sun, m_baselineSun, placeSun);
    if (placeSun)
    {
        m_placingSunBackup = m_sun;
        m_sun.enabled = true;
        if (m_sun.radiance <= 0.0f)
        {
            m_sun.radiance = kEditorSkySunRadiance;
        }
        m_placingSun = true;
        m_customPlacing = false;
        m_customCloning = -1;
        SetCursorMode(false);
    }
    DrawGlobalAtmosphereSection(m_settings, m_baselineSettings);

    ImGui::Separator();
    if (ImGui::Button("Regenerate from map"))
    {
        m_regenPrompt = 1;
        m_loggedRegenLayout = 0;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Mark the level's light models as light materials and write their "
                          "entries to the materials file (created if missing).");
    }
}

void EditorApp::DrawLightEditorPanel()
{
    if (!EditorGuiBeginPanel("LIGHT EDITOR", float(m_clientWidth), float(m_clientHeight)))
    {
        EditorGuiEndPanel();
        return;
    }
    m_panelVisible = true;
    m_panelLeft = ImGui::GetWindowPos().x;

    DrawPanelActionBar(true);
    static constexpr const char *kTabs[] = { "Entity", "Custom", "Global" };
    bool resetClicked = false;
    EditorGuiTabs(kTabs, 3, &m_lightTab, true, &resetClicked,
                  "Reset current light / global parameters to baseline");
    if (resetClicked)
    {
        if (m_lightTab == 0 && m_selectedLight >= 0)
        {
            ResetEntityEmitterLight(m_selectedLight);
        }
        else if (m_lightTab == 1 && m_selectedCustomLight >= 0 &&
                 size_t(m_selectedCustomLight) < m_baselineCustomLights.size())
        {
            m_customLights[size_t(m_selectedCustomLight)] =
                m_baselineCustomLights[size_t(m_selectedCustomLight)];
        }
        else if (m_lightTab == 2)
        {
            m_settings = m_baselineSettings;
            m_sun = m_baselineSun;
        }
    }

    EditorGuiBeginScroll("##LightScroll");
    if (m_lightTab == 0)
    {
        DrawLightEditorEntityTab();
    }
    else if (m_lightTab == 1)
    {
        DrawLightEditorCustomTab();
    }
    else
    {
        DrawLightEditorGlobalTab();
    }
    EditorGuiEndScroll();
    EditorGuiEndPanel();
}
