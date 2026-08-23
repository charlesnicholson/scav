@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

REM The cache lives in out\ by default, so rmdir /s out removes every tool,
REM package and build artifact. Redirecting it is opt-in.
REM
REM SCAV_ENVY_CACHE_ROOT rather than envy's own ENVY_CACHE_ROOT, because that one
REM is global and would retarget every envy project on the machine. A gitignored
REM .env is the place to write it once. Parsed, never executed.
if not defined ENVY_CACHE_ROOT (
  if not defined SCAV_ENVY_CACHE_ROOT (
    if exist .env (
      for /f "usebackq tokens=1,* delims==" %%K in (`findstr /b /c:"SCAV_ENVY_CACHE_ROOT" .env`) do (
        set "SCAV_ENVY_CACHE_ROOT=%%~L"
      )
    )
  )
  if defined SCAV_ENVY_CACHE_ROOT set "ENVY_CACHE_ROOT=!SCAV_ENVY_CACHE_ROOT!"
)

call bin\envy.bat sync || exit /b 1

for /f "usebackq delims=" %%P in (`bin\envy.bat product python3`) do set "SCAV_PYTHON=%%P"
if not defined SCAV_PYTHON (
  echo error: `envy product python3` printed nothing 1>&2
  exit /b 1
)

"%SCAV_PYTHON%" tools\build.py --no-sync %*
exit /b %ERRORLEVEL%
