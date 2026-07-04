@echo off
cd /d "%~dp0"
call "C:\Program Files (x86)\Intel\oneAPI\setvars.bat" >nul 2>&1
"C:\Program Files\Microsoft Visual Studio\MSBuild\Current\Bin\MSBuild.exe" mishkinamish.vcxproj -p:Configuration=Release -p:Platform=x64 -t:Rebuild
pause
