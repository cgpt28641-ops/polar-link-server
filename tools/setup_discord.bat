@echo off
rem One-time setup of the Discord parts on the server PC (run in C:\tldserver, beside tldserver.exe):
rem   - Git, Python libraries (discord.py, cryptography), Ollama and the Polish language model for the chat bot
rem   - a clone of the bridge repository (tldcoop-discord) beside the server
rem Then: put the bot's token into discord_bot_token.txt here, and start the server with start_server.bat.
cd /d "%~dp0"
title tldserver: Discord setup
echo === 1/5 Git ===
where git >nul 2>nul || winget install -e --id Git.Git --accept-source-agreements --accept-package-agreements
echo === 2/5 Python libraries ===
where py >nul 2>nul || winget install -e --id Python.Python.3.12 --accept-source-agreements --accept-package-agreements
py -3 -m pip install --upgrade discord.py cryptography
echo === 3/5 Ollama ===
where ollama >nul 2>nul || winget install -e --id Ollama.Ollama --accept-source-agreements --accept-package-agreements
echo === 4/5 The language model (about 4.5 GB) ===
where ollama >nul 2>nul && ollama pull SpeakLeash/bielik-minitron-7B-v3.0-instruct:Q4_K_M
where ollama >nul 2>nul || echo Ollama was just installed: close this window, open a new one and run setup_discord.bat again for the model.
echo === 5/5 The bridge repository ===
set "PATH=%PATH%;%ProgramFiles%\Git\cmd"
if exist tldcoop-discord\.git (
  git -C tldcoop-discord pull --rebase
) else (
  git clone https://github.com/cgpt28641-ops/tldcoop-discord tldcoop-discord
)
if not exist discord_bot_token.txt (
  type nul > discord_bot_token.txt
  echo.
  echo discord_bot_token.txt is made, EMPTY: paste the bot's token into it ^(one line^) and save.
)
echo.
echo Sign in to GitHub for the bridge ^(a browser window may open^):
git -C tldcoop-discord push origin HEAD:main
echo.
echo Done. Check the lines above for errors, then start the server with start_server.bat.
pause
