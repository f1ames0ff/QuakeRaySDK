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

#include "GgufLoader.h"

#include <cstring>
#include <fstream>
#include <utility>

namespace
{

uint8_t g_dryRunSentinel = 0u;

bool MatchesAnyPrefix(const char *name, const std::vector<std::string> &prefixes)
{
    if (name == nullptr)
    {
        return false;
    }
    if (prefixes.empty())
    {
        return true;
    }
    const std::string_view sv(name);
    for (const std::string &p : prefixes)
    {
        if (sv.rfind(p, 0) == 0)
        {
            return true;
        }
    }
    return false;
}

int InferTensorNDims(const int64_t *ne)
{
    int nDims = 4;
    while (nDims > 1 && ne[nDims - 1] <= 1)
    {
        nDims--;
    }
    return nDims;
}

} // namespace

GgufStageWeights::~GgufStageWeights()
{
    Reset();
}

GgufStageWeights::GgufStageWeights(GgufStageWeights &&other) noexcept
    : m_ctx(std::exchange(other.m_ctx, nullptr))
    , m_buffer(std::exchange(other.m_buffer, nullptr))
    , m_totalBytes(std::exchange(other.m_totalBytes, 0))
    , m_tensors(std::move(other.m_tensors))
{
}

GgufStageWeights &GgufStageWeights::operator=(GgufStageWeights &&other) noexcept
{
    if (this != &other)
    {
        Reset();
        m_ctx = std::exchange(other.m_ctx, nullptr);
        m_buffer = std::exchange(other.m_buffer, nullptr);
        m_totalBytes = std::exchange(other.m_totalBytes, 0);
        m_tensors = std::move(other.m_tensors);
    }
    return *this;
}

void GgufStageWeights::Reset()
{
    m_tensors.clear();
    if (m_buffer != nullptr)
    {
        ggml_backend_buffer_free(m_buffer);
        m_buffer = nullptr;
    }
    if (m_ctx != nullptr)
    {
        ggml_free(m_ctx);
        m_ctx = nullptr;
    }
    m_totalBytes = 0;
}

ggml_tensor *GgufStageWeights::Find(const std::string &name) const
{
    const auto it = m_tensors.find(name);
    return it != m_tensors.end() ? it->second : nullptr;
}

ggml_tensor *GgufStageWeights::Require(const std::string &name, std::string &outError) const
{
    ggml_tensor *t = Find(name);
    if (t == nullptr)
    {
        outError = "Missing required GGUF tensor: " + name;
    }
    return t;
}

GgufModelFile::~GgufModelFile()
{
    Close();
}

void GgufModelFile::Close()
{
    if (m_gguf != nullptr)
    {
        gguf_free(m_gguf);
        m_gguf = nullptr;
    }
    m_dataOffset = 0;
    m_path.clear();
}

bool GgufModelFile::Open(const std::filesystem::path &path, std::string &outError)
{
    Close();
    outError.clear();

    gguf_init_params params = { true, nullptr };
    const std::string pathStr = path.string();
    m_gguf = gguf_init_from_file(pathStr.c_str(), params);
    if (m_gguf == nullptr)
    {
        outError = "Failed opening GGUF model: " + pathStr;
        return false;
    }
    m_path = path;
    m_dataOffset = gguf_get_data_offset(m_gguf);
    return true;
}

std::string GgufModelFile::GetMetaStr(const char *key, const std::string &fallback) const
{
    if (m_gguf == nullptr || key == nullptr)
    {
        return fallback;
    }
    const int64_t idx = gguf_find_key(m_gguf, key);
    if (idx < 0 || gguf_get_kv_type(m_gguf, idx) != GGUF_TYPE_STRING)
    {
        return fallback;
    }
    const char *val = gguf_get_val_str(m_gguf, idx);
    return val != nullptr ? std::string(val) : fallback;
}

float GgufModelFile::GetMetaF32(const char *key, float fallback) const
{
    if (m_gguf == nullptr || key == nullptr)
    {
        return fallback;
    }
    const int64_t idx = gguf_find_key(m_gguf, key);
    if (idx < 0 || gguf_get_kv_type(m_gguf, idx) != GGUF_TYPE_FLOAT32)
    {
        return fallback;
    }
    return gguf_get_val_f32(m_gguf, idx);
}

uint32_t GgufModelFile::GetMetaU32(const char *key, uint32_t fallback) const
{
    if (m_gguf == nullptr || key == nullptr)
    {
        return fallback;
    }
    const int64_t idx = gguf_find_key(m_gguf, key);
    if (idx < 0 || gguf_get_kv_type(m_gguf, idx) != GGUF_TYPE_UINT32)
    {
        return fallback;
    }
    return gguf_get_val_u32(m_gguf, idx);
}

std::vector<int64_t> GgufModelFile::CollectTensorIndices(
    const std::vector<std::string> &prefixes) const
{
    std::vector<int64_t> indices;
    if (m_gguf == nullptr)
    {
        return indices;
    }
    const int64_t n = gguf_get_n_tensors(m_gguf);
    indices.reserve(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; i++)
    {
        if (MatchesAnyPrefix(gguf_get_tensor_name(m_gguf, i), prefixes))
        {
            indices.push_back(i);
        }
    }
    return indices;
}

bool GgufModelFile::PopulateStageMetadata(const std::vector<int64_t> &indices,
                                          GgufStageWeights &outStage,
                                          std::vector<ggml_tensor *> &outOrdered,
                                          std::string &outError) const
{
    outStage.Reset();
    outOrdered.clear();
    if (indices.empty())
    {
        outError = "No matching tensors found in GGUF stage.";
        return false;
    }

    const size_t ctxMem = ggml_tensor_overhead() * (indices.size() + 8u);
    ggml_init_params ctxParams = { ctxMem, nullptr, true };
    outStage.m_ctx = ggml_init(ctxParams);
    if (outStage.m_ctx == nullptr)
    {
        outError = "Failed allocating GGML context for stage weights.";
        return false;
    }

    outOrdered.reserve(indices.size());
    for (int64_t idx : indices)
    {
        const char *name = gguf_get_tensor_name(m_gguf, idx);
        const int64_t *ne = gguf_get_tensor_ne(m_gguf, idx);
        const ggml_type dtype = gguf_get_tensor_type(m_gguf, idx);
        const int nDims = InferTensorNDims(ne);
        ggml_tensor *t = ggml_new_tensor(outStage.m_ctx, dtype, nDims, ne);
        if (t == nullptr)
        {
            outStage.Reset();
            outError = "Failed allocating tensor metadata for GGUF entry.";
            return false;
        }
        ggml_set_name(t, name);
        outStage.m_tensors.emplace(name, t);
        outOrdered.push_back(t);
    }
    return true;
}

uint64_t GgufModelFile::EstimateStageBytes(const std::vector<std::string> &prefixes,
                                           ggml_backend_buffer_type_t buft) const
{
    const std::vector<int64_t> indices = CollectTensorIndices(prefixes);
    GgufStageWeights tmp;
    std::vector<ggml_tensor *> ordered;
    std::string ignoredErr;
    if (!PopulateStageMetadata(indices, tmp, ordered, ignoredErr))
    {
        return 0;
    }
    if (buft != nullptr)
    {
        return static_cast<uint64_t>(ggml_backend_buft_get_alloc_size_n(
            buft, ordered.data(), static_cast<int>(ordered.size())));
    }
    uint64_t total = 0;
    for (ggml_tensor *t : ordered)
    {
        total += static_cast<uint64_t>(ggml_nbytes(t));
    }
    return total;
}

bool GgufModelFile::CreateDryRunStage(const std::vector<std::string> &prefixes,
                                      GgufStageWeights &outStage, std::string &outError) const
{
    const std::vector<int64_t> indices = CollectTensorIndices(prefixes);
    std::vector<ggml_tensor *> ordered;
    if (!PopulateStageMetadata(indices, outStage, ordered, outError))
    {
        return false;
    }
    for (ggml_tensor *t : ordered)
    {
        t->data = &g_dryRunSentinel;
        outStage.m_totalBytes += static_cast<uint64_t>(ggml_nbytes(t));
    }
    return true;
}

bool GgufModelFile::LoadStageWeights(const std::vector<std::string> &prefixes, ggml_backend_t backend,
                                     GgufStageWeights &outStage, std::string &outError) const
{
    if (m_gguf == nullptr || backend == nullptr)
    {
        outError = "GGUF file or backend handle is null.";
        return false;
    }

    const std::vector<int64_t> indices = CollectTensorIndices(prefixes);
    std::vector<ggml_tensor *> ordered;
    if (!PopulateStageMetadata(indices, outStage, ordered, outError))
    {
        return false;
    }

    outStage.m_buffer = ggml_backend_alloc_ctx_tensors(outStage.m_ctx, backend);
    if (outStage.m_buffer == nullptr)
    {
        outStage.Reset();
        outError = "Failed allocating backend buffer for GGUF stage weights.";
        return false;
    }
    outStage.m_totalBytes = static_cast<uint64_t>(ggml_backend_buffer_get_size(outStage.m_buffer));

    std::ifstream fp(m_path, std::ios::binary);
    if (!fp)
    {
        outStage.Reset();
        outError = "Failed re-opening GGUF file for tensor read: " + m_path.string();
        return false;
    }

    std::vector<uint8_t> staging;
    for (size_t i = 0; i < indices.size(); i++)
    {
        ggml_tensor *t = ordered[i];
        const size_t nbytes = ggml_nbytes(t);
        staging.resize(nbytes);
        const uint64_t fileOff = static_cast<uint64_t>(m_dataOffset) +
                                 static_cast<uint64_t>(gguf_get_tensor_offset(m_gguf, indices[i]));
        fp.seekg(static_cast<std::streamoff>(fileOff), std::ios::beg);
        fp.read(reinterpret_cast<char *>(staging.data()), static_cast<std::streamsize>(nbytes));
        if (!fp.good())
        {
            outStage.Reset();
            outError = std::string("Failed reading GGUF tensor bytes: ") + ggml_get_name(t);
            return false;
        }
        ggml_backend_tensor_set(t, staging.data(), 0u, nbytes);
    }
    return true;
}
