@echo off
rem Maintainer: rebuild installer\data-patches (game-data changes) and installer\patches + generated.lock
rem (hand edits to the generated code) from this development tree.
setlocal
cd /d "%~dp0.."
set "ISO=pc\Fable II (USA, Europe) (En,Zh,Ko,Pl,Cs,Hu,Sk) (Rev 2).iso"
if not "%~1"=="" set "ISO=%~1"

rem Any Python 3 will do (fable2_data.py only uses the standard library).
rem PY holds the command; it is quoted when it is a path.
set "PY="
py -3 -c "import sys" >nul 2>&1 && set "PY=py -3"
if not defined PY python -c "import sys; sys.exit(sys.version_info[0] != 3)" >nul 2>&1 && set "PY=python"
if not defined PY python3 -c "import sys" >nul 2>&1 && set "PY=python3"
if not defined PY if exist ".installer\venv\Scripts\python.exe" set PY=".installer\venv\Scripts\python.exe"
if not defined PY if exist "%USERPROFILE%\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe" set PY="%USERPROFILE%\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe"
if not defined PY for /d %%D in ("%LOCALAPPDATA%\Programs\Python\Python3*") do if exist "%%D\python.exe" set PY="%%D\python.exe"
if not defined PY (
  echo Python 3 was not found. Install it from https://www.python.org/downloads/ and tick "Add python.exe to PATH".
  goto :fail
)
echo Using Python: %PY%

%PY% installer\fable2_data.py make --iso "%ISO%" --tree extracted --out installer\data-patches || goto :fail
if not exist "installer\target\release\fable2-installer.exe" (cd installer && cargo build --release && cd .. ) || goto :fail
rem The code layout codegen must reproduce (see [[step]] codegen-partition in fable2-installer.toml).
copy /y "native\generated\default\codegen.partition.json" "installer\codegen.partition.json" || goto :fail
"installer\target\release\fable2-installer.exe" make-patches --game extracted || goto :fail
echo.
echo Done. Commit installer\data-patches, installer\patches, installer\generated.lock and installer\codegen.partition.json.
pause
exit /b 0
:fail
echo.
echo FAILED
pause
exit /b 1
