#include "OpcValueFormatter.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "StringUtil.h"
#include "opcda.h" // константы качества OPC_QUALITY_*

namespace
{

using opcfmt::FormatResult;

void FormatInteger(long long value, FormatResult &out)
{
    out.valueText = strutil::Format("%lld", value);
    out.valueJson = out.valueText;

}

void FormatUnsigned(unsigned long long value, FormatResult &out)
{
    out.valueText = strutil::Format("%llu", value);
    out.valueJson = out.valueText;

}

/// Разворачивает экспоненциальную запись "1.234e-05" в обычную "0.00001234":
/// JSON не допускает экспоненту, поэтому числа нужны в фиксированном виде.
std::string ExpandScientific(const std::string &text)
{
    const size_t ePos = text.find_first_of("eE");
    if (ePos == std::string::npos)
    {
        return text;
    }

    const std::string mantissa = text.substr(0, ePos);
    const int exponent = std::atoi(text.c_str() + ePos + 1);

    bool negative = false;
    std::string digits = mantissa;
    if (!digits.empty() && digits[0] == '-')
    {
        negative = true;
        digits.erase(0, 1);
    }

    const size_t dot = digits.find('.');
    if (dot != std::string::npos)
    {
        digits.erase(dot, 1);
    }

    // В научной записи мантисса "d.ddd" - точка после первой цифры.
    // После умножения на 10^exponent точка сдвигается на exponent позиций.
    const int dotPos = 1 + exponent;

    std::string result;
    if (dotPos <= 0)
    {
        result = "0." + std::string(static_cast<size_t>(-dotPos), '0') + digits;
    }
    else if (dotPos >= static_cast<int>(digits.size()))
    {
        result = digits + std::string(static_cast<size_t>(dotPos - static_cast<int>(digits.size())), '0');
    }
    else
    {
        result = digits.substr(0, static_cast<size_t>(dotPos)) + "." + digits.substr(static_cast<size_t>(dotPos));
    }

    if (negative)
    {
        result = "-" + result;
    }

    return result;

}

void FormatDouble(double value, int precision, FormatResult &out)
{
    if (!std::isfinite(value))
    {
        // NaN/Infinity недопустимы в JSON - в JSON отдаём null, в тексте - как есть
        out.valueJson = "null";
        out.valueText = std::isnan(value) ? "NaN" : (value > 0 ? "Infinity" : "-Infinity");
        return;
    }

    char buffer[64] = {0};
    _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "%.*g", precision, value);

    std::string text(buffer);
    // Локально-зависимый десятичный разделитель (запятая) недопустим в JSON.
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == ',')
        {
            text[i] = '.';
        }
    }
    // Экспоненциальная запись (1e-05) недопустима в JSON - разворачиваем в обычную.
    text = ExpandScientific(text);

    out.valueText = text;
    out.valueJson = text;

}

void FormatString(const std::string &text, FormatResult &out)
{
    out.valueText = text;
    out.valueJson = "\"" + strutil::JsonEscape(text) + "\"";

}

void FormatBool(bool value, FormatResult &out)
{
    out.valueText = value ? "true" : "false";
    out.valueJson = out.valueText;

}

/// Указатель на поле VARIANT нужного типа - для SafeArrayGetElement
void *VariantElementPtr(VARIANT &value)
{
    switch (value.vt)
    {
    case VT_I1:
        return &value.cVal;
    case VT_UI1:
        return &value.bVal;
    case VT_I2:
        return &value.iVal;
    case VT_UI2:
        return &value.uiVal;
    case VT_I4:
        return &value.lVal;
    case VT_UI4:
        return &value.ulVal;
    case VT_INT:
        return &value.intVal;
    case VT_UINT:
        return &value.uintVal;
    case VT_I8:
        return &value.llVal;
    case VT_UI8:
        return &value.ullVal;
    case VT_R4:
        return &value.fltVal;
    case VT_R8:
        return &value.dblVal;
    case VT_BOOL:
        return &value.boolVal;
    case VT_CY:
        return &value.cyVal;
    case VT_DATE:
        return &value.date;
    case VT_DECIMAL:
        return &value.decVal;
    case VT_ERROR:
        return &value.scode;
    default:
        return nullptr;
    }

}

bool FormatArrayElement(SAFEARRAY *array, VARTYPE elementType, LONG index, std::string &json, std::string &text)
{
    json = "null";
    text.clear();

    if (elementType == VT_BSTR)
    {
        BSTR element = nullptr;
        const HRESULT result = ::SafeArrayGetElement(array, &index, &element);
        if (FAILED(result) || element == nullptr)
        {
            if (element != nullptr)
            {
                ::SysFreeString(element);
            }

            return false;
        }

        const std::string value = strutil::ToUtf8(std::wstring(element, ::SysStringLen(element)));
        ::SysFreeString(element);
        text = value;
        json = "\"" + strutil::JsonEscape(value) + "\"";
        return true;
    }

    VARIANT element;
    ::VariantInit(&element);

    if (elementType == VT_VARIANT)
    {
        element.vt = VT_VARIANT;
        const HRESULT result = ::SafeArrayGetElement(array, &index, &element);
        if (SUCCEEDED(result))
        {
            const FormatResult formatted = opcfmt::Format(element);
            json = formatted.valueJson;
            text = formatted.valueText;
            ::VariantClear(&element);
            return formatted.ok;
        }

        ::VariantClear(&element);
        return false;
    }

    element.vt = elementType;
    void *destination = VariantElementPtr(element);
    if (destination == nullptr)
    {
        return false;
    }

    if (FAILED(::SafeArrayGetElement(array, &index, destination)))
    {
        ::VariantClear(&element);
        return false;
    }

    const FormatResult formatted = opcfmt::Format(element);
    json = formatted.valueJson;
    text = formatted.valueText;
    ::VariantClear(&element);
    return formatted.ok;

}

bool FormatArray(const VARIANT &value, FormatResult &out)
{
    out.isArray = true;

    if (value.parray == nullptr)
    {
        out.ok = false;
        return false;
    }

    const VARTYPE elementType = static_cast<VARTYPE>(value.vt & VT_TYPEMASK);

    LONG lowerBound = 0;
    LONG upperBound = -1;
    if (FAILED(::SafeArrayGetLBound(value.parray, 1, &lowerBound)) ||
        FAILED(::SafeArrayGetUBound(value.parray, 1, &upperBound)))
    {
        // многомерный или повреждённый массив
        out.ok = false;
        return false;
    }

    const size_t count = (upperBound >= lowerBound) ? static_cast<size_t>(upperBound - lowerBound + 1) : 0;
    out.arrayLength = count;

    const size_t limit = count < opcfmt::kMaxArrayElements ? count : opcfmt::kMaxArrayElements;
    out.truncated = count > limit;

    if (elementType == VT_UI1)
    {
        // "бинарные" массивы удобнее отдавать hex-строкой
        std::vector<unsigned char> bytes(limit, 0);
        for (size_t i = 0; i < limit; ++i)
        {
            LONG index = lowerBound + static_cast<LONG>(i);
            if (FAILED(::SafeArrayGetElement(value.parray, &index, &bytes[i])))
            {
                out.ok = false;
                return false;
            }
        }

        out.valueText = strutil::HexEncode(bytes.empty() ? nullptr : &bytes[0], bytes.size());
        out.valueJson = "\"" + out.valueText + "\"";
        out.ok = true;
        return true;
    }

    std::string json = "[";
    std::string text;
    for (size_t i = 0; i < limit; ++i)
    {
        std::string elementJson;
        std::string elementText;
        const LONG index = lowerBound + static_cast<LONG>(i);
        if (!FormatArrayElement(value.parray, elementType, index, elementJson, elementText))
        {
            elementJson = "null";
            elementText.clear();
        }

        if (i > 0)
        {
            json += ",";
            text += ",";
        }

        json += elementJson;
        text += elementText;
    }

    json += "]";
    out.valueJson = json;
    out.valueText = text;
    out.ok = true;
    return true;

}

std::string BaseTypeName(VARTYPE vt)
{
    switch (vt)
    {
    case VT_EMPTY:
        return "VT_EMPTY";
    case VT_NULL:
        return "VT_NULL";
    case VT_I1:
        return "VT_I1";
    case VT_UI1:
        return "VT_UI1";
    case VT_I2:
        return "VT_I2";
    case VT_UI2:
        return "VT_UI2";
    case VT_I4:
        return "VT_I4";
    case VT_UI4:
        return "VT_UI4";
    case VT_INT:
        return "VT_INT";
    case VT_UINT:
        return "VT_UINT";
    case VT_I8:
        return "VT_I8";
    case VT_UI8:
        return "VT_UI8";
    case VT_R4:
        return "VT_R4";
    case VT_R8:
        return "VT_R8";
    case VT_BOOL:
        return "VT_BOOL";
    case VT_BSTR:
        return "VT_BSTR";
    case VT_DATE:
        return "VT_DATE";
    case VT_CY:
        return "VT_CY";
    case VT_DECIMAL:
        return "VT_DECIMAL";
    case VT_ERROR:
        return "VT_ERROR";
    case VT_VARIANT:
        return "VT_VARIANT";
    case VT_UNKNOWN:
        return "VT_UNKNOWN";
    case VT_DISPATCH:
        return "VT_DISPATCH";
    default:
        return strutil::Format("VT_0x%04X", static_cast<unsigned>(vt));
    }

}

}

opcfmt::FormatResult opcfmt::Format(const VARIANT &value)
{
    FormatResult out;
    out.type = VariantTypeName(value.vt);

    if ((value.vt & VT_ARRAY) != 0)
    {
        FormatArray(value, out);
        return out;
    }

    switch (static_cast<VARTYPE>(value.vt & VT_TYPEMASK))
    {
    case VT_EMPTY:
    case VT_NULL:
        out.valueJson = "null";
        out.valueText.clear();
        out.ok = true;
        break;
    case VT_I1:
        FormatInteger(static_cast<long long>(value.cVal), out);
        out.ok = true;
        break;
    case VT_UI1:
        FormatUnsigned(static_cast<unsigned long long>(value.bVal), out);
        out.ok = true;
        break;
    case VT_I2:
        FormatInteger(static_cast<long long>(value.iVal), out);
        out.ok = true;
        break;
    case VT_UI2:
        FormatUnsigned(static_cast<unsigned long long>(value.uiVal), out);
        out.ok = true;
        break;
    case VT_I4:
        FormatInteger(static_cast<long long>(value.lVal), out);
        out.ok = true;
        break;
    case VT_UI4:
        FormatUnsigned(static_cast<unsigned long long>(value.ulVal), out);
        out.ok = true;
        break;
    case VT_INT:
        FormatInteger(static_cast<long long>(value.intVal), out);
        out.ok = true;
        break;
    case VT_UINT:
        FormatUnsigned(static_cast<unsigned long long>(value.uintVal), out);
        out.ok = true;
        break;
    case VT_I8:
        FormatInteger(static_cast<long long>(value.llVal), out);
        out.ok = true;
        break;
    case VT_UI8:
        FormatUnsigned(static_cast<unsigned long long>(value.ullVal), out);
        out.ok = true;
        break;
    case VT_R4:
        FormatDouble(value.fltVal, 7, out);
        out.ok = true;
        break;
    case VT_R8:
        FormatDouble(value.dblVal, 15, out);
        out.ok = true;
        break;
    case VT_BOOL:
        FormatBool(value.boolVal != VARIANT_FALSE, out);
        out.ok = true;
        break;
    case VT_BSTR:
        FormatString(value.bstrVal != nullptr
                         ? strutil::ToUtf8(std::wstring(value.bstrVal, ::SysStringLen(value.bstrVal)))
                         : std::string(),
                     out);
        out.ok = true;
        break;
    case VT_DATE:
    {
        SYSTEMTIME systemTime = {0};
        if (::VariantTimeToSystemTime(value.date, &systemTime) != FALSE)
        {
            FormatString(strutil::Format("%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", static_cast<unsigned>(systemTime.wYear),
                                         static_cast<unsigned>(systemTime.wMonth),
                                         static_cast<unsigned>(systemTime.wDay),
                                         static_cast<unsigned>(systemTime.wHour),
                                         static_cast<unsigned>(systemTime.wMinute),
                                         static_cast<unsigned>(systemTime.wSecond),
                                         static_cast<unsigned>(systemTime.wMilliseconds)),
                         out);
        }
        else
        {
            FormatDouble(value.date, 15, out);
        }

        out.ok = true;
        break;
    }
    case VT_ERROR:
        FormatString(HResultToString(value.scode), out);
        out.ok = true;
        break;
    default:
    {
        // VT_CY/VT_DECIMAL/VT_UNKNOWN и прочее - через OLE-конвертацию в строку
        VARIANT source;
        ::VariantInit(&source);
        if (SUCCEEDED(::VariantCopy(&source, const_cast<VARIANT *>(&value))))
        {
            VARIANT converted;
            ::VariantInit(&converted);
            if (SUCCEEDED(::VariantChangeType(&converted, &source, 0, VT_BSTR)) && converted.bstrVal != nullptr)
            {
                FormatString(strutil::ToUtf8(std::wstring(converted.bstrVal, ::SysStringLen(converted.bstrVal))), out);
                out.ok = true;
            }
            else
            {
                out.valueJson = "null";
                out.valueText = strutil::Format("<unprintable %s>", out.type.c_str());
                out.ok = false;
            }

            ::VariantClear(&converted);
        }
        else
        {
            out.valueJson = "null";
            out.valueText = strutil::Format("<unreadable %s>", out.type.c_str());
            out.ok = false;
        }

        ::VariantClear(&source);
        break;
    }
    }

    return out;

}

std::string opcfmt::VariantTypeName(VARTYPE vt)
{
    std::string name = BaseTypeName(static_cast<VARTYPE>(vt & VT_TYPEMASK));
    if ((vt & VT_ARRAY) != 0)
    {
        name = "VT_ARRAY|" + name;
    }

    if ((vt & VT_BYREF) != 0)
    {
        name += "|VT_BYREF";
    }

    return name;

}

std::string opcfmt::QualityToString(WORD quality)
{
    std::string result;

    switch (static_cast<WORD>(quality & OPC_QUALITY_MASK))
    {
    case OPC_QUALITY_GOOD:
        result = "Good";
        break;
    case OPC_QUALITY_UNCERTAIN:
        result = "Uncertain";
        break;
    default:
        result = "Bad";
        break;
    }

    const char *subStatus = nullptr;
    switch (static_cast<WORD>(quality & OPC_STATUS_MASK))
    {
    case OPC_QUALITY_CONFIG_ERROR:
        subStatus = "Config Error";
        break;
    case OPC_QUALITY_NOT_CONNECTED:
        subStatus = "Not Connected";
        break;
    case OPC_QUALITY_DEVICE_FAILURE:
        subStatus = "Device Failure";
        break;
    case OPC_QUALITY_SENSOR_FAILURE:
        subStatus = "Sensor Failure";
        break;
    case OPC_QUALITY_LAST_KNOWN:
        subStatus = "Last Known Value";
        break;
    case OPC_QUALITY_COMM_FAILURE:
        subStatus = "Comm Failure";
        break;
    case OPC_QUALITY_OUT_OF_SERVICE:
        subStatus = "Out Of Service";
        break;
    case OPC_QUALITY_WAITING_FOR_INITIAL_DATA:
        subStatus = "Waiting For Initial Data";
        break;
    default:
        subStatus = nullptr;
        break;
    }

    if (subStatus != nullptr)
    {
        result += " - ";
        result += subStatus;
    }

    switch (static_cast<WORD>(quality & OPC_LIMIT_MASK))
    {
    case 1:
        result += " (Low Limited)";
        break;
    case 2:
        result += " (High Limited)";
        break;
    case 3:
        result += " (Constant)";
        break;
    default:
        break;
    }

    return result;

}

std::string opcfmt::FileTimeToIso8601(const FILETIME &ft)
{
    if (ft.dwLowDateTime == 0 && ft.dwHighDateTime == 0)
    {
        return std::string();
    }

    SYSTEMTIME systemTime = {0};
    if (::FileTimeToSystemTime(&ft, &systemTime) == FALSE)
    {
        return std::string();
    }

    return strutil::Format("%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", static_cast<unsigned>(systemTime.wYear),
                           static_cast<unsigned>(systemTime.wMonth), static_cast<unsigned>(systemTime.wDay),
                           static_cast<unsigned>(systemTime.wHour), static_cast<unsigned>(systemTime.wMinute),
                           static_cast<unsigned>(systemTime.wSecond), static_cast<unsigned>(systemTime.wMilliseconds));

}

std::string opcfmt::HResultToString(HRESULT hr)
{
    return strutil::Format("0x%08X", static_cast<unsigned>(hr));

}

