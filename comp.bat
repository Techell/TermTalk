@echo off
setlocal
rem Old MinGW Make requires ASCII paths. Use the existing Windows short name.
for %%I in ("%~dp0.") do set "TERMTALK_ROOT=%%~sI"
cmake -G "MinGW Makefiles" -S "%TERMTALK_ROOT%" -B "%TERMTALK_ROOT%\build" -DCMAKE_BUILD_TYPE=Debug
if errorlevel 1 exit /b 1
cmake --build "%TERMTALK_ROOT%\build"
exit /b %errorlevel%
