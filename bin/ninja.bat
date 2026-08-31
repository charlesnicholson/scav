@echo off
rem envy-managed schema "3"
rem setlocal, not bare sets: a .bat without it mutates the caller's own environment. PATH
rem would grow a copy of this bin dir per invocation, and a sibling product invoked through
rem that PATH would inherit this script's product path -- passing the guard below and
rem re-running *this* payload, forever. Plain, not EnableDelayedExpansion: a product path
rem containing '!' must survive.
setlocal
rem The bin dir is a fixed fact, so a product that shells out to a sibling finds it. The
rem trailing dot keeps the script dir's own backslash off the closing quote.
set "PATH=%~dp0.;%PATH%"
rem The project root is not: deploy leaves the hop empty for an '@envy root "false"'
rem manifest, whose project depends on where the tree is nested, and the caller's stands.
set "ENVY_PROJECT_ROOT_HOP=.."
if defined ENVY_PROJECT_ROOT_HOP (
    for %%I in ("%~dp0%ENVY_PROJECT_ROOT_HOP%") do set "ENVY_PROJECT_ROOT=%%~fI"
)
rem The sibling launcher injects --project with this directory, so envy resolves the
rem project this script was deployed into, not one rediscovered from the caller's CWD.
rem Cleared first: `for /f` sets nothing when the command produces no output, so without
rem this the guard below would pass on a value inherited from an ancestor product script --
rem children see this scope, setlocal only walls off the caller -- and run that payload.
set "ENVY_PRODUCT_PATH="
for /f "delims=" %%i in ('call "%~dp0envy.bat" product "ninja"') do set "ENVY_PRODUCT_PATH=%%i"
if not defined ENVY_PRODUCT_PATH (
    echo envy: failed to resolve product 'ninja' 1>&2
    exit /b 1
)
call "%ENVY_PRODUCT_PATH%" %*
exit /b %ERRORLEVEL%
