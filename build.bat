@echo off
rem Shadow of War Trainer - build script (Visual Studio 2022 or newer, x64).
rem Output: build\ShadowOfWarTrainer.dll (the trainer) + build\xinput9_1_0.dll (the loader the game loads by itself)
setlocal
set ROOT=%~dp0
set TP=%ROOT%third_party
set MH=%TP%\minhook
set IMG=%TP%\imgui-1.91.9b
set OUT=%ROOT%build
set OBJ=%ROOT%build\obj
if not exist "%OBJ%" mkdir "%OBJ%"
if "%VCINSTALLDIR%"=="" (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSDIR=%%i
)
if "%VCINSTALLDIR%"=="" call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || (echo Visual Studio C++ tools not found & exit /b 1)
set INC=/I"%ROOT%src" /I"%IMG%" /I"%IMG%\backends" /I"%TP%" /I"%MH%\include"
set FLAGS=/nologo /O2 /MT /EHsc /std:c++17 /utf-8 /W3 /DNDEBUG /DNOMINMAX /wd4996 /wd4244 /wd4267

if not exist "%OBJ%\hde64.obj" (
  echo === minhook
  cl %FLAGS% %INC% /c /Fo"%OBJ%\\" "%MH%\src\buffer.c" "%MH%\src\hook.c" "%MH%\src\trampoline.c" "%MH%\src\hde\hde64.c" || exit /b 1
)
if not exist "%OBJ%\imgui_impl_dx11.obj" (
  echo === imgui
  cl %FLAGS% %INC% /c /Fo"%OBJ%\\" "%IMG%\imgui.cpp" "%IMG%\imgui_draw.cpp" "%IMG%\imgui_tables.cpp" "%IMG%\imgui_widgets.cpp" "%IMG%\backends\imgui_impl_dx11.cpp" "%IMG%\backends\imgui_impl_win32.cpp" || exit /b 1
)
echo === trainer
cl %FLAGS% %INC% /c /Fo"%OBJ%\\" "%ROOT%src\core.cpp" "%ROOT%src\server.cpp" "%ROOT%src\sigs.cpp" "%ROOT%src\calls.cpp" "%ROOT%src\overlay.cpp" "%ROOT%src\panel.cpp" "%ROOT%src\skin.cpp" "%ROOT%src\lang.cpp" "%ROOT%src\sow.cpp" "%ROOT%src\sow_prog.cpp" "%ROOT%src\sow_army.cpp" "%ROOT%src\sow_gear.cpp" || exit /b 1
echo === link
link /nologo /DLL /OUT:"%OUT%\ShadowOfWarTrainer.dll" "%OBJ%\*.obj" user32.lib psapi.lib d3d11.lib dxgi.lib windowscodecs.lib ole32.lib || exit /b 1
echo === loader (xinput9_1_0.dll proxy, goes next to ShadowOfWar.exe)
rem its own object folder: everything in %OBJ% is linked into the trainer DLL
set POBJ=%ROOT%build\proxy_obj
if not exist "%POBJ%" mkdir "%POBJ%"
rc /nologo /fo "%POBJ%\proxy.res" "%ROOT%proxy\proxy.rc" || exit /b 1
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /DUNICODE /D_UNICODE /c /Fo"%POBJ%\\" "%ROOT%proxy\xinput_proxy.cpp" || exit /b 1
link /nologo /DLL /DEF:"%ROOT%proxy\xinput9_1_0.def" /OUT:"%OUT%\xinput9_1_0.dll" "%POBJ%\xinput_proxy.obj" "%POBJ%\proxy.res" kernel32.lib || exit /b 1
echo BUILD OK
endlocal
