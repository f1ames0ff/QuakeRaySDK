// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <stdexcept>

namespace qray
{

class FrameAcquireGate final
{
public:
    FrameAcquireGate() = default;
    FrameAcquireGate(const FrameAcquireGate &) = delete;
    FrameAcquireGate &operator=(const FrameAcquireGate &) = delete;

    void Reset()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (state.load(std::memory_order_acquire) == Running)
        {
            throw std::logic_error("frame acquire gate reset while acquisition is running");
        }
        exception = nullptr;
        state.store(Idle, std::memory_order_release);
    }

    template<typename Function>
    void Acquire(Function &&function)
    {
        uint32_t current = state.load(std::memory_order_acquire);
        if (current == Complete)
        {
            return;
        }

        uint32_t expected = Idle;
        if (state.compare_exchange_strong(
                expected, Running, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            try
            {
                function();
                {
                    const std::lock_guard<std::mutex> lock(mutex);
                    state.store(Complete, std::memory_order_release);
                }
                condition.notify_all();
            }
            catch (...)
            {
                {
                    const std::lock_guard<std::mutex> lock(mutex);
                    exception = std::current_exception();
                    state.store(Failed, std::memory_order_release);
                }
                condition.notify_all();
                throw;
            }
            return;
        }

        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [this] {
            return state.load(std::memory_order_acquire) >= Complete;
        });

        if (state.load(std::memory_order_acquire) == Failed)
        {
            const std::exception_ptr failure = exception;
            lock.unlock();

            if (failure != nullptr)
            {
                std::rethrow_exception(failure);
            }

            throw std::runtime_error("frame acquisition failed without an exception");
        }
    }

private:
    static constexpr uint32_t Idle = 0;
    static constexpr uint32_t Running = 1;
    static constexpr uint32_t Complete = 2;
    static constexpr uint32_t Failed = 3;

    std::atomic<uint32_t> state{Idle};
    std::mutex mutex;
    std::condition_variable condition;
    std::exception_ptr exception;
};

}
