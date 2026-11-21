// Copyright (c) 2020-2021 Sultim Tsyrendashiev
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <qray/qray.h>

#include <array>
#include <cstdint>
#include <memory>

#include "Common.h"
#include "CommandBufferManager.h"

namespace qray
{

struct FrameState
{
private:
    struct Slot
    {
        VkCommandBuffer      frameCmd = VK_NULL_HANDLE;
        VkSemaphore          semaphoreToWait = VK_NULL_HANDLE;
        VkPipelineStageFlags semaphoreWaitStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkCommandBuffer      preFrameCmd = VK_NULL_HANDLE;
    };

    uint32_t                 frameIndex;
    std::array<Slot, MAX_FRAMES_IN_FLIGHT> slots;

public:
    FrameState() :
        frameIndex(MAX_FRAMES_IN_FLIGHT - 1)
    {}

    FrameState(const FrameState &other) = delete;
    FrameState(FrameState &&other) noexcept = delete;
    FrameState &operator=(const FrameState &other) = delete;
    FrameState &operator=(FrameState &&other) noexcept = delete;

    uint32_t IncrementFrameIndexAndGet()
    {
        frameIndex = (frameIndex + 1) % MAX_FRAMES_IN_FLIGHT;
        return frameIndex;
    }

    uint32_t GetFrameIndex() const
    {
        assert(frameIndex < MAX_FRAMES_IN_FLIGHT);
        return frameIndex;
    }

    static uint32_t GetPrevFrameIndex(uint32_t frameIndex)
    {
        assert(frameIndex < MAX_FRAMES_IN_FLIGHT);
        return (frameIndex + (MAX_FRAMES_IN_FLIGHT - 1)) % MAX_FRAMES_IN_FLIGHT;
    }

    void OnBeginFrame(VkCommandBuffer cmd)
    {
        Slot &slot = GetSlot();

        assert(slot.frameCmd == VK_NULL_HANDLE);
        slot.frameCmd = cmd;
    }

    void OnEndFrame()
    {
        Slot &slot = GetSlot();

        assert(slot.frameCmd != VK_NULL_HANDLE);
        assert(slot.preFrameCmd == VK_NULL_HANDLE);
        slot.frameCmd = VK_NULL_HANDLE;
    }

    VkCommandBuffer GetCmdBuffer() const
    {
        const Slot &slot = GetSlot();

        assert(WasFrameStarted());
        return slot.frameCmd;
    }

    VkCommandBuffer GetCmdBufferForMaterials(const std::shared_ptr<CommandBufferManager> &cmdManager)
    {
        Slot &slot = GetSlot();

        if (WasFrameStarted())
        {
            return GetCmdBuffer();
        }

        if (slot.preFrameCmd == VK_NULL_HANDLE)
        {
            slot.preFrameCmd = cmdManager->StartGraphicsCmd();
        }

        return slot.preFrameCmd;
    }

    VkCommandBuffer GetPreFrameCmdAndRemove(uint32_t slotIndex)
    {
        assert(slotIndex < MAX_FRAMES_IN_FLIGHT);
        Slot &slot = slots[slotIndex];
        VkCommandBuffer c = slot.preFrameCmd;

        slot.preFrameCmd = VK_NULL_HANDLE;
        return c;
    }

    bool WasFrameStarted() const
    {
        return GetSlot().frameCmd != VK_NULL_HANDLE;
    }

    void SetSemaphore(VkSemaphore s, VkPipelineStageFlags waitStage)
    {
        Slot &slot = GetSlot();

        slot.semaphoreToWait = s;
        slot.semaphoreWaitStage = waitStage;
    }

    VkSemaphore GetSemaphoreForWaitAndRemove(VkPipelineStageFlags *outWaitStage)
    {
        Slot &slot = GetSlot();
        VkSemaphore s = slot.semaphoreToWait;

        slot.semaphoreToWait = VK_NULL_HANDLE;
        *outWaitStage = slot.semaphoreWaitStage;

        return s;
    }

private:
    Slot &GetSlot()
    {
        assert(frameIndex < MAX_FRAMES_IN_FLIGHT);
        return slots[frameIndex];
    }

    const Slot &GetSlot() const
    {
        assert(frameIndex < MAX_FRAMES_IN_FLIGHT);
        return slots[frameIndex];
    }
};

}
