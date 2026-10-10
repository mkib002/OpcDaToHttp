#include "AppConfig.h"

#include <cstdlib>
#include <cwchar>
#include <vector>

#include "StringUtil.h"

namespace
{

std::string ToUtf8(const std::wstring &text)
{
    return strutil::ToUtf8(text);

}

bool ParseUnsigned(const std::wstring &text, unsigned long &value)
{
    if (text.empty())
    {
        return false;
    }

    wchar_t *end = nullptr;
    const unsigned long parsed = std::wcstoul(text.c_str(), &end, 10);
    if (end == text.c_str() || (end != nullptr && *end != L'\0'))
    {
        return false;
    }

    value = parsed;
    return true;

}

bool ParseSize(const std::wstring &text, size_t &value)
{
    unsigned long parsed = 0;
    if (!ParseUnsigned(text, parsed))
    {
        return false;
    }

    value = parsed;
    return true;

}

bool ParseFloat(const std::wstring &text, float &value)
{
    if (text.empty())
    {
        return false;
    }

    wchar_t *end = nullptr;
    const double parsed = std::wcstod(text.c_str(), &end);
    if (end == text.c_str() || (end != nullptr && *end != L'\0'))
    {
        return false;
    }

    value = static_cast<float>(parsed);
    return true;

}

bool ParseLogLevel(const std::string &text, LogLevel &level)
{
    if (strutil::EqualsIgnoreCaseAscii(text, "debug"))
    {
        level = LogLevel::Debug;
        return true;
    }

    if (strutil::EqualsIgnoreCaseAscii(text, "info"))
    {
        level = LogLevel::Info;
        return true;
    }

    if (strutil::EqualsIgnoreCaseAscii(text, "warn") || strutil::EqualsIgnoreCaseAscii(text, "warning"))
    {
        level = LogLevel::Warn;
        return true;
    }

    if (strutil::EqualsIgnoreCaseAscii(text, "error"))
    {
        level = LogLevel::Error;
        return true;
    }

    return false;

}

/// Достаёт значение ключа: "--key value" или "--key=value"
bool TakeValue(int argc, wchar_t *argv[], int &index, const std::wstring &argument, std::wstring &value,
               std::string &errorText)
{
    const std::wstring equalsForm = argument + L"=";
    if (argument.size() > equalsForm.size() &&
        argument.compare(0, equalsForm.size(), equalsForm) == 0)
    {
        value = argument.substr(equalsForm.size());
        return true;
    }

    if (index + 1 >= argc)
    {
        errorText = "для ключа " + ToUtf8(argument) + " не задано значение";
        return false;
    }

    ++index;
    value = argv[index];
    return true;

}

}

bool ParseCommandLine(int argc, wchar_t *argv[], AppConfig &config, std::string &errorText)
{
    for (int index = 1; index < argc; ++index)
    {
        const std::wstring argument = argv[index];

        if (argument == L"--help" || argument == L"-h" || argument == L"/?")
        {
            config.showHelp = true;
            continue;
        }

        if (argument == L"--list-servers")
        {
            config.listServers = true;
            continue;
        }

        std::wstring value;

        if (argument == L"--host" || argument.compare(0, 7, L"--host=") == 0)
        {
            if (!TakeValue(argc, argv, index, argument, value, errorText))
            {
                return false;
            }

            config.host = value;
            continue;
        }

        if (argument == L"--server" || argument.compare(0, 9, L"--server=") == 0)
        {
            if (!TakeValue(argc, argv, index, argument, value, errorText))
            {
                return false;
            }

            config.serverProgId = value;
            continue;
        }

        if (argument == L"--port" || argument.compare(0, 7, L"--port=") == 0)
        {
            unsigned long port = 0;
            if (!TakeValue(argc, argv, index, argument, value, errorText) || !ParseUnsigned(value, port) || port > 65535)
            {
                errorText = "некорректное значение --port (ожидается 0..65535)";
                return false;
            }

            config.port = static_cast<unsigned short>(port);
            continue;
        }

        if (argument == L"--rate" || argument.compare(0, 7, L"--rate=") == 0)
        {
            unsigned long rate = 0;
            if (!TakeValue(argc, argv, index, argument, value, errorText) || !ParseUnsigned(value, rate) || rate < 10)
            {
                errorText = "некорректное значение --rate (период обновления группы, мс, минимум 10)";
                return false;
            }

            config.rateMs = rate;
            continue;
        }

        if (argument == L"--deadband" || argument.compare(0, 11, L"--deadband=") == 0)
        {
            if (!TakeValue(argc, argv, index, argument, value, errorText) || !ParseFloat(value, config.deadBand) ||
                config.deadBand < 0.0f)
            {
                errorText = "некорректное значение --deadband (число >= 0)";
                return false;
            }

            continue;
        }

        if (argument == L"--refresh-sec" || argument.compare(0, 14, L"--refresh-sec=") == 0)
        {
            if (!TakeValue(argc, argv, index, argument, value, errorText) || !ParseUnsigned(value, config.refreshSec))
            {
                errorText = "некорректное значение --refresh-sec (секунды, 0 = выключено)";
                return false;
            }

            continue;
        }

        if (argument == L"--items-file" || argument.compare(0, 13, L"--items-file=") == 0)
        {
            if (!TakeValue(argc, argv, index, argument, value, errorText))
            {
                return false;
            }

            config.itemsFile = strutil::ToUtf8(value);
            continue;
        }

        if (argument == L"--filter" || argument.compare(0, 9, L"--filter=") == 0)
        {
            if (!TakeValue(argc, argv, index, argument, value, errorText))
            {
                return false;
            }

            config.filter = strutil::ToUtf8(value);
            continue;
        }

        if (argument == L"--max-items" || argument.compare(0, 12, L"--max-items=") == 0)
        {
            if (!TakeValue(argc, argv, index, argument, value, errorText) || !ParseSize(value, config.maxItems))
            {
                errorText = "некорректное значение --max-items (целое >= 0, 0 = без ограничения)";
                return false;
            }

            continue;
        }

        if (argument == L"--reconnect-sec" || argument.compare(0, 16, L"--reconnect-sec=") == 0)
        {
            if (!TakeValue(argc, argv, index, argument, value, errorText) || !ParseUnsigned(value, config.reconnectSec))
            {
                errorText = "некорректное значение --reconnect-sec (секунды, 0 = не переподключаться)";
                return false;
            }

            continue;
        }

        if (argument == L"--max-conn" || argument.compare(0, 11, L"--max-conn=") == 0)
        {
            if (!TakeValue(argc, argv, index, argument, value, errorText) ||
                !ParseUnsigned(value, config.maxConnections) || config.maxConnections == 0)
            {
                errorText = "некорректное значение --max-conn (целое > 0)";
                return false;
            }

            continue;
        }

        if (argument == L"--stale-ms" || argument.compare(0, 11, L"--stale-ms=") == 0)
        {
            if (!TakeValue(argc, argv, index, argument, value, errorText) || !ParseUnsigned(value, config.staleMs))
            {
                errorText = "некорректное значение --stale-ms (мс, 0 = авто)";
                return false;
            }

            continue;
        }

        if (argument == L"--timeout-ms" || argument.compare(0, 13, L"--timeout-ms=") == 0)
        {
            if (!TakeValue(argc, argv, index, argument, value, errorText) ||
                !ParseUnsigned(value, config.requestTimeoutMs) || config.requestTimeoutMs < 100)
            {
                errorText = "некорректное значение --timeout-ms (мс, минимум 100)";
                return false;
            }

            continue;
        }

        if (argument == L"--log-level" || argument.compare(0, 12, L"--log-level=") == 0)
        {
            if (!TakeValue(argc, argv, index, argument, value, errorText) ||
                !ParseLogLevel(strutil::ToUtf8(value), config.logLevel))
            {
                errorText = "некорректное значение --log-level (debug|info|warn|error)";
                return false;
            }

            continue;
        }

        errorText = "неизвестный ключ: " + strutil::ToUtf8(argument);
        return false;
    }

    return true;

}

std::string UsageText()
{
    return
        "OpcDaToHttp - мост OPC DA -> HTTP\n"
        "\n"
        "Использование:\n"
        "  OpcDaToHttp.exe [ключи]\n"
        "\n"
        "Ключи:\n"
        "  --server <ProgID>     ProgID OPC DA сервера, например OPC.OpcTestServer_x64\n"
        "                        (если не задан - берётся первый найденный локальный DA-сервер)\n"
        "  --host <имя>          хост, по умолчанию localhost ('', 'localhost', '127.0.0.1' - локальный)\n"
        "  --port <номер>        порт HTTP, по умолчанию 8080 (0 - любой свободный)\n"
        "  --rate <мс>           период обновления группы OPC, по умолчанию 1000\n"
        "  --deadband <число>    зона нечувствительности группы, по умолчанию 0\n"
        "  --refresh-sec <сек>   периодический refresh всей группы (0 = только подписка), по умолчанию 0\n"
        "  --items-file <файл>   файл со списком тегов (по имени в строке) вместо обзора namespace\n"
        "  --filter <подстрока>  брать из namespace только имена, содержащие подстроку\n"
        "  --max-items <N>       ограничить число тегов (0 = без ограничения)\n"
        "  --reconnect-sec <сек> повтор подключения к серверу при обрыве (0 = не переподключаться), по умолчанию 10\n"
        "  --max-conn <N>        предел одновременных HTTP-соединений, по умолчанию 64\n"
        "  --stale-ms <мс>       порог 'устаревания' значения (0 = авто: 10 * rate, но не меньше 60000)\n"
        "  --timeout-ms <мс>     ожидание refresh для ?refresh=1, по умолчанию 5000\n"
        "  --log-level <уровень> debug | info | warn | error (по умолчанию info)\n"
        "  --list-servers        вывести список локальных OPC DA серверов и выйти\n"
        "  --help                эта справка\n"
        "\n"
        "HTTP-сервер слушает на всех сетевых интерфейсах (0.0.0.0). Локально доступен\n"
        "по http://127.0.0.1:<порт>/, с других машин - по http://<IP-машины>:<порт>/.\n"
        "\n"
        "HTTP API:\n"
        "  GET /                              справка и состояние\n"
        "  GET /health                        состояние подключения\n"
        "  GET /items                         список известных тегов (JSON-массив)\n"
        "  GET /value?name=<ТЕГ>              значение тега (JSON)\n"
        "  GET /value/<ТЕГ>                   то же, имя в URL (можно кодировать %20 и т.п.)\n"
        "  GET /raw?name=<ТЕГ>                только значение (text/plain)\n"
        "  GET /values?name=A&name=B          несколько тегов одним запросом\n"
        "  GET /dump                          значения всех известных тегов\n"
        "  К любому запросу: ?refresh=1 - перед ответом дождаться обновления от сервера\n"
        "\n"
        "Примеры:\n"
        "  OpcDaToHttp.exe --server OPC.OpcTestServer_x64 --port 8080\n"
        "  curl \"http://localhost:8080/value?name=Test.Int32\"\n"
        "  curl \"http://localhost:8080/raw?name=Test.Float\"\n"
        "  curl \"http://localhost:8080/value/Test.String\"\n"
        "  OpcDaToHttp.exe --list-servers\n";

}
