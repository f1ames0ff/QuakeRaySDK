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

#include "AiWorkerVram.h"
#include "DeepBumpPipeline.h"
#include "EditorAiWorkerProtocol.h"
#include "GgufLoader.h"
#include "LotusPipeline.h"
#include "RealPlksrPipeline.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace
{

struct CliArgs
{
    std::string modelPath;
    std::string arch;
    std::string inputPath;
    std::string outputPath;
    WorkerRunOptions opts;
};

void EmitProgress(int32_t pct, const std::string &stage)
{
    std::printf("PROGRESS %d %s\n", pct, stage.c_str());
    std::fflush(stdout);
}

void EmitError(const std::string &msg)
{
    std::printf("ERROR %s\n", msg.c_str());
    std::fflush(stdout);
}

bool ParseCliArgs(int argc, char **argv, CliArgs &outArgs, std::string &outError)
{
    for (int i = 1; i < argc; i++)
    {
        const std::string arg = argv[i];
        if (arg == "--force-cpu")
        {
            outArgs.opts.forceCpu = true;
            continue;
        }
        if (arg == "--clamp")
        {
            outArgs.opts.clampEdge = true;
            continue;
        }
        if (i + 1 >= argc)
        {
            outError = "Missing value for argument: " + arg;
            return false;
        }
        const std::string val = argv[++i];
        if (arg == "--model")
        {
            outArgs.modelPath = val;
        }
        else if (arg == "--arch")
        {
            outArgs.arch = val;
        }
        else if (arg == "--input")
        {
            outArgs.inputPath = val;
        }
        else if (arg == "--output")
        {
            outArgs.outputPath = val;
        }
        else if (arg == "--vram-cap")
        {
            outArgs.opts.vramCapFraction = std::strtod(val.c_str(), nullptr);
        }
        else
        {
            outError = "Unknown argument: " + arg;
            return false;
        }
    }
    if (outArgs.modelPath.empty() || outArgs.arch.empty() || outArgs.inputPath.empty() ||
        outArgs.outputPath.empty())
    {
        outError =
            "Usage: quakeray_ai_worker --model <gguf> --arch <lotus|deepbump|delight|realplksr> "
            "--input <qrai> --output <qrai>";
        return false;
    }
    return true;
}

bool ExecuteWorkerJob(CliArgs &args, WorkerRunResult &result, std::string &err)
{
    EmitProgress(2, "Initializing inference worker");
    const WorkerVulkanDevice vkDev =
        args.opts.forceCpu ? WorkerVulkanDevice{} : SelectBestWorkerVulkanDevice();
    if (vkDev.index >= 0)
    {
        const WorkerVramBudget budget =
            ComputeWorkerVramBudget(vkDev.index, args.opts.vramCapFraction);
        std::printf("VRAM total=%llu free=%llu budget=%llu\n",
                    static_cast<unsigned long long>(budget.totalVramBytes),
                    static_cast<unsigned long long>(budget.freeVramBytes),
                    static_cast<unsigned long long>(budget.effectiveBudgetBytes));
        std::fflush(stdout);
    }

    EmitProgress(5, "Loading GGUF header");
    GgufModelFile model;
    if (!model.Open(args.modelPath, err))
    {
        return false;
    }

    EditorAiTensorNchw input;
    if (!ReadAiBinaryTensorFile(args.inputPath, input, err))
    {
        return false;
    }

    args.opts.onProgress = [](int32_t pct, const std::string &stage) { EmitProgress(pct, stage); };
    bool ok = false;
    if (args.arch == "lotus")
    {
        ok = qray_ai::RunLotusGgufPipeline(model, vkDev, args.opts, input, result);
    }
    else if (args.arch == "delight")
    {
        ok = qray_ai::RunDelightGgufPipeline(model, vkDev, args.opts, input, result);
    }
    else if (args.arch == "deepbump")
    {
        ok = qray_ai::RunDeepBumpGgufPipeline(model, vkDev, args.opts, input, result);
    }
    else if (args.arch == "realplksr")
    {
        ok = qray_ai::RunRealPlksrGgufPipeline(model, vkDev, args.opts, input, result);
    }
    else
    {
        result.error = "Unsupported --arch value: " + args.arch;
    }
    model.Close();
    if (!ok)
    {
        err = result.error;
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    CliArgs args;
    std::string err;
    if (!ParseCliArgs(argc, argv, args, err))
    {
        EmitError(err);
        return 1;
    }

    WorkerRunResult result;
    if (!ExecuteWorkerJob(args, result, err))
    {
        EmitError(err);
        return 1;
    }

    if (!result.provider.empty())
    {
        std::printf("PROVIDER %s\n", result.provider.c_str());
        std::fflush(stdout);
    }

    EmitProgress(98, "Writing generated tensor");
    if (!WriteAiBinaryTensorFile(args.outputPath, result.output, err))
    {
        EmitError(err);
        return 1;
    }

    EmitProgress(100, "Complete");
    std::printf("DONE\n");
    std::fflush(stdout);
    return 0;
}
