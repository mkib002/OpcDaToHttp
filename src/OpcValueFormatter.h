/*
OpcDaToHttp

Конвертация VARIANT (значение OPC-тега) в готовый JSON-литерал и plain-текст,
расшифровка качества/типа/времени.
*/
#pragma once

#include <windows.h>
#include <oleauto.h>

#include <string>

namespace opcfmt
{

/// Максимум элементов массива, попадающих в ответ (остальное - только длина/флаг truncated)
const size_t kMaxArrayElements = 1024;

struct FormatResult
{
    std::string type;                    // "VT_I4", "VT_ARRAY|VT_UI2"
    std::string valueJson = "null";      // готовый JSON-литерал
    std::string valueText;               // текстовое представление
    bool ok = false;                     // удалось ли представить значение
    bool isArray = false;
    size_t arrayLength = 0;
    bool truncated = false;

}; // FormatResult

/// Разбор значения. Для VT_ARRAY|VT_UI1 значение отдаётся hex-строкой.
FormatResult Format(const VARIANT &value);

/// "Good", "Uncertain", "Bad - Not Connected (Low Limited)" ...
std::string QualityToString(WORD quality);

/// "VT_I4", "VT_ARRAY|VT_R8" ...
std::string VariantTypeName(VARTYPE vt);

/// FILETIME (UTC) -> "2026-09-24T09:12:33.123Z"; пустая строка, если времени нет
std::string FileTimeToIso8601(const FILETIME &ft);

/// "0x80040400"
std::string HResultToString(HRESULT hr);

}
