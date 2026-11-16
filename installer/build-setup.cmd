@echo off
rem Builds the installer and packs "dist\Fable II Setup.exe" (run make-patches.cmd first).
setlocal
cd /d "%~dp0"
cargo build --release || goto :fail
cd ..
if not exist "installer\generated.lock" (
  echo.
  echo The patch set is missing: run installer\make-patches.cmd once first ^(it regenerates the game code, takes a while^).
  goto :fail
)
"installer\target\release\fable2-installer.exe" package --out "dist\Fable II Setup.exe" || goto :fail
echo.
echo Done: dist\Fable II Setup.exe
pause
exit /b 0
:fail
echo.
echo FAILED
pause
exit /b 1
