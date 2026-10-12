@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "GXX=D:\msys64\mingw64\bin\g++.exe"

if not exist "%GXX%" (
    for %%D in (A B C D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
        if exist "%%D:\msys64\mingw64\bin\g++.exe" set "GXX=%%D:\msys64\mingw64\bin\g++.exe"
        if exist "%%D:\mingw64\mingw64\bin\g++.exe"  set "GXX=%%D:\mingw64\mingw64\bin\g++.exe"
    )
)

if not exist "%GXX%" (
    echo ERRO: g++ do MinGW64 nao encontrado.
    exit /b 1
)

for %%I in ("%GXX%") do set "GXX_DIR=%%~dpI"
set "PATH=%GXX_DIR%;%PATH%"

echo Compilando dbManager.exe...
"%GXX%" -std=c++17 -O2 -Wall -Wno-unused-function -Wno-array-bounds -mwindows ^
    -I. -I./imgui -I./imgui/backends ^
    -o dbManager.exe ^
    main.cpp ^
    db_backend.cpp ^
    imgui/imgui.cpp ^
    imgui/imgui_draw.cpp ^
    imgui/imgui_tables.cpp ^
    imgui/imgui_widgets.cpp ^
    imgui/backends/imgui_impl_win32.cpp ^
    imgui/backends/imgui_impl_dx11.cpp ^
    -ld3d11 -ldxgi -ld3dcompiler -ldwmapi -lcomdlg32 -lshell32 -lole32 -lsqlite3

if errorlevel 1 (
    echo.
    echo Falha na compilacao do dbManager.exe!
    exit /b 1
)

echo.
echo Sucesso: dbManager.exe gerado com exito!
echo.
endlocal
