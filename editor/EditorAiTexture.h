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

#pragma once

#include "EditorAiWorkerProtocol.h"
#include "EditorMaterial.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

enum class EditorAiBackend
{
    LotusNormalD = 0,    // lotus-normal-d-v1-1-fp16.gguf
    LotusDepthD = 1,     // lotus-depth-d-v2-0-disparity-fp16.gguf
    DeepBump = 2,        // deepbump256-fp16.gguf
    StableDelight = 3,   // yoso-delight-v0-4-base-fp16.gguf
    RealPlksrPbrify = 4, // 4x-pbrify-rplksrd-v3-fp16.gguf
};

struct EditorAiLocalConfig
{
    EditorAiBackend activeBackend = EditorAiBackend::DeepBump;
    uint32_t processingRes = 768; // Fit-by-longest-side before Lotus GGUF (>= 512)
    std::string modelsDir;

    bool operator==(const EditorAiLocalConfig &) const = default;
};

struct EditorAiNormalParams
{
    float strength = 1.0f;
    float microDetail = 0.0f;
    bool invertHeight = false;
    bool invertY = false;
    bool seamless = true;
};

struct EditorAiToneParams
{
    float contrastGain = 1.0f;
    float biasThreshold = 0.0f;
};

struct EditorAiDelightParams
{
    float strength = 1.0f;       // 0..1: blend from original diffuse to de-lit diffuse
    float detailPreserve = 0.0f; // 0..1: high-frequency detail transfer from original diffuse
    bool seamless = true;
};

struct EditorAiUpscaleInput
{
    const std::vector<uint8_t> &origDiffuseRgba;
    uint32_t origW = 0;
    uint32_t origH = 0;
    const std::vector<uint8_t> &rawUpscaledRgba;
    uint32_t rawW = 0;
    uint32_t rawH = 0;
};

struct EditorAiLotusDepthDims
{
    uint32_t fitW = 0;
    uint32_t fitH = 0;
    uint32_t haloX = 0;
    uint32_t haloY = 0;
};

struct EditorAiUpscaleParams
{
    int32_t outputScale = 4; // 4 (native 4x) or 2 (2x box-downsampled)
    float strength = 1.0f;   // 0..1: blend between bilinear original and AI upscaled output
    float sharpen = 0.0f;    // 0..1: mild unsharp detail boost
    bool seamless = true;
};

struct EditorAiDiffusePrepParams
{
    float flattenLighting = 0.0f; // 0..1: flattens baked shadows/highlights in AI input only
    float preSmooth = 0.0f;       // 0..1: mild pre-smoothing for pixelated diffuse inputs
    bool seamless = true;
};

struct EditorAiLotusPrep
{
    uint32_t fitW = 0;
    uint32_t fitH = 0;
    uint32_t halo = 0;
    uint32_t haloX = 0;
    uint32_t haloY = 0;
    bool mirrorPad = false;
    EditorAiTensorNchw tensor;
};

using EditorAiProgressCallback = std::function<void(int32_t percent, const std::string &stage)>;

struct EditorAiInferenceRequest
{
    EditorAiBackend backend = EditorAiBackend::DeepBump;
    EditorAiMapKind mapKind = EditorAiMapKind::Normals;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t processingRes = 768;
    bool seamless = true;
    float flattenLighting = 0.0f;
    float preSmooth = 0.0f;
    std::string modelsDir;
    std::string exeDir;
    std::vector<uint8_t> diffuseRgba;
    EditorAiProgressCallback progressCb;
};

struct EditorAiInferenceResult
{
    bool ok = false;
    bool cancelled = false;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    std::string providerLabel;
    std::string errorMessage;
    std::vector<uint8_t> rawNormalRgba;
    std::vector<float> rawHeight;
    std::vector<uint8_t> rawColorRgba;
};

// User-scoped local config path (~/.config/quakeray/ai_local.json on Linux,
// %APPDATA%/QuakeRay/ai_local.json on Windows). Never inside the repo or game folder.
std::filesystem::path DefaultEditorAiLocalConfigPath();
std::filesystem::path DefaultEditorAiModelsDir();

// Returns identifiers, display labels, GGUF filenames, and disk lookup for AI backends.
const char *EditorAiBackendId(EditorAiBackend backend);
const char *EditorAiBackendDisplayName(EditorAiBackend backend);
const char *EditorAiBackendModelFilename(EditorAiBackend backend);
bool IsEditorAiLotusBackend(EditorAiBackend backend);
bool IsEditorAiDiffusionBackend(EditorAiBackend backend);
const char *EditorAiMapKindId(EditorAiMapKind kind);
std::filesystem::path ResolveEditorAiModelPath(EditorAiBackend backend, const std::string &exeDir = "",
                                               const std::string &modelsDir = "");

// Loads/saves local AI preferences (active GGUF model, longest-side resolution, models dir).
bool LoadEditorAiLocalConfig(const std::filesystem::path &path, EditorAiLocalConfig &outConfig,
                             std::string &outError);
bool SaveEditorAiLocalConfig(const std::filesystem::path &path, const EditorAiLocalConfig &config,
                             std::string &outError);

// Computes fit-by-longest-side dimensions rounded to multiples of 64 (Lotus VAE*UNet stride).
void ComputeEditorAiUpscaleFitDimensions(uint32_t srcW, uint32_t srcH, uint32_t targetMaxSide,
                                         uint32_t &outFitW, uint32_t &outFitH);

// Computes fit + wrap/mirror halo dimensions for Lotus Depth v2 aligned to 64-pixel multiples.
void ComputeEditorAiLotusDepthDimensions(uint32_t srcW, uint32_t srcH, uint32_t targetMaxSide,
                                         bool seamless, EditorAiLotusDepthDims &outDims);

// Prepares [1, 3, H, W] float32 [0, 1] NCHW tensor for Lotus with seamless halo padding.
void PrepareEditorLotusInputNchw(const std::vector<uint8_t> &diffuseRgba, uint32_t srcW,
                                 uint32_t srcH, uint32_t processingRes, bool seamless,
                                 EditorAiLotusPrep &outPrep);

// Prepares [1, 3, H, W] float32 [0, 1] NCHW tensor for Lotus Depth v2 with wide wrap/mirror halo.
void PrepareEditorLotusDepthInputNchw(const std::vector<uint8_t> &diffuseRgba, uint32_t srcW,
                                      uint32_t srcH, uint32_t processingRes, bool seamless,
                                      EditorAiLotusPrep &outPrep);

// Crops halo and area/bilinear-downsamples [1, C, H, W] prediction to dstW x dstH HWC [0, 1].
void DownsampleEditorLotusOutputNchw(const EditorAiTensorNchw &pred, const EditorAiLotusPrep &prep,
                                     uint32_t dstW, uint32_t dstH, std::vector<float> &outHwc01);

// Removes monocular perspective bias using halo overlaps, box-downsamples, and percentile-normalizes.
void ExtractDetrendedEditorLotusDepthHeight(const EditorAiTensorNchw &pred,
                                            const EditorAiLotusPrep &prep, uint32_t dstW,
                                            uint32_t dstH, std::vector<float> &outHeight);

// Converts [0, 1] HWC predicted normals into QuakeRay tangent-space RGBA8 (+T=+U, +B=+V, +N=+Z).
void ConvertEditorNormals01ToQuakeRayRgba(const std::vector<float> &hwc01, uint32_t width,
                                          uint32_t height, bool xIsLeft, bool yIsUp,
                                          std::vector<uint8_t> &outRgba);

// Reconstructs normalized [0, 1] height field from QuakeRay tangent-space normals via multigrid Poisson.
void IntegrateEditorNormalMapHeight(const std::vector<uint8_t> &baseNormalRgba, uint32_t width,
                                    uint32_t height, bool seamless, std::vector<float> &outHeight);

// Executes end-to-end native C++ AI inference (Lotus/DeepBump GGUF worker, or local emissive).
EditorAiInferenceResult RunEditorAiTextureInference(
    const EditorAiInferenceRequest &req, const std::atomic<bool> *cancelRequested = nullptr);

// Extracts normalized [0, 1] grayscale height samples from an RGBA buffer, resizing bilinearly if needed.
void ExtractEditorHeightFromRgba(const std::vector<uint8_t> &rgba, uint32_t srcWidth,
                                 uint32_t srcHeight, uint32_t dstWidth, uint32_t dstHeight,
                                 std::vector<float> &outHeight);

// Resizes an RGBA8 buffer to target dimensions using bilinear filtering.
void ResizeEditorRgbaBilinear(const std::vector<uint8_t> &srcRgba, uint32_t srcWidth,
                              uint32_t srcHeight, uint32_t dstWidth, uint32_t dstHeight,
                              std::vector<uint8_t> &outRgba);

// Converts a [0, 1] height map to a normalized tangent-space RGBA8 normal map via Scharr gradients.
void BuildEditorNormalMapFromHeight(const std::vector<float> &heightMap, uint32_t width,
                                    uint32_t height, const std::vector<uint8_t> *diffuseRgba,
                                    const EditorAiNormalParams &params,
                                    std::vector<uint8_t> &outNormalRgba);

// Applies strength, invertHeight, invertY, and optional diffuse micro-detail directly to a base
// tangent-space normal map predicted by Lotus Normal or DeepBump without blurring the prediction.
void AdjustEditorNormalMap(const std::vector<uint8_t> &rawNormalRgba, uint32_t width,
                           uint32_t height, const std::vector<uint8_t> *diffuseRgba,
                           const EditorAiNormalParams &params,
                           std::vector<uint8_t> &outNormalRgba);

// Renders grayscale height map visualization (respecting invertHeight) into RGBA8.
void BuildEditorHeightPreviewRgba(const std::vector<float> &heightMap, uint32_t width,
                                  uint32_t height, bool invertHeight,
                                  std::vector<uint8_t> &outRgba);

// Renders a quick 2D software diffuse+specular relit preview to verify bump convex vs concave sign.
void BuildEditorNormalLitPreviewRgba(const std::vector<uint8_t> &diffuseRgba,
                                     const std::vector<uint8_t> &normalRgba, uint32_t width,
                                     uint32_t height, float lightAngleDeg,
                                     std::vector<uint8_t> &outRgba);

// Applies instant contrast/gain and bias adjustments to a raw gloss RGBA8 buffer.
void ApplyEditorGlossAdjustments(const std::vector<uint8_t> &rawRgba, uint32_t width,
                                 uint32_t height, const EditorAiToneParams &tone,
                                 std::vector<uint8_t> &outGlossRgba);

// Synthesizes a normalized base gloss RGBA8 map from diffuse RGB and integrated height.
void SynthesizeEditorGlossFromDiffuseAndHeight(const std::vector<uint8_t> &diffuseRgba,
                                               const std::vector<float> &height, uint32_t width,
                                               uint32_t heightPx,
                                               std::vector<uint8_t> &outGlossRgba);

// Renders a quick 2D specular reflection preview from diffuse + adjusted gloss RGBA8.
void BuildEditorGlossSpecularPreviewRgba(const std::vector<uint8_t> &diffuseRgba,
                                         const std::vector<uint8_t> &glossRgba, uint32_t width,
                                         uint32_t height, float lightAngleDeg,
                                         std::vector<uint8_t> &outRgba);

// Applies instant gain and floor-threshold cutoffs to a raw emissive RGBA8 buffer.
void ApplyEditorEmissiveAdjustments(const std::vector<uint8_t> &rawRgba, uint32_t width,
                                    uint32_t height, const EditorAiToneParams &tone,
                                    std::vector<uint8_t> &outEmissiveRgba);

// Composites emissive output additively on top of diffuse for quick visual verification.
void BuildEditorEmissiveCompositePreviewRgba(const std::vector<uint8_t> &diffuseRgba,
                                             const std::vector<uint8_t> &emissiveRgba,
                                             uint32_t width, uint32_t height,
                                             std::vector<uint8_t> &outRgba);

// Blends original and de-lit diffuse textures with optional glare-masked detail transfer.
void ApplyEditorDelightAdjustments(const std::vector<uint8_t> &diffuseRgba,
                                   const std::vector<uint8_t> &rawDelitRgba, uint32_t width,
                                   uint32_t height, const EditorAiDelightParams &params,
                                   std::vector<uint8_t> &outRgba);

// Renders a 2D difference visualization highlighting specular glare removed by de-lighting.
void BuildEditorDelightRemovedGlarePreviewRgba(const std::vector<uint8_t> &diffuseRgba,
                                               const std::vector<uint8_t> &delitRgba,
                                               uint32_t width, uint32_t height,
                                               std::vector<uint8_t> &outRgba);

// Applies 4x/2x scaling, blend strength, and unsharp filtering to a raw RealPLKSR upscaled RGBA8 buffer.
void ApplyEditorUpscaleAdjustments(const EditorAiUpscaleInput &in,
                                   const EditorAiUpscaleParams &params,
                                   std::vector<uint8_t> &outRgba, uint32_t &outW, uint32_t &outH);

// Renders a split screen preview comparing bilinear upscaled original (left) vs AI upscaled (right).
void BuildEditorUpscaleComparePreviewRgba(const std::vector<uint8_t> &origDiffuseRgba,
                                          uint32_t origW, uint32_t origH,
                                          const std::vector<uint8_t> &upscaledRgba, uint32_t outW,
                                          uint32_t outH, std::vector<uint8_t> &outRgba);

// Renders an amplified difference visualization showing detail reconstructed by AI upscaling.
void BuildEditorUpscaleDetailPreviewRgba(const std::vector<uint8_t> &origDiffuseRgba,
                                         uint32_t origW, uint32_t origH,
                                         const std::vector<uint8_t> &upscaledRgba, uint32_t outW,
                                         uint32_t outH, std::vector<uint8_t> &outRgba);

// Applies optional pre-smoothing and low-frequency illumination flattening on a copy of diffuse.
void PreprocessEditorAiDiffuse(const std::vector<uint8_t> &diffuseRgba, uint32_t width,
                               uint32_t height, const EditorAiDiffusePrepParams &params,
                               std::vector<uint8_t> &outRgba);

// Writes a 32-bit uncompressed BGRA TGA file (creating parent directories if needed).
bool WriteEditorRgbaTga(const std::filesystem::path &path, uint32_t width, uint32_t height,
                        const std::vector<uint8_t> &rgba);

// Builds relative output path under textures/ai_generated/<material>_<kind>.tga.
std::string BuildGeneratedTextureRelativePath(const std::string &materialName, EditorAiMapKind kind);

// Returns a non-colliding relative path under textures/ai_generated/ (appending _2, _3, ... if taken).
std::string BuildUniqueGeneratedTextureRelativePath(
    const std::filesystem::path &outRoot, const std::string &materialName, EditorAiMapKind kind,
    const std::function<bool(const std::string &)> &extraExists = {});
