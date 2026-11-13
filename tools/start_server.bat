@echo off
rem tldserver with its launcher: restarts after a crash, status on Discord (server_bot.py, discord_webhook.txt).
rem Needs Python 3 (winget install Python.Python.3.12). Without it: tldserver.exe alone.
cd /d "%~dp0"
title tldserver
where py >nul 2>nul && (py -3 server_bot.py %* & goto :eof)
where python >nul 2>nul && (python server_bot.py %* & goto :eof)
echo Brak Pythona: uruchamiam sam tldserver.exe (bez restartow i Discorda).
tldserver.exe %*
pause
