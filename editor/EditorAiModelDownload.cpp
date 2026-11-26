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

#include "EditorAiModelDownload.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#    include <winhttp.h>
#else
#    include <dlfcn.h>
#endif

namespace
{

constexpr uint32_t Ror32(uint32_t val, unsigned bits)
{
    return (val >> bits) | (val << (32u - bits));
}

class Sha256Hasher
{
  public:
    Sha256Hasher()
        : m_state{ 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u }
    {
    }

    void Update(const uint8_t *data, size_t size)
    {
        if (data == nullptr || size == 0)
        {
            return;
        }
        m_totalBytes += uint64_t(size);
        size_t offset = 0;
        if (m_blockUsed > 0)
        {
            const size_t take = std::min(size, size_t(64u) - m_blockUsed);
            std::memcpy(m_block.data() + m_blockUsed, data, take);
            m_blockUsed += take;
            offset += take;
            if (m_blockUsed == 64u)
            {
                TransformBlock(m_block.data());
                m_blockUsed = 0;
            }
        }
        while (offset + 64u <= size)
        {
            TransformBlock(data + offset);
            offset += 64u;
        }
        if (offset < size)
        {
            m_blockUsed = size - offset;
            std::memcpy(m_block.data(), data + offset, m_blockUsed);
        }
    }

    std::string FinalHex()
    {
        std::array<uint8_t, 64> pad{};
        pad[0] = 0x80u;
        const uint64_t totalBits = m_totalBytes * 8ull;
        const size_t padLen = (m_blockUsed < 56u) ? (56u - m_blockUsed) : (120u - m_blockUsed);
        Update(pad.data(), padLen);

        std::array<uint8_t, 8> lenBe{};
        for (int i = 0; i < 8; i++)
        {
            lenBe[size_t(7 - i)] = uint8_t((totalBits >> unsigned(i * 8)) & 0xFFu);
        }
        // Feed length directly so FinalHex does not re-include padding in bit count.
        std::memcpy(m_block.data() + 56u, lenBe.data(), 8u);
        TransformBlock(m_block.data());

        static constexpr char kHex[] = "0123456789abcdef";
        std::string out(64u, '0');
        for (size_t i = 0; i < 8u; i++)
        {
            const uint32_t w = m_state[i];
            for (size_t b = 0; b < 4u; b++)
            {
                const uint8_t byte = uint8_t((w >> unsigned((3u - b) * 8u)) & 0xFFu);
                out[i * 8u + b * 2u + 0u] = kHex[(byte >> 4u) & 0x0Fu];
                out[i * 8u + b * 2u + 1u] = kHex[byte & 0x0Fu];
            }
        }
        return out;
    }

  private:
    void TransformBlock(const uint8_t *blk)
    {
        static constexpr std::array<uint32_t, 64> kK = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
            0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
            0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
            0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
            0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
            0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
            0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
            0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
            0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
        };

        std::array<uint32_t, 64> w{};
        for (size_t i = 0; i < 16u; i++)
        {
            w[i] = (uint32_t(blk[i * 4u + 0u]) << 24u) | (uint32_t(blk[i * 4u + 1u]) << 16u) |
                   (uint32_t(blk[i * 4u + 2u]) << 8u) | uint32_t(blk[i * 4u + 3u]);
        }
        for (size_t i = 16u; i < 64u; i++)
        {
            const uint32_t s0 = Ror32(w[i - 15u], 7u) ^ Ror32(w[i - 15u], 18u) ^ (w[i - 15u] >> 3u);
            const uint32_t s1 = Ror32(w[i - 2u], 17u) ^ Ror32(w[i - 2u], 19u) ^ (w[i - 2u] >> 10u);
            w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
        }

        uint32_t a = m_state[0];
        uint32_t b = m_state[1];
        uint32_t c = m_state[2];
        uint32_t d = m_state[3];
        uint32_t e = m_state[4];
        uint32_t f = m_state[5];
        uint32_t g = m_state[6];
        uint32_t h = m_state[7];

        for (size_t i = 0; i < 64u; i++)
        {
            const uint32_t s1 = Ror32(e, 6u) ^ Ror32(e, 11u) ^ Ror32(e, 25u);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = h + s1 + ch + kK[i] + w[i];
            const uint32_t s0 = Ror32(a, 2u) ^ Ror32(a, 13u) ^ Ror32(a, 22u);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = s0 + maj;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        m_state[0] += a;
        m_state[1] += b;
        m_state[2] += c;
        m_state[3] += d;
        m_state[4] += e;
        m_state[5] += f;
        m_state[6] += g;
        m_state[7] += h;
    }

    std::array<uint32_t, 8> m_state{};
    std::array<uint8_t, 64> m_block{};
    size_t m_blockUsed = 0;
    uint64_t m_totalBytes = 0;
};

std::string LowerAsciiHex(std::string s)
{
    for (char &ch : s)
    {
        ch = char(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}

struct DownloadStreamContext
{
    std::ofstream file;
    Sha256Hasher sha;
    EditorAiDownloadProgress *progress = nullptr;
    const std::atomic<bool> *cancelRequested = nullptr;
    uint64_t writtenBytes = 0;
    bool writeError = false;
    bool cancelled = false;
};

bool AppendDownloadedChunk(DownloadStreamContext &ctx, const char *data, size_t size)
{
    if (ctx.cancelRequested != nullptr && ctx.cancelRequested->load(std::memory_order_relaxed))
    {
        ctx.cancelled = true;
        return false;
    }
    if (size == 0)
    {
        return true;
    }
    ctx.file.write(data, std::streamsize(size));
    if (!ctx.file.good())
    {
        ctx.writeError = true;
        return false;
    }
    ctx.sha.Update(reinterpret_cast<const uint8_t *>(data), size);
    ctx.writtenBytes += uint64_t(size);
    if (ctx.progress != nullptr)
    {
        ctx.progress->bytesDownloaded.store(ctx.writtenBytes, std::memory_order_relaxed);
    }
    return true;
}

#if defined(_WIN32)
std::wstring Utf8ToWideStr(const std::string &utf8)
{
    if (utf8.empty())
    {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    if (needed <= 0)
    {
        return {};
    }
    std::wstring out(size_t(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), out.data(), needed);
    return out;
}

struct ScopedWinHttpHandle
{
    HINTERNET h = nullptr;
    ScopedWinHttpHandle() = default;
    ScopedWinHttpHandle(const ScopedWinHttpHandle &) = delete;
    ScopedWinHttpHandle &operator=(const ScopedWinHttpHandle &) = delete;
    ~ScopedWinHttpHandle()
    {
        if (h != nullptr)
        {
            WinHttpCloseHandle(h);
        }
    }
};

bool StreamWinHttpResponse(HINTERNET hRequest, DownloadStreamContext &ctx, std::string &outError)
{
    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                             WINHTTP_NO_HEADER_INDEX) ||
        statusCode < 200u || statusCode >= 300u)
    {
        outError = "HTTP download failed with status " + std::to_string(statusCode);
        return false;
    }

    uint64_t contentLen = 0;
    DWORD lenSize = sizeof(contentLen);
    if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER64,
                            WINHTTP_HEADER_NAME_BY_INDEX, &contentLen, &lenSize,
                            WINHTTP_NO_HEADER_INDEX) &&
        contentLen > 0 && ctx.progress != nullptr)
    {
        ctx.progress->bytesTotal.store(contentLen, std::memory_order_relaxed);
    }

    std::vector<char> buf(65536u);
    while (true)
    {
        if (ctx.cancelRequested != nullptr && ctx.cancelRequested->load(std::memory_order_relaxed))
        {
            ctx.cancelled = true;
            return false;
        }
        DWORD bytesRead = 0;
        if (!WinHttpReadData(hRequest, buf.data(), DWORD(buf.size()), &bytesRead))
        {
            outError = "WinHttpReadData failed during download.";
            return false;
        }
        if (bytesRead == 0)
        {
            return true;
        }
        if (!AppendDownloadedChunk(ctx, buf.data(), size_t(bytesRead)))
        {
            outError = ctx.cancelled ? "Cancelled" : "Failed writing model file.";
            return false;
        }
    }
}

bool DownloadHttpStream(const std::string &url, DownloadStreamContext &ctx, std::string &outError)
{
    const std::wstring wUrl = Utf8ToWideStr(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = DWORD(-1);
    uc.dwUrlPathLength = DWORD(-1);
    uc.dwExtraInfoLength = DWORD(-1);
    if (!WinHttpCrackUrl(wUrl.c_str(), DWORD(wUrl.size()), 0, &uc) || uc.dwHostNameLength == 0)
    {
        outError = "Invalid download URL: " + url;
        return false;
    }

    const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path = uc.dwUrlPathLength > 0
                            ? std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength)
                            : L"/";
    if (uc.dwExtraInfoLength > 0)
    {
        path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    }

    ScopedWinHttpHandle hSession{ WinHttpOpen(L"QuakeRayEditor/1.0",
                                              WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0) };
    if (!hSession.h)
    {
        outError = "WinHttpOpen failed.";
        return false;
    }
    ScopedWinHttpHandle hConnect{ WinHttpConnect(hSession.h, host.c_str(), uc.nPort, 0) };
    if (!hConnect.h)
    {
        outError = "WinHttpConnect failed.";
        return false;
    }
    const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0u;
    ScopedWinHttpHandle hRequest{ WinHttpOpenRequest(hConnect.h, L"GET", path.c_str(), nullptr,
                                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                     flags) };
    if (!hRequest.h)
    {
        outError = "WinHttpOpenRequest failed.";
        return false;
    }

    DWORD redirPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hRequest.h, WINHTTP_OPTION_REDIRECT_POLICY, &redirPolicy, sizeof(redirPolicy));
    if (!WinHttpSendRequest(hRequest.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                            0, 0) ||
        !WinHttpReceiveResponse(hRequest.h, nullptr))
    {
        outError = "Failed reaching model download server.";
        return false;
    }

    return StreamWinHttpResponse(hRequest.h, ctx, outError);
}
#else
struct CurlApi
{
    using GlobalInitFn = int (*)(long);
    using EasyInitFn = void *(*)();
    using EasySetOptFn = int (*)(void *, int, ...);
    using EasyPerformFn = int (*)(void *);
    using EasyCleanupFn = void (*)(void *);
    using EasyStrErrorFn = const char *(*)(int);

    void *libHandle = nullptr;
    GlobalInitFn globalInit = nullptr;
    EasyInitFn easyInit = nullptr;
    EasySetOptFn easySetOpt = nullptr;
    EasyPerformFn easyPerform = nullptr;
    EasyCleanupFn easyCleanup = nullptr;
    EasyStrErrorFn easyStrError = nullptr;

    CurlApi() = default;
    CurlApi(const CurlApi &) = delete;
    CurlApi &operator=(const CurlApi &) = delete;

    ~CurlApi()
    {
        if (libHandle != nullptr)
        {
            dlclose(libHandle);
            libHandle = nullptr;
        }
    }
};

template <typename Fn>
bool ResolveCurlSymbol(void *lib, const char *name, Fn &outFn)
{
    void *sym = dlsym(lib, name);
    outFn = reinterpret_cast<Fn>(sym);
    return outFn != nullptr;
}

const CurlApi *LoadLibCurlApi(std::string &outError)
{
    static CurlApi api;
    static std::once_flag initOnce;
    static bool initOk = false;
    static std::string initErr;

    std::call_once(initOnce, []() {
        api.libHandle = dlopen("libcurl.so.4", RTLD_NOW | RTLD_LOCAL);
        if (api.libHandle == nullptr)
        {
            api.libHandle = dlopen("libcurl.so", RTLD_NOW | RTLD_LOCAL);
        }
        if (api.libHandle == nullptr)
        {
            initErr = "libcurl.so.4 not found for model downloading.";
            return;
        }
        const bool ok = ResolveCurlSymbol(api.libHandle, "curl_global_init", api.globalInit) &&
                        ResolveCurlSymbol(api.libHandle, "curl_easy_init", api.easyInit) &&
                        ResolveCurlSymbol(api.libHandle, "curl_easy_setopt", api.easySetOpt) &&
                        ResolveCurlSymbol(api.libHandle, "curl_easy_perform", api.easyPerform) &&
                        ResolveCurlSymbol(api.libHandle, "curl_easy_cleanup", api.easyCleanup) &&
                        ResolveCurlSymbol(api.libHandle, "curl_easy_strerror", api.easyStrError);
        if (!ok)
        {
            initErr = "Required libcurl symbols are missing.";
            return;
        }
        api.globalInit(3L);
        initOk = true;
    });

    if (!initOk)
    {
        outError = initErr;
        return nullptr;
    }
    return &api;
}

size_t CurlWriteCallback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *ctx = static_cast<DownloadStreamContext *>(userdata);
    const size_t bytes = size * nmemb;
    return AppendDownloadedChunk(*ctx, ptr, bytes) ? bytes : 0u;
}

int CurlXferCallback(void *clientp, int64_t dltotal, int64_t dlnow, int64_t, int64_t)
{
    (void)dlnow;
    auto *ctx = static_cast<DownloadStreamContext *>(clientp);
    if (dltotal > 0 && ctx->progress != nullptr)
    {
        ctx->progress->bytesTotal.store(uint64_t(dltotal), std::memory_order_relaxed);
    }
    if (ctx->cancelRequested != nullptr && ctx->cancelRequested->load(std::memory_order_relaxed))
    {
        ctx->cancelled = true;
        return 1;
    }
    return 0;
}

bool DownloadHttpStream(const std::string &url, DownloadStreamContext &ctx, std::string &outError)
{
    const CurlApi *curl = LoadLibCurlApi(outError);
    if (curl == nullptr)
    {
        return false;
    }
    void *easy = curl->easyInit();
    if (easy == nullptr)
    {
        outError = "curl_easy_init failed.";
        return false;
    }

    curl->easySetOpt(easy, 10002, url.c_str());
    curl->easySetOpt(easy, 20011, &CurlWriteCallback);
    curl->easySetOpt(easy, 10001, &ctx);
    curl->easySetOpt(easy, 52, 1L);
    curl->easySetOpt(easy, 68, 10L);
    curl->easySetOpt(easy, 45, 1L);
    curl->easySetOpt(easy, 10018, "QuakeRayEditor/1.0");
    curl->easySetOpt(easy, 20219, &CurlXferCallback);
    curl->easySetOpt(easy, 10057, &ctx);
    curl->easySetOpt(easy, 43, 0L);
    curl->easySetOpt(easy, 78, 20L);
    curl->easySetOpt(easy, 19, 1024L);
    curl->easySetOpt(easy, 20, 60L);
    curl->easySetOpt(easy, 99, 1L);

    const int rc = curl->easyPerform(easy);
    curl->easyCleanup(easy);

    if (rc == 0)
    {
        return true;
    }
    if (ctx.cancelled || rc == 42)
    {
        ctx.cancelled = true;
        outError = "Cancelled";
        return false;
    }
    if (ctx.writeError)
    {
        outError = "Failed writing model file to disk.";
        return false;
    }
    const char *errStr = curl->easyStrError(rc);
    outError = std::string("Download failed: ") + (errStr != nullptr ? errStr : "libcurl error");
    return false;
}
#endif

} // namespace

EditorAiModelDownloadSpec GetEditorAiModelDownloadSpec(EditorAiBackend backend)
{
    switch (backend)
    {
    case EditorAiBackend::LotusNormalD:
        return {
            "lotus-normal-d-v1-1.onnx",
            "https://huggingface.co/f13d/lotus-onnx/resolve/main/lotus-normal-d-v1-1.onnx",
            "19e9b74fd75d32b5d54c2790ef3d844eab0009a59b42201072856019c389fadc",
            1904363005ULL,
        };
    case EditorAiBackend::LotusDepthD:
        return {
            "lotus-depth-d-v2-0-disparity.onnx",
            "https://huggingface.co/f13d/lotus-onnx/resolve/main/"
            "lotus-depth-d-v2-0-disparity.onnx",
            "0918caff377306126e4a841260a1d20041d432584e26a37f6f8e2c87c2de1bc4",
            1904363005ULL,
        };
    case EditorAiBackend::DeepBump:
        return {
            "deepbump256.onnx",
            "https://github.com/HugoTini/DeepBump/raw/master/deepbump256.onnx",
            "3a2ababc5fa652b19040a0f3d639b71826368e52ad805cf70ffb0e7c96a54d67",
            26706979ULL,
        };
    }
    return GetEditorAiModelDownloadSpec(EditorAiBackend::LotusNormalD);
}

std::string ComputeEditorBufferSha256Hex(const uint8_t *data, size_t size)
{
    Sha256Hasher sha;
    sha.Update(data, size);
    return sha.FinalHex();
}

bool ComputeEditorFileSha256Hex(const std::filesystem::path &path, std::string &outHex)
{
    outHex.clear();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
    {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return false;
    }
    Sha256Hasher sha;
    std::vector<uint8_t> buf(65536u);
    while (in)
    {
        in.read(reinterpret_cast<char *>(buf.data()), std::streamsize(buf.size()));
        const std::streamsize got = in.gcount();
        if (got > 0)
        {
            sha.Update(buf.data(), size_t(got));
        }
    }
    if (!in.eof())
    {
        return false;
    }
    outHex = sha.FinalHex();
    return true;
}

EditorAiDownloadResult DownloadEditorAiFile(
    const std::string &url, const std::filesystem::path &destPath,
    const std::string &expectedSha256Hex, uint64_t expectedBytesHint,
    EditorAiDownloadProgress *progress, const std::atomic<bool> *cancelRequested)
{
    EditorAiDownloadResult result;
    if (progress != nullptr)
    {
        progress->bytesDownloaded.store(0u, std::memory_order_relaxed);
        progress->bytesTotal.store(expectedBytesHint, std::memory_order_relaxed);
    }

    std::error_code ec;
    if (destPath.has_parent_path())
    {
        std::filesystem::create_directories(destPath.parent_path(), ec);
        if (ec)
        {
            result.errorMessage = "Failed creating models directory: " + ec.message();
            return result;
        }
    }

    const std::filesystem::path partPath = destPath.string() + ".part";
    std::filesystem::remove(partPath, ec);

    DownloadStreamContext ctx;
    ctx.progress = progress;
    ctx.cancelRequested = cancelRequested;
    ctx.file.open(partPath, std::ios::binary | std::ios::trunc);
    if (!ctx.file)
    {
        result.errorMessage = "Failed opening temporary download file: " + partPath.string();
        return result;
    }

    const bool downloadedOk = DownloadHttpStream(url, ctx, result.errorMessage);
    ctx.file.flush();
    ctx.file.close();

    if (!downloadedOk)
    {
        std::filesystem::remove(partPath, ec);
        result.cancelled = ctx.cancelled;
        return result;
    }

    result.actualSha256 = ctx.sha.FinalHex();
    const std::string expectedLower = LowerAsciiHex(expectedSha256Hex);
    if (!expectedLower.empty() && result.actualSha256 != expectedLower)
    {
        std::filesystem::remove(partPath, ec);
        result.errorMessage =
            "Downloaded model SHA-256 mismatch (expected " + expectedLower + ", got " +
            result.actualSha256 + ").";
        return result;
    }

    std::filesystem::rename(partPath, destPath, ec);
    if (ec)
    {
        std::filesystem::remove(partPath, ec);
        result.errorMessage = "Failed saving downloaded model: " + ec.message();
        return result;
    }

    result.ok = true;
    result.savedPath = destPath;
    return result;
}

EditorAiDownloadResult DownloadEditorAiBackendModel(
    EditorAiBackend backend, const std::filesystem::path &modelsDir,
    EditorAiDownloadProgress *progress, const std::atomic<bool> *cancelRequested)
{
    const EditorAiModelDownloadSpec spec = GetEditorAiModelDownloadSpec(backend);
    const std::filesystem::path dir = modelsDir.empty() ? DefaultEditorAiModelsDir() : modelsDir;
    return DownloadEditorAiFile(spec.url, dir / spec.filename, spec.sha256Hex, spec.expectedBytes,
                                progress, cancelRequested);
}
