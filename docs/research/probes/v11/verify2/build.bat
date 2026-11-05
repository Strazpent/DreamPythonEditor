@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /LD /O2 /I"C:\Program Files\Epic Games\UE_5.8\Engine\Source\ThirdParty\Python3\Win64\include" wd.c /link /LIBPATH:"C:\Program Files\Epic Games\UE_5.8\Engine\Source\ThirdParty\Python3\Win64\libs" python311.lib /OUT:wd.dll
