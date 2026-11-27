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

#include "EditorAiWorkerClient.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#else
#    include <csignal>
#    include <fcntl.h>
#    include <poll.h>
#    include <sys/types.h>
#    include <sys/wait.h>
#    include <unistd.h>
#    if defined(__linux__)
#        include <sys/prctl.h>
#    endif
#endif

namespace
{

constexpr uint64_t kMinVramReserveBytes = 512ULL << 20;
constexpr uint64_t kMaxVramReserveBytes = 2048ULL << 20;

#if defined(_WIN32)
constexpr const char *kWorkerExeName = "quakeray_ai_worker.exe";
#else
constexpr const char *kWorkerExeName = "quakeray_ai_worker";
#endif

std::mutex g_activeWorkerMutex;
bool g_cancelActiveRequested = false;
#if defined(_WIN32)
HANDLE g_activeChildProcess = nullptr;
#else
pid_t g_activeChildPid = -1;
#endif

std::atomic<uint64_t> g_tempFileCounter{ 0u };

struct ScopedTempPair
{
    std::filesystem::path inputPath;
    std::filesystem::path outputPath;

    ~ScopedTempPair()
    {
        std::error_code ec;
        if (!inputPath.empty())
        {
            std::filesystem::remove(inputPath, ec);
        }
        if (!outputPath.empty())
        {
            std::filesystem::remove(outputPath, ec);
        }
    }
};

ScopedTempPair AllocateTempTensorPair()
{
    std::error_code ec;
    std::filesystem::path tmpDir = std::filesystem::temp_directory_path(ec);
    if (tmpDir.empty())
    {
        tmpDir = ".";
    }
    const uint64_t seq = g_tempFileCounter.fetch_add(1u, std::memory_order_relaxed);
    const auto nowTick = std::chrono::steady_clock::now().time_since_epoch().count();
#if defined(_WIN32)
    const unsigned long pidVal = static_cast<unsigned long>(GetCurrentProcessId());
#else
    const unsigned long pidVal = static_cast<unsigned long>(getpid());
#endif
    const std::string base = "qray_ai_" + std::to_string(pidVal) + "_" +
                             std::to_string(static_cast<unsigned long long>(nowTick)) + "_" +
                             std::to_string(seq);
    return { tmpDir / (base + "_in.qrai"), tmpDir / (base + "_out.qrai") };
}

struct WorkerStreamState
{
    bool sawDone = false;
    std::string workerError;
    std::string lastLogLine;
};

void ConsumeWorkerLine(const std::string &line, const EditorAiWorkerInvokeRequest &request,
                       EditorAiWorkerInvokeResult &outResult, WorkerStreamState &state)
{
    const EditorAiWorkerMessage msg = ParseAiWorkerStdoutLine(line);
    switch (msg.kind)
    {
    case EditorAiWorkerMsgKind::Progress:
        if (request.onProgress)
        {
            request.onProgress(msg.percent, msg.text);
        }
        break;
    case EditorAiWorkerMsgKind::Vram:
        outResult.totalVramBytes = msg.totalVramBytes;
        outResult.freeVramBytes = msg.freeVramBytes;
        outResult.budgetVramBytes = msg.budgetVramBytes;
        break;
    case EditorAiWorkerMsgKind::Provider:
        outResult.provider = msg.text;
        break;
    case EditorAiWorkerMsgKind::Done:
        state.sawDone = true;
        break;
    case EditorAiWorkerMsgKind::Error:
        state.workerError = msg.text;
        break;
    case EditorAiWorkerMsgKind::Unknown:
        if (!line.empty())
        {
            state.lastLogLine = line;
        }
        break;
    }
}

void FlushLineBuffer(std::string &pending, bool finalFlush,
                     const EditorAiWorkerInvokeRequest &request,
                     EditorAiWorkerInvokeResult &outResult, WorkerStreamState &state)
{
    size_t pos = 0;
    while (true)
    {
        const size_t nl = pending.find('\n', pos);
        if (nl == std::string::npos)
        {
            break;
        }
        ConsumeWorkerLine(pending.substr(pos, nl - pos), request, outResult, state);
        pos = nl + 1u;
    }
    if (pos > 0u)
    {
        pending.erase(0u, pos);
    }
    if (finalFlush && !pending.empty())
    {
        ConsumeWorkerLine(pending, request, outResult, state);
        pending.clear();
    }
}

bool IsCancelRequested(const EditorAiWorkerInvokeRequest &request)
{
    if (request.cancelRequested != nullptr &&
        request.cancelRequested->load(std::memory_order_relaxed))
    {
        return true;
    }
    std::lock_guard<std::mutex> lock(g_activeWorkerMutex);
    return g_cancelActiveRequested;
}

#if defined(_WIN32)
std::wstring Utf8ToWide(const std::string &utf8)
{
    if (utf8.empty())
    {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    if (n <= 0)
    {
        return {};
    }
    std::wstring wide(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), wide.data(), n);
    return wide;
}

std::wstring QuoteWinArg(const std::wstring &arg)
{
    std::wstring out = L"\"";
    for (wchar_t ch : arg)
    {
        if (ch == L'"')
        {
            out += L"\\\"";
        }
        else
        {
            out += ch;
        }
    }
    out += L"\"";
    return out;
}

std::wstring BuildWinWorkerCommandLine(const EditorAiWorkerInvokeRequest &request,
                                        const ScopedTempPair &temps)
{
    const std::wstring exeW = Utf8ToWide(request.workerExecutable.string());
    std::wstring cmdLine =
        QuoteWinArg(exeW) + L" --model " + QuoteWinArg(Utf8ToWide(request.modelPath.string())) +
        L" --arch " + QuoteWinArg(Utf8ToWide(request.arch)) + L" --input " +
        QuoteWinArg(Utf8ToWide(temps.inputPath.string())) + L" --output " +
        QuoteWinArg(Utf8ToWide(temps.outputPath.string()));
    if (request.clampEdge)
    {
        cmdLine += L" --clamp";
    }
    return cmdLine;
}

void DrainWinWorkerPipe(HANDLE childProc, HANDLE readPipe, const EditorAiWorkerInvokeRequest &request,
                        EditorAiWorkerInvokeResult &outResult, WorkerStreamState &state)
{
    std::string pending;
    std::array<char, 2048> buf{};
    for (;;)
    {
        if (IsCancelRequested(request))
        {
            TerminateProcess(childProc, 1u);
            break;
        }
        DWORD avail = 0;
        if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &avail, nullptr))
        {
            break;
        }
        if (avail == 0)
        {
            if (WaitForSingleObject(childProc, 25) != WAIT_TIMEOUT)
            {
                DWORD left = 0;
                if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &left, nullptr) || left == 0)
                {
                    break;
                }
            }
            continue;
        }
        DWORD readBytes = 0;
        if (!ReadFile(readPipe, buf.data(), static_cast<DWORD>(buf.size()), &readBytes, nullptr) ||
            readBytes == 0)
        {
            break;
        }
        pending.append(buf.data(), readBytes);
        FlushLineBuffer(pending, false, request, outResult, state);
    }
    FlushLineBuffer(pending, true, request, outResult, state);
}

bool FinalizeWinWorkerExit(DWORD exitCode, const EditorAiWorkerInvokeRequest &request,
                           const WorkerStreamState &state, EditorAiWorkerInvokeResult &outResult)
{
    if (IsCancelRequested(request))
    {
        outResult.cancelled = true;
        return false;
    }
    if (!state.workerError.empty())
    {
        outResult.errorMessage = state.workerError;
        return false;
    }
    if (exitCode != 0u || !state.sawDone)
    {
        outResult.errorMessage =
            state.lastLogLine.empty()
                ? ("AI worker failed with exit code " + std::to_string(exitCode) + ".")
                : ("AI worker failed: " + state.lastLogLine);
        return false;
    }
    return true;
}

bool SpawnAndWaitWinWorker(const EditorAiWorkerInvokeRequest &request, const ScopedTempPair &temps,
                            EditorAiWorkerInvokeResult &outResult, WorkerStreamState &state)
{
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
    {
        outResult.errorMessage = "Failed creating AI worker pipe.";
        return false;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb = sizeof(STARTUPINFOW);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;

    const std::wstring exeW = Utf8ToWide(request.workerExecutable.string());
    const std::wstring cmdLine = BuildWinWorkerCommandLine(request, temps);
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    PROCESS_INFORMATION pi = {};
    BOOL okSpawn = FALSE;
    {
        std::lock_guard<std::mutex> lock(g_activeWorkerMutex);
        g_cancelActiveRequested = false;
        if (request.cancelRequested != nullptr &&
            request.cancelRequested->load(std::memory_order_relaxed))
        {
            CloseHandle(readPipe);
            CloseHandle(writePipe);
            outResult.cancelled = true;
            return false;
        }
        okSpawn = CreateProcessW(exeW.c_str(), cmdBuf.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        if (okSpawn)
        {
            g_activeChildProcess = pi.hProcess;
        }
    }
    CloseHandle(writePipe);
    if (!okSpawn)
    {
        CloseHandle(readPipe);
        outResult.errorMessage = "Failed launching quakeray_ai_worker.exe.";
        return false;
    }
    CloseHandle(pi.hThread);

    DrainWinWorkerPipe(pi.hProcess, readPipe, request, outResult, state);
    CloseHandle(readPipe);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1u;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    {
        std::lock_guard<std::mutex> lock(g_activeWorkerMutex);
        g_activeChildProcess = nullptr;
    }
    return FinalizeWinWorkerExit(exitCode, request, state, outResult);
}
#else
void ExecPosixChild(int pipeFd0, int pipeFd1, const EditorAiWorkerInvokeRequest &request,
                    const ScopedTempPair &temps)
{
#    if defined(__linux__)
    prctl(PR_SET_PDEATHSIG, SIGKILL);
#    endif
    dup2(pipeFd1, STDOUT_FILENO);
    dup2(pipeFd1, STDERR_FILENO);
    close(pipeFd0);
    close(pipeFd1);

    const std::string exeStr = request.workerExecutable.string();
    const std::string modelStr = request.modelPath.string();
    const std::string inStr = temps.inputPath.string();
    const std::string outStr = temps.outputPath.string();
    std::vector<char *> argv = {
        const_cast<char *>(exeStr.c_str()),       const_cast<char *>("--model"),
        const_cast<char *>(modelStr.c_str()),     const_cast<char *>("--arch"),
        const_cast<char *>(request.arch.c_str()), const_cast<char *>("--input"),
        const_cast<char *>(inStr.c_str()),        const_cast<char *>("--output"),
        const_cast<char *>(outStr.c_str()),
    };
    if (request.clampEdge)
    {
        argv.push_back(const_cast<char *>("--clamp"));
    }
    argv.push_back(nullptr);
    execv(exeStr.c_str(), argv.data());
    _exit(127);
}

bool FinalizePosixWorkerExit(int status, const EditorAiWorkerInvokeRequest &request,
                             const WorkerStreamState &state, EditorAiWorkerInvokeResult &outResult)
{
    if (IsCancelRequested(request))
    {
        outResult.cancelled = true;
        return false;
    }
    if (!state.workerError.empty())
    {
        outResult.errorMessage = state.workerError;
        return false;
    }
    const bool exitedZero = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!exitedZero || !state.sawDone)
    {
        outResult.errorMessage =
            state.lastLogLine.empty()
                ? (WIFSIGNALED(status)
                       ? ("AI worker terminated by signal " + std::to_string(WTERMSIG(status)) +
                          ".")
                       : "AI worker exited unexpectedly without completion.")
                : ("AI worker failed: " + state.lastLogLine);
        return false;
    }
    return true;
}

bool WaitAndDrainPosixWorker(pid_t pid, int readFd, const EditorAiWorkerInvokeRequest &request,
                             EditorAiWorkerInvokeResult &outResult, WorkerStreamState &state)
{
    std::string pending;
    std::array<char, 2048> buf{};
    for (;;)
    {
        if (IsCancelRequested(request))
        {
            kill(pid, SIGKILL);
            break;
        }
        struct pollfd pfd = { readFd, POLLIN | POLLHUP, 0 };
        const int prc = poll(&pfd, 1, 30);
        if (prc < 0)
        {
            break;
        }
        if (prc == 0)
        {
            continue;
        }
        const ssize_t n = read(readFd, buf.data(), buf.size());
        if (n <= 0)
        {
            break;
        }
        pending.append(buf.data(), static_cast<size_t>(n));
        FlushLineBuffer(pending, false, request, outResult, state);
    }
    FlushLineBuffer(pending, true, request, outResult, state);
    close(readFd);

    int status = 0;
    waitpid(pid, &status, 0);
    {
        std::lock_guard<std::mutex> lock(g_activeWorkerMutex);
        g_activeChildPid = -1;
    }
    return FinalizePosixWorkerExit(status, request, state, outResult);
}

bool SpawnAndWaitPosixWorker(const EditorAiWorkerInvokeRequest &request, const ScopedTempPair &temps,
                              EditorAiWorkerInvokeResult &outResult, WorkerStreamState &state)
{
    std::array<int, 2> pipeFds = { -1, -1 };
    if (pipe(pipeFds.data()) != 0)
    {
        outResult.errorMessage = "Failed creating pipe for AI worker stdout.";
        return false;
    }

    pid_t pid = -1;
    {
        std::lock_guard<std::mutex> lock(g_activeWorkerMutex);
        g_cancelActiveRequested = false;
        if (request.cancelRequested != nullptr &&
            request.cancelRequested->load(std::memory_order_relaxed))
        {
            close(pipeFds[0]);
            close(pipeFds[1]);
            outResult.cancelled = true;
            return false;
        }
        pid = fork();
        if (pid > 0)
        {
            g_activeChildPid = pid;
        }
    }

    if (pid < 0)
    {
        close(pipeFds[0]);
        close(pipeFds[1]);
        outResult.errorMessage = "fork() failed launching AI worker process.";
        return false;
    }
    if (pid == 0)
    {
        ExecPosixChild(pipeFds[0], pipeFds[1], request, temps);
    }

    close(pipeFds[1]);
    return WaitAndDrainPosixWorker(pid, pipeFds[0], request, outResult, state);
}
#endif

} // namespace

EditorAiVramBudget ComputeEditorAiVramBudget(uint64_t totalVramBytes, uint64_t freeVramBytes,
                                             double maxFraction)
{
    EditorAiVramBudget budget;
    if (totalVramBytes == 0u)
    {
        return budget;
    }
    const double clampedFrac = std::clamp(maxFraction, 0.05, 0.95);
    const uint64_t reserve =
        std::clamp(totalVramBytes / uint64_t(10u), kMinVramReserveBytes, kMaxVramReserveBytes);
    const uint64_t maxAllowed =
        static_cast<uint64_t>(static_cast<double>(totalVramBytes) * clampedFrac);
    const uint64_t headroom = freeVramBytes > reserve ? (freeVramBytes - reserve) : 0ULL;

    budget.totalVramBytes = totalVramBytes;
    budget.freeVramBytes = freeVramBytes;
    budget.maxAllowedBytes = maxAllowed;
    budget.reserveBytes = reserve;
    budget.effectiveBudgetBytes = std::min(maxAllowed, headroom);
    return budget;
}

std::filesystem::path FindEditorAiWorkerExecutable(const std::filesystem::path &executableDir)
{
    std::error_code ec;
    std::vector<std::filesystem::path> roots;
    if (!executableDir.empty())
    {
        roots.push_back(executableDir);
        roots.push_back(executableDir / "Release");
        roots.push_back(executableDir / "RelWithDebInfo");
        roots.push_back(executableDir / "editor");
        roots.push_back(executableDir / "code" / "quakeraysdk" / "editor");
        if (executableDir.has_parent_path())
        {
            const std::filesystem::path parent = executableDir.parent_path();
            roots.push_back(parent);
            roots.push_back(parent / "editor");
            roots.push_back(parent / "code" / "quakeraysdk" / "editor");
        }
    }
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (!cwd.empty())
    {
        roots.push_back(cwd);
        roots.push_back(cwd / "editor");
        roots.push_back(cwd / "code" / "quakeraysdk" / "editor");
    }

    for (const std::filesystem::path &dir : roots)
    {
        const std::filesystem::path candidate = dir / kWorkerExeName;
        if (std::filesystem::is_regular_file(candidate, ec))
        {
            return candidate.lexically_normal();
        }
    }
    return {};
}

bool IsEditorAiWorkerAvailable(const std::filesystem::path &executableDir)
{
    return !FindEditorAiWorkerExecutable(executableDir).empty();
}

void CancelActiveEditorAiWorker()
{
    std::lock_guard<std::mutex> lock(g_activeWorkerMutex);
    g_cancelActiveRequested = true;
#if defined(_WIN32)
    if (g_activeChildProcess != nullptr)
    {
        TerminateProcess(g_activeChildProcess, 1u);
    }
#else
    if (g_activeChildPid > 0)
    {
        kill(g_activeChildPid, SIGKILL);
    }
#endif
}

bool RunEditorAiWorkerProcess(const EditorAiWorkerInvokeRequest &request,
                              EditorAiWorkerInvokeResult &outResult)
{
    outResult = {};
    if (request.workerExecutable.empty())
    {
        outResult.errorMessage = "quakeray_ai_worker executable not found.";
        return false;
    }

    const ScopedTempPair temps = AllocateTempTensorPair();
    if (!WriteAiBinaryTensorFile(temps.inputPath, request.input, outResult.errorMessage))
    {
        return false;
    }

    WorkerStreamState streamState;
#if defined(_WIN32)
    const bool ran = SpawnAndWaitWinWorker(request, temps, outResult, streamState);
#else
    const bool ran = SpawnAndWaitPosixWorker(request, temps, outResult, streamState);
#endif
    if (!ran)
    {
        return false;
    }

    if (!ReadAiBinaryTensorFile(temps.outputPath, outResult.output, outResult.errorMessage))
    {
        return false;
    }
    outResult.ok = true;
    return true;
}
