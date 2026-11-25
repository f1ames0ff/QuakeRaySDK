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

#include <qray/qray.h>

#include <array>
#include <chrono>

namespace qray
{

class CpuFrameProfiler
{
public:
    void Reset(bool enable)
    {
        enabled = enable;
        milliseconds.fill(0.0f);
        active.fill(false);
    }

    bool IsEnabled() const { return enabled; }

    void Begin(QrCpuPassIndex pass)
    {
        const auto index = static_cast<uint32_t>(pass);
        if (enabled && index < QR_CPU_PASS_COUNT)
        {
            starts[index] = Clock::now();
            active[index] = true;
        }
    }

    void End(QrCpuPassIndex pass)
    {
        const auto index = static_cast<uint32_t>(pass);
        if (enabled && index < QR_CPU_PASS_COUNT && active[index])
        {
            milliseconds[index] += std::chrono::duration<float, std::milli>(Clock::now() - starts[index]).count();
            active[index] = false;
        }
    }

    void Add(QrCpuPassIndex pass, float elapsedMs)
    {
        const auto index = static_cast<uint32_t>(pass);
        if (enabled && index < QR_CPU_PASS_COUNT)
        {
            milliseconds[index] += elapsedMs;
        }
    }

    const std::array<float, QR_CPU_PASS_COUNT> &GetMilliseconds() const { return milliseconds; }

private:
    using Clock = std::chrono::steady_clock;

    bool enabled = false;
    std::array<Clock::time_point, QR_CPU_PASS_COUNT> starts{};
    std::array<bool, QR_CPU_PASS_COUNT> active{};
    std::array<float, QR_CPU_PASS_COUNT> milliseconds{};
};

class CpuProfileScope
{
public:
    CpuProfileScope(CpuFrameProfiler *pProfiler, QrCpuPassIndex index)
        : profiler(pProfiler), pass(index)
    {
        if (profiler != nullptr)
        {
            profiler->Begin(pass);
        }
    }

    ~CpuProfileScope() { Finish(); }

    CpuProfileScope(const CpuProfileScope &) = delete;
    CpuProfileScope &operator=(const CpuProfileScope &) = delete;

    void Finish()
    {
        if (profiler != nullptr)
        {
            profiler->End(pass);
            profiler = nullptr;
        }
    }

private:
    CpuFrameProfiler *profiler;
    QrCpuPassIndex pass;
};

}
