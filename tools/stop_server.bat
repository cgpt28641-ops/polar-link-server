@echo off
rem Wylacza serwer: najpierw grzecznie (tldserver zapisuje swiat), potem sprzata reszte (bot Discorda, launcher).
rem Karol 21.11: bez klikania w menedzerze zadan.
cd /d "%~dp0"
title stop tldserver
echo Wylaczam serwer (zapisuje swiat)...
echo stop> stop_server.flag
set /a n=0
:wait
tasklist /fi "imagename eq tldserver.exe" 2>nul | find /i "tldserver.exe" >nul || goto stopped
set /a n+=1
if %n% geq 30 goto force
timeout /t 1 /nobreak >nul
goto wait
:force
echo Serwer nie odpowiada od 30 s - zamykam go na sile (bez zapisu ostatnich zmian).
:stopped
del stop_server.flag 2>nul
rem launcher (server_bot.py), czat Discorda (discord_chat.py), most Discorda (run_bridge.py)
powershell -NoProfile -ExecutionPolicy Bypass -Command "Get-CimInstance Win32_Process -Filter \"Name like 'py%%.exe'\" | Where-Object { $_.CommandLine -match 'server_bot\.py|discord_chat\.py|run_bridge\.py' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }"
taskkill /f /im tldserver.exe >nul 2>nul
echo Serwer wylaczony.
timeout /t 3 >nul
