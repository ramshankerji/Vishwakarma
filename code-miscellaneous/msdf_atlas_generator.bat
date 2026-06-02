@echo off
SETLOCAL EnableDelayedExpansion

SET "CORE_DIR=%~dp0"
SET "ROOT_DIR=%CORE_DIR%.."
SET "BUILD_DIR=%ROOT_DIR%\build"
SET "EXTERNAL_DIR=%ROOT_DIR%\code-external\msdf-atlas-gen"
SET "FREETYPE_DIR=%ROOT_DIR%\code-external\freetype"
SET "ZLIB_DIR=%ROOT_DIR%\code-external\zlib"
SET "PNG_DIR=%ROOT_DIR%\code-external\libpng-code"
SET "FONT_FILE=%ROOT_DIR%\Fonts\NotoSans-VariableFont_wdth,wght.ttf"

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

:: Verify Tools
where cmake >nul 2>nul
if errorlevel 1 ( echo [ERROR] CMake is not recognized. & exit /b 1 )
where python >nul 2>nul
if errorlevel 1 ( echo [ERROR] Python is not recognized. & exit /b 1 )

:: 1A. Compile local ZLIB (Forced Static CRT)
if not exist "%ZLIB_DIR%\build\Release\zs.lib" (
    echo [Pre-Build] Compiling local ZLIB...
    cmake -S "%ZLIB_DIR%" -B "%ZLIB_DIR%\build" -A x64 -DCMAKE_BUILD_TYPE=Release ^
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
        -DBUILD_SHARED_LIBS=OFF
    if errorlevel 1 ( echo [ERROR] ZLIB Config failed. & exit /b 1 )
    cmake --build "%ZLIB_DIR%\build" --config Release
    if errorlevel 1 ( echo [ERROR] ZLIB Build failed. & exit /b 1 )
)

:: 1B. Compile local LibPNG (Forced Static CRT)
if not exist "%PNG_DIR%\build\Release\libpng16_static.lib" (
    echo [Pre-Build] Compiling local LibPNG...
    cmake -S "%PNG_DIR%" -B "%PNG_DIR%\build" -A x64 -DCMAKE_BUILD_TYPE=Release ^
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
        -DPNG_SHARED=OFF -DPNG_TESTS=OFF ^
        -DZLIB_INCLUDE_DIR="%ZLIB_DIR%;%ZLIB_DIR%\build" ^
        -DZLIB_LIBRARY="%ZLIB_DIR%\build\Release\zs.lib"
    if errorlevel 1 ( echo [ERROR] LibPNG Config failed. & exit /b 1 )
    cmake --build "%PNG_DIR%\build" --config Release
    if errorlevel 1 ( echo [ERROR] LibPNG Build failed. & exit /b 1 )
)

:: 1C. Compile local FreeType (Forced Static CRT)
if not exist "%FREETYPE_DIR%\build\Release\freetype.lib" (
    echo [Pre-Build] Compiling local FreeType...
    cmake -S "%FREETYPE_DIR%" -B "%FREETYPE_DIR%\build" -A x64 -DCMAKE_BUILD_TYPE=Release ^
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
        -DBUILD_SHARED_LIBS=OFF -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON ^
        -DFT_DISABLE_PNG=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON
    if errorlevel 1 ( echo [ERROR] FreeType Config failed. & exit /b 1 )
    cmake --build "%FREETYPE_DIR%\build" --config Release
    if errorlevel 1 ( echo [ERROR] FreeType Build failed. & exit /b 1 )
)

:: 1D. Compile msdf-atlas-gen (Forced Static CRT)
:: NOTE: Updated path to \bin\Release\
if not exist "%EXTERNAL_DIR%\build\bin\Release\msdf-atlas-gen.exe" (
    echo [Pre-Build] Compiling msdf-atlas-gen...
    
    if exist "%EXTERNAL_DIR%\build\CMakeCache.txt" del /F /Q "%EXTERNAL_DIR%\build\CMakeCache.txt"

    cmake -S "%EXTERNAL_DIR%" -B "%EXTERNAL_DIR%\build" -A x64 -DCMAKE_BUILD_TYPE=Release ^
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
        -DMSDF_ATLAS_USE_VCPKG=OFF ^
        -DMSDFGEN_USE_VCPKG=OFF ^
        -DMSDF_ATLAS_USE_SKIA=OFF ^
        -DMSDFGEN_USE_SKIA=OFF ^
        -DFREETYPE_INCLUDE_DIR_ft2build="%FREETYPE_DIR%\include" ^
        -DFREETYPE_INCLUDE_DIR_freetype2="%FREETYPE_DIR%\include" ^
        -DFREETYPE_LIBRARY="%FREETYPE_DIR%\build\Release\freetype.lib" ^
        -DZLIB_INCLUDE_DIR="%ZLIB_DIR%;%ZLIB_DIR%\build" ^
        -DZLIB_LIBRARY="%ZLIB_DIR%\build\Release\zs.lib" ^
        -DPNG_PNG_INCLUDE_DIR="%PNG_DIR%;%PNG_DIR%\build" ^
        -DPNG_LIBRARY="%PNG_DIR%\build\Release\libpng16_static.lib"
    
    if errorlevel 1 ( echo [ERROR] msdf-atlas-gen Config failed. & exit /b 1 )
    cmake --build "%EXTERNAL_DIR%\build" --config Release
    if errorlevel 1 ( echo [ERROR] msdf-atlas-gen Build failed. & exit /b 1 )
)

:: 2. Bake MSDF Atlas
:: NOTE: Updated path to \bin\Release\ and added -allglyphs
echo [Pre-Build] Baking MSDF Atlas for NotoSans...
"%EXTERNAL_DIR%\build\bin\Release\msdf-atlas-gen.exe" ^
    -font "%FONT_FILE%" ^
    -allglyphs ^
    -type msdf ^
    -format png ^
    -imageout "%BUILD_DIR%\NotoSansMSDF.png" ^
    -json "%BUILD_DIR%\NotoSansMSDF.json" ^
    -size 32 ^
    -pxrange 4

if errorlevel 1 (
    echo [ERROR] msdf-atlas-gen failed to generate the atlas.
    exit /b 1
)

:: 3. Convert JSON and PNG directly into a C++ Header
echo [Pre-Build] Converting to C++ Header...
python "%ROOT_DIR%\code-miscellaneous\msdf_atlas_json_parser.py" ^
    "%BUILD_DIR%\NotoSansMSDF.png" ^
    "%BUILD_DIR%\NotoSansMSDF.json" ^
    "%BUILD_DIR%\NotoSansMSDF_Compiled.h"

if errorlevel 1 ( echo [ERROR] Python conversion script failed. & exit /b 1 )

echo [Pre-Build] MSDF Generation Complete!
exit /b 0
