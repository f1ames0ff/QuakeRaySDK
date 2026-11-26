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

#include "EditorMaterial.h"
#include "EditorOnnxRuntime.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

enum class EditorAiBackend
{
    LotusNormalD = 0, // lotus-normal-d-v1-1.onnx
    LotusDepthD = 1,  // lotus-depth-d-v2-0-disparity.onnx
    DeepBump = 2,     // deepbump256.onnx
};

struct EditorAiLocalConfig
{
    EditorAiBackend activeBackend = EditorAiBackend::LotusNormalD;
    uint32_t processingRes = 768; // Fit-by-longest-side before Lotus ONNX (>= 512)
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

struct EditorAiLotusPrep
{
    uint32_t fitW = 0;
    uint32_t fitH = 0;
    uint32_t halo = 0;
    EditorOnnxTensorNchw tensor;
};

struct EditorAiInferenceRequest
{
    EditorAiBackend backend = EditorAiBackend::LotusNormalD;
    EditorAiMapKind mapKind = EditorAiMapKind::Normals;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t processingRes = 768;
    bool seamless = true;
    std::string modelsDir;
    std::string exeDir;
    std::vector<uint8_t> diffuseRgba;
};

struct EditorAiInferenceResult
{
    bool ok = false;
    bool cancelled = false;
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

// Returns identifiers, display labels, ONNX filenames, and disk lookup for AI backends.
const char *EditorAiBackendId(EditorAiBackend backend);
const char *EditorAiBackendDisplayName(EditorAiBackend backend);
const char *EditorAiBackendModelFilename(EditorAiBackend backend);
bool IsEditorAiLotusBackend(EditorAiBackend backend);
const char *EditorAiMapKindId(EditorAiMapKind kind);
std::filesystem::path ResolveEditorAiModelPath(EditorAiBackend backend, const std::string &exeDir = "",
                                               const std::string &modelsDir = "");

// Loads/saves local AI preferences (active ONNX model, longest-side resolution, models dir).
bool LoadEditorAiLocalConfig(const std::filesystem::path &path, EditorAiLocalConfig &outConfig,
                             std::string &outError);
bool SaveEditorAiLocalConfig(const std::filesystem::path &path, const EditorAiLocalConfig &config,
                             std::string &outError);

// Computes fit-by-longest-side dimensions rounded to multiples of 64 (Lotus VAE*UNet stride).
void ComputeEditorAiUpscaleFitDimensions(uint32_t srcW, uint32_t srcH, uint32_t targetMaxSide,
                                         uint32_t &outFitW, uint32_t &outFitH);

// Prepares [1, 3, H, W] float32 [0, 1] NCHW tensor for Lotus with seamless halo padding.
void PrepareEditorLotusInputNchw(const std::vector<uint8_t> &diffuseRgba, uint32_t srcW,
                                 uint32_t srcH, uint32_t processingRes, bool seamless,
                                 EditorAiLotusPrep &outPrep);

// Crops halo and area/bilinear-downsamples [1, C, H, W] prediction to dstW x dstH HWC [0, 1].
void DownsampleEditorLotusOutputNchw(const EditorOnnxTensorNchw &pred, const EditorAiLotusPrep &prep,
                                     uint32_t dstW, uint32_t dstH, std::vector<float> &outHwc01);

// Converts [0, 1] HWC predicted normals into QuakeRay tangent-space RGBA8 (+T=+U, +B=+V, +N=+Z).
void ConvertEditorNormals01ToQuakeRayRgba(const std::vector<float> &hwc01, uint32_t width,
                                          uint32_t height, bool xIsLeft, bool yIsUp,
                                          std::vector<uint8_t> &outRgba);

// Reconstructs normalized [0, 1] height field from QuakeRay tangent-space normals via multigrid Poisson.
void IntegrateEditorNormalMapHeight(const std::vector<uint8_t> &baseNormalRgba, uint32_t width,
                                    uint32_t height, bool seamless, std::vector<float> &outHeight);

// Executes end-to-end native C++ AI inference (Lotus ONNX, DeepBump ONNX, or local emissive).
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

// Writes a 32-bit uncompressed BGRA TGA file (creating parent directories if needed).
bool WriteEditorRgbaTga(const std::filesystem::path &path, uint32_t width, uint32_t height,
                        const std::vector<uint8_t> &rgba);

// Builds relative output path under textures/ai_generated/<material>_<kind>.tga.
std::string BuildGeneratedTextureRelativePath(const std::string &materialName, EditorAiMapKind kind);
