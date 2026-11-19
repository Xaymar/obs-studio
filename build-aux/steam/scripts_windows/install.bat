@echo off
@cd /d "%~dp0"

goto checkAdmin


:checkAdmin
	net session >nul 2>&1
	if %errorLevel% == 0 (
		echo.
	) else (
		echo Administrative rights are required. Please re-run this script as Administrator.
		goto end
	)

:writeRegistry
	reg add "HKLM\SOFTWARE\OBS Studio" /f /t REG_SZ /d %1 /reg:32
	reg add "HKLM\SOFTWARE\OBS Studio" /f /t REG_SZ /d %1 /reg:64

:setupProgramData
	:: Required for UWP applications
	mkdir "%PROGRAMDATA%\obs-studio-hook"
	icacls "%PROGRAMDATA%\obs-studio-hook" /grant "ALL APPLICATION PACKAGES":(OI)(CI)(GR,GE)

:endSuccess
	echo Installed!
	echo.

:end
	exit
