# OpcDaToHttp

An **OPC DA → HTTP** bridge: the application reads tag values from an OPC DA server and serves them over HTTP by parameter name. The OPC part is built with **OPCClientToolKit** library (https://github.com/mkib002/OPC-Client-X64).

## Requirements

* Windows, MSVC (Visual Studio 2019 / MSVC 14.29 was used) + CMake.
* **OPC Core Components** of the matching bitness — the standard redistributable package of libraries and services from the OPC Foundation (https://opcfoundation.org/developer-tools/samples-and-tools-classic/core-components/) required by all OPC Classic applications. Installed files: `C:\Program Files\Common Files\OPC Foundation\Bin\`.
* An OPC DA server. For testing, "OPC Server Simulator AggSoft" (https://www.aggsoft.ru/opc-server-sim/index.htm) will do. COM launches it automatically on connect.
* For a **remote** host (`--host <name>`) you additionally need the `OpcEnum` service, the `RemoteRegistry` service, and DCOM settings on the server machine.

## Usage

```bat
OpcDaToHttp.exe --server OPC.OpcTestServer_x64.1 --port 8080
OpcDaToHttp.exe                                  :: first local DA server found
OpcDaToHttp.exe --list-servers                   :: list local OPC DA 2.0 servers
OpcDaToHttp.exe --help
```

| Option | Default | Purpose |
|---|---|---|
| `--server <ProgID>` | first found | ProgID of the OPC DA server (e.g. `OPC.OpcTestServer_x64.1`) |
| `--host <name>` | `localhost` | host; `''`, `localhost`, `127.0.0.1` → selects the machine that runs the OPC DA server |
| `--port <N>` | `8080` | HTTP port; `0` — any free port |
| `--rate <ms>` | `1000` | OPC group update period |
| `--deadband <value>` | `0` | group deadband |
| `--refresh-sec <sec>` | `0` | extra periodic group refresh (0 — subscription only) |
| `--items-file <file>` | — | newline-separated tag list (like `OPCPerformance\items.txt`) instead of browsing the namespace |
| `--filter <substring>` | — | take only namespace names containing the substring |
| `--max-items <N>` | `0` | limit the number of tags |
| `--reconnect-sec <sec>` | `10` | reconnect on disconnect (0 — do not reconnect) |
| `--max-conn <N>` | `64` | limit of simultaneous HTTP connections |
| `--stale-ms <ms>` | `0` (auto) | value "staleness" threshold |
| `--timeout-ms <ms>` | `5000` | wait for refresh for `?refresh=1` |
| `--log-level <level>` | `info` | `debug`, `info`, `warn`, `error` |
| `--list-servers` | — | print local DA servers and exit |

Stop with `Ctrl+C` (the HTTP server shuts down and the OPC session is released cleanly).

## HTTP API

| Request | Response |
|---|---|
| `GET /` | `text/plain` — help and current state |
| `GET /health` | JSON state; `503` if there is no connection to OPC |
| `GET /items` | JSON: `{"count":N,"items":[...]}` — known tags |
| `GET /value?name=<TAG>` | JSON value of the tag |
| `GET /value/<TAG>` | same, name in the path (can be URL-encoded: `%2F`, `%20`) |
| `GET /raw?name=<TAG>` | value only, `text/plain` (details in the `X-Opc-*` headers) |
| `GET /values?name=A&name=B` | several tags in one request |
| `GET /dump` | values of all known tags |

The `?refresh=1` parameter (for `/value`, `/raw`, `/values`) makes the OPC thread perform a `refresh` and wait for it to complete — the HTTP thread does not read data from OPC in the meantime.

Example `/value` response:

```json
{"name":"Test/Int32","value":42,"type":"VT_I4","quality":192,"qualityText":"Good",
 "timestamp":"2026-09-24T17:21:46.937Z","ageMs":11531,"stale":false,"hasValue":true,"error":null}
```

Fields: `value` — a ready-made JSON literal (number, string, `true/false`, `null`, array; a byte array is a
hex string); `type` — the OPC type (`VT_R4`, `VT_ARRAY|VT_UI2`, …); `quality`/`qualityText` — OPC
quality; `timestamp` — value time from the server (UTC, ISO-8601); `ageMs` — how many ms ago the value
was received from the server; `stale` — the `--stale-ms` threshold was exceeded; `hasValue` — read
without an error; `error` — the `HRESULT` of the error or `null`. For arrays, `length` and `truncated`
are added (no more than 1024 elements are included in the response).

Example `/health`:

```json
{"status":"ok","opcConnected":true,"server":"OPC.OpcTestServer_x64.1","host":"localhost","port":8080,
 "serverState":1,"tags":3,"values":3,"items":3,"lastUpdateAgeMs":11515,"serverContactAgeMs":1500,
 "snapshotVersion":6,"refreshAckId":0,"uptimeSec":11,"lastError":null,"connections":1,"version":"1.0.0"}
```

`status`: `ok` (connected and values present) / `degraded` (connected, but no values yet) /
`down` (no connection, HTTP 503).

Response codes:

| Code | When |
|---|---|
| `200` | success |
| `400` | missing `name` parameter / request body not supported |
| `404` | tag not found (`suggestions` with similar names in the body) |
| `405` | method is not GET/HEAD |
| `431` | header block too large |
| `503` | no connection to OPC (or `--max-conn` exceeded) |
| `504` | `?refresh=1` did not get a refresh within `--timeout-ms` |

Tag names are looked up as follows: exact match → case-insensitive → separator substitution (`Test/Int32` ↔ `Test.Int32`). The name can be passed in the query (`?name=`) or in the path (`/value/...`), URL-encoded. Connections: HTTP/1.1 keep-alive (up to 100 requests per connection), the client's `Connection: close` is honored, read timeout is 5 s.

## Limits

* Values are served from the server-side cache filled by the subscription: the rate is the group period
  (`--rate`). For a "snapshot as of the request", use `?refresh=1`.
* OPC tag browsing is for servers whose tags live in branches, e.g. (`Mera.Recorder.1`). If the server does not support browsing, set the tag list with the `--items-file` option.
* For a 32-bit OPC server, it is recommended to use the 32-bit OpcDaToHttp, and likewise for 64-bit.
* There is no authentication or HTTPS; it is recommended to use it on a local network.
