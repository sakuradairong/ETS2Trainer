@echo off
setlocal
cd /d "%~dp0"

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo [hook] vcvars64.bat not found.
    exit /b 1
)
call "%VCVARS%" >nul 2>&1

if not exist build\hook mkdir build\hook

set "IMGUI=vendor\imgui-1.92.9"
set "IMGUI_SRC=%IMGUI%\imgui.cpp %IMGUI%\imgui_draw.cpp %IMGUI%\imgui_tables.cpp %IMGUI%\imgui_widgets.cpp %IMGUI%\backends\imgui_impl_win32.cpp %IMGUI%\backends\imgui_impl_dx11.cpp"
set "INCS=/I %IMGUI% /I %IMGUI%\backends"
set "FLAGS=/nologo /std:c++17 /O2 /MT /EHsc /W3 /wd4100 /utf-8 /DNDEBUG /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX"
set "LIBS=d3d11.lib dxgi.lib user32.lib gdi32.lib"

del /q build\hook\*.obj >nul 2>&1
echo [hook] 1/2 compiling ...
cl %FLAGS% /MP4 %INCS% /c hook\hook_main.cpp %IMGUI_SRC% /Fo:build\hook\ > build\hook_compile.log 2>&1
if errorlevel 1 (
    echo [hook] FAILED compile:
    findstr /i "error C" build\hook_compile.log
    exit /b 1
)

echo [hook] 2/2 linking DLL ...
link /nologo /DLL /OUT:"..\ETS2TrainerHook.dll" build\hook\*.obj %LIBS% > build\hook_link.log 2>&1
if errorlevel 1 (
    echo [hook] FAILED link:
    findstr /i "error" build\hook_link.log
    exit /b 1
)

echo [hook] OK, output: ..\ETS2TrainerHook.dll
exit /b 0
