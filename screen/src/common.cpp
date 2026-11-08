#include "common.h"

#include <cstdio>

namespace la {

LogSink g_logSink = nullptr;

SharedState& State()
{
    static SharedState instance;
    return instance;
}

void Log(const char* fmt, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    buffer[sizeof(buffer) - 1] = '\0';
    if (g_logSink)
        g_logSink(buffer);
}

std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty())
        return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0)
        return std::string();
    std::string out((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], len, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty())
        return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (len <= 0)
        return std::wstring();
    std::wstring out((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], len);
    return out;
}

std::wstring ToLower(const std::wstring& s)
{
    std::wstring out = s;
    for (wchar_t& c : out) {
        if (c >= L'A' && c <= L'Z')
            c = (wchar_t)(c - L'A' + L'a');
    }
    return out;
}

} // namespace la
