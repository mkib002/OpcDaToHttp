/*
OpcDaToHttp

Мелкие строковые утилиты: UTF-8 <-> UTF-16, URL-декодирование, JSON-экранирование.
Единственное место конвертации строк в приложении (тулкит использует свои WS2S/S2WS).
*/
#pragma once

#include <string>
#include <vector>

namespace strutil
{

/// UTF-16 -> UTF-8
std::string ToUtf8(const std::wstring &text);

/// UTF-8 -> UTF-16 (при некорректном UTF-8 используется фолбэк через ACP)
std::wstring ToWide(const std::string &text);

/// Понижение регистра только для ASCII (имена OPC-тегов - латиница/цифры/разделители)
std::string ToLowerAscii(const std::string &text);

std::string Trim(const std::string &text);

bool StartsWith(const std::string &text, const std::string &prefix);

bool EndsWith(const std::string &text, const std::string &suffix);

/// application/x-www-form-urlencoded: '%XX' -> байт, '+' -> пробел
bool UrlDecode(const std::string &text, std::string &decoded);

/// Экранирование для вставки внутрь JSON-литерала (без обрамляющих кавычек)
std::string JsonEscape(const std::string &utf8);

std::string HexEncode(const unsigned char *data, size_t size);

/// printf-совместимое форматирование в std::string
std::string Format(const char *format, ...);

bool EqualsIgnoreCaseAscii(const std::string &first, const std::string &second);

bool ContainsIgnoreCaseAscii(const std::string &text, const std::string &needle);

std::vector<std::string> Split(const std::string &text, char delimiter);

}
