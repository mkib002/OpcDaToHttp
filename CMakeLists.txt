cmake_minimum_required(VERSION 3.15)

project(OpcDaToHttp LANGUAGES CXX)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# Каталог с исходниками OPCClientToolKit (по умолчанию ..\OPC-Client-X64\OPCClientToolKit).
# Переопределяется ключом:  cmake -DOPC_TOOLKIT_DIR=D:/libs/OPC-Client-X64/OPCClientToolKit
# ---------------------------------------------------------------------------
if (NOT OPC_TOOLKIT_DIR)
  set(OPC_TOOLKIT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../OPC-Client-X64/OPCClientToolKit")
endif()

if (NOT EXISTS "${OPC_TOOLKIT_DIR}/OPCClient.cpp")
  message(FATAL_ERROR
    "OPCClientToolKit не найден в '${OPC_TOOLKIT_DIR}'. "
    "Укажите путь ключом -DOPC_TOOLKIT_DIR=<каталог OPCClientToolKit>.")
endif()

if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(WARNING
    "Сборка не x64: 64-битный OPC-сервер, зарегистрированный только в 64-битной ветке реестра, "
    "будет недоступен. Для 32-битного сервера собирайте приложение под Win32.")
endif()

# Тулкит и приложение обязаны использовать один стандарт языка и один вариант CRT:
# через границу статической библиотеки передаются типы STL/ATL, поэтому смешивать
# Debug/Release и /std:c++NN нельзя.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Собираем тулкит статически прямо в нашем build-дереве: тогда рядом с exe
# не нужен OPCClientToolKit.dll и нет проблем с CRT через границу DLL.
set(OPCDACLIENT_STATIC ON CACHE BOOL "Собрать OPCClientToolKit как статическую библиотеку" FORCE)
set(OPCDACLIENT_TESTING OFF CACHE BOOL "Примеры тулкита не собираем" FORCE)

add_definitions(-DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS)
# Тулкит использует std::wstring_convert/codecvt_utf8 (deprecated в C++17) - глушим предупреждение.
add_definitions(-D_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)

add_subdirectory("${OPC_TOOLKIT_DIR}" "${CMAKE_BINARY_DIR}/OPCClientToolKit")

add_executable(OpcDaToHttp
    src/AppConfig.cpp
    src/HttpServer.cpp
    src/Logger.cpp
    src/OpcDaClient.cpp
    src/OpcValueFormatter.cpp
    src/StringUtil.cpp
    src/TagStore.cpp
    src/main.cpp
)

target_include_directories(OpcDaToHttp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${OPC_TOOLKIT_DIR}"
)

# Без этого заголовки тулкита объявляют классы как __declspec(dllimport) и линковка
# со статической библиотекой не пройдёт.
target_compile_definitions(OpcDaToHttp PRIVATE NO_OPCDACLIENT_LIBRARY)

target_link_libraries(OpcDaToHttp PRIVATE OPCClientToolKit ws2_32)

# Исходники приложения - в UTF-8 (кириллица в строках логов и справки).
# Тулкит остаётся на системной кодировке: его заголовки и строки - ASCII.
if (MSVC)
  target_compile_options(OpcDaToHttp PRIVATE /utf-8)
endif()

set_target_properties(OpcDaToHttp PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

install(TARGETS OpcDaToHttp RUNTIME DESTINATION bin)
