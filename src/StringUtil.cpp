#include "StringUtil.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace strutil
{

std::string ToUtf8(const std::wstring &text)
{
    if (text.empty())
    {
        return std::string();
    }

    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                           nullptr);
    if (size <= 0)
    {
        return std::string();
    }

    std::string result(static_cast<size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &result[0], size, nullptr, nullptr);
    return result;

}

std::wstring ToWide(const std::string &text)
{
    if (text.empty())
    {
        return std::wstring();
    }

    int size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), static_cast<int>(text.size()), nullptr,
                                     0);
    UINT codePage = CP_UTF8;
    if (size <= 0)
    {
        // пришёл не UTF-8 (например, имя тега в ANSI) - пробуем ACP
        codePage = CP_ACP;
        size = ::MultiByteToWideChar(codePage, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    }

    if (size <= 0)
    {
        return std::wstring();
    }

    std::wstring result(static_cast<size_t>(size), L'\0');
    ::MultiByteToWideChar(codePage, 0, text.c_str(), static_cast<int>(text.size()), &result[0], size);
    return result;

}

std::string ToLowerAscii(const std::string &text)
{
    std::string result(text);
    for (size_t i = 0; i < result.size(); ++i)
    {
        const char c = result[i];
        if (c >= 'A' && c <= 'Z')
        {
            result[i] = static_cast<char>(c - 'A' + 'a');
        }
    }

    return result;

}

std::string Trim(const std::string &text)
{
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' || text[begin] == '\n'))
    {
        ++begin;
    }

    while (end > begin &&
           (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n'))
    {
        --end;
    }

    return text.substr(begin, end - begin);

}

bool StartsWith(const std::string &text, const std::string &prefix)
{
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;

}

bool EndsWith(const std::string &text, const std::string &suffix)
{
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;

}

namespace
{

int HexDigit(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }

    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }

    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }

    return -1;

}

}

bool UrlDecode(const std::string &text, std::string &decoded)
{
    decoded.clear();
    decoded.reserve(text.size());

    for (size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c == '+')
        {
            decoded.push_back(' ');
        }
        else if (c == '%')
        {
            if (i + 2 >= text.size())
            {
                return false;
            }

            const int high = HexDigit(text[i + 1]);
            const int low = HexDigit(text[i + 2]);
            if (high < 0 || low < 0)
            {
                return false;
            }

            decoded.push_back(static_cast<char>((high << 4) | low));
            i += 2;
        }
        else
        {
            decoded.push_back(c);
        }
    }

    return true;

}

std::string JsonEscape(const std::string &utf8)
{
    std::string result;
    result.reserve(utf8.size() + 8);

    for (size_t i = 0; i < utf8.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        switch (c)
        {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (c < 0x20)
            {
                result += Format("\\u%04x", static_cast<unsigned>(c));
            }
            else
            {
                // байты >= 0x80 передаём как есть: строка уже в UTF-8
                result.push_back(static_cast<char>(c));
            }
            break;
        }
    }

    return result;

}

std::string HexEncode(const unsigned char *data, size_t size)
{
    static const char hexDigits[] = "0123456789ABCDEF";

    std::string result;
    result.reserve(size * 2);
    for (size_t i = 0; i < size; ++i)
    {
        result.push_back(hexDigits[(data[i] >> 4) & 0x0F]);
        result.push_back(hexDigits[data[i] & 0x0F]);
    }

    return result;

}

std::string Format(const char *format, ...)
{
    char stackBuffer[512];

    va_list args;
    va_start(args, format);
    const int written = _vsnprintf_s(stackBuffer, sizeof(stackBuffer), _TRUNCATE, format, args);
    va_end(args);

    if (written >= 0)
    {
        return std::string(stackBuffer, static_cast<size_t>(written));
    }

    // не поместилось в стек - повторяем в куче
    std::vector<char> heapBuffer(4096, '\0');
    va_start(args, format);
    const int heapWritten = _vsnprintf_s(&heapBuffer[0], heapBuffer.size(), _TRUNCATE, format, args);
    va_end(args);

    if (heapWritten < 0)
    {
        return std::string();
    }

    return std::string(&heapBuffer[0], static_cast<size_t>(heapWritten));

}

bool EqualsIgnoreCaseAscii(const std::string &first, const std::string &second)
{
    return ToLowerAscii(first) == ToLowerAscii(second);

}

bool ContainsIgnoreCaseAscii(const std::string &text, const std::string &needle)
{
    if (needle.empty())
    {
        return true;
    }

    return ToLowerAscii(text).find(ToLowerAscii(needle)) != std::string::npos;

}

std::vector<std::string> Split(const std::string &text, char delimiter)
{
    std::vector<std::string> parts;
    std::string current;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == delimiter)
        {
            parts.push_back(current);
            current.clear();
        }
        else
        {
            current.push_back(text[i]);
        }
    }

    parts.push_back(current);
    return parts;

}

}

