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
constexpr uint32_t kDeepBumpTileSize = 256u;
constexpr uint32_t kDeepBumpTileStride = 192u;

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
    const std::string low = LowerAscii(TrimWhitespace(raw));
    if (low == "lotus_normal_d" || low == "lotus_normal" || low == "lotus" ||
        low == "lotus_normal_g")
    {
        return EditorAiBackend::LotusNormalD;
    }
    if (low == "lotus_depth_d" || low == "lotus_depth")
    {
        return EditorAiBackend::LotusDepthD;
    }
    if (low == "deepbump" || low == "local")
    {
        return EditorAiBackend::DeepBump;
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

int WrapOrClampCoord(int coord, int limit, bool seamless)
{
    if (limit <= 0)
    {
        return 0;
    }
    if (seamless)
    {
        const int rem = coord % limit;
        return rem < 0 ? rem + limit : rem;
    }
    return std::clamp(coord, 0, limit - 1);
}

void SampleRgbaBilinearWrapped(const uint8_t *src, uint32_t srcW, uint32_t srcH, float u, float v,
                               bool seamless, float out[4])
{
    const float rawFx = u * float(srcW) - 0.5f;
    const float rawFy = v * float(srcH) - 0.5f;
    const int xBase = int(std::floor(rawFx));
    const int yBase = int(std::floor(rawFy));
    const float tx = rawFx - float(xBase);
    const float ty = rawFy - float(yBase);

    const int x0 = WrapOrClampCoord(xBase, int(srcW), seamless);
    const int y0 = WrapOrClampCoord(yBase, int(srcH), seamless);
    const int x1 = WrapOrClampCoord(xBase + 1, int(srcW), seamless);
    const int y1 = WrapOrClampCoord(yBase + 1, int(srcH), seamless);

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

std::vector<float> BuildDeepBumpTileWeights()
{
    constexpr uint32_t kMargin = kDeepBumpTileSize - kDeepBumpTileStride;
    std::array<float, kDeepBumpTileSize> w1d{};
    w1d.fill(1.0f);
    for (uint32_t i = 0; i < kMargin; i++)
    {
        const float t = float(i + 1u) / float(kMargin + 1u);
        w1d[i] = t;
        w1d[kDeepBumpTileSize - 1u - i] = t;
    }
    std::vector<float> weights(size_t(kDeepBumpTileSize) * kDeepBumpTileSize);
    for (uint32_t y = 0; y < kDeepBumpTileSize; y++)
    {
        for (uint32_t x = 0; x < kDeepBumpTileSize; x++)
        {
            weights[size_t(y) * kDeepBumpTileSize + x] = w1d[y] * w1d[x];
        }
    }
    return weights;
}

void SynthesizeGlossFromDiffuseAndHeight(const std::vector<uint8_t> &diffuseRgba,
                                         const std::vector<float> &height, uint32_t width,
                                         uint32_t heightPx, std::vector<uint8_t> &outGlossRgba)
{
    const size_t pixels = size_t(width) * size_t(heightPx);
    outGlossRgba.resize(pixels * 4u);
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
        const float val = std::clamp(0.20f + 0.50f * metallicCue + 0.35f * (hVal - 0.5f),
                                     0.06f, 0.92f);
        const uint8_t gv = FloatToByte(val);
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

bool RunLotusPredictionPipeline(const EditorAiInferenceRequest &req,
                                const std::filesystem::path &modelPath,
                                const std::atomic<bool> *cancelRequested,
                                EditorAiInferenceResult &out)
{
    EditorAiLotusPrep prep;
    PrepareEditorLotusInputNchw(req.diffuseRgba, req.width, req.height, req.processingRes,
                                req.seamless, prep);

    EditorOnnxTensorNchw pred;
    if (!RunEditorOnnxModelNchw(modelPath, req.exeDir, prep.tensor, pred, cancelRequested,
                                out.providerLabel, out.errorMessage))
    {
        out.cancelled = (cancelRequested != nullptr &&
                         cancelRequested->load(std::memory_order_relaxed));
        return false;
    }

    std::vector<float> hwc01;
    DownsampleEditorLotusOutputNchw(pred, prep, req.width, req.height, hwc01);
    const size_t pixels = size_t(req.width) * size_t(req.height);
    if (hwc01.size() != pixels * 3u)
    {
        out.errorMessage = "Lotus ONNX output dimensions mismatch.";
        return false;
    }

    if (req.backend == EditorAiBackend::LotusDepthD)
    {
        out.rawHeight.resize(pixels);
        for (size_t i = 0; i < pixels; i++)
        {
            out.rawHeight[i] =
                (hwc01[i * 3u + 0u] + hwc01[i * 3u + 1u] + hwc01[i * 3u + 2u]) * (1.0f / 3.0f);
        }
        NormalizePercentile01(out.rawHeight);
    }
    else
    {
        ConvertEditorNormals01ToQuakeRayRgba(hwc01, req.width, req.height, true, true,
                                             out.rawNormalRgba);
        IntegrateEditorNormalMapHeight(out.rawNormalRgba, req.width, req.height, req.seamless,
                                       out.rawHeight);
    }
    return true;
}

struct DeepBumpAccumulator
{
    const EditorAiInferenceRequest &req;
    const std::filesystem::path &modelPath;
    const std::vector<float> &luma;
    const std::vector<float> &weights;
    const std::atomic<bool> *cancelRequested = nullptr;
    EditorAiInferenceResult &out;
    std::vector<float> acc;
    std::vector<float> wsum;
};

void FillDeepBumpTileInput(const DeepBumpAccumulator &ctx, uint32_t x0, uint32_t y0,
                           EditorOnnxTensorNchw &tileIn)
{
    const uint32_t w = ctx.req.width;
    const uint32_t h = ctx.req.height;
    for (uint32_t dy = 0; dy < kDeepBumpTileSize; dy++)
    {
        const int sy = WrapOrClampCoord(int(y0 + dy), int(h), ctx.req.seamless);
        for (uint32_t dx = 0; dx < kDeepBumpTileSize; dx++)
        {
            const int sx = WrapOrClampCoord(int(x0 + dx), int(w), ctx.req.seamless);
            tileIn.data[size_t(dy) * kDeepBumpTileSize + dx] =
                ctx.luma[size_t(sy) * w + size_t(sx)];
        }
    }
}

void AccumulateDeepBumpTileOutput(DeepBumpAccumulator &ctx, uint32_t x0, uint32_t y0,
                                  const EditorOnnxTensorNchw &tileOut)
{
    constexpr size_t kPlane = size_t(kDeepBumpTileSize) * kDeepBumpTileSize;
    const uint32_t w = ctx.req.width;
    const uint32_t h = ctx.req.height;
    for (uint32_t dy = 0; dy < kDeepBumpTileSize; dy++)
    {
        const int sy = WrapOrClampCoord(int(y0 + dy), int(h), ctx.req.seamless);
        for (uint32_t dx = 0; dx < kDeepBumpTileSize; dx++)
        {
            const int sx = WrapOrClampCoord(int(x0 + dx), int(w), ctx.req.seamless);
            const size_t srcIdx = size_t(dy) * kDeepBumpTileSize + dx;
            const size_t dstIdx = size_t(sy) * w + size_t(sx);
            const float wt = ctx.weights[srcIdx];
            ctx.acc[dstIdx * 3u + 0u] += tileOut.data[srcIdx] * wt;
            ctx.acc[dstIdx * 3u + 1u] += tileOut.data[kPlane + srcIdx] * wt;
            ctx.acc[dstIdx * 3u + 2u] += tileOut.data[2u * kPlane + srcIdx] * wt;
            ctx.wsum[dstIdx] += wt;
        }
    }
}

bool ProcessSingleDeepBumpTile(DeepBumpAccumulator &ctx, uint32_t x0, uint32_t y0,
                               EditorOnnxTensorNchw &tileIn)
{
    if (ctx.cancelRequested != nullptr && ctx.cancelRequested->load(std::memory_order_relaxed))
    {
        ctx.out.cancelled = true;
        return false;
    }
    FillDeepBumpTileInput(ctx, x0, y0, tileIn);

    EditorOnnxTensorNchw tileOut;
    if (!RunEditorOnnxModelNchw(ctx.modelPath, ctx.req.exeDir, tileIn, tileOut, ctx.cancelRequested,
                                ctx.out.providerLabel, ctx.out.errorMessage))
    {
        ctx.out.cancelled = (ctx.cancelRequested != nullptr &&
                             ctx.cancelRequested->load(std::memory_order_relaxed));
        return false;
    }
    if (tileOut.channels != 3 || tileOut.height != int64_t(kDeepBumpTileSize) ||
        tileOut.width != int64_t(kDeepBumpTileSize))
    {
        ctx.out.errorMessage = "Unexpected DeepBump tile output shape.";
        return false;
    }

    AccumulateDeepBumpTileOutput(ctx, x0, y0, tileOut);
    return true;
}

bool RunDeepBumpPredictionPipeline(const EditorAiInferenceRequest &req,
                                   const std::filesystem::path &modelPath,
                                   const std::atomic<bool> *cancelRequested,
                                   EditorAiInferenceResult &out)
{
    const size_t pixels = size_t(req.width) * size_t(req.height);
    std::vector<float> luma(pixels);
    for (size_t i = 0; i < pixels; i++)
    {
        const float r = float(req.diffuseRgba[i * 4u + 0u]) / 255.0f;
        const float g = float(req.diffuseRgba[i * 4u + 1u]) / 255.0f;
        const float b = float(req.diffuseRgba[i * 4u + 2u]) / 255.0f;
        luma[i] = 0.299f * r + 0.587f * g + 0.114f * b;
    }

    const std::vector<float> weights = BuildDeepBumpTileWeights();
    DeepBumpAccumulator ctx{ req, modelPath, luma, weights, cancelRequested, out, {}, {} };
    ctx.acc.assign(pixels * 3u, 0.0f);
    ctx.wsum.assign(pixels, 0.0f);

    EditorOnnxTensorNchw tileIn;
    tileIn.channels = 1;
    tileIn.height = int64_t(kDeepBumpTileSize);
    tileIn.width = int64_t(kDeepBumpTileSize);
    tileIn.data.resize(size_t(kDeepBumpTileSize) * kDeepBumpTileSize);

    for (uint32_t y0 = 0; y0 < req.height; y0 += kDeepBumpTileStride)
    {
        for (uint32_t x0 = 0; x0 < req.width; x0 += kDeepBumpTileStride)
        {
            if (!ProcessSingleDeepBumpTile(ctx, x0, y0, tileIn))
            {
                return false;
            }
        }
    }

    for (size_t i = 0; i < pixels; i++)
    {
        const float invW = 1.0f / std::max(ctx.wsum[i], 1e-6f);
        ctx.acc[i * 3u + 0u] *= invW;
        ctx.acc[i * 3u + 1u] *= invW;
        ctx.acc[i * 3u + 2u] *= invW;
    }

    ConvertEditorNormals01ToQuakeRayRgba(ctx.acc, req.width, req.height, false, true,
                                         out.rawNormalRgba);
    IntegrateEditorNormalMapHeight(out.rawNormalRgba, req.width, req.height, req.seamless,
                                   out.rawHeight);
    return true;
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
    }
    return "lotus_normal_d";
}

const char *EditorAiBackendDisplayName(EditorAiBackend backend)
{
    switch (backend)
    {
    case EditorAiBackend::LotusNormalD:
        return "Lotus Normal (ONNX)";
    case EditorAiBackend::LotusDepthD:
        return "Lotus Depth (ONNX)";
    case EditorAiBackend::DeepBump:
        return "DeepBump (Fast ONNX)";
    }
    return "Lotus Normal (ONNX)";
}

const char *EditorAiBackendModelFilename(EditorAiBackend backend)
{
    switch (backend)
    {
    case EditorAiBackend::LotusNormalD:
        return "lotus-normal-d-v1-1.onnx";
    case EditorAiBackend::LotusDepthD:
        return "lotus-depth-d-v2-0-disparity.onnx";
    case EditorAiBackend::DeepBump:
        return "deepbump256.onnx";
    }
    return "lotus-normal-d-v1-1.onnx";
}

bool IsEditorAiLotusBackend(EditorAiBackend backend)
{
    return backend != EditorAiBackend::DeepBump;
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
    const auto roundTo64 = [](double val) -> uint32_t {
        const long rounded = std::lround(val / 64.0) * 64l;
        return uint32_t(std::max(64l, rounded));
    };
    outFitW = roundTo64(double(srcW) * scale);
    outFitH = roundTo64(double(srcH) * scale);
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

    const uint32_t workW = outPrep.fitW + 2u * outPrep.halo;
    const uint32_t workH = outPrep.fitH + 2u * outPrep.halo;
    const size_t plane = size_t(workW) * size_t(workH);
    outPrep.tensor.channels = 3;
    outPrep.tensor.height = int64_t(workH);
    outPrep.tensor.width = int64_t(workW);
    outPrep.tensor.data.resize(3u * plane);

    for (uint32_t y = 0; y < workH; y++)
    {
        const float v = (float(y) + 0.5f - float(outPrep.halo)) / float(outPrep.fitH);
        for (uint32_t x = 0; x < workW; x++)
        {
            const float u = (float(x) + 0.5f - float(outPrep.halo)) / float(outPrep.fitW);
            float sample[4] = {};
            SampleRgbaBilinearWrapped(diffuseRgba.data(), srcW, srcH, u, v, seamless, sample);
            const size_t idx = size_t(y) * workW + x;
            outPrep.tensor.data[0u * plane + idx] = std::clamp(sample[0] / 255.0f, 0.0f, 1.0f);
            outPrep.tensor.data[1u * plane + idx] = std::clamp(sample[1] / 255.0f, 0.0f, 1.0f);
            outPrep.tensor.data[2u * plane + idx] = std::clamp(sample[2] / 255.0f, 0.0f, 1.0f);
        }
    }
}

namespace
{
void AverageNchwBoxToHwcPixel(const EditorOnnxTensorNchw &pred, uint32_t x0, uint32_t x1,
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

void DownsampleEditorLotusOutputNchw(const EditorOnnxTensorNchw &pred, const EditorAiLotusPrep &prep,
                                     uint32_t dstW, uint32_t dstH, std::vector<float> &outHwc01)
{
    outHwc01.clear();
    if (pred.channels < 3 || pred.height <= 0 || pred.width <= 0 || prep.fitW == 0 ||
        prep.fitH == 0 || dstW == 0 || dstH == 0)
    {
        return;
    }
    const uint32_t predW = uint32_t(pred.width);
    const uint32_t predH = uint32_t(pred.height);
    const size_t plane = size_t(predW) * size_t(predH);
    if (pred.data.size() < 3u * plane || predW < prep.fitW + 2u * prep.halo ||
        predH < prep.fitH + 2u * prep.halo)
    {
        return;
    }

    outHwc01.resize(size_t(dstW) * size_t(dstH) * 3u);
    const float scaleX = float(prep.fitW) / float(dstW);
    const float scaleY = float(prep.fitH) / float(dstH);

    for (uint32_t dy = 0; dy < dstH; dy++)
    {
        const uint32_t sy0 = prep.halo + uint32_t(std::floor(float(dy) * scaleY));
        const uint32_t sy1 = std::min(
            prep.halo + prep.fitH,
            std::max(sy0 + 1u, prep.halo + uint32_t(std::ceil(float(dy + 1u) * scaleY))));
        for (uint32_t dx = 0; dx < dstW; dx++)
        {
            const uint32_t sx0 = prep.halo + uint32_t(std::floor(float(dx) * scaleX));
            const uint32_t sx1 = std::min(
                prep.halo + prep.fitW,
                std::max(sx0 + 1u, prep.halo + uint32_t(std::ceil(float(dx + 1u) * scaleX))));
            const size_t dstIdx = (size_t(dy) * dstW + dx) * 3u;
            AverageNchwBoxToHwcPixel(pred, sx0, sx1, sy0, sy1, outHwc01.data() + dstIdx);
        }
    }
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
        res.providerLabel = "Native C++";
        res.ok = true;
        return res;
    }

    if (!IsEditorOnnxRuntimeAvailable(req.exeDir))
    {
        res.errorMessage =
            "ONNX Runtime library missing (expected libonnxruntime next to executable or in "
            "~/.cache/quakeray/onnx/lib).";
        return res;
    }

    const std::filesystem::path modelPath =
        ResolveEditorAiModelPath(req.backend, req.exeDir, req.modelsDir);
    if (modelPath.empty())
    {
        res.errorMessage = std::string("Missing ONNX model: ") +
                           EditorAiBackendModelFilename(req.backend) + " in " +
                           DefaultEditorAiModelsDir().string();
        return res;
    }

    if (IsEditorAiLotusBackend(req.backend))
    {
        res.ok = RunLotusPredictionPipeline(req, modelPath, cancelRequested, res);
    }
    else
    {
        res.ok = RunDeepBumpPredictionPipeline(req, modelPath, cancelRequested, res);
    }
    if (!res.ok)
    {
        return res;
    }

    if (req.mapKind == EditorAiMapKind::Gloss)
    {
        SynthesizeGlossFromDiffuseAndHeight(req.diffuseRgba, res.rawHeight, req.width, req.height,
                                            res.rawColorRgba);
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
