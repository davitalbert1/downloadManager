@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

echo ============================================================
echo   SISTEMA DE COMPILACAO AUTOMATICA VIA CMAKE
echo ============================================================

:: 1. Deteccao automatica do MinGW / GCC
set "MINGW_DIR="
if exist "D:\msys64\mingw64\bin\gcc.exe" set "MINGW_DIR=D:\msys64\mingw64\bin"
if not defined MINGW_DIR (
    if exist "C:\msys64\mingw64\bin\gcc.exe" set "MINGW_DIR=C:\msys64\mingw64\bin"
)
if not defined MINGW_DIR (
    for %%D in (C D E F G H) do (
        if not defined MINGW_DIR (
            if exist "%%D:\msys64\mingw64\bin\gcc.exe" set "MINGW_DIR=%%D:\msys64\mingw64\bin"
            if exist "%%D:\mingw64\bin\gcc.exe" set "MINGW_DIR=%%D:\mingw64\bin"
        )
    )
)

if defined MINGW_DIR (
    echo [DETECTADO] MinGW toolchain: !MINGW_DIR!
    set "PATH=!MINGW_DIR!;%PATH%"
) else (
    where gcc >nul 2>nul
    if errorlevel 1 (
        echo [ERRO] Compilador GCC / MinGW nao encontrado no sistema.
        exit /b 1
    )
    echo [DETECTADO] MinGW localizado no PATH do sistema.
)

:: 2. Deteccao automatica do CMake
set "CMAKE_EXE="
where cmake >nul 2>nul
if not errorlevel 1 (
    set "CMAKE_EXE=cmake"
) else (
    if defined MINGW_DIR (
        if exist "!MINGW_DIR!\cmake.exe" set "CMAKE_EXE=!MINGW_DIR!\cmake.exe"
    )
    if not defined CMAKE_EXE (
        for %%D in (C D E F) do (
            if not defined CMAKE_EXE (
                if exist "%%D:\msys64\mingw64\bin\cmake.exe" set "CMAKE_EXE=%%D:\msys64\mingw64\bin\cmake.exe"
                if exist "%%D:\Program Files\CMake\bin\cmake.exe" set "CMAKE_EXE=%%D:\Program Files\CMake\bin\cmake.exe"
            )
        )
    )
)

if not defined CMAKE_EXE (
    echo [ERRO] cmake.exe nao foi encontrado! Instale com 'pacman -S mingw-w64-x86_64-cmake' no MSYS2.
    exit /b 1
)
echo [DETECTADO] CMake: !CMAKE_EXE!

:: 3. Deteccao automatica do gerador de build (Ninja ou MinGW Makefiles)
set "CMAKE_GENERATOR="
where ninja >nul 2>nul
if not errorlevel 1 (
    set "CMAKE_GENERATOR=Ninja"
) else (
    if defined MINGW_DIR (
        if exist "!MINGW_DIR!\ninja.exe" set "CMAKE_GENERATOR=Ninja"
    )
)
if not defined CMAKE_GENERATOR (
    set "CMAKE_GENERATOR=MinGW Makefiles"
)
echo [DETECTADO] Gerador CMake: !CMAKE_GENERATOR!
echo.

:: 3.5. Verificacao da dependencia Dear ImGui (dbManager\imgui)
if not exist "dbManager\imgui\imgui.cpp" (
    echo [AVISO] Pasta dbManager\imgui nao encontrada ^(ou incompleta^).
    where git >nul 2>nul
    if errorlevel 1 (
        echo [ERRO] 'git' nao encontrado no sistema. Instale o Git para clonar o Dear ImGui.
        exit /b 1
    )
    echo [DETECTADO] Clonando repositorio Dear ImGui em dbManager\imgui ...
    git clone --depth 1 https://github.com/ocornut/imgui.git "dbManager\imgui"
    if errorlevel 1 (
        echo.
        echo [ERRO] Falha ao clonar o repositorio do Dear ImGui!
        exit /b 1
    )
    echo [OK] Dear ImGui clonado com sucesso.
    echo.
) else (
    echo [DETECTADO] Dear ImGui: dbManager\imgui
)

:: 4. Configuracao do Projeto via CMake
echo [1/2] Configurando projeto CMake (pasta build)...
if not exist "build" mkdir "build"

"!CMAKE_EXE!" -S . -B build -G "!CMAKE_GENERATOR!" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo.
    echo [ERRO] Falha ao configurar projeto com CMake.
    exit /b 1
)
echo.

:: 5. Compilacao de todos os alvos via CMake
echo [2/2] Compilando todos os executaveis via CMake...
"!CMAKE_EXE!" --build build --config Release
if errorlevel 1 (
    echo.
    echo [ERRO] Falha durante a compilacao dos executaveis!
    exit /b 1
)

echo.
echo ============================================================
echo  TODOS OS EXECUTAVEIS COMPILADOS COM SUCESSO VIA CMAKE!
echo ============================================================
echo   - download.exe       (Download generico, ERA5 e Google Drive)
echo   - era5_import.exe    (Importador GRIB ERA5 para SQLite)
echo   - dbManager.exe      (Interface grafica ImGui DirectX 11)
echo   - clima_auditor.exe  (Auditor e verificador SQLite)
if exist "build\clima_viewer.exe" (
echo   - clima_viewer.exe   (Interface Qt 6)
)
echo ============================================================
endlocal
