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

#include <qray/gpu_context.h>
#include <qray/qray.h>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <X11/Xlib.h>
    #undef None
    #undef Success
    #undef Always
    #undef Bool
    #undef Status
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace
{

constexpr uint32_t kWidth = 648;
constexpr uint32_t kHeight = 360;
constexpr uint32_t kTotalFrames = 32;
constexpr uint32_t kBlueNoiseSize = 128;
constexpr uint32_t kBlueNoiseLayers = 128;
constexpr double kPi = 3.14159265358979323846;

struct FileBlob
{
    std::vector<uint8_t> bytes;
};

struct BenchmarkArgs
{
    std::string shaderDir;
    std::string textureDir;
    std::string outDir;
    std::string stepName = "step0_baseline";
};

struct RgbaImage
{
    uint32_t width = 0;
    uint32_t height = 0;
    // Linear RGB luminance + RGB in [0, +inf) stored top-down (row 0 = top of view).
    std::vector<float> rgb;
};

struct RoiRect
{
    uint32_t x0 = 0;
    uint32_t y0 = 0;
    uint32_t x1 = 0;
    uint32_t y1 = 0;
};

struct BenchmarkMetrics
{
    double spatialVarianceHfLf = 0.0;
    double fireflyMaxOverMedian = 0.0;
    double temporalFlickerRmse = 0.0;
    double contactEdgeGradient = 0.0;
    double motionSpatialVarianceF06 = 0.0;
    double grazingFlickerRmse = 0.0;
    double gridRingingEnergy = 0.0;
};

void PrintMessage(const char *message, void *)
{
    if (message != nullptr)
    {
        std::fprintf(stderr, "%s\n", message);
    }
}

bool CheckQr(QrResult result, const char *what)
{
    if (result != QR_SUCCESS)
    {
        std::fprintf(stderr, "%s: %s\n", what, qrGetResultDescription(result));
        return false;
    }
    return true;
}

void AppendBe32(std::vector<uint8_t> &out, uint32_t value)
{
    out.push_back(uint8_t((value >> 24u) & 0xFFu));
    out.push_back(uint8_t((value >> 16u) & 0xFFu));
    out.push_back(uint8_t((value >> 8u) & 0xFFu));
    out.push_back(uint8_t(value & 0xFFu));
}

uint32_t UpdateCrc32(const uint8_t *data, size_t size)
{
    uint32_t state = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; i++)
    {
        state ^= data[i];
        for (int bit = 0; bit < 8; bit++)
        {
            state = (state & 1u) != 0u ? ((state >> 1u) ^ 0xEDB88320u) : (state >> 1u);
        }
    }
    return state ^ 0xFFFFFFFFu;
}

uint32_t ComputeAdler32(const uint8_t *data, size_t size)
{
    constexpr uint32_t kModAdler = 65521u;
    uint32_t s1 = 1u;
    uint32_t s2 = 0u;
    for (size_t i = 0; i < size; i++)
    {
        s1 = (s1 + data[i]) % kModAdler;
        s2 = (s2 + s1) % kModAdler;
    }
    return (s2 << 16u) | s1;
}

void WritePngChunk(std::vector<uint8_t> &out, const char type[4], const uint8_t *data, size_t size)
{
    AppendBe32(out, uint32_t(size));
    const size_t typeOffset = out.size();
    out.insert(out.end(), type, type + 4);
    if (size > 0 && data != nullptr)
    {
        out.insert(out.end(), data, data + size);
    }
    AppendBe32(out, UpdateCrc32(out.data() + typeOffset, 4u + size));
}

std::vector<uint8_t> EncodeRgbaPngMemory(uint32_t width, uint32_t height, uint32_t rowPitch,
                                         const uint8_t *rgba)
{
    const size_t rawStride = 1u + size_t(width) * 4u;
    std::vector<uint8_t> raw(size_t(height) * rawStride, 0u);
    for (uint32_t y = 0; y < height; y++)
    {
        uint8_t *dst = raw.data() + size_t(y) * rawStride;
        dst[0] = 0u;
        std::copy_n(rgba + size_t(y) * rowPitch, size_t(width) * 4u, dst + 1u);
    }

    std::vector<uint8_t> z;
    z.reserve(raw.size() + 256u);
    z.push_back(0x78u);
    z.push_back(0x01u);

    size_t offset = 0;
    while (offset < raw.size())
    {
        const size_t blockLen = std::min<size_t>(65535u, raw.size() - offset);
        const bool isFinal = (offset + blockLen) >= raw.size();
        const uint16_t len16 = uint16_t(blockLen);
        const uint16_t nlen16 = uint16_t(~len16);
        z.push_back(isFinal ? 0x01u : 0x00u);
        z.push_back(uint8_t(len16 & 0xFFu));
        z.push_back(uint8_t(len16 >> 8u));
        z.push_back(uint8_t(nlen16 & 0xFFu));
        z.push_back(uint8_t(nlen16 >> 8u));
        z.insert(z.end(), raw.data() + offset, raw.data() + offset + blockLen);
        offset += blockLen;
    }
    AppendBe32(z, ComputeAdler32(raw.data(), raw.size()));

    std::vector<uint8_t> png = { 137u, 80u, 78u, 71u, 13u, 10u, 26u, 10u };
    std::vector<uint8_t> ihdr;
    AppendBe32(ihdr, width);
    AppendBe32(ihdr, height);
    ihdr.push_back(8u);
    ihdr.push_back(6u);
    ihdr.push_back(0u);
    ihdr.push_back(0u);
    ihdr.push_back(0u);
    WritePngChunk(png, "IHDR", ihdr.data(), ihdr.size());
    WritePngChunk(png, "IDAT", z.data(), z.size());
    WritePngChunk(png, "IEND", nullptr, 0u);
    return png;
}

uint32_t HashU32(uint32_t x)
{
    x = (x ^ 61u) ^ (x >> 16u);
    x += (x << 3u);
    x ^= (x >> 4u);
    x *= 0x27d4eb2du;
    x ^= (x >> 15u);
    return x;
}

std::vector<uint8_t> SynthesizeBlueNoisePng()
{
    const uint32_t height = kBlueNoiseSize * kBlueNoiseLayers;
    std::vector<uint8_t> pixels(size_t(kBlueNoiseSize) * height * 4u, 0u);
    for (uint32_t layer = 0; layer < kBlueNoiseLayers; layer++)
    {
        for (uint32_t y = 0; y < kBlueNoiseSize; y++)
        {
            for (uint32_t x = 0; x < kBlueNoiseSize; x++)
            {
                const uint32_t base = ((layer * kBlueNoiseSize + y) * kBlueNoiseSize + x) * 4u;
                const uint32_t h0 = HashU32(base ^ 0x13579BDFu);
                const uint32_t h1 = HashU32(base ^ 0x2468ACE0u);
                const size_t idx = (size_t(layer * kBlueNoiseSize + y) * kBlueNoiseSize + x) * 4u;
                pixels[idx + 0u] = uint8_t(h0 & 0xFFu);
                pixels[idx + 1u] = uint8_t((h0 >> 16u) & 0xFFu);
                pixels[idx + 2u] = uint8_t(h1 & 0xFFu);
                pixels[idx + 3u] = uint8_t((h1 >> 16u) & 0xFFu);
            }
        }
    }
    return EncodeRgbaPngMemory(kBlueNoiseSize, height, kBlueNoiseSize * 4u, pixels.data());
}

void OpenFile(const char *path, void *, const void **data, uint32_t *size, void **fileHandle)
{
    *data = nullptr;
    *size = 0;
    *fileHandle = nullptr;

    if (path == nullptr)
    {
        return;
    }

    std::error_code ec;
    const std::filesystem::path fsPath(path);
    if (std::filesystem::is_regular_file(fsPath, ec))
    {
        std::ifstream file(fsPath, std::ios::binary | std::ios::ate);
        if (file)
        {
            const std::streamsize length = file.tellg();
            if (length > 0)
            {
                auto *blob = new FileBlob();
                blob->bytes.resize(size_t(length));
                file.seekg(0);
                file.read(reinterpret_cast<char *>(blob->bytes.data()), length);
                if (file)
                {
                    *data = blob->bytes.data();
                    *size = uint32_t(blob->bytes.size());
                    *fileHandle = blob;
                    return;
                }
                delete blob;
            }
        }
    }

    if (std::string(path).find("BlueNoise") != std::string::npos)
    {
        auto *blob = new FileBlob();
        blob->bytes = SynthesizeBlueNoisePng();
        *data = blob->bytes.data();
        *size = uint32_t(blob->bytes.size());
        *fileHandle = blob;
    }
}

void CloseFile(void *fileHandle, void *)
{
    delete static_cast<FileBlob *>(fileHandle);
}

struct TestWindow
{
#ifdef _WIN32
    HWND handle = nullptr;
#else
    Display *display = nullptr;
    ::Window handle = 0;
#endif

    TestWindow() = default;
    TestWindow(const TestWindow &) = delete;
    TestWindow &operator=(const TestWindow &) = delete;

    ~TestWindow()
    {
        Destroy();
    }

    bool Create()
    {
#ifdef _WIN32
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"QuakeRayEmissiveBenchmark";
        if (RegisterClassExW(&wc) == 0)
        {
            return false;
        }
        handle = CreateWindowExW(0, wc.lpszClassName, L"QuakeRay Emissive Benchmark",
                                 WS_OVERLAPPEDWINDOW, 0, 0, int(kWidth), int(kHeight),
                                 nullptr, nullptr, wc.hInstance, nullptr);
        if (handle == nullptr)
        {
            return false;
        }
        ShowWindow(handle, SW_SHOWNOACTIVATE);
        SetWindowPos(handle, nullptr, -32000, -32000, int(kWidth), int(kHeight),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return true;
#else
        display = XOpenDisplay(nullptr);
        if (display == nullptr)
        {
            std::fprintf(stderr, "open X11 display failed\n");
            return false;
        }
        XSetWindowAttributes attributes = {};
        attributes.override_redirect = True;
        handle = XCreateWindow(display, DefaultRootWindow(display), -32000, -32000, kWidth, kHeight,
                               0, CopyFromParent, InputOutput, CopyFromParent, CWOverrideRedirect,
                               &attributes);
        if (handle == 0)
        {
            Destroy();
            return false;
        }
        XStoreName(display, handle, "QuakeRay emissive denoiser benchmark");
        XMapWindow(display, handle);
        XSync(display, False);
        return true;
#endif
    }

    void Destroy()
    {
#ifdef _WIN32
        if (handle != nullptr)
        {
            DestroyWindow(handle);
            handle = nullptr;
        }
#else
        if (display != nullptr)
        {
            if (handle != 0)
            {
                XDestroyWindow(display, handle);
                handle = 0;
            }
            XCloseDisplay(display);
            display = nullptr;
        }
#endif
    }
};

void LookAtRhZUp(const float eye[3], const float target[3], float outView[16])
{
    const float fx = target[0] - eye[0];
    const float fy = target[1] - eye[1];
    const float fz = target[2] - eye[2];
    const float flen = std::sqrt(fx * fx + fy * fy + fz * fz);
    const float fwd[3] = { fx / flen, fy / flen, fz / flen };

    const float up[3] = { 0.0f, 0.0f, 1.0f };
    float sx = fwd[1] * up[2] - fwd[2] * up[1];
    float sy = fwd[2] * up[0] - fwd[0] * up[2];
    float sz = fwd[0] * up[1] - fwd[1] * up[0];
    const float slen = std::sqrt(sx * sx + sy * sy + sz * sz);
    sx /= slen;
    sy /= slen;
    sz /= slen;

    const float ux = sy * fwd[2] - sz * fwd[1];
    const float uy = sz * fwd[0] - sx * fwd[2];
    const float uz = sx * fwd[1] - sy * fwd[0];

    outView[0] = sx;  outView[1] = ux;  outView[2] = -fwd[0];  outView[3] = 0.0f;
    outView[4] = sy;  outView[5] = uy;  outView[6] = -fwd[1];  outView[7] = 0.0f;
    outView[8] = sz;  outView[9] = uz;  outView[10] = -fwd[2]; outView[11] = 0.0f;
    outView[12] = -(sx * eye[0] + sy * eye[1] + sz * eye[2]);
    outView[13] = -(ux * eye[0] + uy * eye[1] + uz * eye[2]);
    outView[14] = (fwd[0] * eye[0] + fwd[1] * eye[1] + fwd[2] * eye[2]);
    outView[15] = 1.0f;
}

float DecodeMiniFloat(uint32_t bits, uint32_t mantissaBits)
{
    const uint32_t expMask = 0x1Fu;
    const uint32_t mantMask = (1u << mantissaBits) - 1u;
    const uint32_t exponent = (bits >> mantissaBits) & expMask;
    const uint32_t mantissa = bits & mantMask;
    if (exponent == 0x1Fu)
    {
        return 0.0f;
    }
    if (exponent == 0u)
    {
        return std::ldexp(float(mantissa), -14 - int(mantissaBits));
    }
    return std::ldexp(1.0f + float(mantissa) / float(1u << mantissaBits), int(exponent) - 15);
}

float HalfToFloat(uint16_t bits)
{
    const uint32_t sign = (bits >> 15u) & 1u;
    const uint32_t exponent = (bits >> 10u) & 0x1Fu;
    const uint32_t mantissa = bits & 0x3FFu;
    float value = 0.0f;
    if (exponent == 0u)
    {
        value = std::ldexp(float(mantissa), -24);
    }
    else if (exponent != 31u)
    {
        value = std::ldexp(1.0f + float(mantissa) / 1024.0f, int(exponent) - 15);
    }
    return sign != 0u ? -value : value;
}

float SrgbToLinear(float c)
{
    if (c <= 0.04045f)
    {
        return c / 12.92f;
    }
    return std::pow((c + 0.055f) / 1.055f, 2.4f);
}

uint8_t LinearToSrgbByte(float c)
{
    const float clamped = std::clamp(c, 0.0f, 1.0f);
    const float srgb = (clamped <= 0.0031308f)
        ? (clamped * 12.92f)
        : (1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f);
    const long v = std::lround(std::clamp(srgb, 0.0f, 1.0f) * 255.0f);
    return uint8_t(std::clamp<long>(v, 0L, 255L));
}

void DecodeTextureRow(nvrhi::Format format, const uint8_t *src, uint32_t width, float *dstRgb)
{
    for (uint32_t x = 0; x < width; x++)
    {
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
        if (format == nvrhi::Format::R11G11B10_FLOAT)
        {
            uint32_t packed = 0;
            std::memcpy(&packed, src + size_t(x) * 4u, sizeof(uint32_t));
            r = DecodeMiniFloat(packed & 0x7FFu, 6u);
            g = DecodeMiniFloat((packed >> 11u) & 0x7FFu, 6u);
            b = DecodeMiniFloat((packed >> 22u) & 0x3FFu, 5u);
        }
        else if (format == nvrhi::Format::RGBA16_FLOAT)
        {
            uint16_t halfs[4] = {};
            std::memcpy(halfs, src + size_t(x) * 8u, sizeof(halfs));
            r = std::max(0.0f, HalfToFloat(halfs[0]));
            g = std::max(0.0f, HalfToFloat(halfs[1]));
            b = std::max(0.0f, HalfToFloat(halfs[2]));
        }
        else
        {
            const bool isBgra = (format == nvrhi::Format::BGRA8_UNORM ||
                                 format == nvrhi::Format::SBGRA8_UNORM);
            const uint8_t c0 = src[size_t(x) * 4u + (isBgra ? 2u : 0u)];
            const uint8_t c1 = src[size_t(x) * 4u + 1u];
            const uint8_t c2 = src[size_t(x) * 4u + (isBgra ? 0u : 2u)];
            r = SrgbToLinear(float(c0) / 255.0f);
            g = SrgbToLinear(float(c1) / 255.0f);
            b = SrgbToLinear(float(c2) / 255.0f);
        }
        dstRgb[size_t(x) * 3u + 0u] = r;
        dstRgb[size_t(x) * 3u + 1u] = g;
        dstRgb[size_t(x) * 3u + 2u] = b;
    }
}

void WriteLinearImagePng(const std::filesystem::path &path, const RgbaImage &img)
{
    if (img.width == 0 || img.height == 0 || img.rgb.empty())
    {
        return;
    }
    std::error_code ec;
    if (path.has_parent_path())
    {
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    std::vector<uint8_t> rgba(size_t(img.width) * img.height * 4u, 255u);
    const size_t n = size_t(img.width) * img.height;
    for (size_t i = 0; i < n; i++)
    {
        rgba[i * 4u + 0u] = LinearToSrgbByte(img.rgb[i * 3u + 0u]);
        rgba[i * 4u + 1u] = LinearToSrgbByte(img.rgb[i * 3u + 1u]);
        rgba[i * 4u + 2u] = LinearToSrgbByte(img.rgb[i * 3u + 2u]);
        rgba[i * 4u + 3u] = 255u;
    }

    const std::vector<uint8_t> png =
        EncodeRgbaPngMemory(img.width, img.height, img.width * 4u, rgba.data());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (out)
    {
        out.write(reinterpret_cast<const char *>(png.data()), std::streamsize(png.size()));
    }
}

float PixelLuminance(const RgbaImage &img, uint32_t x, uint32_t y)
{
    const size_t idx = (size_t(y) * img.width + x) * 3u;
    return 0.2126f * img.rgb[idx] + 0.7152f * img.rgb[idx + 1u] + 0.0722f * img.rgb[idx + 2u];
}

struct CapturedSlot
{
    uint32_t frameNumber = 0;
    nvrhi::StagingTextureHandle staging;
    nvrhi::Format format = nvrhi::Format::UNKNOWN;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct BenchmarkSceneMesh
{
    QrTransform identity = {};
    uint32_t indices[6] = { 0, 1, 2, 0, 2, 3 };
};

void MakeIdentityTransform(QrTransform &t)
{
    t = {};
    t.matrix[0][0] = 1.0f;
    t.matrix[1][1] = 1.0f;
    t.matrix[2][2] = 1.0f;
}

void MakeQuadVertices(const float pts[4][3], const float normal[3], QrVertex out[4])
{
    const float uvs[4][2] = {
        { 0.0f, 0.0f },
        { 1.0f, 0.0f },
        { 1.0f, 1.0f },
        { 0.0f, 1.0f },
    };
    for (int i = 0; i < 4; i++)
    {
        out[i] = {};
        out[i].position[0] = pts[i][0];
        out[i].position[1] = pts[i][1];
        out[i].position[2] = pts[i][2];
        out[i].normal[0] = normal[0];
        out[i].normal[1] = normal[1];
        out[i].normal[2] = normal[2];
        out[i].texCoord[0] = uvs[i][0];
        out[i].texCoord[1] = uvs[i][1];
        out[i].packedColor = 0xFFFFFFFFu;
        out[i].cluster = 1u;
    }
}

bool UploadStaticQuad(QrInstance instance, QrMaterial material, uint32_t uniqueID,
                      const float pts[4][3], const float normal[3], const float color[3],
                      float roughness, float metallic, float emission, uint32_t extraFlags)
{
    QrVertex verts[4] = {};
    MakeQuadVertices(pts, normal, verts);
    const uint32_t indices[6] = { 0, 1, 2, 0, 2, 3 };

    QrTransform identity = {};
    MakeIdentityTransform(identity);

    QrGeometryUploadInfo geom = {};
    geom.uniqueID = uniqueID;
    geom.flags = QR_GEOMETRY_UPLOAD_EXACT_NORMALS_BIT | extraFlags;
    geom.geomType = QR_GEOMETRY_TYPE_STATIC;
    geom.passThroughType = QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE;
    geom.visibilityType = QR_GEOMETRY_VISIBILITY_TYPE_WORLD_0;
    geom.vertexCount = 4;
    geom.pVertices = verts;
    geom.indexCount = 6;
    geom.pIndices = indices;
    geom.layerColors[0] = { color[0], color[1], color[2], 1.0f };
    geom.layerBlendingTypes[0] = QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE;
    geom.geomMaterial.layerMaterials[0] = material;
    geom.defaultRoughness = roughness;
    geom.defaultMetallicity = metallic;
    geom.defaultEmission = emission;
    geom.transform = identity;
    return CheckQr(qrUploadGeometry(instance, &geom), "upload benchmark quad");
}

bool UploadBox(QrInstance instance, QrMaterial material, uint32_t baseId,
               const float mins[3], const float maxs[3], const float color[3],
               float roughness, float metallic)
{
    const float faces[6][4][3] = {
        { { mins[0], mins[1], maxs[2] }, { maxs[0], mins[1], maxs[2] },
          { maxs[0], maxs[1], maxs[2] }, { mins[0], maxs[1], maxs[2] } },
        { { mins[0], mins[1], mins[2] }, { mins[0], maxs[1], mins[2] },
          { maxs[0], maxs[1], mins[2] }, { maxs[0], mins[1], mins[2] } },
        { { mins[0], mins[1], mins[2] }, { maxs[0], mins[1], mins[2] },
          { maxs[0], mins[1], maxs[2] }, { mins[0], mins[1], maxs[2] } },
        { { maxs[0], maxs[1], mins[2] }, { mins[0], maxs[1], mins[2] },
          { mins[0], maxs[1], maxs[2] }, { maxs[0], maxs[1], maxs[2] } },
        { { mins[0], maxs[1], mins[2] }, { mins[0], mins[1], mins[2] },
          { mins[0], mins[1], maxs[2] }, { mins[0], maxs[1], maxs[2] } },
        { { maxs[0], mins[1], mins[2] }, { maxs[0], maxs[1], mins[2] },
          { maxs[0], maxs[1], maxs[2] }, { maxs[0], mins[1], maxs[2] } },
    };
    const float normals[6][3] = {
        { 0.0f, 0.0f, 1.0f },  { 0.0f, 0.0f, -1.0f }, { 0.0f, -1.0f, 0.0f },
        { 0.0f, 1.0f, 0.0f },  { -1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f },
    };
    for (uint32_t f = 0; f < 6; f++)
    {
        if (!UploadStaticQuad(instance, material, baseId + f, faces[f], normals[f], color,
                              roughness, metallic, 0.0f, 0u))
        {
            return false;
        }
    }
    return true;
}

QrDtalMemberUpload MakeDtalQuadMember(const float p0[3], const float p1[3], const float p3[3],
                                      const float normal[3])
{
    QrDtalMemberUpload m = {};
    const float ax = p1[0] - p0[0];
    const float ay = p1[1] - p0[1];
    const float az = p1[2] - p0[2];
    const float bx = p3[0] - p0[0];
    const float by = p3[1] - p0[1];
    const float bz = p3[2] - p0[2];
    const float cx = ay * bz - az * by;
    const float cy = az * bx - ax * bz;
    const float cz = ax * by - ay * bx;

    m.A = { { ax, ay, az } };
    m.B = { { bx, by, bz } };
    m.C = { { p0[0], p0[1], p0[2] } };
    m.area = std::sqrt(cx * cx + cy * cy + cz * cz);
    m.numVerts = 4.0f;
    m.prob = 1.0f;
    m.marginalProb = 1.0f;
    m.normal = { { normal[0], normal[1], normal[2] } };
    m.uv[0] = { { 0.0f, 0.0f } };
    m.uv[1] = { { 1.0f, 0.0f } };
    m.uv[2] = { { 1.0f, 1.0f } };
    m.uv[3] = { { 0.0f, 1.0f } };
    m.aliasIndex = 0u;
    return m;
}

QrDtalGroupUploadInfo MakeDtalGroupInfo(uint64_t uniqueID, uint32_t memberBase,
                                          const QrDtalMemberUpload &m, const float color[3],
                                          const float normal[3], float reach)
{
    const float p0[3] = { m.C.data[0], m.C.data[1], m.C.data[2] };
    const float p2[3] = {
        m.C.data[0] + m.A.data[0] + m.B.data[0],
        m.C.data[1] + m.A.data[1] + m.B.data[1],
        m.C.data[2] + m.A.data[2] + m.B.data[2],
    };
    const float p1[3] = { m.C.data[0] + m.A.data[0], m.C.data[1] + m.A.data[1],
                          m.C.data[2] + m.A.data[2] };
    const float p3[3] = { m.C.data[0] + m.B.data[0], m.C.data[1] + m.B.data[1],
                          m.C.data[2] + m.B.data[2] };

    QrDtalGroupUploadInfo g = {};
    g.uniqueID = uniqueID;
    g.color = { { color[0], color[1], color[2] } };
    g.normal = { { normal[0], normal[1], normal[2] } };
    for (int k = 0; k < 3; k++)
    {
        g.boundsMin.data[k] = std::min({ p0[k], p1[k], p2[k], p3[k] });
        g.boundsMax.data[k] = std::max({ p0[k], p1[k], p2[k], p3[k] });
        g.center.data[k] = 0.5f * (g.boundsMin.data[k] + g.boundsMax.data[k]);
    }
    const float dx = g.boundsMax.data[0] - g.boundsMin.data[0];
    const float dy = g.boundsMax.data[1] - g.boundsMin.data[1];
    const float dz = g.boundsMax.data[2] - g.boundsMin.data[2];
    g.boundsRadius = std::max(0.05f, 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz));
    g.material = QR_NO_MATERIAL;
    g.area = m.area;
    g.meanEmiss = 1.0f;
    g.angleInner = 0.0f;
    g.angleOuter = 0.0f;
    g.projector = 0.0f;
    g.reach = reach;
    const float lum = 0.2126f * color[0] + 0.7152f * color[1] + 0.0722f * color[2];
    g.estimatedPower = m.area * lum;
    g.memberBase = memberBase;
    g.memberCount = 1u;
    return g;
}

class EmissiveBenchmarkRunner
{
public:
    explicit EmissiveBenchmarkRunner(const BenchmarkArgs &args)
        : m_args(args)
    {
    }

    ~EmissiveBenchmarkRunner()
    {
        Shutdown();
    }

    EmissiveBenchmarkRunner(const EmissiveBenchmarkRunner &) = delete;
    EmissiveBenchmarkRunner &operator=(const EmissiveBenchmarkRunner &) = delete;

    bool Init()
    {
        if (!m_window.Create())
        {
            return false;
        }
        if (!CreateInstanceAndHooks())
        {
            return false;
        }
        if (!UploadStaticBenchmarkScene())
        {
            return false;
        }
        return true;
    }

    bool RunSequence(std::vector<RgbaImage> &outFrames)
    {
        outFrames.clear();
        for (uint32_t f = 0; f < kTotalFrames; f++)
        {
            if (!DrawOneFrame(f))
            {
                return false;
            }
        }
        if (!m_gpuContext.pDevice->waitForIdle())
        {
            std::fprintf(stderr, "waitForIdle failed\n");
            return false;
        }
        return ReadCapturedFrames(outFrames);
    }

    bool SavePublicReadbackCheck(RgbaImage &outFrame32)
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t rowPitch = 0;
        uint64_t bufferSize = 0;
        if (!CheckQr(qrGetFrameReadbackRequirements(m_instance, QR_FRAME_READBACK_FORMAT_RGBA8_UNORM,
                                                    QR_TRUE, &width, &height, &rowPitch, &bufferSize),
                     "readback requirements"))
        {
            return false;
        }

        std::vector<uint8_t> pixels(size_t(bufferSize), 0u);
        uint64_t reqId = 0;
        bool requested = false;
        for (uint32_t extra = 0; extra < 4u; extra++)
        {
            const uint32_t f = kTotalFrames + extra;
            QrStartFrameInfo start = {};
            start.presentMode = QR_PRESENT_MODE_IMMEDIATE;
            start.maxFrameLatency = 1;
            start.hasFocus = 1;
            if (!CheckQr(qrStartFrame(m_instance, &start), "readback start") ||
                !CheckQr(qrAcquireFrame(m_instance), "readback acquire"))
            {
                return false;
            }
            if (!requested)
            {
                QrFrameReadbackInfo rb = {};
                rb.width = width;
                rb.height = height;
                rb.rowPitch = rowPitch;
                rb.bufferSize = bufferSize;
                rb.format = QR_FRAME_READBACK_FORMAT_RGBA8_UNORM;
                rb.includeUi = QR_TRUE;
                rb.pDst = pixels.data();
                rb.frameId = QR_FRAME_ID_LATEST;
                if (CheckQr(qrRequestFrameReadback(m_instance, &rb, &reqId), "request readback"))
                {
                    requested = true;
                }
            }
            if (!UploadFrameLights(false) || !SubmitDrawFrame(f, 0.0f))
            {
                return false;
            }
            QrBool32 done = QR_FALSE;
            if (requested &&
                CheckQr(qrGetFrameReadbackStatus(m_instance, reqId, &done), "readback status") &&
                done == QR_TRUE)
            {
                outFrame32.width = width;
                outFrame32.height = height;
                outFrame32.rgb.resize(size_t(width) * height * 3u);
                for (uint32_t y = 0; y < height; y++)
                {
                    DecodeTextureRow(nvrhi::Format::RGBA8_UNORM,
                                     pixels.data() + size_t(y) * rowPitch, width,
                                     outFrame32.rgb.data() + size_t(y) * width * 3u);
                }
                return true;
            }
        }
        std::fprintf(stderr, "public readback did not complete\n");
        return false;
    }

private:
    static bool ShouldCaptureFrame(uint32_t frameIdx)
    {
        return frameIdx == 0u || frameIdx == 5u || (frameIdx >= 23u && frameIdx <= 31u);
    }

    static void QRCONV FrameCallback(const qray::QrayGpuFrameCallbackInfo *pInfo, void *pUserData)
    {
        auto *self = static_cast<EmissiveBenchmarkRunner *>(pUserData);
        if (self == nullptr || pInfo == nullptr || pInfo->pCommandList == nullptr ||
            pInfo->phase != qray::QrayGpuFramePhase::BeforePresent || pInfo->pOutputColor == nullptr)
        {
            return;
        }
        self->CaptureAlbedoTexture(pInfo->pCommandList, pInfo->pOutputColor);
    }

    void CaptureAlbedoTexture(nvrhi::ICommandList *cmd, nvrhi::ITexture *albedo)
    {
        if (!ShouldCaptureFrame(m_activeFrame))
        {
            return;
        }
        const nvrhi::TextureDesc &srcDesc = albedo->getDesc();
        nvrhi::TextureDesc stagingDesc = srcDesc;
        stagingDesc.isRenderTarget = false;
        stagingDesc.isUAV = false;
        stagingDesc.debugName = "EmissiveBenchmarkStaging";
        nvrhi::StagingTextureHandle staging =
            m_gpuContext.pDevice->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
        if (staging == nullptr)
        {
            return;
        }

        cmd->copyTexture(staging, nvrhi::TextureSlice(), albedo, nvrhi::TextureSlice());
        cmd->setTextureState(albedo, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        m_capturedSlots.push_back({ m_activeFrame + 1u, staging, srcDesc.format,
                                    srcDesc.width, srcDesc.height });
    }

    bool CreateInstanceAndHooks()
    {
        const std::filesystem::path bnCandidate =
            std::filesystem::path(m_args.textureDir) / "BlueNoise_LDR_RGBA_128.png";
        m_blueNoisePath = bnCandidate.string();

#ifdef _WIN32
        QrWin32SurfaceCreateInfo surface = {
            .hinstance = GetModuleHandleW(nullptr),
            .hwnd = m_window.handle,
        };
#else
        QrXlibSurfaceCreateInfo surface = {
            .dpy = m_window.display,
            .window = m_window.handle,
        };
#endif
        QrInstanceCreateInfo info = {};
        info.pAppName = "QuakeRay Emissive Denoiser Benchmark";
        info.pAppGUID = "4b9c7161-5f2a-43e1-93ab-98ef41b8d302";
#ifdef _WIN32
        info.pWin32SurfaceInfo = &surface;
#else
        info.pXlibSurfaceCreateInfo = &surface;
#endif
        info.pfnPrint = PrintMessage;
        info.pfnOpenFile = OpenFile;
        info.pfnCloseFile = CloseFile;
        info.pShaderFolderPath = m_args.shaderDir.c_str();
        info.pBlueNoiseFilePath = m_blueNoisePath.c_str();
        info.primaryRaysMaxAlbedoLayers = 2;
        info.indirectIlluminationMaxAlbedoLayers = 1;
        info.rayCullBackFacingTriangles = 1;
        info.allowGeometryWithSkyFlag = 1;
        info.rasterizedMaxVertexCount = 1u << 16;
        info.rasterizedMaxIndexCount = 1u << 17;
        info.rasterizedVertexColorGamma = 1;
        info.rasterizedSkyCubemapSize = 64;
        info.maxTextureCount = 256;
        info.textureSamplerForceMinificationFilterLinear = 1;
        info.textureSamplerForceNormalMapFilterLinear = 1;
        info.originalAlbedoAlphaTextureIsSRGB = 1;
        info.overridenAlbedoAlphaTextureIsSRGB = 1;

        const nvrhi::Feature reqFeature = nvrhi::Feature::RayTracingPipeline;
        const char *reqInstExt = VK_KHR_SURFACE_EXTENSION_NAME;
        const char *reqDevExt = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        qray::QrayGpuRequirements reqs = {};
        reqs.header = { sizeof(qray::QrayGpuRequirements), qray::kGpuContextContractVersion };
        reqs.minVulkanApiVersion = VK_API_VERSION_1_3;
        reqs.minimumFramesInFlight = 1;
        reqs.requiredNvrhiFeatures = { &reqFeature, 1 };
        reqs.requiredInstanceExtensions = { &reqInstExt, 1 };
        reqs.requiredDeviceExtensions = { &reqDevExt, 1 };

        if (!CheckQr(qray::qrCreateInstanceWithGpuRequirements(&info, &reqs, &m_instance),
                     "create benchmark instance"))
        {
            return false;
        }
        m_gpuContext = {};
        m_gpuContext.header = { sizeof(qray::QrayGpuContext), qray::kGpuContextContractVersion };
        if (!CheckQr(qray::qrGetGpuContext(m_instance, &m_gpuContext), "get gpu context"))
        {
            return false;
        }

        QrSpatialConvention conv = {};
        conv.size = sizeof(QrSpatialConvention);
        conv.version = QR_SPATIAL_CONVENTION_VERSION;
        conv.sceneXAxis = QR_AXIS_POSITIVE_X;
        conv.sceneYAxis = QR_AXIS_POSITIVE_Y;
        conv.sceneZAxis = QR_AXIS_POSITIVE_Z;
        conv.hostIsRightHanded = QR_TRUE;
        if (!CheckQr(qrSetSpatialConvention(m_instance, &conv), "set spatial convention"))
        {
            return false;
        }

        qray::QrayGpuFrameHooks hooks = {};
        hooks.header = { sizeof(qray::QrayGpuFrameHooks), qray::kGpuContextContractVersion };
        hooks.pfnCallback = &EmissiveBenchmarkRunner::FrameCallback;
        hooks.pUserData = this;
        if (!CheckQr(qray::qrSetGpuFrameHooks(m_instance, &hooks), "set gpu frame hooks"))
        {
            return false;
        }

        const uint8_t white[16] = {
            255, 255, 255, 255, 255, 255, 255, 255,
            255, 255, 255, 255, 255, 255, 255, 255,
        };
        QrMaterialCreateInfo matInfo = {};
        matInfo.size = { 2, 2 };
        matInfo.textures.pDataAlbedoAlpha = white;
        matInfo.filter = QR_SAMPLER_FILTER_LINEAR;
        matInfo.addressModeU = QR_SAMPLER_ADDRESS_MODE_REPEAT;
        matInfo.addressModeV = QR_SAMPLER_ADDRESS_MODE_REPEAT;
        return CheckQr(qrCreateMaterial(m_instance, &matInfo, &m_whiteMat), "create white material");
    }

    bool UploadRoomShell()
    {
        const float floorC[3] = { 0.65f, 0.65f, 0.65f };
        const float rightC[3] = { 0.66f, 0.62f, 0.56f };
        const float leftC[3] = { 0.60f, 0.63f, 0.68f };
        const float wallC[3] = { 0.54f, 0.54f, 0.56f };
        const float ceilC[3] = { 0.42f, 0.42f, 0.44f };

        const float floorPts[4][3] = {
            { -2.4f, -0.5f, 0.0f }, { 2.4f, -0.5f, 0.0f },
            { 2.4f, 7.5f, 0.0f },   { -2.4f, 7.5f, 0.0f },
        };
        const float floorN[3] = { 0.0f, 0.0f, 1.0f };

        const float ceilPts[4][3] = {
            { -2.4f, 7.5f, 3.2f },  { 2.4f, 7.5f, 3.2f },
            { 2.4f, -0.5f, 3.2f },  { -2.4f, -0.5f, 3.2f },
        };
        const float ceilN[3] = { 0.0f, 0.0f, -1.0f };

        const float leftPts[4][3] = {
            { -2.4f, -0.5f, 0.0f }, { -2.4f, 7.5f, 0.0f },
            { -2.4f, 7.5f, 3.2f },  { -2.4f, -0.5f, 3.2f },
        };
        const float leftN[3] = { 1.0f, 0.0f, 0.0f };

        const float rightPts[4][3] = {
            { 2.4f, 7.5f, 0.0f },   { 2.4f, -0.5f, 0.0f },
            { 2.4f, -0.5f, 3.2f },  { 2.4f, 7.5f, 3.2f },
        };
        const float rightN[3] = { -1.0f, 0.0f, 0.0f };

        const float backPts[4][3] = {
            { -2.4f, 7.5f, 0.0f },  { 2.4f, 7.5f, 0.0f },
            { 2.4f, 7.5f, 3.2f },   { -2.4f, 7.5f, 3.2f },
        };
        const float backN[3] = { 0.0f, -1.0f, 0.0f };

        const float frontPts[4][3] = {
            { 2.4f, -0.5f, 0.0f },  { -2.4f, -0.5f, 0.0f },
            { -2.4f, -0.5f, 3.2f }, { 2.4f, -0.5f, 3.2f },
        };
        const float frontN[3] = { 0.0f, 1.0f, 0.0f };

        return UploadStaticQuad(m_instance, m_whiteMat, 1, floorPts, floorN, floorC, 0.52f, 0.0f, 0.0f, 0u) &&
               UploadStaticQuad(m_instance, m_whiteMat, 2, ceilPts, ceilN, ceilC, 0.85f, 0.0f, 0.0f, 0u) &&
               UploadStaticQuad(m_instance, m_whiteMat, 3, leftPts, leftN, leftC, 0.28f, 0.25f, 0.0f, 0u) &&
               UploadStaticQuad(m_instance, m_whiteMat, 4, rightPts, rightN, rightC, 0.78f, 0.0f, 0.0f, 0u) &&
               UploadStaticQuad(m_instance, m_whiteMat, 5, backPts, backN, wallC, 0.72f, 0.0f, 0.0f, 0u) &&
               UploadStaticQuad(m_instance, m_whiteMat, 6, frontPts, frontN, wallC, 0.85f, 0.0f, 0.0f, 0u);
    }

    bool UploadFixturesAndEmitters()
    {
        const float pillarMin[3] = { -0.55f, 2.95f, 0.0f };
        const float pillarMax[3] = { -0.05f, 3.55f, 0.98f };
        const float pillarCol[3] = { 0.20f, 0.20f, 0.22f };
        if (!UploadBox(m_instance, m_whiteMat, 10, pillarMin, pillarMax, pillarCol, 0.82f, 0.0f))
        {
            return false;
        }

        // Baffle blocking direct left-DTAL illumination onto the back-right indirect bounce zone.
        const float baffleMin[3] = { 0.15f, 4.75f, 0.0f };
        const float baffleMax[3] = { 0.35f, 7.10f, 2.35f };
        const float baffleCol[3] = { 0.44f, 0.44f, 0.48f };
        if (!UploadBox(m_instance, m_whiteMat, 20, baffleMin, baffleMax, baffleCol, 0.75f, 0.0f))
        {
            return false;
        }

        // Visible backing cards for the 2 DTAL emitters (not EMISSION_RAY_ONLY so they aren't double-counted).
        const float mainLampPts[4][3] = {
            { -2.36f, 3.85f, 1.72f }, { -2.36f, 4.30f, 1.72f },
            { -2.36f, 4.30f, 2.08f }, { -2.36f, 3.85f, 2.08f },
        };
        const float rightLampPts[4][3] = {
            { 2.36f, 2.72f, 0.16f }, { 2.36f, 2.36f, 0.16f },
            { 2.36f, 2.36f, 0.40f }, { 2.36f, 2.72f, 0.40f },
        };
        const float leftN[3] = { 1.0f, 0.0f, 0.0f };
        const float rightN[3] = { -1.0f, 0.0f, 0.0f };
        const float lampAlb[3] = { 1.0f, 0.85f, 0.60f };

        if (!UploadStaticQuad(m_instance, m_whiteMat, 30, mainLampPts, leftN, lampAlb, 0.4f, 0.0f, 0.8f, 0u) ||
            !UploadStaticQuad(m_instance, m_whiteMat, 31, rightLampPts, rightN, lampAlb, 0.4f, 0.0f, 0.8f, 0u))
        {
            return false;
        }

        // Small intense EMISSION_RAY_ONLY bounce emitter lighting the indirect-only back-right bay.
        const float rayOnlyPts[4][3] = {
            { 0.36f, 5.20f, 1.35f }, { 0.36f, 5.44f, 1.35f },
            { 0.36f, 5.44f, 1.54f }, { 0.36f, 5.20f, 1.54f },
        };
        const float rayOnlyCol[3] = { 1.0f, 0.72f, 0.38f };
        return UploadStaticQuad(m_instance, m_whiteMat, 32, rayOnlyPts, leftN, rayOnlyCol,
                                0.35f, 0.0f, 1.0f, QR_GEOMETRY_UPLOAD_EMISSION_RAY_ONLY_BIT);
    }

    bool UploadStaticBenchmarkScene()
    {
        if (!CheckQr(qrBeginStaticGeometries(m_instance), "begin static"))
        {
            return false;
        }
        if (!UploadRoomShell() || !UploadFixturesAndEmitters())
        {
            return false;
        }
        if (!CheckQr(qrSubmitStaticGeometries(m_instance), "submit static"))
        {
            return false;
        }

        const QrFloat3D cMins[2] = {
            { { -2.5f, -0.6f, -0.1f } },
            { { -2.5f, -0.6f, -0.1f } },
        };
        const QrFloat3D cMaxs[2] = {
            { { 2.5f, 7.6f, 3.3f } },
            { { 2.5f, 7.6f, 3.3f } },
        };
        const uint8_t cFlags[2] = { 0u, 0u };
        QrWorldLightsUploadInfo world = {};
        world.numClusters = 2;
        world.pClusterMins = cMins;
        world.pClusterMaxs = cMaxs;
        world.pClusterFlags = cFlags;
        world.clusterGeneration = 1;
        return CheckQr(qrUploadWorldLights(m_instance, &world), "upload world lights");
    }

    bool UploadFrameLights(bool submitClusterSources)
    {
        const float mainP0[3] = { -2.35f, 3.85f, 1.72f };
        const float mainP1[3] = { -2.35f, 4.30f, 1.72f };
        const float mainP3[3] = { -2.35f, 3.85f, 2.08f };
        const float mainN[3] = { 1.0f, 0.0f, 0.0f };
        const float mainCol[3] = { 48.0f, 36.0f, 22.0f };

        const float nearP0[3] = { 2.35f, 2.72f, 0.16f };
        const float nearP1[3] = { 2.35f, 2.36f, 0.16f };
        const float nearP3[3] = { 2.35f, 2.72f, 0.40f };
        const float nearN[3] = { -1.0f, 0.0f, 0.0f };
        const float nearCol[3] = { 34.0f, 24.0f, 14.0f };

        std::array<QrDtalMemberUpload, 2> members = {
            MakeDtalQuadMember(mainP0, mainP1, mainP3, mainN),
            MakeDtalQuadMember(nearP0, nearP1, nearP3, nearN),
        };
        std::array<QrDtalGroupUploadInfo, 2> groups = {
            MakeDtalGroupInfo(1001ull, 0u, members[0], mainCol, mainN, 12.0f),
            MakeDtalGroupInfo(1002ull, 1u, members[1], nearCol, nearN, 4.5f),
        };

        QrDtalGroupUploadBatch batch = {};
        batch.groupCount = 2u;
        batch.pGroups = groups.data();
        batch.memberCount = 2u;
        batch.pMembers = members.data();
        if (!CheckQr(qrUploadDtalGroups(m_instance, &batch), "upload dtal groups"))
        {
            return false;
        }

        if (!submitClusterSources)
        {
            return true;
        }

        std::array<QrClusterLightSource, 2> sources = {};
        for (size_t i = 0; i < 2; i++)
        {
            sources[i].uniqueID = groups[i].uniqueID;
            sources[i].origin = groups[i].center;
            sources[i].cluster = 1u;
            sources[i].reach = groups[i].reach;
            sources[i].radius = groups[i].boundsRadius;
            sources[i].power = groups[i].estimatedPower;
        }
        QrClusterLightSourcesUploadInfo clInfo = {};
        clInfo.numLights = 2u;
        clInfo.pLights = sources.data();
        clInfo.topUpReach = 12.0f;
        clInfo.allowIncremental = 1;
        clInfo.allowOverflow = 1;
        return CheckQr(qrUploadClusterLightSources(m_instance, &clInfo), "upload cluster light sources");
    }

    bool SubmitDrawFrame(uint32_t frameIdx, float camX)
    {
        const float eye[3] = { camX, 0.35f, 0.62f };
        const float target[3] = { 0.15f, 4.60f, 0.62f };

        QrDrawFrameRenderResolutionParams res = {};
        res.upscaleTechnique = QR_RENDER_UPSCALE_TECHNIQUE_NEAREST;
        res.sharpenTechnique = QR_RENDER_SHARPEN_TECHNIQUE_NONE;
        res.resolutionMode = QR_RENDER_RESOLUTION_MODE_CUSTOM;
        res.customRenderSize = { kWidth, kHeight };

        QrDrawFrameSkyParams sky = {};
        sky.skyType = QR_SKY_TYPE_COLOR;
        sky.skyColorDefault = { 0.0f, 0.0f, 0.0f };
        sky.skyColorMultiplier = 0.0f;
        sky.skyLightMultiplier = 0.0f;
        sky.skyNee = QR_FALSE;

        QrDrawFrameTexturesParams tex = {};
        tex.dynamicSamplerFilter = QR_SAMPLER_FILTER_LINEAR;
        tex.normalMapStrength = 1.0f;
        tex.emissionMapBoost = 110.0f;
        tex.emissionMaxScreenColor = 2.0f;
        tex.emissionSharpMask = 1.0f;
        tex.squareInputRoughness = QR_FALSE;
        tex.minRoughness = 0.02f;
        tex.emissionBlendStrength = 1.0f;
        std::fill(std::begin(tex.lightStyleScales), std::end(tex.lightStyleScales), 1.0f);

        QrDrawFrameTonemappingParams tm = {};
        tm.minLogLuminance = -8.0f;
        tm.maxLogLuminance = 4.0f;
        tm.luminanceWhitePoint = 6.0f;
        tm.exposureBias = 0.0f;
        tm.tonemapPower = 1.0f;
        tm.highlightDesaturation = 0.0f;
        tm.chromaPreservation = 1.0f;
        tm.exposureSpeedUp = 0.0f;
        tm.exposureSpeedDown = 0.0f;
        tm.exposureLowPercentile = 0.0f;
        tm.exposureHighPercentile = 100.0f;
        tm.minAdaptedLuminance = 0.25f;
        tm.maxAdaptedLuminance = 0.25f;
        tm.tonemapType = 0;

        QrDrawFrameIlluminationParams illum = {};
        illum.cellWorldSize = 2.0f;
        illum.giBounceRays = 1.0f;
        illum.enableSecondBounceForIndirect = QR_FALSE;
        illum.neeLightSamples = 1u;
        illum.restirEnabled = QR_FALSE;
        illum.restirCandidates = 8u;
        illum.denoiserEnabled = QR_TRUE;
        illum.directDiffuseSensitivityToChange = 0.5f;
        illum.indirectDiffuseSensitivityToChange = 0.5f;
        illum.specularSensitivityToChange = 0.5f;
        illum.q2DepthGradMode = 1u;
        illum.q2LightStatsMode = 1u;
        illum.reflRefrEarlyOut = QR_TRUE;

        QrDrawFrameInfo draw = {};
        LookAtRhZUp(eye, target, draw.view);
        draw.worldUpVector = { { 0.0f, 0.0f, 1.0f } };
        draw.fovYRadians = 1.0471975512f;
        draw.cameraNear = 0.05f;
        draw.cameraFar = 100.0f;
        draw.rayLength = 100.0f;
        draw.rayCullMaskWorld = QR_DRAW_FRAME_RAY_CULL_WORLD_0_BIT | QR_DRAW_FRAME_RAY_CULL_SKY_BIT;
        draw.currentTime = double(frameIdx) / 60.0;
        draw.disableEyeAdaptation = 1;
        draw.forceAntiFirefly = QR_FALSE;
        draw.pRenderResolutionParams = &res;
        draw.pSkyParams = &sky;
        draw.pTexturesParams = &tex;
        draw.pTonemappingParams = &tm;
        draw.pIlluminationParams = &illum;
        return CheckQr(qrDrawFrame(m_instance, &draw), "draw benchmark frame");
    }

    bool DrawOneFrame(uint32_t frameIdx)
    {
        m_activeFrame = frameIdx;
        QrStartFrameInfo start = {};
        start.presentMode = QR_PRESENT_MODE_IMMEDIATE;
        start.maxFrameLatency = 1;
        start.hasFocus = 1;
        if (!CheckQr(qrStartFrame(m_instance, &start), "start frame") ||
            !CheckQr(qrAcquireFrame(m_instance), "acquire frame"))
        {
            return false;
        }
        const float camX = (frameIdx < 8u) ? (-0.35f + 0.05f * float(frameIdx)) : 0.0f;
        if (!UploadFrameLights(true))
        {
            return false;
        }
        return SubmitDrawFrame(frameIdx, camX);
    }

    bool ReadCapturedFrames(std::vector<RgbaImage> &outFrames)
    {
        outFrames.resize(m_capturedSlots.size());
        for (size_t i = 0; i < m_capturedSlots.size(); i++)
        {
            const CapturedSlot &slot = m_capturedSlots[i];
            size_t rowPitch = 0;
            const void *mapped = m_gpuContext.pDevice->mapStagingTexture(
                slot.staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch);
            if (mapped == nullptr)
            {
                std::fprintf(stderr, "mapStagingTexture failed for frame %u\n", slot.frameNumber);
                return false;
            }

            RgbaImage &img = outFrames[i];
            img.width = slot.width;
            img.height = slot.height;
            img.rgb.resize(size_t(slot.width) * slot.height * 3u);
            const auto *bytes = static_cast<const uint8_t *>(mapped);
            for (uint32_t y = 0; y < slot.height; y++)
            {
                DecodeTextureRow(slot.format, bytes + size_t(y) * rowPitch, slot.width,
                                 img.rgb.data() + size_t(y) * slot.width * 3u);
            }
            m_gpuContext.pDevice->unmapStagingTexture(slot.staging);
        }
        m_capturedSlots.clear();
        return true;
    }

    void Shutdown()
    {
        m_capturedSlots.clear();
        if (m_instance != nullptr)
        {
            qray::qrSetGpuFrameHooks(m_instance, nullptr);
            if (m_whiteMat != QR_NO_MATERIAL)
            {
                qrDestroyMaterial(m_instance, m_whiteMat);
                m_whiteMat = QR_NO_MATERIAL;
            }
            qrDestroyInstance(m_instance);
            m_instance = nullptr;
        }
    }

    BenchmarkArgs m_args;
    TestWindow m_window;
    std::string m_blueNoisePath;
    QrInstance m_instance = nullptr;
    qray::QrayGpuContext m_gpuContext = {};
    QrMaterial m_whiteMat = QR_NO_MATERIAL;
    uint32_t m_activeFrame = 0;
    std::vector<CapturedSlot> m_capturedSlots;
};

double ComputeLocal5x5Variance(const RgbaImage &img, const RoiRect &roi)
{
    double sumVar = 0.0;
    uint64_t count = 0;
    for (uint32_t y = std::max(2u, roi.y0); y + 2u < std::min(img.height, roi.y1); y++)
    {
        for (uint32_t x = std::max(2u, roi.x0); x + 2u < std::min(img.width, roi.x1); x++)
        {
            double s1 = 0.0;
            double s2 = 0.0;
            for (int dy = -2; dy <= 2; dy++)
            {
                for (int dx = -2; dx <= 2; dx++)
                {
                    const double l = double(PixelLuminance(img, uint32_t(int(x) + dx),
                                                             uint32_t(int(y) + dy)));
                    s1 += l;
                    s2 += l * l;
                }
            }
            const double mean = s1 / 25.0;
            sumVar += std::max(0.0, s2 / 25.0 - mean * mean);
            count++;
        }
    }
    return count > 0 ? (sumVar / double(count)) : 0.0;
}

double ComputeFireflyRatio(const RgbaImage &img, const RoiRect &roi)
{
    std::vector<double> lums;
    lums.reserve(size_t(roi.x1 - roi.x0) * (roi.y1 - roi.y0));
    for (uint32_t y = roi.y0; y < std::min(img.height, roi.y1); y++)
    {
        for (uint32_t x = roi.x0; x < std::min(img.width, roi.x1); x++)
        {
            lums.push_back(double(PixelLuminance(img, x, y)));
        }
    }
    if (lums.empty())
    {
        return 0.0;
    }
    std::sort(lums.begin(), lums.end());
    const double median = lums[lums.size() / 2u];
    const size_t idx995 = std::min(lums.size() - 1u, size_t(double(lums.size() - 1u) * 0.995));
    return lums[idx995] / std::max(median, 1e-4);
}

double ComputeTemporalFlickerRmse(const std::vector<RgbaImage> &frames, size_t firstIdx,
                                  size_t lastIdx, const RoiRect &roi)
{
    double totalRmse = 0.0;
    uint32_t pairs = 0;
    for (size_t f = firstIdx; f < lastIdx; f++)
    {
        double sqSum = 0.0;
        uint64_t count = 0;
        for (uint32_t y = roi.y0; y < std::min(frames[f].height, roi.y1); y++)
        {
            for (uint32_t x = roi.x0; x < std::min(frames[f].width, roi.x1); x++)
            {
                const double d = double(PixelLuminance(frames[f + 1u], x, y)) -
                                 double(PixelLuminance(frames[f], x, y));
                sqSum += d * d;
                count++;
            }
        }
        if (count > 0)
        {
            totalRmse += std::sqrt(sqSum / double(count));
            pairs++;
        }
    }
    return pairs > 0 ? (totalRmse / double(pairs)) : 0.0;
}

double ComputeContactEdgeGradient(const RgbaImage &img, const RoiRect &roi)
{
    double sumPeakGrad = 0.0;
    uint32_t rows = 0;
    for (uint32_t y = std::max(1u, roi.y0); y + 1u < std::min(img.height, roi.y1); y++)
    {
        double rowMaxGrad = 0.0;
        for (uint32_t x = std::max(1u, roi.x0); x + 1u < std::min(img.width, roi.x1); x++)
        {
            const double gx = 0.5 * (double(PixelLuminance(img, x + 1u, y)) -
                                     double(PixelLuminance(img, x - 1u, y)));
            const double gy = 0.5 * (double(PixelLuminance(img, x, y + 1u)) -
                                     double(PixelLuminance(img, x, y - 1u)));
            rowMaxGrad = std::max(rowMaxGrad, std::sqrt(gx * gx + gy * gy));
        }
        sumPeakGrad += rowMaxGrad;
        rows++;
    }
    return rows > 0 ? (sumPeakGrad / double(rows)) : 0.0;
}

double ComputeGridRingingEnergy(const RgbaImage &img, const RoiRect &roi)
{
    double energy = 0.0;
    uint64_t count = 0;
    for (uint32_t y = std::max(8u, roi.y0); y + 8u < std::min(img.height, roi.y1); y++)
    {
        for (uint32_t x = std::max(8u, roi.x0); x + 8u < std::min(img.width, roi.x1); x++)
        {
            const double c = double(PixelLuminance(img, x, y));
            const double lapX4 = double(PixelLuminance(img, x - 4u, y)) +
                                 double(PixelLuminance(img, x + 4u, y)) - 2.0 * c;
            const double lapY4 = double(PixelLuminance(img, x, y - 4u)) +
                                 double(PixelLuminance(img, x, y + 4u)) - 2.0 * c;
            const double lapX8 = double(PixelLuminance(img, x - 8u, y)) +
                                 double(PixelLuminance(img, x + 8u, y)) - 2.0 * c;
            const double lapY8 = double(PixelLuminance(img, x, y - 8u)) +
                                 double(PixelLuminance(img, x, y + 8u)) - 2.0 * c;
            const double fineLapX = double(PixelLuminance(img, x - 1u, y)) +
                                    double(PixelLuminance(img, x + 1u, y)) - 2.0 * c;
            const double fineLapY = double(PixelLuminance(img, x, y - 1u)) +
                                    double(PixelLuminance(img, x, y + 1u)) - 2.0 * c;
            energy += std::abs(lapX4 - 16.0 * fineLapX) * 0.02 +
                      std::abs(lapY4 - 16.0 * fineLapY) * 0.02 +
                      std::abs(lapX8) + std::abs(lapY8);
            count++;
        }
    }
    return count > 0 ? (energy / double(count)) : 0.0;
}

BenchmarkMetrics ComputeAllMetrics(const std::vector<RgbaImage> &frames)
{
    // Captured order: [0]=f1, [1]=f6, [2..10]=f24..f32.
    const RgbaImage &f06 = frames[1];
    const RgbaImage &f32 = frames.back();

    const RoiRect litFloorAndWall = { 210, 150, 610, 345 };
    const RoiRect indirectBounceRoi = { 342, 85, 414, 212 };
    const RoiRect grazingRoi = { 355, 225, 625, 350 };
    const RoiRect contactEdgeRoi = { 225, 215, 340, 340 };
    const RoiRect smoothFalloffRoi = { 415, 130, 615, 305 };

    BenchmarkMetrics m;
    m.spatialVarianceHfLf = ComputeLocal5x5Variance(f32, litFloorAndWall);
    m.fireflyMaxOverMedian = ComputeFireflyRatio(f32, indirectBounceRoi);
    m.temporalFlickerRmse = ComputeTemporalFlickerRmse(frames, 2u, 10u, litFloorAndWall);
    m.contactEdgeGradient = ComputeContactEdgeGradient(f32, contactEdgeRoi);
    m.motionSpatialVarianceF06 = ComputeLocal5x5Variance(f06, litFloorAndWall);
    m.grazingFlickerRmse = ComputeTemporalFlickerRmse(frames, 2u, 10u, grazingRoi);
    m.gridRingingEnergy = ComputeGridRingingEnergy(f32, smoothFalloffRoi);
    return m;
}

void WriteReportsAndImages(const BenchmarkArgs &args, const std::vector<RgbaImage> &frames,
                           const BenchmarkMetrics &m)
{
    if (args.outDir.empty())
    {
        return;
    }

    const std::filesystem::path dir(args.outDir);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    WriteLinearImagePng(dir / "frame_01_cold.png", frames[0]);
    WriteLinearImagePng(dir / "frame_06_motion.png", frames[1]);
    WriteLinearImagePng(dir / "frame_32_static.png", frames.back());

    std::ostringstream json;
    json << std::fixed << std::setprecision(8)
         << "{\n"
         << "  \"step\": \"" << args.stepName << "\",\n"
         << "  \"spatial_variance_hf_lf\": " << m.spatialVarianceHfLf << ",\n"
         << "  \"firefly_max_over_median\": " << m.fireflyMaxOverMedian << ",\n"
         << "  \"temporal_flicker_rmse\": " << m.temporalFlickerRmse << ",\n"
         << "  \"contact_edge_gradient\": " << m.contactEdgeGradient << ",\n"
         << "  \"motion_spatial_variance_f06\": " << m.motionSpatialVarianceF06 << ",\n"
         << "  \"grazing_flicker_rmse\": " << m.grazingFlickerRmse << ",\n"
         << "  \"grid_ringing_energy\": " << m.gridRingingEnergy << "\n"
         << "}\n";

    std::ofstream jf(dir / "metrics.json", std::ios::trunc);
    if (jf)
    {
        jf << json.str();
    }

    std::ofstream tf(dir / "metrics.txt", std::ios::trunc);
    if (tf)
    {
        tf << std::fixed << std::setprecision(6)
           << "step=" << args.stepName << "\n"
           << "spatial_variance_hf_lf=" << m.spatialVarianceHfLf << "\n"
           << "firefly_max_over_median=" << m.fireflyMaxOverMedian << "\n"
           << "temporal_flicker_rmse=" << m.temporalFlickerRmse << "\n"
           << "contact_edge_gradient=" << m.contactEdgeGradient << "\n"
           << "motion_spatial_variance_f06=" << m.motionSpatialVarianceF06 << "\n"
           << "grazing_flicker_rmse=" << m.grazingFlickerRmse << "\n"
           << "grid_ringing_energy=" << m.gridRingingEnergy << "\n";
    }
}

bool ParseArgs(int argc, char **argv, BenchmarkArgs &out)
{
    if (argc < 3)
    {
        std::fprintf(stderr,
                     "usage: qray_emissive_denoiser_benchmark <BuildDir> <TextureDir> "
                     "[--out-dir <dir>] [--step <name>]\n");
        return false;
    }
    out.shaderDir = argv[1];
    if (!out.shaderDir.empty() && out.shaderDir.back() != '/' && out.shaderDir.back() != '\\')
    {
        out.shaderDir.push_back('/');
    }
    out.textureDir = argv[2];
    for (int i = 3; i + 1 < argc; i++)
    {
        const std::string arg = argv[i];
        if (arg == "--out-dir")
        {
            out.outDir = argv[++i];
        }
        else if (arg == "--step")
        {
            out.stepName = argv[++i];
        }
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    BenchmarkArgs args;
    if (!ParseArgs(argc, argv, args))
    {
        return 1;
    }

    EmissiveBenchmarkRunner runner(args);
    if (!runner.Init())
    {
        return 1;
    }

    std::vector<RgbaImage> frames;
    if (!runner.RunSequence(frames) || frames.size() != 11u)
    {
        std::fprintf(stderr, "benchmark frame sequence failed (captured=%zu)\n", frames.size());
        return 1;
    }

    RgbaImage readbackCheck;
    if (!runner.SavePublicReadbackCheck(readbackCheck))
    {
        return 1;
    }

    const BenchmarkMetrics m = ComputeAllMetrics(frames);
    if (!(m.contactEdgeGradient > 1e-4) || !std::isfinite(m.spatialVarianceHfLf) ||
        !std::isfinite(m.fireflyMaxOverMedian) || !std::isfinite(m.temporalFlickerRmse))
    {
        std::fprintf(stderr, "invalid benchmark metrics computed\n");
        return 1;
    }

    WriteReportsAndImages(args, frames, m);
    std::printf(
        "PASS [%s]: spatial_var=%.6f firefly=%.4f flicker_rmse=%.6f contact_grad=%.6f "
        "motion_var_f06=%.6f grazing_rmse=%.6f grid_ring=%.6f\n",
        args.stepName.c_str(), m.spatialVarianceHfLf, m.fireflyMaxOverMedian, m.temporalFlickerRmse,
        m.contactEdgeGradient, m.motionSpatialVarianceF06, m.grazingFlickerRmse,
        m.gridRingingEnergy);
    return 0;
}
