# OpcDaToHttp

Мост **OPC DA → HTTP**: приложение читает значения тегов OPC DA сервера и отдаёт их по HTTP по имени параметра. OPC-часть построена на библиотеке **OPCClientToolKit**, HTTP-часть на Winsock.

## Требования

* Windows, MSVC (использовалась Visual Studio 2019 / MSVC 14.29) + CMake.
* **OPC Core Components** нужной разрядности — стандартный пакет распространяемых библиотек и служб от фонда OPC Foundation (https://opcfoundation.org/developer-tools/samples-and-tools-classic/core-components/) необходимых для работы всех приложений OPC Classic. Установленные файлы: `C:\Program Files\Common Files\OPC Foundation\Bin\`.
* OPC DA сервер. Для проверки подойдёт например «OPC Server Simulator AggSoft» (https://www.aggsoft.ru/opc-server-sim/index.htm). COM запускает его сам при подключении.
* Для **удалённого** хоста (`--host <имя>`) дополнительно нужны служба `OpcEnum`, служба `RemoteRegistry` и настройки DCOM на серверной машине.

## Запуск

```bat
OpcDaToHttp.exe --server OPC.OpcTestServer_x64.1 --port 8080
OpcDaToHttp.exe                                  :: первый найденный локальный DA-сервер
OpcDaToHttp.exe --list-servers                   :: список локальных OPC DA 2.0 серверов
OpcDaToHttp.exe --help
```

| Ключ | По умолчанию | Назначение |
|---|---|---|
| `--server <ProgID>` | первый найденный | ProgID OPC DA сервера (например `OPC.OpcTestServer_x64.1`) |
| `--host <имя>` | `localhost` | хост; `''`, `localhost`, `127.0.0.1` → задаёт машину, на которой работает OPC DA сервер |
| `--port <N>` | `8080` | порт HTTP; `0` — любой свободный |
| `--rate <мс>` | `1000` | период обновления OPC-группы |
| `--deadband <число>` | `0` | зона нечувствительности группы |
| `--refresh-sec <сек>` | `0` | дополнительный периодический refresh группы (0 — только подписка) |
| `--items-file <файл>` | — | список тегов построчно (как `OPCPerformance\items.txt`) вместо обзора namespace |
| `--filter <подстрока>` | — | брать из namespace только имена с подстрокой |
| `--max-items <N>` | `0` | ограничение числа тегов |
| `--reconnect-sec <сек>` | `10` | повтор подключения при обрыве (0 — не переподключаться) |
| `--max-conn <N>` | `64` | предел одновременных HTTP-соединений |
| `--stale-ms <мс>` | `0` (авто) | порог «устаревания» значения |
| `--timeout-ms <мс>` | `5000` | ожидание refresh для `?refresh=1` |
| `--log-level <уровень>` | `info` | `debug`, `info`, `warn`, `error` |
| `--list-servers` | — | вывести локальные DA-серверы и выйти |

Останов — `Ctrl+C` (HTTP-сервер закрывается, OPC-сессия освобождается корректно).

## HTTP API

| Запрос | Ответ |
|---|---|
| `GET /` | `text/plain` — справка и текущее состояние |
| `GET /health` | JSON-состояние; `503`, если нет связи с OPC |
| `GET /items` | JSON: `{"count":N,"items":[...]}` — известные теги |
| `GET /value?name=<ТЕГ>` | JSON-значение тега |
| `GET /value/<ТЕГ>` | то же, имя в пути (можно URL-кодировать: `%2F`, `%20`) |
| `GET /raw?name=<ТЕГ>` | только значение, `text/plain` (детали в заголовках `X-Opc-*`) |
| `GET /values?name=A&name=B` | несколько тегов одним запросом |
| `GET /dump` | значения всех известных тегов |

Параметр `?refresh=1` (для `/value`, `/raw`, `/values`) заставляет OPC-поток выполнить `refresh` и дождаться его завершения — HTTP-поток при этом данных от OPC не читает.

Пример ответа `/value`:

```json
{"name":"Test/Int32","value":42,"type":"VT_I4","quality":192,"qualityText":"Good",
 "timestamp":"2026-09-24T17:21:46.937Z","ageMs":11531,"stale":false,"hasValue":true,"error":null}
```

Поля: `value` — готовый JSON-литерал (число, строка, `true/false`, `null`, массив; массив байт —
hex-строка); `type` — тип OPC (`VT_R4`, `VT_ARRAY|VT_UI2`, …); `quality`/`qualityText` — качество
OPC; `timestamp` — время значения от сервера (UTC, ISO-8601); `ageMs` — сколько мс назад значение
получено от сервера; `stale` — превышен порог `--stale-ms`; `hasValue` — чтение без ошибки;
`error` — `HRESULT` ошибки или `null`. Для массивов добавляются `length` и `truncated`
(в ответ попадает не более 1024 элементов).

Пример `/health`:

```json
{"status":"ok","opcConnected":true,"server":"OPC.OpcTestServer_x64.1","host":"localhost","port":8080,
 "serverState":1,"tags":3,"values":3,"items":3,"lastUpdateAgeMs":11515,"serverContactAgeMs":1500,
 "snapshotVersion":6,"refreshAckId":0,"uptimeSec":11,"lastError":null,"connections":1,"version":"1.0.0"}
```

`status`: `ok` (есть связь и значения) / `degraded` (связь есть, значений пока нет) /
`down` (связи нет, HTTP 503).

Коды ответов:

| Код | Когда |
|---|---|
| `200` | успех |
| `400` | нет параметра `name` / тело запроса не поддерживается |
| `404` | тег не найден (в теле — `suggestions` с похожими именами) |
| `405` | метод не GET/HEAD |
| `431` | слишком большой блок заголовков |
| `503` | нет связи с OPC (или превышен `--max-conn`) |
| `504` | `?refresh=1` не дождался refresh за `--timeout-ms` |

Имена тегов ищутся так: точное совпадение → регистронезависимо → замена разделителя (`Test/Int32` ↔ `Test.Int32`). Имя можно передавать в query (`?name=`) или в пути (`/value/...`), в URL-кодированном виде. Соединения: HTTP/1.1 keep-alive (до 100 запросов на соединение), `Connection: close` клиента уважается, таймаут чтения 5 с.

## Ограничения

* Значения отдаются из серверного кэша, наполняемого подпиской: частота — период группы
  (`--rate`). Для «снимка на момент запроса» используйте `?refresh=1`.
* Обзор тегов OPC — для серверов, у которых теги лежат в ветках, например (`Mera.Recorder.1`). Если сервер не поддерживает обзор, задайте список тегов ключом `--items-file`.
* Для 32-бит OPC сервера, рекомендуется использовать 32-бит OpcDaToHttp, тоже для 64-бит.
* Аутентификации и HTTPS нет, рекомендуется использовать в локальной сети.
