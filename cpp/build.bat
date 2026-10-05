@echo off
rem ---------------------------------------------------------------
rem  ETS2 Trainer - C++ / Dear ImGui / Direct3D 11 build script
rem    build.bat            full build, output ..\ETS2Trainer.exe
rem    build.bat check      compile only, no linking
rem    build.bat test       build non-elevated test exe (for self tests)
rem    build.bat readable   build ..\ETS2Trainer_readable_export.exe
rem    build.bat clean      remove build dir and exe
rem  NOTE keep this file ASCII only (cmd parses it with the OEM page)
rem  NOTE do not put %VCVARS% inside "( )" blocks: the (x86) paren would
rem       end the block early. This script uses goto instead.
rem  NOTE two stage build with /MP: compiling all files in ONE cl process
rem       accumulates heap and can hit "fatal error C1060".
rem ---------------------------------------------------------------
setlocal
cd /d "%~dp0"

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" goto no_vcvars
call "%VCVARS%" >nul 2>&1
where cl >nul 2>&1
if errorlevel 1 goto no_cl

if not exist build mkdir build

set "IMGUI=vendor\imgui-1.92.9"
set "IMGUI_SRC=%IMGUI%\imgui.cpp %IMGUI%\imgui_draw.cpp %IMGUI%\imgui_tables.cpp %IMGUI%\imgui_widgets.cpp %IMGUI%\backends\imgui_impl_win32.cpp %IMGUI%\backends\imgui_impl_dx11.cpp"
set "INCS=/I src /I %IMGUI% /I %IMGUI%\backends"
set "FLAGS=/nologo /std:c++17 /O2 /MT /EHsc /W3 /wd4100 /utf-8 /DNDEBUG /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX"
set "LIBS=d3d11.lib dxgi.lib bcrypt.lib user32.lib gdi32.lib shell32.lib ole32.lib advapi32.lib dwmapi.lib"

if /i "%~1"=="check" goto check
if /i "%~1"=="clean" goto clean
if /i "%~1"=="test" goto testbuild
set "OUTPUT=..\ETS2Trainer.exe"
if /i "%~1"=="next" set "OUTPUT=..\ETS2Trainer_next.exe"
if /i "%~1"=="readable" set "OUTPUT=..\ETS2Trainer_readable_export.exe"

del /q build\*.obj >nul 2>&1
echo [build] 1/2 compiling with /MP ...
cl %FLAGS% /MP4 %INCS% /c src\*.cpp %IMGUI_SRC% /Fo:build\ > build\compile.log 2>&1
if errorlevel 1 goto build_fail
echo [build] 2/2 linking ...
link /nologo /OUT:"%OUTPUT%" /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTUAC:NO /MANIFESTINPUT:app.manifest build\*.obj %LIBS% > build\link.log 2>&1
if errorlevel 1 goto link_fail
echo [build] OK, output: %OUTPUT%
exit /b 0

:check
echo [check] compiling only ...
cl %FLAGS% /MP4 %INCS% /c src\*.cpp %IMGUI_SRC% /Fo:build\ > build\check.log 2>&1
if errorlevel 1 goto check_fail
echo [check] PASSED, no errors
exit /b 0

:check_fail
echo [check] FAILED, error lines:
findstr /i "error C" build\check.log
exit /b 1

:testbuild
if not exist build\test mkdir build\test
del /q build\test\*.obj >nul 2>&1
echo [test] 1/2 compiling with /MP ...
cl %FLAGS% /MP4 %INCS% /c src\*.cpp %IMGUI_SRC% /Fo:build\test\ > build\test_compile.log 2>&1
if errorlevel 1 goto test_fail
echo [test] 2/2 linking ...
link /nologo /OUT:"build\ETS2Trainer_test.exe" /SUBSYSTEM:WINDOWS build\test\*.obj %LIBS% > build\test_link.log 2>&1
if errorlevel 1 goto test_fail
echo [test] OK, output: build\ETS2Trainer_test.exe
exit /b 0

:test_fail
echo [test] FAILED, error lines:
findstr /i "error" build\test_compile.log
findstr /i "error" build\test_link.log
exit /b 1

:build_fail
echo [build] FAILED, error lines:
findstr /i "error C" build\compile.log
exit /b 1

:link_fail
echo [build] LINK FAILED, error lines:
findstr /i "error" build\link.log
exit /b 1

:clean
if exist build rmdir /s /q build
if exist ..\ETS2Trainer.exe del ..\ETS2Trainer.exe
echo [build] cleaned
exit /b 0

:no_vcvars
echo [build] vcvars64.bat not found. Expected path:
echo   %VCVARS%
echo [build] please edit VCVARS in this script.
exit /b 1

:no_cl
echo [build] cl.exe not available after vcvars.
exit /b 1
