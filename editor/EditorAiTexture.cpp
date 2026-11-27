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

#include "EditorAiTexture.h"
#include "EditorAiWorkerClient.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string_view>

namespace
{

constexpr uint32_t kMinProcessingRes = 512u;
constexpr uint32_t kMaxProcessingRes = 2048u;
constexpr uint32_t kDefaultProcessingRes = 768u;
constexpr uint32_t kSeamlessHaloMaxPx = 64u;

std::string GetEnvString(const char *name)
{
    if (name == nullptr)
    {
        return {};
    }
    const char *val = std::getenv(name);
    return val != nullptr ? std::string(val) : std::string{};
}

std::string LowerAscii(std::string s)
{
    for (char &ch : s)
    {
        ch = char(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}

std::string TrimWhitespace(std::string_view sv)
{
    size_t start = 0;
    while (start < sv.size() && std::isspace(static_cast<unsigned char>(sv[start])) != 0)
    {
        start++;
    }
    size_t end = sv.size();
    while (end > start && std::isspace(static_cast<unsigned char>(sv[end - 1])) != 0)
    {
        end--;
    }
    return std::string(sv.substr(start, end - start));
}

bool IsUsableRegularFile(const std::filesystem::path &path)
{
    std::error_code ec;
    return !path.empty() && std::filesystem::is_regular_file(path, ec);
}

std::filesystem::path UserAiCacheBaseDir()
{
#if defined(_WIN32)
    std::string base = GetEnvString("LOCALAPPDATA");
    if (base.empty())
    {
        base = GetEnvString("USERPROFILE");
    }
    if (base.empty())
    {
        base = ".";
    }
    return std::filesystem::path(base) / "QuakeRay";
#else
    std::string xdgCache = GetEnvString("XDG_CACHE_HOME");
    if (!xdgCache.empty())
    {
        return std::filesystem::path(xdgCache) / "quakeray";
    }
    std::string home = GetEnvString("HOME");
    if (home.empty())
    {
        home = ".";
    }
    return std::filesystem::path(home) / ".cache" / "quakeray";
#endif
}

EditorAiBackend ParseBackendName(const std::string &raw, EditorAiBackend fallback)
{
    struct BackendAlias
    {
        const char *name;
        EditorAiBackend backend;
    };
    static constexpr BackendAlias kAliases[] = {
        { "lotus_normal_d", EditorAiBackend::LotusNormalD },
        { "lotus_normal", EditorAiBackend::LotusNormalD },
        { "lotus", EditorAiBackend::LotusNormalD },
        { "lotus_normal_g", EditorAiBackend::LotusNormalD },
        { "lotus_depth_d", EditorAiBackend::LotusDepthD },
        { "lotus_depth", EditorAiBackend::LotusDepthD },
        { "deepbump", EditorAiBackend::DeepBump },
        { "local", EditorAiBackend::DeepBump },
        { "stable_delight", EditorAiBackend::StableDelight },
        { "delight", EditorAiBackend::StableDelight },
        { "yoso_delight", EditorAiBackend::StableDelight },
        { "realplksr_pbrify", EditorAiBackend::RealPlksrPbrify },
        { "realplksr", EditorAiBackend::RealPlksrPbrify },
        { "pbrify", EditorAiBackend::RealPlksrPbrify },
        { "4x-pbrify-rplksrd-v3", EditorAiBackend::RealPlksrPbrify },
    };
    const std::string low = LowerAscii(TrimWhitespace(raw));
    for (const BackendAlias &entry : kAliases)
    {
        if (low == entry.name)
        {
            return entry.backend;
        }
    }
    return fallback;
}

struct JsonCursor
{
    const std::string &src;
    size_t pos = 0;

    void SkipWs()
    {
        while (pos < src.size() && std::isspace(static_cast<unsigned char>(src[pos])) != 0)
        {
            pos++;
        }
    }

    bool Consume(char expected)
    {
        SkipWs();
        if (pos < src.size() && src[pos] == expected)
        {
            pos++;
            return true;
        }
        return false;
    }
};

bool ParseJsonString(JsonCursor &c, std::string &out)
{
    c.SkipWs();
    if (c.pos >= c.src.size() || c.src[c.pos] != '"')
    {
        return false;
    }
    c.pos++;
    out.clear();
    while (c.pos < c.src.size())
    {
        const char ch = c.src[c.pos++];
        if (ch == '"')
        {
            return true;
        }
        if (ch != '\\')
        {
            out.push_back(ch);
            continue;
        }
        if (c.pos >= c.src.size())
        {
            return false;
        }
        const char esc = c.src[c.pos++];
        switch (esc)
        {
        case '"':
        case '\\':
        case '/':
            out.push_back(esc);
            break;
        case 'n':
            out.push_back('\n');
            break;
        case 'r':
            out.push_back('\r');
            break;
        case 't':
            out.push_back('\t');
            break;
        default:
            out.push_back(esc);
            break;
        }
    }
    return false;
}

bool ParseJsonUint(JsonCursor &c, uint32_t &outVal)
{
    c.SkipWs();
    const size_t start = c.pos;
    while (c.pos < c.src.size() && std::isdigit(static_cast<unsigned char>(c.src[c.pos])) != 0)
    {
        c.pos++;
    }
    if (c.pos == start)
    {
        return false;
    }
    const std::string token = c.src.substr(start, c.pos - start);
    char *endPtr = nullptr;
    const unsigned long parsed = std::strtoul(token.c_str(), &endPtr, 10);
    if (endPtr == nullptr || *endPtr != '\0')
    {
        return false;
    }
    outVal = uint32_t(parsed);
    return true;
}

bool SkipJsonValue(JsonCursor &c, int depth = 0);

bool SkipJsonComposite(JsonCursor &c, char closeCh, int depth)
{
    c.pos++;
    c.SkipWs();
    if (c.pos < c.src.size() && c.src[c.pos] == closeCh)
    {
        c.pos++;
        return true;
    }
    while (c.pos < c.src.size())
    {
        if (closeCh == '}')
        {
            std::string dummyKey;
            if (!ParseJsonString(c, dummyKey) || !c.Consume(':'))
            {
                return false;
            }
        }
        if (!SkipJsonValue(c, depth + 1))
        {
            return false;
        }
        if (c.Consume(','))
        {
            continue;
        }
        return c.Consume(closeCh);
    }
    return false;
}

bool SkipJsonValue(JsonCursor &c, int depth)
{
    if (depth > 16)
    {
        return false;
    }
    c.SkipWs();
    if (c.pos >= c.src.size())
    {
        return false;
    }
    const char ch = c.src[c.pos];
    if (ch == '"')
    {
        std::string dummy;
        return ParseJsonString(c, dummy);
    }
    if (ch == '{')
    {
        return SkipJsonComposite(c, '}', depth);
    }
    if (ch == '[')
    {
        return SkipJsonComposite(c, ']', depth);
    }
    const size_t start = c.pos;
    while (c.pos < c.src.size() && c.src[c.pos] != ',' && c.src[c.pos] != '}' &&
           c.src[c.pos] != ']' && std::isspace(static_cast<unsigned char>(c.src[c.pos])) == 0)
    {
        c.pos++;
    }
    return c.pos > start;
}

bool ParseLocalConfigField(const std::string &key, JsonCursor &c, EditorAiLocalConfig &outConfig)
{
    if (key == "active_backend")
    {
        std::string val;
        if (!ParseJsonString(c, val))
        {
            return false;
        }
        outConfig.activeBackend = ParseBackendName(val, outConfig.activeBackend);
        return true;
    }
    if (key == "processing_res")
    {
        uint32_t val = kDefaultProcessingRes;
        if (!ParseJsonUint(c, val))
        {
            return false;
        }
        outConfig.processingRes = std::clamp(val, kMinProcessingRes, kMaxProcessingRes);
        return true;
    }
    if (key == "models_dir")
    {
        return ParseJsonString(c, outConfig.modelsDir);
    }
    return SkipJsonValue(c);
}

bool ParseLocalConfigObject(JsonCursor &c, EditorAiLocalConfig &outConfig)
{
    if (!c.Consume('{'))
    {
        return false;
    }
    if (c.Consume('}'))
    {
        return true;
    }
    while (c.pos < c.src.size())
    {
        std::string key;
        if (!ParseJsonString(c, key) || !c.Consume(':'))
        {
            return false;
        }
        if (!ParseLocalConfigField(key, c, outConfig))
        {
            return false;
        }
        if (c.Consume(','))
        {
            continue;
        }
        return c.Consume('}');
    }
    return false;
}

void AppendEscapedJsonString(std::string &out, std::string_view text)
{
    out.push_back('"');
    for (char ch : text)
    {
        switch (ch)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out.push_back(ch);
            break;
        }
    }
    out.push_back('"');
}

std::string SerializeLocalConfigJson(const EditorAiLocalConfig &config)
{
    const uint32_t clampedRes =
        std::clamp(config.processingRes, kMinProcessingRes, kMaxProcessingRes);
    std::string json;
    json.reserve(192);
    json += "{\n  \"active_backend\": ";
    AppendEscapedJsonString(json, EditorAiBackendId(config.activeBackend));
    json += ",\n  \"processing_res\": " + std::to_string(clampedRes);
    json += ",\n  \"models_dir\": ";
    AppendEscapedJsonString(json, config.modelsDir);
    json += "\n}\n";
    return json;
}

bool WriteTextFileAtomic(const std::filesystem::path &path, const std::string &contents,
                         std::string &outError)
{
    std::error_code ec;
    if (path.has_parent_path())
    {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec)
        {
            outError = "Failed creating config directory: " + ec.message();
            return false;
        }
    }

    const std::filesystem::path tmpPath = path.string() + ".tmp";
    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            outError = "Failed opening temporary config file";
            return false;
        }
        out.write(contents.data(), std::streamsize(contents.size()));
        if (!out.good())
        {
            std::filesystem::remove(tmpPath, ec);
            outError = "Failed writing temporary config file";
            return false;
        }
    }

    std::filesystem::rename(tmpPath, path, ec);
    if (ec)
    {
        std::filesystem::remove(tmpPath, ec);
        outError = "Failed finalizing config file: " + ec.message();
        return false;
    }
    return true;
}

bool ReadRegularTextFile(const std::filesystem::path &path, std::string &outText)
{
    if (!IsUsableRegularFile(path))
    {
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return false;
    }
    outText.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return file.good() || file.eof();
}

float RgbLuminance01(uint8_t r, uint8_t g, uint8_t b)
{
    return (0.2126f * float(r) + 0.7152f * float(g) + 0.0722f * float(b)) / 255.0f;
}

uint8_t FloatToByte(float v)
{
    return uint8_t(std::clamp(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f), 0l, 255l));
}

enum class EdgeSampleMode
{
    Clamp = 0,
    Wrap = 1,
    Mirror = 2,
};

int MirrorCoord(int coord, int limit)
{
    if (limit <= 1)
    {
        return 0;
    }
    const int period = 2 * limit;
    int m = coord % period;
    if (m < 0)
    {
        m += period;
    }
    return m >= limit ? (period - 1 - m) : m;
}

int ApplyEdgeSampleMode(int coord, int limit, EdgeSampleMode mode)
{
    if (limit <= 0)
    {
        return 0;
    }
    switch (mode)
    {
    case EdgeSampleMode::Wrap:
    {
        const int rem = coord % limit;
        return rem < 0 ? rem + limit : rem;
    }
    case EdgeSampleMode::Mirror:
        return MirrorCoord(coord, limit);
    case EdgeSampleMode::Clamp:
    default:
        return std::clamp(coord, 0, limit - 1);
    }
}

int WrapOrClampCoord(int coord, int limit, bool seamless)
{
    return ApplyEdgeSampleMode(coord, limit, seamless ? EdgeSampleMode::Wrap : EdgeSampleMode::Clamp);
}

void SampleRgbaBilinearWithMode(const uint8_t *src, uint32_t srcW, uint32_t srcH, float u, float v,
                                EdgeSampleMode mode, float out[4])
{
    const float rawFx = u * float(srcW) - 0.5f;
    const float rawFy = v * float(srcH) - 0.5f;
    const int xBase = int(std::floor(rawFx));
    const int yBase = int(std::floor(rawFy));
    const float tx = rawFx - float(xBase);
    const float ty = rawFy - float(yBase);

    const int x0 = ApplyEdgeSampleMode(xBase, int(srcW), mode);
    const int y0 = ApplyEdgeSampleMode(yBase, int(srcH), mode);
    const int x1 = ApplyEdgeSampleMode(xBase + 1, int(srcW), mode);
    const int y1 = ApplyEdgeSampleMode(yBase + 1, int(srcH), mode);

    const uint8_t *p00 = src + (size_t(y0) * srcW + size_t(x0)) * 4u;
    const uint8_t *p10 = src + (size_t(y0) * srcW + size_t(x1)) * 4u;
    const uint8_t *p01 = src + (size_t(y1) * srcW + size_t(x0)) * 4u;
    const uint8_t *p11 = src + (size_t(y1) * srcW + size_t(x1)) * 4u;

    for (int c = 0; c < 4; c++)
    {
        const float top = float(p00[c]) + (float(p10[c]) - float(p00[c])) * tx;
        const float bot = float(p01[c]) + (float(p11[c]) - float(p01[c])) * tx;
        out[c] = top + (bot - top) * ty;
    }
}

void SampleRgbaBilinearWrapped(const uint8_t *src, uint32_t srcW, uint32_t srcH, float u, float v,
                               bool seamless, float out[4])
{
    SampleRgbaBilinearWithMode(
        src, srcW, srcH, u, v, seamless ? EdgeSampleMode::Wrap : EdgeSampleMode::Clamp, out);
}

void SampleRgbaBilinear(const uint8_t *src, uint32_t srcW, uint32_t srcH, float u, float v,
                        float out[4])
{
    SampleRgbaBilinearWrapped(src, srcW, srcH, std::clamp(u, 0.0f, 1.0f), std::clamp(v, 0.0f, 1.0f),
                              false, out);
}

void AddDiffuseMicroDetailField(uint32_t width, uint32_t height,
                                const std::vector<uint8_t> *diffuseRgba,
                                const EditorAiNormalParams &params, std::vector<float> &field)
{
    const size_t pixels = size_t(width) * size_t(height);
    if (diffuseRgba == nullptr || diffuseRgba->size() != pixels * 4u || !(params.microDetail > 0.0f))
    {
        return;
    }

    std::vector<float> luma(pixels);
    for (size_t i = 0; i < pixels; i++)
    {
        luma[i] = RgbLuminance01((*diffuseRgba)[i * 4u + 0u], (*diffuseRgba)[i * 4u + 1u],
                                 (*diffuseRgba)[i * 4u + 2u]);
    }

    const int w = int(width);
    const int h = int(height);
    for (int y = 0; y < h; y++)
    {
        for (int x = 0; x < w; x++)
        {
            float sum = 0.0f;
            for (int dy = -1; dy <= 1; dy++)
            {
                const int sy = WrapOrClampCoord(y + dy, h, params.seamless);
                for (int dx = -1; dx <= 1; dx++)
                {
                    const int sx = WrapOrClampCoord(x + dx, w, params.seamless);
                    sum += luma[size_t(sy) * width + size_t(sx)];
                }
            }
            const size_t idx = size_t(y) * width + size_t(x);
            const float highPass = luma[idx] - sum * (1.0f / 9.0f);
            field[idx] = std::clamp(field[idx] + params.microDetail * highPass, -0.5f, 1.5f);
        }
    }
}

struct ScalarFieldView
{
    const std::vector<float> &field;
    uint32_t width = 0;
    uint32_t height = 0;
    bool seamless = true;
};

void ComputeScharrSlopeAt(const ScalarFieldView &view, int x, int y, float &outDhDx, float &outDhDy)
{
    const int w = int(view.width);
    const int h = int(view.height);
    const int xm = WrapOrClampCoord(x - 1, w, view.seamless);
    const int xp = WrapOrClampCoord(x + 1, w, view.seamless);
    const int ym = WrapOrClampCoord(y - 1, h, view.seamless);
    const int yp = WrapOrClampCoord(y + 1, h, view.seamless);

    const float h00 = view.field[size_t(ym) * view.width + size_t(xm)];
    const float h10 = view.field[size_t(ym) * view.width + size_t(x)];
    const float h20 = view.field[size_t(ym) * view.width + size_t(xp)];
    const float h01 = view.field[size_t(y) * view.width + size_t(xm)];
    const float h21 = view.field[size_t(y) * view.width + size_t(xp)];
    const float h02 = view.field[size_t(yp) * view.width + size_t(xm)];
    const float h12 = view.field[size_t(yp) * view.width + size_t(x)];
    const float h22 = view.field[size_t(yp) * view.width + size_t(xp)];

    outDhDx =
        ((3.0f * h20 + 10.0f * h21 + 3.0f * h22) - (3.0f * h00 + 10.0f * h01 + 3.0f * h02)) /
        32.0f;
    outDhDy =
        ((3.0f * h02 + 10.0f * h12 + 3.0f * h22) - (3.0f * h00 + 10.0f * h10 + 3.0f * h20)) /
        32.0f;
}

void WriteNormalizedNormalPixel(float nx, float ny, float nz, uint8_t *dst)
{
    const float invLen = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
    const float ux = nx * invLen;
    const float uy = ny * invLen;
    const float uz = nz * invLen;
    dst[0] = uint8_t(std::clamp(std::lround(128.0f + ux * 127.0f), 0l, 255l));
    dst[1] = uint8_t(std::clamp(std::lround(128.0f + uy * 127.0f), 0l, 255l));
    dst[2] = uint8_t(std::clamp(std::lround(128.0f + uz * 127.0f), 0l, 255l));
    dst[3] = 255u;
}

void ComputeLightDirection(float lightAngleDeg, float outDir[3])
{
    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    const float az = lightAngleDeg * kDegToRad;
    const float elev = 38.0f * kDegToRad;
    const float cosE = std::cos(elev);
    outDir[0] = std::cos(az) * cosE;
    outDir[1] = std::sin(az) * cosE;
    outDir[2] = std::sin(elev);
}

void NormalizePercentile01(std::vector<float> &vals)
{
    if (vals.empty())
    {
        return;
    }
    std::vector<float> sorted = vals;
    std::sort(sorted.begin(), sorted.end());
    const size_t idxLo = (sorted.size() - 1u) / 100u;
    const size_t idxHi = ((sorted.size() - 1u) * 99u) / 100u;
    const float lo = sorted[idxLo];
    const float hi = sorted[idxHi];
    const float span = std::max(1e-5f, hi - lo);
    for (float &v : vals)
    {
        v = std::clamp((v - lo) / span, 0.0f, 1.0f);
    }
}

struct RgbFieldView
{
    const std::vector<float> &rgb;
    uint32_t width = 0;
    uint32_t height = 0;
    bool seamless = true;
};

void SmoothRgbHorizontal(const RgbFieldView &view, std::vector<float> &dst)
{
    dst.resize(view.rgb.size());
    const int w = int(view.width);
    for (uint32_t y = 0; y < view.height; y++)
    {
        const size_t row = size_t(y) * view.width;
        for (int x = 0; x < w; x++)
        {
            const size_t iL = (row + size_t(WrapOrClampCoord(x - 1, w, view.seamless))) * 3u;
            const size_t iC = (row + size_t(x)) * 3u;
            const size_t iR = (row + size_t(WrapOrClampCoord(x + 1, w, view.seamless))) * 3u;
            for (size_t c = 0; c < 3u; c++)
            {
                dst[iC + c] =
                    0.25f * view.rgb[iL + c] + 0.5f * view.rgb[iC + c] + 0.25f * view.rgb[iR + c];
            }
        }
    }
}

void SmoothRgbVertical(const RgbFieldView &view, std::vector<float> &dst)
{
    dst.resize(view.rgb.size());
    const int h = int(view.height);
    for (int y = 0; y < h; y++)
    {
        const size_t rT = size_t(WrapOrClampCoord(y - 1, h, view.seamless)) * view.width;
        const size_t rC = size_t(y) * view.width;
        const size_t rB = size_t(WrapOrClampCoord(y + 1, h, view.seamless)) * view.width;
        for (uint32_t x = 0; x < view.width; x++)
        {
            const size_t iT = (rT + x) * 3u;
            const size_t iC = (rC + x) * 3u;
            const size_t iB = (rB + x) * 3u;
            for (size_t c = 0; c < 3u; c++)
            {
                dst[iC + c] =
                    0.25f * view.rgb[iT + c] + 0.5f * view.rgb[iC + c] + 0.25f * view.rgb[iB + c];
            }
        }
    }
}

void SmoothRgb3x3Pass(const std::vector<float> &srcRgb, uint32_t w, uint32_t h, bool seamless,
                      std::vector<float> &dstRgb)
{
    std::vector<float> horiz;
    SmoothRgbHorizontal(RgbFieldView{ srcRgb, w, h, seamless }, horiz);
    SmoothRgbVertical(RgbFieldView{ horiz, w, h, seamless }, dstRgb);
}

void BoxBlurScalarHoriz(const ScalarFieldView &view, int radius, std::vector<float> &dst)
{
    dst.resize(view.field.size());
    const int w = int(view.width);
    const float invSpan = 1.0f / float(2 * radius + 1);
    for (uint32_t y = 0; y < view.height; y++)
    {
        const float *row = view.field.data() + size_t(y) * view.width;
        float *outRow = dst.data() + size_t(y) * view.width;
        float sum = 0.0f;
        for (int k = -radius; k <= radius; k++)
        {
            sum += row[WrapOrClampCoord(k, w, view.seamless)];
        }
        outRow[0] = sum * invSpan;
        for (int x = 1; x < w; x++)
        {
            const int xOut = WrapOrClampCoord(x - 1 - radius, w, view.seamless);
            const int xIn = WrapOrClampCoord(x + radius, w, view.seamless);
            sum = std::max(0.0f, sum + row[xIn] - row[xOut]);
            outRow[x] = sum * invSpan;
        }
    }
}

void BoxBlurScalarVert(const ScalarFieldView &view, int radius, std::vector<float> &dst)
{
    dst.resize(view.field.size());
    const int h = int(view.height);
    const float invSpan = 1.0f / float(2 * radius + 1);
    for (uint32_t x = 0; x < view.width; x++)
    {
        float sum = 0.0f;
        for (int k = -radius; k <= radius; k++)
        {
            sum += view.field[size_t(WrapOrClampCoord(k, h, view.seamless)) * view.width + x];
        }
        dst[x] = sum * invSpan;
        for (int y = 1; y < h; y++)
        {
            const int yOut = WrapOrClampCoord(y - 1 - radius, h, view.seamless);
            const int yIn = WrapOrClampCoord(y + radius, h, view.seamless);
            sum = std::max(0.0f, sum + view.field[size_t(yIn) * view.width + x] -
                                         view.field[size_t(yOut) * view.width + x]);
            dst[size_t(y) * view.width + x] = sum * invSpan;
        }
    }
}

void BoxBlurScalarPass(const std::vector<float> &src, uint32_t w, uint32_t h, int radius,
                       bool seamless, std::vector<float> &dst)
{
    std::vector<float> horiz;
    BoxBlurScalarHoriz(ScalarFieldView{ src, w, h, seamless }, radius, horiz);
    BoxBlurScalarVert(ScalarFieldView{ horiz, w, h, seamless }, radius, dst);
}

void BlendRgbBuffers(const std::vector<float> &a, const std::vector<float> &b, float alpha,
                     std::vector<float> &out)
{
    out.resize(a.size());
    for (size_t i = 0; i < a.size(); i++)
    {
        out[i] = a[i] + alpha * (b[i] - a[i]);
    }
}

void ApplyPreSmoothRgb(uint32_t w, uint32_t h, const EditorAiDiffusePrepParams &params,
                       std::vector<float> &rgb)
{
    const float smooth = std::clamp(params.preSmooth, 0.0f, 1.0f);
    if (!(smooth > 1e-4f))
    {
        return;
    }
    std::vector<float> pass1;
    SmoothRgb3x3Pass(rgb, w, h, params.seamless, pass1);
    if (smooth <= 0.5f)
    {
        std::vector<float> blended;
        BlendRgbBuffers(rgb, pass1, smooth * 2.0f, blended);
        rgb = std::move(blended);
        return;
    }
    std::vector<float> pass2;
    SmoothRgb3x3Pass(pass1, w, h, params.seamless, pass2);
    BlendRgbBuffers(pass1, pass2, (smooth - 0.5f) * 2.0f, rgb);
}

void ApplyFlattenLightingRgb(uint32_t w, uint32_t h, const EditorAiDiffusePrepParams &params,
                             std::vector<float> &rgb)
{
    const float flatten = std::clamp(params.flattenLighting, 0.0f, 1.0f);
    if (!(flatten > 1e-4f))
    {
        return;
    }
    const size_t pixels = size_t(w) * size_t(h);
    std::vector<float> luma(pixels);
    double sumY = 0.0;
    for (size_t i = 0; i < pixels; i++)
    {
        const float y =
            0.2126f * rgb[i * 3u + 0u] + 0.7152f * rgb[i * 3u + 1u] + 0.0722f * rgb[i * 3u + 2u];
        luma[i] = y;
        sumY += double(y);
    }
    const float meanY = float(sumY / double(pixels));
    const int radius = std::clamp(int(std::max(w, h) / 8u), 4, 48);
    std::vector<float> tmpBlur;
    std::vector<float> blurY;
    BoxBlurScalarPass(luma, w, h, radius, params.seamless, tmpBlur);
    BoxBlurScalarPass(tmpBlur, w, h, radius, params.seamless, blurY);
    for (size_t i = 0; i < pixels; i++)
    {
        const float rawGain = (meanY + 0.08f) / (blurY[i] + 0.08f);
        const float gain = 1.0f + flatten * (std::clamp(rawGain, 0.45f, 2.2f) - 1.0f);
        rgb[i * 3u + 0u] = std::clamp(rgb[i * 3u + 0u] * gain, 0.0f, 1.0f);
        rgb[i * 3u + 1u] = std::clamp(rgb[i * 3u + 1u] * gain, 0.0f, 1.0f);
        rgb[i * 3u + 2u] = std::clamp(rgb[i * 3u + 2u] * gain, 0.0f, 1.0f);
    }
}

void SubtractZeroMean(std::vector<float> &vals)
{
    if (vals.empty())
    {
        return;
    }
    double sum = 0.0;
    for (float v : vals)
    {
        sum += double(v);
    }
    const float mean = float(sum / double(vals.size()));
    for (float &v : vals)
    {
        v -= mean;
    }
}

struct PoissonGridLevel
{
    uint32_t w = 0;
    uint32_t h = 0;
    std::vector<float> hVal;
    std::vector<float> rhs;
};

float SampleGridBilinear(const std::vector<float> &grid, uint32_t w, uint32_t h, float u, float v,
                         bool seamless)
{
    const float rawFx = u * float(w) - 0.5f;
    const float rawFy = v * float(h) - 0.5f;
    const int xBase = int(std::floor(rawFx));
    const int yBase = int(std::floor(rawFy));
    const float tx = rawFx - float(xBase);
    const float ty = rawFy - float(yBase);
    const int x0 = WrapOrClampCoord(xBase, int(w), seamless);
    const int x1 = WrapOrClampCoord(xBase + 1, int(w), seamless);
    const int y0 = WrapOrClampCoord(yBase, int(h), seamless);
    const int y1 = WrapOrClampCoord(yBase + 1, int(h), seamless);
    const float a = grid[size_t(y0) * w + size_t(x0)];
    const float b = grid[size_t(y0) * w + size_t(x1)];
    const float c = grid[size_t(y1) * w + size_t(x0)];
    const float d = grid[size_t(y1) * w + size_t(x1)];
    const float top = a + (b - a) * tx;
    const float bot = c + (d - c) * tx;
    return top + (bot - top) * ty;
}

PoissonGridLevel CoarsenPoissonLevel(const PoissonGridLevel &fine, bool seamless)
{
    PoissonGridLevel coarse;
    coarse.w = std::max(1u, fine.w / 2u);
    coarse.h = std::max(1u, fine.h / 2u);
    coarse.hVal.assign(size_t(coarse.w) * size_t(coarse.h), 0.0f);
    coarse.rhs.resize(size_t(coarse.w) * size_t(coarse.h), 0.0f);
    for (uint32_t y = 0; y < coarse.h; y++)
    {
        const float v = (float(y) + 0.5f) / float(coarse.h);
        for (uint32_t x = 0; x < coarse.w; x++)
        {
            const float u = (float(x) + 0.5f) / float(coarse.w);
            // Restriction scales RHS by 4x because coarse step size dx = 2 * fine_dx.
            coarse.rhs[size_t(y) * coarse.w + x] =
                4.0f * SampleGridBilinear(fine.rhs, fine.w, fine.h, u, v, seamless);
        }
    }
    return coarse;
}

void RelaxPoissonRedBlack(PoissonGridLevel &lvl, bool seamless, int iterations)
{
    constexpr float kOmega = 1.65f;
    const int w = int(lvl.w);
    const int h = int(lvl.h);
    for (int iter = 0; iter < iterations; iter++)
    {
        for (int parity = 0; parity < 2; parity++)
        {
            for (int y = 0; y < h; y++)
            {
                const int ym = seamless ? ((y - 1 + h) % h) : std::max(0, y - 1);
                const int yp = seamless ? ((y + 1) % h) : std::min(h - 1, y + 1);
                for (int x = (y + parity) & 1; x < w; x += 2)
                {
                    const int xm = seamless ? ((x - 1 + w) % w) : std::max(0, x - 1);
                    const int xp = seamless ? ((x + 1) % w) : std::min(w - 1, x + 1);
                    const size_t idx = size_t(y) * lvl.w + size_t(x);
                    const float sumN = lvl.hVal[size_t(y) * lvl.w + size_t(xm)] +
                                       lvl.hVal[size_t(y) * lvl.w + size_t(xp)] +
                                       lvl.hVal[size_t(ym) * lvl.w + size_t(x)] +
                                       lvl.hVal[size_t(yp) * lvl.w + size_t(x)];
                    const float target = 0.25f * (sumN - lvl.rhs[idx]);
                    lvl.hVal[idx] += kOmega * (target - lvl.hVal[idx]);
                }
            }
        }
        SubtractZeroMean(lvl.hVal);
    }
}

void BuildPoissonDivergence(const std::vector<uint8_t> &baseNormalRgba, uint32_t width,
                            uint32_t height, bool seamless, std::vector<float> &outRhs)
{
    const size_t pixels = size_t(width) * size_t(height);
    std::vector<float> gx(pixels);
    std::vector<float> gy(pixels);
    for (size_t i = 0; i < pixels; i++)
    {
        const float nx = (float(baseNormalRgba[i * 4u + 0u]) - 128.0f) / 127.0f;
        const float ny = (float(baseNormalRgba[i * 4u + 1u]) - 128.0f) / 127.0f;
        const float nz = std::max(0.1f, (float(baseNormalRgba[i * 4u + 2u]) - 128.0f) / 127.0f);
        gx[i] = -nx / nz;
        gy[i] = -ny / nz;
    }

    outRhs.assign(pixels, 0.0f);
    const int w = int(width);
    const int h = int(height);
    for (int y = 0; y < h; y++)
    {
        const int ym = WrapOrClampCoord(y - 1, h, seamless);
        const int yp = WrapOrClampCoord(y + 1, h, seamless);
        for (int x = 0; x < w; x++)
        {
            const int xm = WrapOrClampCoord(x - 1, w, seamless);
            const int xp = WrapOrClampCoord(x + 1, w, seamless);
            const float dGx = 0.5f * (gx[size_t(y) * width + size_t(xp)] -
                                      gx[size_t(y) * width + size_t(xm)]);
            const float dGy = 0.5f * (gy[size_t(yp) * width + size_t(x)] -
                                      gy[size_t(ym) * width + size_t(x)]);
            outRhs[size_t(y) * width + size_t(x)] = dGx + dGy;
        }
    }
    SubtractZeroMean(outRhs);
}

void SynthesizeGlossFromDiffuseAndHeight(const std::vector<uint8_t> &diffuseRgba,
                                         const std::vector<float> &height, uint32_t width,
                                         uint32_t heightPx, std::vector<uint8_t> &outGlossRgba)
{
    const size_t pixels = size_t(width) * size_t(heightPx);
    if (pixels == 0 || diffuseRgba.size() < pixels * 4u)
    {
        outGlossRgba.clear();
        return;
    }

    std::vector<float> raw(pixels);
    for (size_t i = 0; i < pixels; i++)
    {
        const uint8_t r = diffuseRgba[i * 4u + 0u];
        const uint8_t g = diffuseRgba[i * 4u + 1u];
        const uint8_t b = diffuseRgba[i * 4u + 2u];
        const float cmax = float(std::max({ r, g, b })) / 255.0f;
        const float cmin = float(std::min({ r, g, b })) / 255.0f;
        const float lum = (0.299f * float(r) + 0.587f * float(g) + 0.114f * float(b)) / 255.0f;
        const float sat = cmax > 1e-4f ? ((cmax - cmin) / cmax) : 0.0f;
        const float metallicCue = lum * (1.0f - 0.55f * sat);
        const float hVal = i < height.size() ? height[i] : 0.5f;
        raw[i] = 0.55f * metallicCue + 0.45f * hVal;
    }

    std::vector<float> sorted = raw;
    std::sort(sorted.begin(), sorted.end());
    const float pLo = sorted[((pixels - 1u) * 2u) / 100u];
    const float pHi = sorted[((pixels - 1u) * 98u) / 100u];
    const float span = pHi - pLo;
    const float normStrength = std::clamp((span - 0.03f) / 0.12f, 0.0f, 1.0f);

    outGlossRgba.resize(pixels * 4u);
    for (size_t i = 0; i < pixels; i++)
    {
        const float absVal = std::clamp(0.25f + 0.65f * raw[i], 0.10f, 0.92f);
        const float normVal =
            0.10f + 0.82f * std::clamp((raw[i] - pLo) / std::max(span, 1e-4f), 0.0f, 1.0f);
        const uint8_t gv = FloatToByte(absVal + normStrength * (normVal - absVal));
        outGlossRgba[i * 4u + 0u] = gv;
        outGlossRgba[i * 4u + 1u] = gv;
        outGlossRgba[i * 4u + 2u] = gv;
        outGlossRgba[i * 4u + 3u] = 255u;
    }
}

void BuildLocalEmissiveMap(const std::vector<uint8_t> &diffuseRgba, uint32_t width, uint32_t height,
                           std::vector<uint8_t> &outRgba)
{
    const size_t pixels = size_t(width) * size_t(height);
    outRgba.resize(pixels * 4u);
    for (size_t i = 0; i < pixels; i++)
    {
        const uint8_t r = diffuseRgba[i * 4u + 0u];
        const uint8_t g = diffuseRgba[i * 4u + 1u];
        const uint8_t b = diffuseRgba[i * 4u + 2u];
        const float maxC = float(std::max({ r, g, b })) / 255.0f;
        const float minC = float(std::min({ r, g, b })) / 255.0f;
        const float sat = maxC > 1e-4f ? ((maxC - minC) / maxC) : 0.0f;
        const float score = maxC * (0.45f + 0.55f * sat);
        if (score < 0.72f)
        {
            outRgba[i * 4u + 0u] = 0u;
            outRgba[i * 4u + 1u] = 0u;
            outRgba[i * 4u + 2u] = 0u;
            outRgba[i * 4u + 3u] = 255u;
        }
        else
        {
            const float gain = std::min(1.0f, (score - 0.72f) / 0.28f);
            outRgba[i * 4u + 0u] = FloatToByte((float(r) / 255.0f) * gain);
            outRgba[i * 4u + 1u] = FloatToByte((float(g) / 255.0f) * gain);
            outRgba[i * 4u + 2u] = FloatToByte((float(b) / 255.0f) * gain);
            outRgba[i * 4u + 3u] = 255u;
        }
    }
}

void ExtractRgbaLumaNchw(const std::vector<uint8_t> &rgba, uint32_t width, uint32_t height,
                         EditorAiTensorNchw &outLuma)
{
    const size_t pixels = size_t(width) * size_t(height);
    outLuma.channels = 1u;
    outLuma.width = width;
    outLuma.height = height;
    outLuma.data.resize(pixels);
    for (size_t i = 0; i < pixels; i++)
    {
        const float r = float(rgba[i * 4u + 0u]) / 255.0f;
        const float g = float(rgba[i * 4u + 1u]) / 255.0f;
        const float b = float(rgba[i * 4u + 2u]) / 255.0f;
        outLuma.data[i] = 0.299f * r + 0.587f * g + 0.114f * b;
    }
}

void FinalizeLotusHwcResult(const EditorAiInferenceRequest &req, const std::vector<float> &hwc01,
                            EditorAiInferenceResult &out)
{
    const size_t pixels = size_t(req.width) * size_t(req.height);
    if (req.backend == EditorAiBackend::LotusDepthD)
    {
        out.rawHeight.resize(pixels);
        for (size_t i = 0; i < pixels; i++)
        {
            out.rawHeight[i] =
                (hwc01[i * 3u + 0u] + hwc01[i * 3u + 1u] + hwc01[i * 3u + 2u]) * (1.0f / 3.0f);
        }
        NormalizePercentile01(out.rawHeight);
        return;
    }
    ConvertEditorNormals01ToQuakeRayRgba(hwc01, req.width, req.height, true, true,
                                         out.rawNormalRgba);
    IntegrateEditorNormalMapHeight(out.rawNormalRgba, req.width, req.height, req.seamless,
                                   out.rawHeight);
}

struct DiffusionWorkerStageSpec
{
    const std::filesystem::path &workerExe;
    const std::filesystem::path &modelPath;
    const char *arch = nullptr;
    const char *prepLabel = nullptr;
    const char *postLabel = nullptr;
    const std::atomic<bool> *cancelRequested = nullptr;
};

bool RunDiffusionWorkerAndDownsample(const EditorAiInferenceRequest &req,
                                     const DiffusionWorkerStageSpec &spec,
                                     std::vector<float> &outHwc01, EditorAiInferenceResult &out)
{
    if (req.progressCb)
    {
        req.progressCb(1, spec.prepLabel);
    }
    EditorAiLotusPrep prep;
    PrepareEditorLotusInputNchw(req.diffuseRgba, req.width, req.height, req.processingRes,
                                req.seamless, prep);

    EditorAiWorkerInvokeRequest workerReq;
    workerReq.workerExecutable = spec.workerExe;
    workerReq.modelPath = spec.modelPath;
    workerReq.arch = spec.arch;
    workerReq.input = std::move(prep.tensor);
    workerReq.cancelRequested = spec.cancelRequested;
    workerReq.onProgress = req.progressCb;

    EditorAiWorkerInvokeResult workerRes;
    if (!RunEditorAiWorkerProcess(workerReq, workerRes))
    {
        out.cancelled = workerRes.cancelled;
        out.errorMessage = workerRes.errorMessage;
        return false;
    }
    out.providerLabel = workerRes.provider;

    if (req.progressCb)
    {
        req.progressCb(99, spec.postLabel);
    }
    DownsampleEditorLotusOutputNchw(workerRes.output, prep, req.width, req.height, outHwc01);
    if (outHwc01.size() != size_t(req.width) * size_t(req.height) * 3u)
    {
        out.errorMessage = std::string(spec.arch) + " GGUF output dimensions mismatch.";
        return false;
    }
    return true;
}

bool RunLotusDepthPipeline(const EditorAiInferenceRequest &req,
                          const std::filesystem::path &workerExe,
                          const std::filesystem::path &modelPath,
                          const std::atomic<bool> *cancelRequested,
                          EditorAiInferenceResult &out)
{
    if (req.progressCb)
    {
        req.progressCb(1, "Preparing Lotus Depth input");
    }
    EditorAiLotusPrep prep;
    PrepareEditorLotusDepthInputNchw(req.diffuseRgba, req.width, req.height, req.processingRes,
                                     req.seamless, prep);

    EditorAiWorkerInvokeRequest workerReq;
    workerReq.workerExecutable = workerExe;
    workerReq.modelPath = modelPath;
    workerReq.arch = "lotus";
    workerReq.input = std::move(prep.tensor);
    workerReq.cancelRequested = cancelRequested;
    workerReq.onProgress = req.progressCb;

    EditorAiWorkerInvokeResult workerRes;
    if (!RunEditorAiWorkerProcess(workerReq, workerRes))
    {
        out.cancelled = workerRes.cancelled;
        out.errorMessage = workerRes.errorMessage;
        return false;
    }
    out.providerLabel = workerRes.provider;

    if (req.progressCb)
    {
        req.progressCb(99, "Detrending & normalizing depth");
    }
    ExtractDetrendedEditorLotusDepthHeight(workerRes.output, prep, req.width, req.height,
                                           out.rawHeight);
    if (out.rawHeight.size() != size_t(req.width) * size_t(req.height))
    {
        out.errorMessage = "lotus GGUF depth output dimensions mismatch.";
        return false;
    }
    return true;
}

bool RunLotusPredictionPipeline(const EditorAiInferenceRequest &req,
                                const std::filesystem::path &workerExe,
                                const std::filesystem::path &modelPath,
                                const std::atomic<bool> *cancelRequested,
                                EditorAiInferenceResult &out)
{
    if (req.backend == EditorAiBackend::LotusDepthD)
    {
        return RunLotusDepthPipeline(req, workerExe, modelPath, cancelRequested, out);
    }
    std::vector<float> hwc01;
    const DiffusionWorkerStageSpec spec{ workerExe,
                                         modelPath,
                                         "lotus",
                                         "Preparing Lotus input",
                                         "Post-processing normal map",
                                         cancelRequested };
    if (!RunDiffusionWorkerAndDownsample(req, spec, hwc01, out))
    {
        return false;
    }
    FinalizeLotusHwcResult(req, hwc01, out);
    return true;
}

bool RunDelightPredictionPipeline(const EditorAiInferenceRequest &req,
                                  const std::filesystem::path &workerExe,
                                  const std::filesystem::path &modelPath,
                                  const std::atomic<bool> *cancelRequested,
                                  EditorAiInferenceResult &out)
{
    std::vector<float> hwc01;
    const DiffusionWorkerStageSpec spec{ workerExe,
                                         modelPath,
                                         "delight",
                                         "Preparing StableDelight input",
                                         "Finalizing de-lit diffuse",
                                         cancelRequested };
    if (!RunDiffusionWorkerAndDownsample(req, spec, hwc01, out))
    {
        return false;
    }
    const size_t pixels = size_t(req.width) * size_t(req.height);
    out.rawColorRgba.resize(pixels * 4u);
    for (size_t i = 0; i < pixels; i++)
    {
        out.rawColorRgba[i * 4u + 0u] = FloatToByte(hwc01[i * 3u + 0u]);
        out.rawColorRgba[i * 4u + 1u] = FloatToByte(hwc01[i * 3u + 1u]);
        out.rawColorRgba[i * 4u + 2u] = FloatToByte(hwc01[i * 3u + 2u]);
        out.rawColorRgba[i * 4u + 3u] = req.diffuseRgba[i * 4u + 3u];
    }
    return true;
}

bool RunDeepBumpPredictionPipeline(const EditorAiInferenceRequest &req,
                                   const std::filesystem::path &workerExe,
                                   const std::filesystem::path &modelPath,
                                   const std::atomic<bool> *cancelRequested,
                                   EditorAiInferenceResult &out)
{
    if (req.progressCb)
    {
        req.progressCb(1, "Preparing DeepBump input");
    }
    EditorAiWorkerInvokeRequest workerReq;
    workerReq.workerExecutable = workerExe;
    workerReq.modelPath = modelPath;
    workerReq.arch = "deepbump";
    workerReq.clampEdge = !req.seamless;
    ExtractRgbaLumaNchw(req.diffuseRgba, req.width, req.height, workerReq.input);
    workerReq.cancelRequested = cancelRequested;
    workerReq.onProgress = req.progressCb;

    EditorAiWorkerInvokeResult workerRes;
    if (!RunEditorAiWorkerProcess(workerReq, workerRes))
    {
        out.cancelled = workerRes.cancelled;
        out.errorMessage = workerRes.errorMessage;
        return false;
    }
    out.providerLabel = workerRes.provider;
    const size_t pixels = size_t(req.width) * size_t(req.height);
    if (workerRes.output.channels != 3u || workerRes.output.width != req.width ||
        workerRes.output.height != req.height || workerRes.output.data.size() != pixels * 3u)
    {
        out.errorMessage = "DeepBump GGUF output dimensions mismatch.";
        return false;
    }

    if (req.progressCb)
    {
        req.progressCb(99, "Integrating height map");
    }
    std::vector<float> hwc01(pixels * 3u);
    for (size_t i = 0; i < pixels; i++)
    {
        hwc01[i * 3u + 0u] = workerRes.output.data[0u * pixels + i];
        hwc01[i * 3u + 1u] = workerRes.output.data[1u * pixels + i];
        hwc01[i * 3u + 2u] = workerRes.output.data[2u * pixels + i];
    }

    ConvertEditorNormals01ToQuakeRayRgba(hwc01, req.width, req.height, false, true,
                                         out.rawNormalRgba);
    IntegrateEditorNormalMapHeight(out.rawNormalRgba, req.width, req.height, req.seamless,
                                   out.rawHeight);
    return true;
}

void ExtractRgbaRgbNchw(const std::vector<uint8_t> &rgba, uint32_t width, uint32_t height,
                        EditorAiTensorNchw &outRgb, bool &outAllOpaque)
{
    const size_t pixels = size_t(width) * size_t(height);
    outRgb.channels = 3u;
    outRgb.width = width;
    outRgb.height = height;
    outRgb.data.resize(pixels * 3u);
    outAllOpaque = true;
    for (size_t i = 0; i < pixels; i++)
    {
        outRgb.data[0u * pixels + i] = float(rgba[i * 4u + 0u]) / 255.0f;
        outRgb.data[1u * pixels + i] = float(rgba[i * 4u + 1u]) / 255.0f;
        outRgb.data[2u * pixels + i] = float(rgba[i * 4u + 2u]) / 255.0f;
        if (rgba[i * 4u + 3u] != 255u)
        {
            outAllOpaque = false;
        }
    }
}

bool RunRealPlksrUpscalePipeline(const EditorAiInferenceRequest &req,
                                 const std::filesystem::path &workerExe,
                                 const std::filesystem::path &modelPath,
                                 const std::atomic<bool> *cancelRequested,
                                 EditorAiInferenceResult &out)
{
    if (req.progressCb)
    {
        req.progressCb(1, "Preparing RealPLKSR input");
    }
    EditorAiWorkerInvokeRequest workerReq;
    workerReq.workerExecutable = workerExe;
    workerReq.modelPath = modelPath;
    workerReq.arch = "realplksr";
    workerReq.clampEdge = !req.seamless;
    bool allOpaque = true;
    ExtractRgbaRgbNchw(req.diffuseRgba, req.width, req.height, workerReq.input, allOpaque);
    workerReq.cancelRequested = cancelRequested;
    workerReq.onProgress = req.progressCb;

    EditorAiWorkerInvokeResult workerRes;
    if (!RunEditorAiWorkerProcess(workerReq, workerRes))
    {
        out.cancelled = workerRes.cancelled;
        out.errorMessage = workerRes.errorMessage;
        return false;
    }
    out.providerLabel = workerRes.provider;

    const uint32_t outW = req.width * 4u;
    const uint32_t outH = req.height * 4u;
    const size_t outPixels = size_t(outW) * size_t(outH);
    if (workerRes.output.channels != 3u || workerRes.output.width != outW ||
        workerRes.output.height != outH || workerRes.output.data.size() != outPixels * 3u)
    {
        out.errorMessage = "RealPLKSR GGUF output dimensions mismatch.";
        return false;
    }

    if (req.progressCb)
    {
        req.progressCb(99, "Finalizing upscaled diffuse");
    }
    out.outputWidth = outW;
    out.outputHeight = outH;
    out.rawColorRgba.resize(outPixels * 4u);
    for (uint32_t y = 0; y < outH; y++)
    {
        const float v = (float(y) + 0.5f) / float(outH);
        for (uint32_t x = 0; x < outW; x++)
        {
            const size_t i = size_t(y) * outW + x;
            out.rawColorRgba[i * 4u + 0u] = FloatToByte(workerRes.output.data[0u * outPixels + i]);
            out.rawColorRgba[i * 4u + 1u] = FloatToByte(workerRes.output.data[1u * outPixels + i]);
            out.rawColorRgba[i * 4u + 2u] = FloatToByte(workerRes.output.data[2u * outPixels + i]);
            if (allOpaque)
            {
                out.rawColorRgba[i * 4u + 3u] = 255u;
            }
            else
            {
                const float u = (float(x) + 0.5f) / float(outW);
                float sample[4] = {};
                SampleRgbaBilinearWrapped(req.diffuseRgba.data(), req.width, req.height, u, v,
                                          req.seamless, sample);
                out.rawColorRgba[i * 4u + 3u] =
                    uint8_t(std::clamp(std::lround(sample[3]), 0l, 255l));
            }
        }
    }
    return true;
}

const EditorAiInferenceRequest &SelectPreparedAiRequest(const EditorAiInferenceRequest &req,
                                                        EditorAiInferenceRequest &storage)
{
    if (!(req.flattenLighting > 1e-4f) && !(req.preSmooth > 1e-4f))
    {
        return req;
    }
    storage = req;
    storage.diffuseRgba.clear();
    const EditorAiDiffusePrepParams prepParams{
        req.flattenLighting,
        req.preSmooth,
        req.seamless,
    };
    PreprocessEditorAiDiffuse(req.diffuseRgba, req.width, req.height, prepParams,
                              storage.diffuseRgba);
    return storage;
}

} // namespace

std::filesystem::path DefaultEditorAiLocalConfigPath()
{
#if defined(_WIN32)
    std::string base = GetEnvString("APPDATA");
    if (base.empty())
    {
        base = GetEnvString("USERPROFILE");
    }
    if (base.empty())
    {
        base = ".";
    }
    return std::filesystem::path(base) / "QuakeRay" / "ai_local.json";
#else
    std::string xdg = GetEnvString("XDG_CONFIG_HOME");
    if (!xdg.empty())
    {
        return std::filesystem::path(xdg) / "quakeray" / "ai_local.json";
    }
    std::string home = GetEnvString("HOME");
    if (home.empty())
    {
        home = ".";
    }
    return std::filesystem::path(home) / ".config" / "quakeray" / "ai_local.json";
#endif
}

std::filesystem::path DefaultEditorAiModelsDir()
{
    return UserAiCacheBaseDir() / "models";
}

const char *EditorAiBackendId(EditorAiBackend backend)
{
    switch (backend)
    {
    case EditorAiBackend::LotusNormalD:
        return "lotus_normal_d";
    case EditorAiBackend::LotusDepthD:
        return "lotus_depth_d";
    case EditorAiBackend::DeepBump:
        return "deepbump";
    case EditorAiBackend::StableDelight:
        return "stable_delight";
    case EditorAiBackend::RealPlksrPbrify:
        return "realplksr_pbrify";
    }
    return "lotus_normal_d";
}

const char *EditorAiBackendDisplayName(EditorAiBackend backend)
{
    switch (backend)
    {
    case EditorAiBackend::LotusNormalD:
        return "Lotus Normal (GGUF)";
    case EditorAiBackend::LotusDepthD:
        return "Lotus Depth (GGUF)";
    case EditorAiBackend::DeepBump:
        return "DeepBump (Fast GGUF)";
    case EditorAiBackend::StableDelight:
        return "StableDelight (GGUF)";
    case EditorAiBackend::RealPlksrPbrify:
        return "4x-PBRify RealPLKSR (GGUF)";
    }
    return "Lotus Normal (GGUF)";
}

const char *EditorAiBackendModelFilename(EditorAiBackend backend)
{
    switch (backend)
    {
    case EditorAiBackend::LotusNormalD:
        return "lotus-normal-d-v1-1-fp16.gguf";
    case EditorAiBackend::LotusDepthD:
        return "lotus-depth-d-v2-0-disparity-fp16.gguf";
    case EditorAiBackend::DeepBump:
        return "deepbump256-fp16.gguf";
    case EditorAiBackend::StableDelight:
        return "yoso-delight-v0-4-base-fp16.gguf";
    case EditorAiBackend::RealPlksrPbrify:
        return "4x-pbrify-rplksrd-v3-fp16.gguf";
    }
    return "lotus-normal-d-v1-1-fp16.gguf";
}

bool IsEditorAiLotusBackend(EditorAiBackend backend)
{
    return backend == EditorAiBackend::LotusNormalD || backend == EditorAiBackend::LotusDepthD;
}

bool IsEditorAiDiffusionBackend(EditorAiBackend backend)
{
    return IsEditorAiLotusBackend(backend) || backend == EditorAiBackend::StableDelight;
}

const char *EditorAiMapKindId(EditorAiMapKind kind)
{
    switch (kind)
    {
    case EditorAiMapKind::Normals:
        return "normals";
    case EditorAiMapKind::Gloss:
        return "gloss";
    case EditorAiMapKind::Emissive:
        return "emissive";
    case EditorAiMapKind::Delight:
        return "delit";
    case EditorAiMapKind::Height:
        return "height";
    case EditorAiMapKind::Upscale:
        return "upscaled";
    }
    return "normals";
}

std::filesystem::path ResolveEditorAiModelPath(EditorAiBackend backend, const std::string &exeDir,
                                               const std::string &modelsDir)
{
    const char *filename = EditorAiBackendModelFilename(backend);
    std::vector<std::filesystem::path> candidates;

    if (!modelsDir.empty())
    {
        candidates.push_back(std::filesystem::path(modelsDir) / filename);
    }
    const std::string envDir = GetEnvString("QUAKERAY_AI_MODELS_DIR");
    if (!envDir.empty())
    {
        candidates.push_back(std::filesystem::path(envDir) / filename);
    }
    if (!exeDir.empty())
    {
        candidates.push_back(std::filesystem::path(exeDir) / "models" / filename);
    }
    candidates.push_back(DefaultEditorAiModelsDir() / filename);
    if (backend == EditorAiBackend::DeepBump)
    {
        candidates.push_back(UserAiCacheBaseDir() / filename);
    }

    for (const std::filesystem::path &candidate : candidates)
    {
        if (IsUsableRegularFile(candidate))
        {
            return candidate.lexically_normal();
        }
    }
    return {};
}

bool LoadEditorAiLocalConfig(const std::filesystem::path &path, EditorAiLocalConfig &outConfig,
                             std::string &outError)
{
    outError.clear();
    outConfig = EditorAiLocalConfig{};

    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        return true;
    }

    std::string raw;
    if (!ReadRegularTextFile(path, raw))
    {
        outError = "Failed reading AI local config file: " + path.string();
        return false;
    }

    EditorAiLocalConfig parsed;
    JsonCursor cursor{ raw, 0 };
    if (!ParseLocalConfigObject(cursor, parsed))
    {
        outError = "Invalid JSON syntax in AI local config file";
        return false;
    }
    parsed.processingRes = std::clamp(parsed.processingRes, kMinProcessingRes, kMaxProcessingRes);
    outConfig = std::move(parsed);
    return true;
}

bool SaveEditorAiLocalConfig(const std::filesystem::path &path, const EditorAiLocalConfig &config,
                             std::string &outError)
{
    outError.clear();
    const std::string json = SerializeLocalConfigJson(config);
    return WriteTextFileAtomic(path, json, outError);
}

namespace
{

uint32_t RoundToGridAtLeast64(double val, uint32_t step)
{
    const long stepL = long(step);
    const long rounded = std::lround(val / double(step)) * stepL;
    return uint32_t(std::max<long>(64l, rounded));
}

void FillPreparedLotusTensorNchw(const std::vector<uint8_t> &diffuseRgba, uint32_t srcW,
                                 uint32_t srcH, EdgeSampleMode edgeMode,
                                 EditorAiLotusPrep &outPrep)
{
    const uint32_t workW = outPrep.fitW + 2u * outPrep.haloX;
    const uint32_t workH = outPrep.fitH + 2u * outPrep.haloY;
    const size_t plane = size_t(workW) * size_t(workH);
    outPrep.tensor.channels = 3;
    outPrep.tensor.height = int64_t(workH);
    outPrep.tensor.width = int64_t(workW);
    outPrep.tensor.data.resize(3u * plane);

    for (uint32_t y = 0; y < workH; y++)
    {
        const float v = (float(y) + 0.5f - float(outPrep.haloY)) / float(outPrep.fitH);
        for (uint32_t x = 0; x < workW; x++)
        {
            const float u = (float(x) + 0.5f - float(outPrep.haloX)) / float(outPrep.fitW);
            float sample[4] = {};
            SampleRgbaBilinearWithMode(diffuseRgba.data(), srcW, srcH, u, v, edgeMode, sample);
            const size_t idx = size_t(y) * workW + x;
            outPrep.tensor.data[0u * plane + idx] = std::clamp(sample[0] / 255.0f, 0.0f, 1.0f);
            outPrep.tensor.data[1u * plane + idx] = std::clamp(sample[1] / 255.0f, 0.0f, 1.0f);
            outPrep.tensor.data[2u * plane + idx] = std::clamp(sample[2] / 255.0f, 0.0f, 1.0f);
        }
    }
}

void AccumulateNormalEq3x3(double (&AtA)[3][3], double (&Atb)[3], const double row[3], double rhs)
{
    for (int i = 0; i < 3; i++)
    {
        Atb[i] += row[i] * rhs;
        for (int j = i; j < 3; j++)
        {
            AtA[i][j] += row[i] * row[j];
        }
    }
}

bool SolveLinearSystem3x3(double AtA[3][3], double Atb[3], double outCoef[3])
{
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < i; j++)
        {
            AtA[i][j] = AtA[j][i];
        }
        AtA[i][i] += 1e-6;
    }

    for (int col = 0; col < 3; col++)
    {
        int pivot = col;
        double bestVal = std::abs(AtA[col][col]);
        for (int row = col + 1; row < 3; row++)
        {
            const double val = std::abs(AtA[row][col]);
            if (val > bestVal)
            {
                bestVal = val;
                pivot = row;
            }
        }
        if (bestVal < 1e-12)
        {
            return false;
        }
        if (pivot != col)
        {
            for (int c = col; c < 3; c++)
            {
                std::swap(AtA[col][c], AtA[pivot][c]);
            }
            std::swap(Atb[col], Atb[pivot]);
        }
        const double invDiag = 1.0 / AtA[col][col];
        for (int row = col + 1; row < 3; row++)
        {
            const double factor = AtA[row][col] * invDiag;
            if (factor == 0.0)
            {
                continue;
            }
            for (int c = col; c < 3; c++)
            {
                AtA[row][c] -= factor * AtA[col][c];
            }
            Atb[row] -= factor * Atb[col];
        }
    }

    for (int i = 2; i >= 0; i--)
    {
        double sum = Atb[i];
        for (int c = i + 1; c < 3; c++)
        {
            sum -= AtA[i][c] * outCoef[c];
        }
        outCoef[i] = sum / AtA[i][i];
    }
    return true;
}

void AccumulateWrapHaloTiltEquations(const std::vector<float> &fullD, uint32_t workW,
                                      uint32_t workH, const EditorAiLotusPrep &prep,
                                      double (&AtA)[3][3], double (&Atb)[3])
{
    const uint32_t hx = prep.haloX > 0u ? prep.haloX : prep.halo;
    const uint32_t hy = prep.haloY > 0u ? prep.haloY : prep.halo;
    const uint32_t mX = std::min(hx, prep.fitW / 2u);
    const uint32_t mY = std::min(hy, prep.fitH / 2u);

    for (uint32_t y = hy; y < hy + prep.fitH && y < workH; y++)
    {
        const double vy = (double(y) + 0.5 - double(hy)) / double(prep.fitH) * 2.0 - 1.0;
        for (uint32_t x = hx - mX; x < hx + mX && x + prep.fitW < workW; x++)
        {
            const double row[3] = { 2.0, 0.0, 2.0 * vy };
            const double diff =
                double(fullD[size_t(y) * workW + x + prep.fitW]) - double(fullD[size_t(y) * workW + x]);
            AccumulateNormalEq3x3(AtA, Atb, row, diff);
        }
    }
    for (uint32_t y = hy - mY; y < hy + mY && y + prep.fitH < workH; y++)
    {
        for (uint32_t x = hx; x < hx + prep.fitW && x < workW; x++)
        {
            const double ux = (double(x) + 0.5 - double(hx)) / double(prep.fitW) * 2.0 - 1.0;
            const double row[3] = { 0.0, 2.0, 2.0 * ux };
            const double diff =
                double(fullD[size_t(y + prep.fitH) * workW + x]) - double(fullD[size_t(y) * workW + x]);
            AccumulateNormalEq3x3(AtA, Atb, row, diff);
        }
    }
}

void AccumulateMirrorHaloTiltEquations(const std::vector<float> &fullD, uint32_t workW,
                                        uint32_t workH, const EditorAiLotusPrep &prep,
                                        double (&AtA)[3][3], double (&Atb)[3])
{
    const uint32_t hx = prep.haloX > 0u ? prep.haloX : prep.halo;
    const uint32_t hy = prep.haloY > 0u ? prep.haloY : prep.halo;
    const auto uCoord = [hx, &prep](uint32_t x) {
        return (double(x) + 0.5 - double(hx)) / double(prep.fitW) * 2.0 - 1.0;
    };
    const auto vCoord = [hy, &prep](uint32_t y) {
        return (double(y) + 0.5 - double(hy)) / double(prep.fitH) * 2.0 - 1.0;
    };
    const auto valAt = [&fullD, workW](uint32_t x, uint32_t y) {
        return double(fullD[size_t(y) * workW + x]);
    };

    const uint32_t mX = std::max(1u, std::min(hx / 2u, prep.fitW / 4u));
    const uint32_t mY = std::max(1u, std::min(hy / 2u, prep.fitH / 4u));
    for (uint32_t y = hy; y < hy + prep.fitH && y < workH; y++)
    {
        const double vy = vCoord(y);
        for (uint32_t k = 0; k < mX; k++)
        {
            const uint32_t xLIn = hx + k;
            const uint32_t xLOut = hx - 1u - k;
            const double tL = uCoord(xLIn) + 1.0;
            const double rowL[3] = { 2.0 * tL, 0.0, 2.0 * tL * vy };
            AccumulateNormalEq3x3(AtA, Atb, rowL, valAt(xLIn, y) - valAt(xLOut, y));

            const uint32_t xROut = hx + prep.fitW + k;
            const uint32_t xRIn = hx + prep.fitW - 1u - k;
            const double tR = uCoord(xROut) - 1.0;
            const double rowR[3] = { 2.0 * tR, 0.0, 2.0 * tR * vy };
            AccumulateNormalEq3x3(AtA, Atb, rowR, valAt(xROut, y) - valAt(xRIn, y));
        }
    }
    for (uint32_t k = 0; k < mY; k++)
    {
        const uint32_t yTIn = hy + k;
        const uint32_t yTOut = hy - 1u - k;
        const double sT = vCoord(yTIn) + 1.0;

        const uint32_t yBOut = hy + prep.fitH + k;
        const uint32_t yBIn = hy + prep.fitH - 1u - k;
        const double sB = vCoord(yBOut) - 1.0;

        for (uint32_t x = hx; x < hx + prep.fitW && x < workW; x++)
        {
            const double ux = uCoord(x);
            const double rowT[3] = { 0.0, 2.0 * sT, 2.0 * sT * ux };
            AccumulateNormalEq3x3(AtA, Atb, rowT, valAt(x, yTIn) - valAt(x, yTOut));

            const double rowB[3] = { 0.0, 2.0 * sB, 2.0 * sB * ux };
            AccumulateNormalEq3x3(AtA, Atb, rowB, valAt(x, yBOut) - valAt(x, yBIn));
        }
    }
}

void BuildHaloTiltNormalEquations(const std::vector<float> &fullD, uint32_t workW, uint32_t workH,
                                  const EditorAiLotusPrep &prep, double (&AtA)[3][3],
                                  double (&Atb)[3])
{
    if (!prep.mirrorPad)
    {
        AccumulateWrapHaloTiltEquations(fullD, workW, workH, prep, AtA, Atb);
    }
    else
    {
        AccumulateMirrorHaloTiltEquations(fullD, workW, workH, prep, AtA, Atb);
    }
}

} // namespace

void ComputeEditorAiUpscaleFitDimensions(uint32_t srcW, uint32_t srcH, uint32_t targetMaxSide,
                                         uint32_t &outFitW, uint32_t &outFitH)
{
    if (srcW == 0 || srcH == 0)
    {
        outFitW = 0;
        outFitH = 0;
        return;
    }
    const uint32_t clampedTarget = std::clamp(targetMaxSide, kMinProcessingRes, kMaxProcessingRes);
    const uint32_t maxSrc = std::max(srcW, srcH);
    const double scale = double(clampedTarget) / double(maxSrc);
    outFitW = RoundToGridAtLeast64(double(srcW) * scale, 64u);
    outFitH = RoundToGridAtLeast64(double(srcH) * scale, 64u);
}

void ComputeEditorAiLotusDepthDimensions(uint32_t srcW, uint32_t srcH, uint32_t targetMaxSide,
                                         bool seamless, EditorAiLotusDepthDims &outDims)
{
    outDims = {};
    if (srcW == 0 || srcH == 0)
    {
        return;
    }
    const uint32_t clampedTarget = std::clamp(targetMaxSide, kMinProcessingRes, kMaxProcessingRes);
    const uint32_t workTarget = clampedTarget + 128u;
    const uint32_t maxSrc = std::max(srcW, srcH);

    if (seamless)
    {
        const uint32_t maxFit = workTarget / 2u;
        const double scale = double(maxFit) / double(maxSrc);
        outDims.fitW = RoundToGridAtLeast64(double(srcW) * scale, 32u);
        outDims.fitH = RoundToGridAtLeast64(double(srcH) * scale, 32u);
        outDims.haloX = outDims.fitW / 2u;
        outDims.haloY = outDims.fitH / 2u;
        while (outDims.fitW + 2u * outDims.haloX < 256u)
        {
            outDims.haloX += 32u;
        }
        while (outDims.fitH + 2u * outDims.haloY < 256u)
        {
            outDims.haloY += 32u;
        }
        return;
    }

    const uint32_t maxFit = RoundToGridAtLeast64(double(workTarget) * (2.0 / 3.0), 64u);
    const double scale = double(maxFit) / double(maxSrc);
    outDims.fitW = RoundToGridAtLeast64(double(srcW) * scale, 64u);
    outDims.fitH = RoundToGridAtLeast64(double(srcH) * scale, 64u);
    outDims.haloX =
        std::max<uint32_t>(32u, uint32_t(std::floor(double(outDims.fitW) * 0.25 / 32.0)) * 32u);
    outDims.haloY =
        std::max<uint32_t>(32u, uint32_t(std::floor(double(outDims.fitH) * 0.25 / 32.0)) * 32u);
    while (outDims.fitW + 2u * outDims.haloX < 192u)
    {
        outDims.haloX += 32u;
    }
    while (outDims.fitH + 2u * outDims.haloY < 192u)
    {
        outDims.haloY += 32u;
    }
}

void PrepareEditorLotusInputNchw(const std::vector<uint8_t> &diffuseRgba, uint32_t srcW,
                                 uint32_t srcH, uint32_t processingRes, bool seamless,
                                 EditorAiLotusPrep &outPrep)
{
    outPrep = {};
    if (srcW == 0 || srcH == 0 || diffuseRgba.size() != size_t(srcW) * size_t(srcH) * 4u)
    {
        return;
    }

    ComputeEditorAiUpscaleFitDimensions(srcW, srcH, processingRes, outPrep.fitW, outPrep.fitH);
    if (seamless)
    {
        const uint32_t maxHalo = std::min({ kSeamlessHaloMaxPx, outPrep.fitW, outPrep.fitH });
        outPrep.halo = (maxHalo / 32u) * 32u;
    }
    else
    {
        outPrep.halo = 0u;
    }
    outPrep.haloX = outPrep.halo;
    outPrep.haloY = outPrep.halo;
    outPrep.mirrorPad = false;

    FillPreparedLotusTensorNchw(diffuseRgba, srcW, srcH,
                                seamless ? EdgeSampleMode::Wrap : EdgeSampleMode::Clamp, outPrep);
}

void PrepareEditorLotusDepthInputNchw(const std::vector<uint8_t> &diffuseRgba, uint32_t srcW,
                                      uint32_t srcH, uint32_t processingRes, bool seamless,
                                      EditorAiLotusPrep &outPrep)
{
    outPrep = {};
    if (srcW == 0 || srcH == 0 || diffuseRgba.size() != size_t(srcW) * size_t(srcH) * 4u)
    {
        return;
    }

    EditorAiLotusDepthDims dims;
    ComputeEditorAiLotusDepthDimensions(srcW, srcH, processingRes, seamless, dims);
    outPrep.fitW = dims.fitW;
    outPrep.fitH = dims.fitH;
    outPrep.haloX = dims.haloX;
    outPrep.haloY = dims.haloY;
    outPrep.halo = std::min(outPrep.haloX, outPrep.haloY);
    outPrep.mirrorPad = !seamless;

    FillPreparedLotusTensorNchw(
        diffuseRgba, srcW, srcH, seamless ? EdgeSampleMode::Wrap : EdgeSampleMode::Mirror, outPrep);
}

namespace
{
void AverageNchwBoxToHwcPixel(const EditorAiTensorNchw &pred, uint32_t x0, uint32_t x1,
                              uint32_t y0, uint32_t y1, float *dstRgb)
{
    const uint32_t predW = uint32_t(pred.width);
    const size_t plane = size_t(predW) * size_t(pred.height);
    float sum[3] = {};
    uint32_t count = 0u;
    for (uint32_t sy = y0; sy < y1; sy++)
    {
        for (uint32_t sx = x0; sx < x1; sx++)
        {
            const size_t srcIdx = size_t(sy) * predW + sx;
            sum[0] += pred.data[0u * plane + srcIdx];
            sum[1] += pred.data[1u * plane + srcIdx];
            sum[2] += pred.data[2u * plane + srcIdx];
            count++;
        }
    }
    const float inv = count > 0u ? (1.0f / float(count)) : 0.0f;
    dstRgb[0] = std::clamp(sum[0] * inv, 0.0f, 1.0f);
    dstRgb[1] = std::clamp(sum[1] * inv, 0.0f, 1.0f);
    dstRgb[2] = std::clamp(sum[2] * inv, 0.0f, 1.0f);
}
} // namespace

void DownsampleEditorLotusOutputNchw(const EditorAiTensorNchw &pred, const EditorAiLotusPrep &prep,
                                     uint32_t dstW, uint32_t dstH, std::vector<float> &outHwc01)
{
    outHwc01.clear();
    const uint32_t hx = prep.haloX > 0u ? prep.haloX : prep.halo;
    const uint32_t hy = prep.haloY > 0u ? prep.haloY : prep.halo;
    if (pred.channels < 3 || pred.height <= 0 || pred.width <= 0 || prep.fitW == 0 ||
        prep.fitH == 0 || dstW == 0 || dstH == 0)
    {
        return;
    }
    const uint32_t predW = uint32_t(pred.width);
    const uint32_t predH = uint32_t(pred.height);
    const size_t plane = size_t(predW) * size_t(predH);
    if (pred.data.size() < 3u * plane || predW < prep.fitW + 2u * hx ||
        predH < prep.fitH + 2u * hy)
    {
        return;
    }

    outHwc01.resize(size_t(dstW) * size_t(dstH) * 3u);
    const float scaleX = float(prep.fitW) / float(dstW);
    const float scaleY = float(prep.fitH) / float(dstH);

    for (uint32_t dy = 0; dy < dstH; dy++)
    {
        const uint32_t sy0 = hy + uint32_t(std::floor(float(dy) * scaleY));
        const uint32_t sy1 = std::min(
            hy + prep.fitH,
            std::max(sy0 + 1u, hy + uint32_t(std::ceil(float(dy + 1u) * scaleY))));
        for (uint32_t dx = 0; dx < dstW; dx++)
        {
            const uint32_t sx0 = hx + uint32_t(std::floor(float(dx) * scaleX));
            const uint32_t sx1 = std::min(
                hx + prep.fitW,
                std::max(sx0 + 1u, hx + uint32_t(std::ceil(float(dx + 1u) * scaleX))));
            const size_t dstIdx = (size_t(dy) * dstW + dx) * 3u;
            AverageNchwBoxToHwcPixel(pred, sx0, sx1, sy0, sy1, outHwc01.data() + dstIdx);
        }
    }
}

void ComputeMeanRgbPlane01(const EditorAiTensorNchw &pred, size_t plane,
                           std::vector<float> &outPlane)
{
    outPlane.resize(plane);
    for (size_t i = 0; i < plane; i++)
    {
        outPlane[i] =
            (pred.data[0u * plane + i] + pred.data[1u * plane + i] + pred.data[2u * plane + i]) *
            (1.0f / 3.0f);
    }
}

void BoxDownsampleDetrendedHeight(const std::vector<float> &fullD, uint32_t workW,
                                  const EditorAiLotusPrep &prep, const double coef[3],
                                  uint32_t dstW, uint32_t dstH, std::vector<float> &outHeight)
{
    const uint32_t hx = prep.haloX > 0u ? prep.haloX : prep.halo;
    const uint32_t hy = prep.haloY > 0u ? prep.haloY : prep.halo;
    outHeight.resize(size_t(dstW) * size_t(dstH));
    const float scaleX = float(prep.fitW) / float(dstW);
    const float scaleY = float(prep.fitH) / float(dstH);
    for (uint32_t dy = 0; dy < dstH; dy++)
    {
        const uint32_t sy0 = hy + uint32_t(std::floor(float(dy) * scaleY));
        const uint32_t sy1 = std::min(
            hy + prep.fitH,
            std::max(sy0 + 1u, hy + uint32_t(std::ceil(float(dy + 1u) * scaleY))));
        for (uint32_t dx = 0; dx < dstW; dx++)
        {
            const uint32_t sx0 = hx + uint32_t(std::floor(float(dx) * scaleX));
            const uint32_t sx1 = std::min(
                hx + prep.fitW,
                std::max(sx0 + 1u, hx + uint32_t(std::ceil(float(dx + 1u) * scaleX))));
            double sum = 0.0;
            uint32_t count = 0u;
            for (uint32_t sy = sy0; sy < sy1; sy++)
            {
                const double vy = (double(sy) + 0.5 - double(hy)) / double(prep.fitH) * 2.0 - 1.0;
                for (uint32_t sx = sx0; sx < sx1; sx++)
                {
                    const double ux =
                        (double(sx) + 0.5 - double(hx)) / double(prep.fitW) * 2.0 - 1.0;
                    const double bias = coef[0] * ux + coef[1] * vy + coef[2] * ux * vy;
                    sum += double(fullD[size_t(sy) * workW + sx]) - bias;
                    count++;
                }
            }
            outHeight[size_t(dy) * dstW + dx] = count > 0u ? float(sum / double(count)) : 0.0f;
        }
    }
}

void SolveHaloTiltCoefs(const std::vector<float> &fullD, uint32_t workW, uint32_t workH,
                        const EditorAiLotusPrep &prep, double coef[3])
{
    std::fill(coef, coef + 3, 0.0);
    const uint32_t hx = prep.haloX > 0u ? prep.haloX : prep.halo;
    const uint32_t hy = prep.haloY > 0u ? prep.haloY : prep.halo;
    if (hx == 0u || hy == 0u)
    {
        return;
    }
    double AtA[3][3] = {};
    double Atb[3] = {};
    BuildHaloTiltNormalEquations(fullD, workW, workH, prep, AtA, Atb);
    if (!SolveLinearSystem3x3(AtA, Atb, coef))
    {
        std::fill(coef, coef + 3, 0.0);
    }
}

void ExtractDetrendedEditorLotusDepthHeight(const EditorAiTensorNchw &pred,
                                            const EditorAiLotusPrep &prep, uint32_t dstW,
                                            uint32_t dstH, std::vector<float> &outHeight)
{
    outHeight.clear();
    const uint32_t hx = prep.haloX > 0u ? prep.haloX : prep.halo;
    const uint32_t hy = prep.haloY > 0u ? prep.haloY : prep.halo;
    if (pred.channels < 3 || pred.height <= 0 || pred.width <= 0 || prep.fitW == 0u ||
        prep.fitH == 0u || dstW == 0u || dstH == 0u)
    {
        return;
    }
    const uint32_t workW = uint32_t(pred.width);
    const uint32_t workH = uint32_t(pred.height);
    const size_t plane = size_t(workW) * size_t(workH);
    if (pred.data.size() < 3u * plane || workW < prep.fitW + 2u * hx || workH < prep.fitH + 2u * hy)
    {
        return;
    }

    std::vector<float> fullD;
    ComputeMeanRgbPlane01(pred, plane, fullD);

    double coef[3] = {};
    SolveHaloTiltCoefs(fullD, workW, workH, prep, coef);

    BoxDownsampleDetrendedHeight(fullD, workW, prep, coef, dstW, dstH, outHeight);
    NormalizePercentile01(outHeight);
}

void ConvertEditorNormals01ToQuakeRayRgba(const std::vector<float> &hwc01, uint32_t width,
                                          uint32_t height, bool xIsLeft, bool yIsUp,
                                          std::vector<uint8_t> &outRgba)
{
    outRgba.clear();
    const size_t pixels = size_t(width) * size_t(height);
    if (width == 0 || height == 0 || hwc01.size() != pixels * 3u)
    {
        return;
    }

    outRgba.resize(pixels * 4u);
    for (size_t i = 0; i < pixels; i++)
    {
        const float vx = hwc01[i * 3u + 0u] * 2.0f - 1.0f;
        const float vy = hwc01[i * 3u + 1u] * 2.0f - 1.0f;
        const float vz = hwc01[i * 3u + 2u] * 2.0f - 1.0f;
        const float nx = xIsLeft ? -vx : vx;
        const float ny = yIsUp ? -vy : vy;
        const float nz = std::max(vz, 0.08f);
        WriteNormalizedNormalPixel(nx, ny, nz, outRgba.data() + i * 4u);
    }
}

void IntegrateEditorNormalMapHeight(const std::vector<uint8_t> &baseNormalRgba, uint32_t width,
                                    uint32_t height, bool seamless, std::vector<float> &outHeight)
{
    outHeight.clear();
    const size_t pixels = size_t(width) * size_t(height);
    if (width == 0 || height == 0 || baseNormalRgba.size() != pixels * 4u)
    {
        return;
    }

    std::vector<PoissonGridLevel> levels;
    levels.emplace_back();
    levels.back().w = width;
    levels.back().h = height;
    levels.back().hVal.assign(pixels, 0.0f);
    BuildPoissonDivergence(baseNormalRgba, width, height, seamless, levels.back().rhs);

    while (levels.back().w > 4u || levels.back().h > 4u)
    {
        levels.push_back(CoarsenPoissonLevel(levels.back(), seamless));
    }

    for (size_t lvlIdx = levels.size(); lvlIdx > 0u; lvlIdx--)
    {
        const size_t i = lvlIdx - 1u;
        PoissonGridLevel &cur = levels[i];
        if (i + 1u < levels.size())
        {
            const PoissonGridLevel &coarse = levels[i + 1u];
            for (uint32_t y = 0; y < cur.h; y++)
            {
                const float v = (float(y) + 0.5f) / float(cur.h);
                for (uint32_t x = 0; x < cur.w; x++)
                {
                    const float u = (float(x) + 0.5f) / float(cur.w);
                    cur.hVal[size_t(y) * cur.w + x] =
                        SampleGridBilinear(coarse.hVal, coarse.w, coarse.h, u, v, seamless);
                }
            }
        }
        const int iters = (i == levels.size() - 1u) ? 64 : 28;
        RelaxPoissonRedBlack(cur, seamless, iters);
    }

    outHeight = std::move(levels.front().hVal);
    NormalizePercentile01(outHeight);
}

namespace
{
bool DispatchAiPredictionBackend(const EditorAiInferenceRequest &activeReq,
                                 const std::filesystem::path &workerExe,
                                 const std::filesystem::path &modelPath,
                                 const std::atomic<bool> *cancelRequested,
                                 EditorAiInferenceResult &res)
{
    if (activeReq.backend == EditorAiBackend::StableDelight)
    {
        return RunDelightPredictionPipeline(activeReq, workerExe, modelPath, cancelRequested, res);
    }
    if (activeReq.backend == EditorAiBackend::RealPlksrPbrify)
    {
        return RunRealPlksrUpscalePipeline(activeReq, workerExe, modelPath, cancelRequested, res);
    }
    if (IsEditorAiLotusBackend(activeReq.backend))
    {
        return RunLotusPredictionPipeline(activeReq, workerExe, modelPath, cancelRequested, res);
    }
    return RunDeepBumpPredictionPipeline(activeReq, workerExe, modelPath, cancelRequested, res);
}
} // namespace

EditorAiInferenceResult RunEditorAiTextureInference(const EditorAiInferenceRequest &req,
                                                    const std::atomic<bool> *cancelRequested)
{
    EditorAiInferenceResult res;
    const size_t expectedBytes = size_t(req.width) * size_t(req.height) * 4u;
    if (req.width == 0 || req.height == 0 || req.diffuseRgba.size() != expectedBytes)
    {
        res.errorMessage = "Invalid diffuse texture buffer.";
        return res;
    }

    if (req.mapKind == EditorAiMapKind::Emissive)
    {
        BuildLocalEmissiveMap(req.diffuseRgba, req.width, req.height, res.rawColorRgba);
        res.outputWidth = req.width;
        res.outputHeight = req.height;
        res.providerLabel = "Native C++";
        res.ok = true;
        return res;
    }

    const std::filesystem::path workerExe = FindEditorAiWorkerExecutable(req.exeDir);
    if (workerExe.empty())
    {
        res.errorMessage = "AI inference worker missing (expected quakeray_ai_worker next to "
                           "quakerayeditor).";
        return res;
    }

    const std::filesystem::path modelPath =
        ResolveEditorAiModelPath(req.backend, req.exeDir, req.modelsDir);
    if (modelPath.empty())
    {
        res.errorMessage = std::string("Missing GGUF model: ") +
                           EditorAiBackendModelFilename(req.backend) + " in " +
                           DefaultEditorAiModelsDir().string();
        return res;
    }

    EditorAiInferenceRequest preparedStorage;
    const bool skipPrep =
        req.backend == EditorAiBackend::StableDelight ||
        req.backend == EditorAiBackend::RealPlksrPbrify ||
        req.mapKind == EditorAiMapKind::Delight || req.mapKind == EditorAiMapKind::Upscale;
    const EditorAiInferenceRequest &activeReq =
        skipPrep ? req : SelectPreparedAiRequest(req, preparedStorage);

    res.ok =
        DispatchAiPredictionBackend(activeReq, workerExe, modelPath, cancelRequested, res);
    if (!res.ok)
    {
        return res;
    }

    if (res.outputWidth == 0u || res.outputHeight == 0u)
    {
        res.outputWidth = req.width;
        res.outputHeight = req.height;
    }

    if (req.mapKind == EditorAiMapKind::Gloss)
    {
        SynthesizeEditorGlossFromDiffuseAndHeight(req.diffuseRgba, res.rawHeight, req.width,
                                                  req.height, res.rawColorRgba);
    }
    return res;
}

void ExtractEditorHeightFromRgba(const std::vector<uint8_t> &rgba, uint32_t srcWidth,
                                 uint32_t srcHeight, uint32_t dstWidth, uint32_t dstHeight,
                                 std::vector<float> &outHeight)
{
    outHeight.clear();
    if (srcWidth == 0 || srcHeight == 0 || dstWidth == 0 || dstHeight == 0 ||
        rgba.size() != size_t(srcWidth) * size_t(srcHeight) * 4u)
    {
        return;
    }

    const size_t pixels = size_t(dstWidth) * size_t(dstHeight);
    outHeight.resize(pixels);

    if (srcWidth == dstWidth && srcHeight == dstHeight)
    {
        for (size_t i = 0; i < pixels; i++)
        {
            outHeight[i] =
                RgbLuminance01(rgba[i * 4u + 0u], rgba[i * 4u + 1u], rgba[i * 4u + 2u]);
        }
        return;
    }

    for (uint32_t y = 0; y < dstHeight; y++)
    {
        const float v = (float(y) + 0.5f) / float(dstHeight);
        for (uint32_t x = 0; x < dstWidth; x++)
        {
            const float u = (float(x) + 0.5f) / float(dstWidth);
            float sample[4] = {};
            SampleRgbaBilinear(rgba.data(), srcWidth, srcHeight, u, v, sample);
            outHeight[size_t(y) * dstWidth + x] =
                (0.2126f * sample[0] + 0.7152f * sample[1] + 0.0722f * sample[2]) / 255.0f;
        }
    }
}

void ResizeEditorRgbaBilinear(const std::vector<uint8_t> &srcRgba, uint32_t srcWidth,
                              uint32_t srcHeight, uint32_t dstWidth, uint32_t dstHeight,
                              std::vector<uint8_t> &outRgba)
{
    outRgba.clear();
    if (srcWidth == 0 || srcHeight == 0 || dstWidth == 0 || dstHeight == 0 ||
        srcRgba.size() != size_t(srcWidth) * size_t(srcHeight) * 4u)
    {
        return;
    }
    if (srcWidth == dstWidth && srcHeight == dstHeight)
    {
        outRgba = srcRgba;
        return;
    }

    outRgba.resize(size_t(dstWidth) * size_t(dstHeight) * 4u);
    for (uint32_t y = 0; y < dstHeight; y++)
    {
        const float v = (float(y) + 0.5f) / float(dstHeight);
        for (uint32_t x = 0; x < dstWidth; x++)
        {
            const float u = (float(x) + 0.5f) / float(dstWidth);
            float sample[4] = {};
            SampleRgbaBilinear(srcRgba.data(), srcWidth, srcHeight, u, v, sample);
            uint8_t *dst = outRgba.data() + (size_t(y) * dstWidth + x) * 4u;
            for (int c = 0; c < 4; c++)
            {
                dst[c] = uint8_t(std::clamp(std::lround(sample[c]), 0l, 255l));
            }
        }
    }
}

void BuildEditorNormalMapFromHeight(const std::vector<float> &heightMap, uint32_t width,
                                    uint32_t height, const std::vector<uint8_t> *diffuseRgba,
                                    const EditorAiNormalParams &params,
                                    std::vector<uint8_t> &outNormalRgba)
{
    outNormalRgba.clear();
    const size_t pixels = size_t(width) * size_t(height);
    if (width == 0 || height == 0 || heightMap.size() != pixels)
    {
        return;
    }

    std::vector<float> field(pixels);
    for (size_t i = 0; i < pixels; i++)
    {
        const float h = std::clamp(heightMap[i], 0.0f, 1.0f);
        field[i] = params.invertHeight ? (1.0f - h) : h;
    }
    AddDiffuseMicroDetailField(width, height, diffuseRgba, params, field);
    outNormalRgba.resize(pixels * 4u);

    const float scale = 4.0f * std::clamp(params.strength, 0.05f, 6.0f);
    const float ySign = params.invertY ? 1.0f : -1.0f;
    const ScalarFieldView fieldView{ field, width, height, params.seamless };
    for (int y = 0; y < int(height); y++)
    {
        for (int x = 0; x < int(width); x++)
        {
            float dhdx = 0.0f;
            float dhdy = 0.0f;
            ComputeScharrSlopeAt(fieldView, x, y, dhdx, dhdy);
            WriteNormalizedNormalPixel(-dhdx * scale, ySign * dhdy * scale, 1.0f,
                                       outNormalRgba.data() + (size_t(y) * width + size_t(x)) * 4u);
        }
    }
}

void AdjustEditorNormalMap(const std::vector<uint8_t> &rawNormalRgba, uint32_t width,
                           uint32_t height, const std::vector<uint8_t> *diffuseRgba,
                           const EditorAiNormalParams &params,
                           std::vector<uint8_t> &outNormalRgba)
{
    outNormalRgba.clear();
    const size_t pixels = size_t(width) * size_t(height);
    if (width == 0 || height == 0 || rawNormalRgba.size() != pixels * 4u)
    {
        return;
    }

    std::vector<float> detailField;
    if (diffuseRgba != nullptr && diffuseRgba->size() == pixels * 4u && params.microDetail > 0.0f)
    {
        detailField.assign(pixels, 0.0f);
        AddDiffuseMicroDetailField(width, height, diffuseRgba, params, detailField);
    }

    outNormalRgba.resize(pixels * 4u);
    const float strength = std::clamp(params.strength, 0.05f, 6.0f);
    const float reliefSign = params.invertHeight ? -1.0f : 1.0f;
    const float yChannelSign = params.invertY ? -1.0f : 1.0f;
    const ScalarFieldView detailView{ detailField, width, height, params.seamless };

    for (int y = 0; y < int(height); y++)
    {
        for (int x = 0; x < int(width); x++)
        {
            const size_t idx = size_t(y) * width + size_t(x);
            const uint8_t *src = rawNormalRgba.data() + idx * 4u;
            const float bx = (float(src[0]) - 128.0f) / 127.0f;
            const float by = (float(src[1]) - 128.0f) / 127.0f;
            const float bz = std::max(0.08f, (float(src[2]) - 128.0f) / 127.0f);

            float nx = (bx / bz) * reliefSign * strength;
            float ny = (by / bz) * reliefSign * yChannelSign * strength;

            if (!detailField.empty())
            {
                float dhdx = 0.0f;
                float dhdy = 0.0f;
                ComputeScharrSlopeAt(detailView, x, y, dhdx, dhdy);
                nx += -dhdx * (4.0f * strength);
                ny += (params.invertY ? 1.0f : -1.0f) * dhdy * (4.0f * strength);
            }

            WriteNormalizedNormalPixel(nx, ny, 1.0f, outNormalRgba.data() + idx * 4u);
        }
    }
}

void BuildEditorHeightPreviewRgba(const std::vector<float> &heightMap, uint32_t width,
                                  uint32_t height, bool invertHeight,
                                  std::vector<uint8_t> &outRgba)
{
    const size_t pixels = size_t(width) * size_t(height);
    if (width == 0 || height == 0 || heightMap.size() != pixels)
    {
        outRgba.clear();
        return;
    }

    outRgba.resize(pixels * 4u);
    for (size_t i = 0; i < pixels; i++)
    {
        const float h = invertHeight ? (1.0f - heightMap[i]) : heightMap[i];
        const uint8_t v = FloatToByte(h);
        outRgba[i * 4u + 0u] = v;
        outRgba[i * 4u + 1u] = v;
        outRgba[i * 4u + 2u] = v;
        outRgba[i * 4u + 3u] = 255u;
    }
}

void BuildEditorNormalLitPreviewRgba(const std::vector<uint8_t> &diffuseRgba,
                                     const std::vector<uint8_t> &normalRgba, uint32_t width,
                                     uint32_t height, float lightAngleDeg,
                                     std::vector<uint8_t> &outRgba)
{
    const size_t pixels = size_t(width) * size_t(height);
    if (width == 0 || height == 0 || diffuseRgba.size() != pixels * 4u ||
        normalRgba.size() != pixels * 4u)
    {
        outRgba.clear();
        return;
    }

    float l[3] = {};
    ComputeLightDirection(lightAngleDeg, l);
    float hVec[3] = { l[0], l[1], l[2] + 1.0f };
    const float hInv = 1.0f / std::sqrt(hVec[0] * hVec[0] + hVec[1] * hVec[1] + hVec[2] * hVec[2]);
    hVec[0] *= hInv;
    hVec[1] *= hInv;
    hVec[2] *= hInv;

    outRgba.resize(pixels * 4u);
    for (size_t i = 0; i < pixels; i++)
    {
        const uint8_t *d = diffuseRgba.data() + i * 4u;
        const uint8_t *n = normalRgba.data() + i * 4u;
        float nx = (float(n[0]) - 128.0f) / 127.0f;
        float ny = (float(n[1]) - 128.0f) / 127.0f;
        float nz = std::max(0.05f, (float(n[2]) - 128.0f) / 127.0f);
        const float invN = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
        nx *= invN;
        ny *= invN;
        nz *= invN;

        const float ndl = std::max(0.0f, nx * l[0] + ny * l[1] + nz * l[2]);
        const float ndh = std::max(0.0f, nx * hVec[0] + ny * hVec[1] + nz * hVec[2]);
        const float spec = 0.32f * std::pow(ndh, 28.0f);
        const float lightMul = 0.22f + 0.85f * ndl;

        for (int c = 0; c < 3; c++)
        {
            const float lit = (float(d[c]) / 255.0f) * lightMul + spec;
            outRgba[i * 4u + size_t(c)] = FloatToByte(lit);
        }
        outRgba[i * 4u + 3u] = 255u;
    }
}

void SynthesizeEditorGlossFromDiffuseAndHeight(const std::vector<uint8_t> &diffuseRgba,
                                               const std::vector<float> &height, uint32_t width,
                                               uint32_t heightPx, std::vector<uint8_t> &outGlossRgba)
{
    SynthesizeGlossFromDiffuseAndHeight(diffuseRgba, height, width, heightPx, outGlossRgba);
}

void ApplyEditorGlossAdjustments(const std::vector<uint8_t> &rawRgba, uint32_t width,
                                 uint32_t height, const EditorAiToneParams &tone,
                                 std::vector<uint8_t> &outGlossRgba)
{
    const size_t pixels = size_t(width) * size_t(height);
    if (width == 0 || height == 0 || rawRgba.size() != pixels * 4u)
    {
        outGlossRgba.clear();
        return;
    }

    const float gain = std::clamp(tone.contrastGain, 0.1f, 4.0f);
    const float bias = std::clamp(tone.biasThreshold, -1.0f, 1.0f);
    outGlossRgba.resize(pixels * 4u);
    for (size_t i = 0; i < pixels; i++)
    {
        const float lum =
            RgbLuminance01(rawRgba[i * 4u + 0u], rawRgba[i * 4u + 1u], rawRgba[i * 4u + 2u]);
        const uint8_t v = FloatToByte((lum - 0.5f) * gain + 0.5f + bias);
        outGlossRgba[i * 4u + 0u] = v;
        outGlossRgba[i * 4u + 1u] = v;
        outGlossRgba[i * 4u + 2u] = v;
        outGlossRgba[i * 4u + 3u] = 255u;
    }
}

void BuildEditorGlossSpecularPreviewRgba(const std::vector<uint8_t> &diffuseRgba,
                                         const std::vector<uint8_t> &glossRgba, uint32_t width,
                                         uint32_t height, float lightAngleDeg,
                                         std::vector<uint8_t> &outRgba)
{
    const size_t pixels = size_t(width) * size_t(height);
    if (width == 0 || height == 0 || diffuseRgba.size() != pixels * 4u ||
        glossRgba.size() != pixels * 4u)
    {
        outRgba.clear();
        return;
    }

    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    const float rad = lightAngleDeg * kDegToRad;
    const float dx = std::cos(rad);
    const float dy = std::sin(rad);

    outRgba.resize(pixels * 4u);
    for (uint32_t y = 0; y < height; y++)
    {
        const float ny = (float(y) + 0.5f) / float(height) * 2.0f - 1.0f;
        for (uint32_t x = 0; x < width; x++)
        {
            const float nx = (float(x) + 0.5f) / float(width) * 2.0f - 1.0f;
            const float align = std::clamp(0.55f + 0.45f * (nx * dx + ny * dy), 0.0f, 1.0f);
            const size_t i = size_t(y) * width + x;
            const float gloss = float(glossRgba[i * 4u]) / 255.0f;
            const float shininess = 4.0f + gloss * 72.0f;
            const float highlight = (0.08f + 0.92f * gloss) * std::pow(align, shininess * 0.14f);

            for (int c = 0; c < 3; c++)
            {
                const float base = (float(diffuseRgba[i * 4u + size_t(c)]) / 255.0f) * 0.65f;
                outRgba[i * 4u + size_t(c)] = FloatToByte(base + highlight * 0.85f);
            }
            outRgba[i * 4u + 3u] = 255u;
        }
    }
}

void ApplyEditorEmissiveAdjustments(const std::vector<uint8_t> &rawRgba, uint32_t width,
                                    uint32_t height, const EditorAiToneParams &tone,
                                    std::vector<uint8_t> &outEmissiveRgba)
{
    const size_t pixels = size_t(width) * size_t(height);
    if (width == 0 || height == 0 || rawRgba.size() != pixels * 4u)
    {
        outEmissiveRgba.clear();
        return;
    }

    const float gain = std::clamp(tone.contrastGain, 0.1f, 4.0f);
    const float bias = std::clamp(tone.biasThreshold, -0.5f, 0.9f);
    const float floorCut = std::max(0.0f, bias);
    const float lift = std::max(0.0f, -bias);
    outEmissiveRgba.resize(pixels * 4u);

    for (size_t i = 0; i < pixels; i++)
    {
        const uint8_t *src = rawRgba.data() + i * 4u;
        const float maxChan = float(std::max({ src[0], src[1], src[2] })) / 255.0f;
        if (maxChan <= floorCut)
        {
            outEmissiveRgba[i * 4u + 0u] = 0u;
            outEmissiveRgba[i * 4u + 1u] = 0u;
            outEmissiveRgba[i * 4u + 2u] = 0u;
            outEmissiveRgba[i * 4u + 3u] = 255u;
            continue;
        }

        const float denom = std::max(1e-4f, 1.0f - floorCut);
        const float maskGain =
            ((maxChan - floorCut) / denom + lift) * gain / std::max(maxChan, 1e-4f);
        for (int c = 0; c < 3; c++)
        {
            outEmissiveRgba[i * 4u + size_t(c)] = FloatToByte((float(src[c]) / 255.0f) * maskGain);
        }
        outEmissiveRgba[i * 4u + 3u] = 255u;
    }
}

void BuildEditorEmissiveCompositePreviewRgba(const std::vector<uint8_t> &diffuseRgba,
                                             const std::vector<uint8_t> &emissiveRgba,
                                             uint32_t width, uint32_t height,
                                             std::vector<uint8_t> &outRgba)
{
    const size_t pixels = size_t(width) * size_t(height);
    if (width == 0 || height == 0 || diffuseRgba.size() != pixels * 4u ||
        emissiveRgba.size() != pixels * 4u)
    {
        outRgba.clear();
        return;
    }

    outRgba.resize(pixels * 4u);
    for (size_t i = 0; i < pixels; i++)
    {
        for (int c = 0; c < 3; c++)
        {
            const float d = (float(diffuseRgba[i * 4u + size_t(c)]) / 255.0f) * 0.35f;
            const float e = float(emissiveRgba[i * 4u + size_t(c)]) / 255.0f;
            outRgba[i * 4u + size_t(c)] = FloatToByte(d + e);
        }
        outRgba[i * 4u + 3u] = 255u;
    }
}

void ApplyEditorDelightAdjustments(const std::vector<uint8_t> &diffuseRgba,
                                   const std::vector<uint8_t> &rawDelitRgba, uint32_t width,
                                   uint32_t height, const EditorAiDelightParams &params,
                                   std::vector<uint8_t> &outRgba)
{
    const size_t pixels = size_t(width) * size_t(height);
    if (width == 0 || height == 0 || diffuseRgba.size() != pixels * 4u ||
        rawDelitRgba.size() != pixels * 4u)
    {
        outRgba.clear();
        return;
    }

    const float strength = std::clamp(params.strength, 0.0f, 1.0f);
    const float detailPreserve = std::clamp(params.detailPreserve, 0.0f, 1.0f);

    std::vector<float> origRgb;
    std::vector<float> origBlur;
    if (detailPreserve > 1e-4f)
    {
        origRgb.resize(pixels * 3u);
        for (size_t i = 0; i < pixels; i++)
        {
            origRgb[i * 3u + 0u] = float(diffuseRgba[i * 4u + 0u]) / 255.0f;
            origRgb[i * 3u + 1u] = float(diffuseRgba[i * 4u + 1u]) / 255.0f;
            origRgb[i * 3u + 2u] = float(diffuseRgba[i * 4u + 2u]) / 255.0f;
        }
        SmoothRgb3x3Pass(origRgb, width, height, params.seamless, origBlur);
    }

    outRgba.resize(pixels * 4u);
    for (size_t i = 0; i < pixels; i++)
    {
        const float origLum = RgbLuminance01(diffuseRgba[i * 4u + 0u], diffuseRgba[i * 4u + 1u],
                                             diffuseRgba[i * 4u + 2u]);
        const float delitLum = RgbLuminance01(rawDelitRgba[i * 4u + 0u], rawDelitRgba[i * 4u + 1u],
                                              rawDelitRgba[i * 4u + 2u]);
        // Reduce detail re-injection where bright highlights were stripped so glare stays cleanly removed.
        const float glareAtten =
            1.0f - std::clamp((origLum - delitLum) * 4.0f, 0.0f, 0.85f);
        for (size_t c = 0; c < 3u; c++)
        {
            const float d0 = float(diffuseRgba[i * 4u + c]) / 255.0f;
            const float d1 = float(rawDelitRgba[i * 4u + c]) / 255.0f;
            float val = d0 + strength * (d1 - d0);
            if (!origBlur.empty())
            {
                const float detail = d0 - origBlur[i * 3u + c];
                val += detailPreserve * strength * glareAtten * detail;
            }
            outRgba[i * 4u + c] = FloatToByte(val);
        }
        outRgba[i * 4u + 3u] = diffuseRgba[i * 4u + 3u];
    }
}

void BuildEditorDelightRemovedGlarePreviewRgba(const std::vector<uint8_t> &diffuseRgba,
                                               const std::vector<uint8_t> &delitRgba,
                                               uint32_t width, uint32_t height,
                                               std::vector<uint8_t> &outRgba)
{
    const size_t pixels = size_t(width) * size_t(height);
    if (width == 0 || height == 0 || diffuseRgba.size() != pixels * 4u ||
        delitRgba.size() != pixels * 4u)
    {
        outRgba.clear();
        return;
    }

    outRgba.resize(pixels * 4u);
    for (size_t i = 0; i < pixels; i++)
    {
        for (size_t c = 0; c < 3u; c++)
        {
            const float d0 = float(diffuseRgba[i * 4u + c]) / 255.0f;
            const float d1 = float(delitRgba[i * 4u + c]) / 255.0f;
            outRgba[i * 4u + c] = FloatToByte(std::abs(d0 - d1) * 2.0f);
        }
        outRgba[i * 4u + 3u] = 255u;
    }
}

namespace
{
void BoxDownsample2xRgba(const std::vector<uint8_t> &rawRgba, uint32_t rawW, uint32_t rawH,
                         std::vector<uint8_t> &outRgba, uint32_t &outW, uint32_t &outH)
{
    outW = rawW / 2u;
    outH = rawH / 2u;
    outRgba.resize(size_t(outW) * size_t(outH) * 4u);
    for (uint32_t y = 0u; y < outH; y++)
    {
        const uint32_t sy = y * 2u;
        for (uint32_t x = 0u; x < outW; x++)
        {
            const uint32_t sx = x * 2u;
            const uint8_t *p00 = rawRgba.data() + (size_t(sy) * rawW + sx) * 4u;
            const uint8_t *p10 = rawRgba.data() + (size_t(sy) * rawW + sx + 1u) * 4u;
            const uint8_t *p01 = rawRgba.data() + (size_t(sy + 1u) * rawW + sx) * 4u;
            const uint8_t *p11 = rawRgba.data() + (size_t(sy + 1u) * rawW + sx + 1u) * 4u;
            uint8_t *dst = outRgba.data() + (size_t(y) * outW + x) * 4u;
            for (int c = 0; c < 4; c++)
            {
                dst[c] = uint8_t(
                    (uint32_t(p00[c]) + uint32_t(p10[c]) + uint32_t(p01[c]) + uint32_t(p11[c]) +
                     2u) /
                    4u);
            }
        }
    }
}

void ApplyUpscaleUnsharpPass(uint32_t outW, uint32_t outH, bool seamless, float sharpen,
                             std::vector<uint8_t> &outRgba)
{
    if (!(sharpen > 1e-4f))
    {
        return;
    }
    const size_t outPixels = size_t(outW) * size_t(outH);
    std::vector<float> workRgb(outPixels * 3u);
    for (size_t i = 0; i < outPixels; i++)
    {
        workRgb[i * 3u + 0u] = float(outRgba[i * 4u + 0u]) / 255.0f;
        workRgb[i * 3u + 1u] = float(outRgba[i * 4u + 1u]) / 255.0f;
        workRgb[i * 3u + 2u] = float(outRgba[i * 4u + 2u]) / 255.0f;
    }
    std::vector<float> blurredRgb;
    SmoothRgb3x3Pass(workRgb, outW, outH, seamless, blurredRgb);
    const float sharpAmt = sharpen * 0.6f;
    for (size_t i = 0; i < outPixels; i++)
    {
        for (size_t c = 0; c < 3u; c++)
        {
            const float v = workRgb[i * 3u + c];
            outRgba[i * 4u + c] = FloatToByte(v + sharpAmt * (v - blurredRgb[i * 3u + c]));
        }
    }
}

void BlendUpscaledWithOriginal(const EditorAiUpscaleInput &in, uint32_t outW, uint32_t outH,
                               float strength, std::vector<uint8_t> &outRgba)
{
    if (!(strength < 0.999f))
    {
        return;
    }
    std::vector<uint8_t> baseResized;
    ResizeEditorRgbaBilinear(in.origDiffuseRgba, in.origW, in.origH, outW, outH, baseResized);
    const size_t outPixels = size_t(outW) * size_t(outH);
    for (size_t i = 0; i < outPixels; i++)
    {
        for (size_t c = 0; c < 3u; c++)
        {
            const float o = float(baseResized[i * 4u + c]) / 255.0f;
            const float u = float(outRgba[i * 4u + c]) / 255.0f;
            outRgba[i * 4u + c] = FloatToByte(o + strength * (u - o));
        }
    }
}
} // namespace

void ApplyEditorUpscaleAdjustments(const EditorAiUpscaleInput &in,
                                   const EditorAiUpscaleParams &params,
                                   std::vector<uint8_t> &outRgba, uint32_t &outW, uint32_t &outH)
{
    if (in.origW == 0u || in.origH == 0u || in.rawW == 0u || in.rawH == 0u ||
        in.origDiffuseRgba.size() != size_t(in.origW) * size_t(in.origH) * 4u ||
        in.rawUpscaledRgba.size() != size_t(in.rawW) * size_t(in.rawH) * 4u)
    {
        outRgba.clear();
        outW = 0u;
        outH = 0u;
        return;
    }

    if (params.outputScale == 2 && in.rawW >= 2u && in.rawH >= 2u)
    {
        BoxDownsample2xRgba(in.rawUpscaledRgba, in.rawW, in.rawH, outRgba, outW, outH);
    }
    else
    {
        outW = in.rawW;
        outH = in.rawH;
        outRgba = in.rawUpscaledRgba;
    }

    ApplyUpscaleUnsharpPass(outW, outH, params.seamless, std::clamp(params.sharpen, 0.0f, 1.0f),
                            outRgba);
    BlendUpscaledWithOriginal(in, outW, outH, std::clamp(params.strength, 0.0f, 1.0f), outRgba);
}

void BuildEditorUpscaleComparePreviewRgba(const std::vector<uint8_t> &origDiffuseRgba,
                                          uint32_t origW, uint32_t origH,
                                          const std::vector<uint8_t> &upscaledRgba, uint32_t outW,
                                          uint32_t outH, std::vector<uint8_t> &outRgba)
{
    if (origW == 0u || origH == 0u || outW == 0u || outH == 0u ||
        origDiffuseRgba.size() != size_t(origW) * size_t(origH) * 4u ||
        upscaledRgba.size() != size_t(outW) * size_t(outH) * 4u)
    {
        outRgba.clear();
        return;
    }

    std::vector<uint8_t> origResized;
    ResizeEditorRgbaBilinear(origDiffuseRgba, origW, origH, outW, outH, origResized);
    const size_t pixels = size_t(outW) * size_t(outH);
    outRgba.resize(pixels * 4u);
    const uint32_t splitX = outW / 2u;
    for (uint32_t y = 0u; y < outH; y++)
    {
        for (uint32_t x = 0u; x < outW; x++)
        {
            const size_t idx = (size_t(y) * outW + x) * 4u;
            if (x == splitX || (splitX > 0u && x == splitX - 1u))
            {
                outRgba[idx + 0u] = 245u;
                outRgba[idx + 1u] = 245u;
                outRgba[idx + 2u] = 245u;
                outRgba[idx + 3u] = 255u;
                continue;
            }
            const std::vector<uint8_t> &src = (x < splitX) ? origResized : upscaledRgba;
            outRgba[idx + 0u] = src[idx + 0u];
            outRgba[idx + 1u] = src[idx + 1u];
            outRgba[idx + 2u] = src[idx + 2u];
            outRgba[idx + 3u] = 255u;
        }
    }
}

void BuildEditorUpscaleDetailPreviewRgba(const std::vector<uint8_t> &origDiffuseRgba,
                                         uint32_t origW, uint32_t origH,
                                         const std::vector<uint8_t> &upscaledRgba, uint32_t outW,
                                         uint32_t outH, std::vector<uint8_t> &outRgba)
{
    if (origW == 0u || origH == 0u || outW == 0u || outH == 0u ||
        origDiffuseRgba.size() != size_t(origW) * size_t(origH) * 4u ||
        upscaledRgba.size() != size_t(outW) * size_t(outH) * 4u)
    {
        outRgba.clear();
        return;
    }

    std::vector<uint8_t> origResized;
    ResizeEditorRgbaBilinear(origDiffuseRgba, origW, origH, outW, outH, origResized);
    const size_t pixels = size_t(outW) * size_t(outH);
    outRgba.resize(pixels * 4u);
    for (size_t i = 0; i < pixels; i++)
    {
        for (size_t c = 0; c < 3u; c++)
        {
            const float o = float(origResized[i * 4u + c]) / 255.0f;
            const float u = float(upscaledRgba[i * 4u + c]) / 255.0f;
            outRgba[i * 4u + c] = FloatToByte(0.5f + 2.0f * (u - o));
        }
        outRgba[i * 4u + 3u] = 255u;
    }
}

void PreprocessEditorAiDiffuse(const std::vector<uint8_t> &diffuseRgba, uint32_t width,
                               uint32_t height, const EditorAiDiffusePrepParams &params,
                               std::vector<uint8_t> &outRgba)
{
    const size_t pixels = size_t(width) * size_t(height);
    if (width == 0 || height == 0 || diffuseRgba.size() != pixels * 4u)
    {
        outRgba.clear();
        return;
    }

    const float flatten = std::clamp(params.flattenLighting, 0.0f, 1.0f);
    const float smooth = std::clamp(params.preSmooth, 0.0f, 1.0f);
    if (!(flatten > 1e-4f) && !(smooth > 1e-4f))
    {
        outRgba = diffuseRgba;
        return;
    }

    std::vector<float> rgb(pixels * 3u);
    for (size_t i = 0; i < pixels; i++)
    {
        rgb[i * 3u + 0u] = float(diffuseRgba[i * 4u + 0u]) / 255.0f;
        rgb[i * 3u + 1u] = float(diffuseRgba[i * 4u + 1u]) / 255.0f;
        rgb[i * 3u + 2u] = float(diffuseRgba[i * 4u + 2u]) / 255.0f;
    }

    ApplyPreSmoothRgb(width, height, params, rgb);
    ApplyFlattenLightingRgb(width, height, params, rgb);

    outRgba.resize(pixels * 4u);
    for (size_t i = 0; i < pixels; i++)
    {
        outRgba[i * 4u + 0u] = FloatToByte(rgb[i * 3u + 0u]);
        outRgba[i * 4u + 1u] = FloatToByte(rgb[i * 3u + 1u]);
        outRgba[i * 4u + 2u] = FloatToByte(rgb[i * 3u + 2u]);
        outRgba[i * 4u + 3u] = diffuseRgba[i * 4u + 3u];
    }
}

bool WriteEditorRgbaTga(const std::filesystem::path &path, uint32_t width, uint32_t height,
                        const std::vector<uint8_t> &rgba)
{
    const size_t pixels = size_t(width) * size_t(height);
    if (width == 0 || height == 0 || width > 65535u || height > 65535u ||
        rgba.size() != pixels * 4u)
    {
        return false;
    }

    std::error_code ec;
    if (path.has_parent_path())
    {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec)
        {
            return false;
        }
    }

    std::vector<uint8_t> bytes(18u + pixels * 4u, 0u);
    bytes[2] = 2u;
    bytes[12] = uint8_t(width & 0xFFu);
    bytes[13] = uint8_t((width >> 8u) & 0xFFu);
    bytes[14] = uint8_t(height & 0xFFu);
    bytes[15] = uint8_t((height >> 8u) & 0xFFu);
    bytes[16] = 32u;
    bytes[17] = 0x08u;

    uint8_t *dst = bytes.data() + 18u;
    for (uint32_t row = 0; row < height; row++)
    {
        const uint32_t srcY = height - 1u - row;
        const uint8_t *srcRow = rgba.data() + size_t(srcY) * width * 4u;
        for (uint32_t x = 0; x < width; x++)
        {
            dst[0] = srcRow[x * 4u + 2u];
            dst[1] = srcRow[x * 4u + 1u];
            dst[2] = srcRow[x * 4u + 0u];
            dst[3] = srcRow[x * 4u + 3u];
            dst += 4;
        }
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        return false;
    }
    out.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
    return out.good();
}

std::string BuildGeneratedTextureRelativePath(const std::string &materialName, EditorAiMapKind kind)
{
    static constexpr std::string_view kInvalidChars = ":*?\"<>| ";
    std::string stem = LowerAscii(materialName);
    for (char &ch : stem)
    {
        if (ch == '\\')
        {
            ch = '/';
        }
        else if (kInvalidChars.find(ch) != std::string_view::npos)
        {
            ch = '_';
        }
    }
    while (stem.rfind("./", 0) == 0)
    {
        stem.erase(0, 2);
    }
    while (!stem.empty() && stem.front() == '/')
    {
        stem.erase(stem.begin());
    }
    if (stem.rfind("textures/", 0) == 0)
    {
        stem.erase(0, 9);
    }
    const size_t dot = stem.find_last_of('.');
    const size_t slash = stem.find_last_of('/');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
    {
        stem.resize(dot);
    }
    if (stem.empty())
    {
        stem = "material";
    }
    return "textures/ai_generated/" + stem + "_" + EditorAiMapKindId(kind) + ".tga";
}

std::string BuildUniqueGeneratedTextureRelativePath(
    const std::filesystem::path &outRoot, const std::string &materialName, EditorAiMapKind kind,
    const std::function<bool(const std::string &)> &extraExists)
{
    std::string base = BuildGeneratedTextureRelativePath(materialName, kind);
    const auto isTaken = [&outRoot, &extraExists](const std::string &candidate) {
        std::error_code ec;
        if (std::filesystem::exists(outRoot / candidate, ec))
        {
            return true;
        }
        return extraExists && extraExists(candidate);
    };

    if (!isTaken(base))
    {
        return base;
    }

    const size_t dot = base.rfind(".tga");
    const std::string prefix = (dot != std::string::npos) ? base.substr(0, dot) : base;
    const std::string ext = (dot != std::string::npos) ? base.substr(dot) : ".tga";
    for (uint32_t suffix = 2u; suffix < 100000u; suffix++)
    {
        std::string candidate = prefix;
        candidate += '_';
        candidate += std::to_string(suffix);
        candidate += ext;
        if (!isTaken(candidate))
        {
            return candidate;
        }
    }
    return {};
}
