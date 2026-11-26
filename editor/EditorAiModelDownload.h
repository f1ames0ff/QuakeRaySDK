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

#include "EditorAiTexture.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

struct EditorAiModelDownloadSpec
{
    const char *filename = "";
    const char *url = "";
    const char *sha256Hex = "";
    uint64_t expectedBytes = 0;
};

struct EditorAiDownloadProgress
{
    std::atomic<uint64_t> bytesDownloaded{ 0 };
    std::atomic<uint64_t> bytesTotal{ 0 };
};

struct EditorAiDownloadResult
{
    bool ok = false;
    bool cancelled = false;
    std::filesystem::path savedPath;
    std::string actualSha256;
    std::string errorMessage;
};

// Returns verified URL, expected byte count, and SHA-256 digest for each local ONNX backend.
EditorAiModelDownloadSpec GetEditorAiModelDownloadSpec(EditorAiBackend backend);

// Computes lowercase hexadecimal FIPS 180-4 SHA-256 digests without external crypto libraries.
std::string ComputeEditorBufferSha256Hex(const uint8_t *data, size_t size);
bool ComputeEditorFileSha256Hex(const std::filesystem::path &path, std::string &outHex);

// Streams an HTTPS download into a `.part` file, verifies SHA-256, and renames atomically.
EditorAiDownloadResult DownloadEditorAiFile(
    const std::string &url, const std::filesystem::path &destPath,
    const std::string &expectedSha256Hex, uint64_t expectedBytesHint,
    EditorAiDownloadProgress *progress = nullptr,
    const std::atomic<bool> *cancelRequested = nullptr);

// Downloads the ONNX model file for `backend` into `modelsDir` and verifies its SHA-256 checksum.
EditorAiDownloadResult DownloadEditorAiBackendModel(
    EditorAiBackend backend, const std::filesystem::path &modelsDir,
    EditorAiDownloadProgress *progress = nullptr,
    const std::atomic<bool> *cancelRequested = nullptr);
