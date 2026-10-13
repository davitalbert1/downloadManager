cmake_minimum_required(VERSION 3.16)

project(temperatura LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Garante que as ferramentas/DLLs do MinGW (moc, libQt6Core.dll, ...) sejam
# encontradas mesmo quando o cmake roda fora do shell MSYS2 (ex.: PowerShell/VS Code)
get_filename_component(_MINGW_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
if(WIN32)
    set(ENV{PATH} "${_MINGW_BIN};$ENV{PATH}")
endif()

# Os executáveis são gerados na raiz de build/ (build/clima_viewer.exe, build/download.exe)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# clima_viewer.exe - interface Qt 6 (opcional se Qt6 estiver instalado)
find_package(Qt6 COMPONENTS Core Gui Widgets Sql QUIET)
if(Qt6_FOUND)
    add_executable(clima_viewer WIN32 app/main.cpp)
    set_target_properties(clima_viewer PROPERTIES AUTOMOC ON)
    target_link_libraries(clima_viewer PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Sql)
else()
    message(STATUS "Qt6 nao encontrado. clima_viewer nao sera compilado via CMake.")
endif()

# era5_import.exe - importador de arquivos GRIB / ERA5 para SQLite
find_package(GDAL REQUIRED)

# download.exe - download de dados genericos/climaticos
find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

add_executable(download
    download/main.cpp
    download/utils.cpp
    download/config.cpp
    download/rate_limit.cpp
    download/storage_sqlite.cpp
    download/http_client.cpp
    download/json_parser.cpp
    download/file_dataset.cpp
    download/engine_generico.cpp
    download/era5_importer.cpp
    download/google_drive.cpp
    download/wikipedia_importer.cpp
)
target_include_directories(download PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
find_package(OpenSSL REQUIRED)
target_link_libraries(download PRIVATE CURL::libcurl SQLite3::SQLite3 GDAL::GDAL OpenSSL::SSL OpenSSL::Crypto Threads::Threads bz2)

# clima_auditor app/test.cpp
add_executable(clima_auditor app/test.cpp)
target_link_libraries(clima_auditor PRIVATE SQLite3::SQLite3)

# era5_import.exe - importador de arquivos GRIB / ERA5 para SQLite
find_package(GDAL REQUIRED)

add_executable(era5_import
    download/era5_main.cpp
    download/era5_importer.cpp
)
target_include_directories(era5_import PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(era5_import PRIVATE GDAL::GDAL SQLite3::SQLite3 Threads::Threads)

# nlohmann/json (header-only): usa o pacote CMake se existir,
# senão procura o include padrão nlohmann/json.hpp
find_package(nlohmann_json CONFIG QUIET)
if(nlohmann_json_FOUND)
    target_link_libraries(download PRIVATE nlohmann_json::nlohmann_json)
else()
    find_path(NLOHMANN_JSON_INCLUDE_DIR NAMES nlohmann/json.hpp)
    if(NOT NLOHMANN_JSON_INCLUDE_DIR)
        message(FATAL_ERROR "nlohmann/json.hpp nao encontrado. Instale mingw-w64-x86_64-nlohmann-json (MSYS2).")
    endif()
    target_include_directories(download PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
endif()

# db_manager.exe - visualizador grafico generico SQLite Dear ImGui + DirectX 11
add_executable(db_manager WIN32
    dbManager/main.cpp
    dbManager/db_backend.cpp
    dbManager/imgui/imgui.cpp
    dbManager/imgui/imgui_draw.cpp
    dbManager/imgui/imgui_tables.cpp
    dbManager/imgui/imgui_widgets.cpp
    dbManager/imgui/backends/imgui_impl_win32.cpp
    dbManager/imgui/backends/imgui_impl_dx11.cpp
)
target_include_directories(db_manager PRIVATE
    dbManager
    dbManager/imgui
    dbManager/imgui/backends
)
target_link_libraries(db_manager PRIVATE
    SQLite3::SQLite3
    d3d11
    dxgi
    d3dcompiler
    dwmapi
    comdlg32
    shell32
    ole32
)

# Copia executaveis para a raiz do projeto para conveniencia
add_custom_command(TARGET download POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:download> ${CMAKE_SOURCE_DIR}/download.exe
)
add_custom_command(TARGET era5_import POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:era5_import> ${CMAKE_SOURCE_DIR}/era5_import.exe
)
add_custom_command(TARGET clima_auditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:clima_auditor> ${CMAKE_SOURCE_DIR}/clima_auditor.exe
)
add_custom_command(TARGET db_manager POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:db_manager> ${CMAKE_SOURCE_DIR}/dbManager.exe
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:db_manager> ${CMAKE_SOURCE_DIR}/dbManager/dbManager.exe
)
