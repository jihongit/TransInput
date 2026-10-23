@echo off
setlocal
set "TI_ARCH=%~1"
if not "%TI_ARCH%"=="x64" if not "%TI_ARCH%"=="x86" exit /b 2
set "TI_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%TI_VSWHERE%" (
  echo Visual Studio C++ Build Tools are required.
  exit /b 3
)
for /f "usebackq delims=" %%I in (`"%TI_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "TI_VS=%%I"
if not defined TI_VS exit /b 3
call "%TI_VS%\VC\Auxiliary\Build\vcvarsall.bat" %TI_ARCH% >nul
if errorlevel 1 exit /b %errorlevel%
if "%~2"=="cargo" goto cargo
if "%~2"=="live" goto live
if "%~2"=="native" goto native
exit /b 2

:cargo
set "CARGO_HOME=%~dp0..\.tools\cargo"
set "RUSTUP_HOME=%~dp0..\.tools\rustup"
set "RUSTUP_TOOLCHAIN=stable"
if exist "%~dp0..\.tools\toolchain-name.txt" set /p RUSTUP_TOOLCHAIN=<"%~dp0..\.tools\toolchain-name.txt"
set "PATH=%CARGO_HOME%\bin;%PATH%"
set "TI_TARGET="
if "%TI_ARCH%"=="x86" set "TI_TARGET=--target i686-pc-windows-msvc"
if not exist "%CARGO_HOME%\bin\cargo.exe" exit /b 4
for /f "tokens=2" %%V in ('rustc --version') do if not "%%V"=="1.99.1" (
  echo Expected Rust 1.99.1. Run scripts\bootstrap-rust.ps1.
  exit /b 5
)
if "%~3"=="test" cargo test --workspace --all-targets %TI_TARGET%
if "%~3"=="build" cargo build --workspace --release %TI_TARGET%
if "%~3"=="check" cargo clippy --workspace --all-targets %TI_TARGET% -- -D warnings
if "%~3"=="fmt" cargo fmt --all -- --check
if "%~3"=="format" cargo fmt --all
exit /b %errorlevel%

:live
set "TI_OUT=%~dp0..\build\%TI_ARCH%"
if not exist "%TI_OUT%" mkdir "%TI_OUT%"
cl /nologo /std:c++20 /utf-8 /EHsc /W4 /WX /MT /DUNICODE /D_UNICODE /I"%~dp0..\bindings" "%~dp0..\platforms\windows\tests\deepseek_live.cpp" /Fo"%TI_OUT%\deepseek-live.obj" /Fe"%TI_OUT%\deepseek-live.exe" /link windowsapp.lib winhttp.lib advapi32.lib crypt32.lib bcrypt.lib shell32.lib ole32.lib
exit /b %errorlevel%

:native
set "TI_OUT=%~dp0..\build\%TI_ARCH%"
if not exist "%TI_OUT%" mkdir "%TI_OUT%"
set "TI_CORE=%~dp0..\target\release\transinput_core.dll"
if "%TI_ARCH%"=="x86" set "TI_CORE=%~dp0..\target\i686-pc-windows-msvc\release\transinput_core.dll"
if not exist "%TI_CORE%" exit /b 6
copy /y "%TI_CORE%" "%TI_OUT%\transinput_core.dll" >nul
if errorlevel 1 exit /b %errorlevel%
call :build_resources
if errorlevel 1 exit /b %errorlevel%
if "%~3"=="translation" goto native_tests
if "%~3"=="learning" goto native_learning
if "%~3"=="tsf" goto native_tsf
if "%~3"=="editor" goto native_editor
if "%~3"=="integration" goto native_integration
if "%~3"=="delivery" goto native_delivery
call :build_preflight
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++20 /utf-8 /EHsc /W4 /WX /MT /DUNICODE /D_UNICODE /I"%~dp0..\bindings" "%~dp0..\platforms\windows\broker\main.cpp" /Fo"%TI_OUT%\broker.obj" /Fe"%TI_OUT%\TransInputBroker.exe" "%TI_OUT%\product-app.res" /link advapi32.lib crypt32.lib shell32.lib ole32.lib
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++20 /utf-8 /EHsc /W4 /WX /MT /DUNICODE /D_UNICODE /I"%~dp0..\bindings" "%~dp0..\platforms\windows\tests\broker_smoke.cpp" /Fo"%TI_OUT%\broker-smoke.obj" /Fe"%TI_OUT%\broker-smoke.exe" /link advapi32.lib user32.lib
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++20 /utf-8 /EHsc /W4 /WX /MT /DUNICODE /D_UNICODE /I"%~dp0..\bindings" "%~dp0..\platforms\windows\translator\main.cpp" /Fo"%TI_OUT%\translator.obj" /Fe"%TI_OUT%\TransInputTranslator.exe" "%TI_OUT%\product-app.res" /link windowsapp.lib winhttp.lib advapi32.lib crypt32.lib bcrypt.lib shell32.lib ole32.lib
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++20 /utf-8 /EHsc /W4 /WX /MT /DUNICODE /D_UNICODE /I"%~dp0..\bindings" "%~dp0..\platforms\windows\settings\main.cpp" /Fo"%TI_OUT%\settings.obj" /Fe"%TI_OUT%\TransInputSettings.exe" "%TI_OUT%\product-app.res" /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:"%~dp0..\platforms\windows\settings\settings.manifest" windowsapp.lib winhttp.lib advapi32.lib crypt32.lib bcrypt.lib shell32.lib ole32.lib user32.lib gdi32.lib gdiplus.lib
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++20 /utf-8 /EHsc /W4 /WX /MT /DUNICODE /D_UNICODE /DTI_SETTINGS_FIXTURE /I"%~dp0..\bindings" "%~dp0..\platforms\windows\settings\main.cpp" /Fo"%TI_OUT%\settings-test.obj" /Fe"%TI_OUT%\TransInputSettingsTest.exe" /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:"%~dp0..\platforms\windows\settings\settings.manifest" windowsapp.lib winhttp.lib advapi32.lib crypt32.lib bcrypt.lib shell32.lib ole32.lib user32.lib gdi32.lib gdiplus.lib
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++20 /utf-8 /EHsc /W4 /WX /MT /DUNICODE /D_UNICODE /I"%~dp0..\bindings" "%~dp0..\platforms\windows\tests\words_smoke.cpp" /Fo"%TI_OUT%\words-smoke.obj" /Fe"%TI_OUT%\words-smoke.exe" /link advapi32.lib crypt32.lib shell32.lib ole32.lib
if errorlevel 1 exit /b %errorlevel%
copy /y "%~dp0..\assets\branding\original\transinput-icon.png" "%TI_OUT%\transinput-icon.png" >nul
if errorlevel 1 exit /b %errorlevel%
set "TI_SHARED=%~dp0..\build\shared"
if not exist "%TI_SHARED%" mkdir "%TI_SHARED%"
if "%TI_ARCH%"=="x64" goto shared
if not exist "%TI_SHARED%\TransInputBroker.exe" goto shared
goto native_tests
:shared
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0copy-shared-broker.ps1" -NativeDirectory "%TI_OUT%" -CorePath "%TI_CORE%"
if errorlevel 1 exit /b %errorlevel%
:native_tests
if "%~3"=="words" exit /b 0
cl /nologo /std:c++20 /utf-8 /EHsc /W4 /WX /MT /DUNICODE /D_UNICODE /DTI_HTTP_TEST /I"%~dp0..\bindings" "%~dp0..\platforms\windows\tests\translation_smoke.cpp" /Fo"%TI_OUT%\translation-smoke.obj" /Fe"%TI_OUT%\translation-smoke.exe" /link windowsapp.lib winhttp.lib ws2_32.lib advapi32.lib crypt32.lib bcrypt.lib shell32.lib ole32.lib
if errorlevel 1 exit /b %errorlevel%
if "%~3"=="translation" exit /b 0
:native_learning
cl /nologo /std:c++20 /utf-8 /EHsc /W4 /WX /MT /DUNICODE /D_UNICODE /I"%~dp0..\bindings" "%~dp0..\platforms\windows\tests\learning_smoke.cpp" /Fo"%TI_OUT%\learning-smoke.obj" /Fe"%TI_OUT%\learning-smoke.exe" /link advapi32.lib crypt32.lib shell32.lib ole32.lib
if errorlevel 1 exit /b %errorlevel%
if "%~3"=="learning" exit /b 0
cl /nologo /std:c++20 /utf-8 /EHsc /W4 /WX /MT /DUNICODE /D_UNICODE /I"%~dp0..\bindings" "%~dp0..\platforms\windows\tests\translator_process.cpp" /Fo"%TI_OUT%\translator-process.obj" /Fe"%TI_OUT%\translator-process.exe" /link advapi32.lib user32.lib
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++20 /utf-8 /EHsc /W4 /WX /MT /DUNICODE /D_UNICODE /I"%~dp0..\bindings" "%~dp0..\platforms\windows\tests\translation_fixture.cpp" /Fo"%TI_OUT%\translation-fixture.obj" /Fe"%TI_OUT%\TransInputTranslationFixture.exe" /link advapi32.lib
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++20 /utf-8 /EHsc /W4 /WX /MT /DUNICODE /D_UNICODE /I"%~dp0..\bindings" "%~dp0..\platforms\windows\tests\abi_smoke.cpp" /Fo"%TI_OUT%\abi_smoke.obj" /Fe"%TI_OUT%\abi-smoke.exe" /link user32.lib
if errorlevel 1 exit /b %errorlevel%
:native_tsf
cl /nologo /std:c++20 /utf-8 /EHsc /W4 /WX /MT /DUNICODE /D_UNICODE /DTI_TEST_DIAGNOSTICS /I"%~dp0..\bindings" /LD "%~dp0..\platforms\windows\tsf\module.cpp" /Fo"%TI_OUT%\tsf-module.obj" /Fe"%TI_OUT%\TransInputPrototype.dll" "%TI_OUT%\product-tip.res" /link /DEF:"%~dp0..\platforms\windows\tsf\exports.def" user32.lib gdi32.lib ole32.lib oleaut32.lib uuid.lib advapi32.lib bcrypt.lib uiautomationcore.lib
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++20 /utf-8 /EHsc /W4 /WX /MT /DUNICODE /D_UNICODE /DTI_TEST_DIAGNOSTICS /DTI_TRANSLATION_FIXTURE /I"%~dp0..\bindings" /LD "%~dp0..\platforms\windows\tsf\module.cpp" /Fo"%TI_OUT%\tsf-test-module.obj" /Fe"%TI_OUT%\TransInputPrototypeTest.dll" "%TI_OUT%\product-tip.res" /link /DEF:"%~dp0..\platforms\windows\tsf\exports.test.def" user32.lib gdi32.lib gdiplus.lib ole32.lib oleaut32.lib uuid.lib advapi32.lib bcrypt.lib uiautomationcore.lib
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++20 /utf-8 /EHsc /W4 /WX /MT /DUNICODE /D_UNICODE "%~dp0..\platforms\windows\tests\tsf_lifecycle.cpp" /Fo"%TI_OUT%\tsf-lifecycle.obj" /Fe"%TI_OUT%\tsf-lifecycle.exe" /link ole32.lib uuid.lib user32.lib
if errorlevel 1 exit /b %errorlevel%
:native_integration
cl /nologo /std:c++20 /utf-8 /EHsc /W4 /WX /MT /DUNICODE /D_UNICODE /I"%~dp0..\bindings" "%~dp0..\platforms\windows\tests\tsf_integration.cpp" /Fo"%TI_OUT%\tsf-integration.obj" /Fe"%TI_OUT%\tsf-integration.exe" /link ole32.lib oleaut32.lib uuid.lib user32.lib advapi32.lib
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++20 /utf-8 /EHsc /W4 /WX /MT /DUNICODE /D_UNICODE "%~dp0..\platforms\windows\tests\ui_element_smoke.cpp" /Fo"%TI_OUT%\ui-element-smoke.obj" /Fe"%TI_OUT%\ui-element-smoke.exe" /link ole32.lib oleaut32.lib uuid.lib user32.lib
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++20 /utf-8 /EHsc /W4 /WX /MT /DUNICODE /D_UNICODE "%~dp0..\platforms\windows\tests\accessibility_smoke.cpp" /Fo"%TI_OUT%\accessibility-smoke.obj" /Fe"%TI_OUT%\accessibility-smoke.exe" /link ole32.lib oleaut32.lib uuid.lib user32.lib uiautomationcore.lib
if errorlevel 1 exit /b %errorlevel%
if "%~3"=="integration" exit /b 0
cl /nologo /std:c++20 /utf-8 /EHsc /W4 /WX /MT /DUNICODE /D_UNICODE /I"%~dp0..\bindings" "%~dp0..\platforms\windows\tests\candidate_preview.cpp" /Fo"%TI_OUT%\candidate-preview.obj" /Fe"%TI_OUT%\candidate-preview.exe" /link ole32.lib uuid.lib
if errorlevel 1 exit /b %errorlevel%
:native_editor
cl /nologo /std:c++20 /utf-8 /EHsc /W4 /WX /MT /DUNICODE /D_UNICODE /I"%~dp0..\bindings" "%~dp0..\platforms\windows\tests\input_pad.cpp" /Fo"%TI_OUT%\input-pad.obj" /Fe"%TI_OUT%\TransInputTestHost.exe" /link /SUBSYSTEM:WINDOWS ole32.lib oleaut32.lib uuid.lib user32.lib advapi32.lib gdi32.lib shell32.lib
if errorlevel 1 exit /b %errorlevel%
exit /b 0

:native_delivery
call :build_preflight
exit /b %errorlevel%
:build_preflight
cl /nologo /std:c++20 /utf-8 /EHsc /W4 /WX /MT /DUNICODE /D_UNICODE "%~dp0..\platforms\windows\setup\preflight.cpp" /Fo"%TI_OUT%\preflight.obj" /Fe"%TI_OUT%\TransInputPreflight.exe" "%TI_OUT%\product-app.res" /link wintrust.lib crypt32.lib advapi32.lib
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++20 /utf-8 /EHsc /W4 /WX /MT /DUNICODE /D_UNICODE "%~dp0..\platforms\windows\setup\register.cpp" /Fo"%TI_OUT%\register.obj" /Fe"%TI_OUT%\TransInputRegister.exe" "%TI_OUT%\product-app.res" /link wintrust.lib crypt32.lib advapi32.lib ole32.lib uuid.lib shell32.lib version.lib
exit /b %errorlevel%

:build_resources
if not exist "%~dp0..\assets\branding\windows" mkdir "%~dp0..\assets\branding\windows"
cl /nologo /std:c++20 /utf-8 /EHsc /W4 /WX /MT "%~dp0..\platforms\windows\resources\generate_icon.cpp" /Fo"%TI_OUT%\generate-icon.obj" /Fe"%TI_OUT%\generate-icon.exe"
if errorlevel 1 exit /b %errorlevel%
"%TI_OUT%\generate-icon.exe" "%~dp0..\assets\branding\windows\input-indicator.ico"
if errorlevel 1 exit /b %errorlevel%
rc /nologo /fo"%TI_OUT%\product-app.res" "%~dp0..\platforms\windows\resources\product.rc"
if errorlevel 1 exit /b %errorlevel%
rc /nologo /dTI_DLL /fo"%TI_OUT%\product-tip.res" "%~dp0..\platforms\windows\resources\product.rc"
exit /b %errorlevel%
