@echo off
setlocal

REM SCAV_ENVY_CACHE_ROOT is scav's own spelling of envy's ENVY_CACHE_ROOT, and it
REM exists because that one is global: set for the user it would retarget the
REM cache of every other envy project on the machine, including ones that chose a
REM project-local sandbox on purpose. An explicit ENVY_CACHE_ROOT still wins.
if not defined ENVY_CACHE_ROOT (
  if defined SCAV_ENVY_CACHE_ROOT set "ENVY_CACHE_ROOT=%SCAV_ENVY_CACHE_ROOT%"
)

cd /d "%~dp0"

call bin\envy.bat sync || exit /b 1

for /f "usebackq delims=" %%P in (`bin\envy.bat product python3`) do set "SCAV_PYTHON=%%P"
if not defined SCAV_PYTHON (
  echo error: `envy product python3` printed nothing 1>&2
  exit /b 1
)

"%SCAV_PYTHON%" tools\build.py --no-sync %*
exit /b %ERRORLEVEL%
