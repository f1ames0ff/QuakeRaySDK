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
