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

#include "EditorApp.h"

#include <string>
#include <vector>

#if defined(_WIN32)

#include <windows.h>
#include <shellapi.h>

namespace
{

std::string Utf8FromWide(const wchar_t *text)
{
    if (text == nullptr)
    {
        return {};
    }

    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);

    if (length <= 1)
    {
        return {};
    }

    std::string result(size_t(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);

    if (!result.empty() && result.back() == '\0')
    {
        result.pop_back();
    }

    return result;
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand)
{
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;

    if (argv != nullptr)
    {
        args.reserve(size_t(argc));

        for (int i = 0; i < argc; i++)
        {
            args.push_back(Utf8FromWide(argv[i]));
        }

        LocalFree(argv);
    }

    EditorApp app(instance);
    return app.Run(args, showCommand);
}

#else

int main(int argc, char **argv)
{
    std::vector<std::string> args;
    args.reserve(size_t(argc > 0 ? argc : 0));

    for (int i = 0; i < argc; i++)
    {
        args.emplace_back(argv[i] != nullptr ? argv[i] : "");
    }

    EditorApp app(nullptr);
    return app.Run(args, 0);
}

#endif
