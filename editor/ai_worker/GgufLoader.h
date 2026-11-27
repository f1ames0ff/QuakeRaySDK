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

#include <ggml-backend.h>
#include <ggml.h>
#include <gguf.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

class GgufStageWeights
{
  public:
    GgufStageWeights() = default;
    ~GgufStageWeights();

    GgufStageWeights(const GgufStageWeights &) = delete;
    GgufStageWeights &operator=(const GgufStageWeights &) = delete;
    GgufStageWeights(GgufStageWeights &&other) noexcept;
    GgufStageWeights &operator=(GgufStageWeights &&other) noexcept;

    void Reset();

    ggml_tensor *Find(const std::string &name) const;
    ggml_tensor *Require(const std::string &name, std::string &outError) const;

    uint64_t TotalBytes() const
    {
        return m_totalBytes;
    }
    size_t Count() const
    {
        return m_tensors.size();
    }

  private:
    friend class GgufModelFile;
    ggml_context *m_ctx = nullptr;
    ggml_backend_buffer_t m_buffer = nullptr;
    uint64_t m_totalBytes = 0;
    std::unordered_map<std::string, ggml_tensor *> m_tensors;
};

class GgufModelFile
{
  public:
    GgufModelFile() = default;
    ~GgufModelFile();

    GgufModelFile(const GgufModelFile &) = delete;
    GgufModelFile &operator=(const GgufModelFile &) = delete;

    bool Open(const std::filesystem::path &path, std::string &outError);
    void Close();

    std::string GetMetaStr(const char *key, const std::string &fallback = "") const;
    float GetMetaF32(const char *key, float fallback = 0.0f) const;
    uint32_t GetMetaU32(const char *key, uint32_t fallback = 0u) const;

    uint64_t EstimateStageBytes(const std::vector<std::string> &prefixes,
                                ggml_backend_buffer_type_t buft) const;

    bool CreateDryRunStage(const std::vector<std::string> &prefixes, GgufStageWeights &outStage,
                           std::string &outError) const;

    bool LoadStageWeights(const std::vector<std::string> &prefixes, ggml_backend_t backend,
                          GgufStageWeights &outStage, std::string &outError) const;

  private:
    std::vector<int64_t> CollectTensorIndices(const std::vector<std::string> &prefixes) const;
    bool PopulateStageMetadata(const std::vector<int64_t> &indices, GgufStageWeights &outStage,
                               std::vector<ggml_tensor *> &outOrdered,
                               std::string &outError) const;

    std::filesystem::path m_path;
    gguf_context *m_gguf = nullptr;
    size_t m_dataOffset = 0;
};
