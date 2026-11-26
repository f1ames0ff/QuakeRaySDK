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

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct EditorOnnxTensorNchw
{
    int64_t channels = 0;
    int64_t height = 0;
    int64_t width = 0;
    std::vector<float> data;
};

// Locates libonnxruntime (env override, executable dir, or user cache dir).
std::filesystem::path ResolveEditorOnnxRuntimeLibraryPath(const std::string &exeDir = "");

// Returns true when an ONNX Runtime shared library is discoverable on disk.
bool IsEditorOnnxRuntimeAvailable(const std::string &exeDir = "");

// Runs a float32 NCHW [1, C, H, W] ONNX model on the best available execution provider
// (WebGPU/Vulkan, MIGraphX, DirectML, CUDA, or CPU fallback), caching the session across calls.
bool RunEditorOnnxModelNchw(const std::filesystem::path &modelPath, const std::string &exeDir,
                            const EditorOnnxTensorNchw &input, EditorOnnxTensorNchw &output,
                            const std::atomic<bool> *cancelRequested, std::string &outProviderLabel,
                            std::string &outError);

// Requests immediate termination of an in-flight ONNX Runtime inference call, if any.
void CancelActiveEditorOnnxRun();

// Releases any cached ONNX Runtime sessions, environment, and dynamic library handle.
void ShutdownEditorOnnxRuntime();
