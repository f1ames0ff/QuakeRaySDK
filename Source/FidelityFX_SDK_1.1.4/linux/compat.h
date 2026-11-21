#pragma once

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cmath>
#include <locale>
#include <cstdarg>

using std::floor;
using std::log2;

#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif

#ifndef FFX_UNUSED
#define FFX_UNUSED(x) (void)(x)
#endif

template <size_t N>
inline int wcscpy_s(wchar_t (&dest)[N], const wchar_t *src)
{
    if (src == nullptr)
    {
        dest[0] = L'\0';
        return 0;
    }
    std::wcsncpy(dest, src, N);
    dest[N - 1] = L'\0';
    return 0;
}

template <size_t N>
inline int wcscpy_s(wchar_t (&dest)[N], size_t, const wchar_t *src)
{
    return wcscpy_s(dest, src);
}

template <size_t N>
inline int wcsncpy_s(wchar_t (&dest)[N], const wchar_t *src, size_t count)
{
    if (src == nullptr)
    {
        dest[0] = L'\0';
        return 0;
    }
    const size_t n = count < (N - 1) ? count : (N - 1);
    std::wcsncpy(dest, src, n);
    dest[n] = L'\0';
    return 0;
}

template <size_t N>
inline int wcscat_s(wchar_t (&dest)[N], const wchar_t *src)
{
    const size_t used = std::wcslen(dest);
    if (used >= N)
    {
        return 1;
    }
    std::wcsncpy(dest + used, src, N - used);
    dest[N - 1] = L'\0';
    return 0;
}

template <size_t N, typename... Args>
inline int sprintf_s(char (&dest)[N], const char *format, Args... args)
{
    return std::snprintf(dest, N, format, args...);
}

template <size_t N>
inline int strcpy_s(char (&dest)[N], const char *src)
{
    if (src == nullptr)
    {
        dest[0] = '\0';
        return 0;
    }
    std::strncpy(dest, src, N);
    dest[N - 1] = '\0';
    return 0;
}

template <size_t N>
inline int strcpy_s(char (&dest)[N], size_t, const char *src)
{
    return strcpy_s(dest, src);
}

inline int sprintf_s(char *dest, size_t size, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    const int result = std::vsnprintf(dest, size, format, args);
    va_end(args);
    return result;
}

inline int strcpy_s(char *dest, size_t size, const char *src)
{
    if (dest == nullptr || size == 0)
    {
        return 1;
    }
    if (src == nullptr)
    {
        dest[0] = '\0';
        return 0;
    }
    std::strncpy(dest, src, size);
    dest[size - 1] = '\0';
    return 0;
}

inline int wcscpy_s(wchar_t *dest, size_t size, const wchar_t *src)
{
    if (dest == nullptr || size == 0)
    {
        return 1;
    }
    if (src == nullptr)
    {
        dest[0] = L'\0';
        return 0;
    }
    std::wcsncpy(dest, src, size);
    dest[size - 1] = L'\0';
    return 0;
}
