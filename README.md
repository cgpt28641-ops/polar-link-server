# polar-link-server

Serwer dedykowany do moda co-op TLDCoop_DLC ([polar-link](https://github.com/cgpt28641-ops/polar-link)) do The Long
Dark. Działa bez gry, 24/7, a wszyscy gracze łączą się z nim jako klienci.

Plan i etapy są w `mod/SERVER_PLAN.md`. Kod sieciowy (protokół, sesja) pochodzi z moda, który jest podpięty jako
submoduł `mod/`, więc serwer zawsze mówi tym samym protokołem co mod.

## Pobranie

Najnowszy `tldserver.exe` (budowany po każdym pushu na `main`):
https://github.com/cgpt28641-ops/polar-link-server/releases/download/server-latest/tldserver.exe

## Uruchomienie

1. Wrzuć `tldserver.exe` do dowolnego folderu.
2. Obok niego skopiuj `version.txt` z gry: `The Long Dark\tld_Data\StreamingAssets\version.txt`. Wersja gry musi
   się zgadzać z wersją u graczy.
3. Uruchom `tldserver.exe`. Domyślnie słucha na UDP 27015. Opcje: `--port`, `--name`, `--game-version`, `--game-dir`,
   `--world PLIK`.
4. Komendy w konsoli: `players`, `kick <id>`, `say <tekst>`, `world`, `save`, `stop`. Wszystko trafia też do
   `tldserver.log`.

W grze: w `coop.ini` w sekcji `[net]` ustaw `transport=local`, `host_ip=<IP serwera>` i `port=27015`, a potem dołącz
z menu CO-OP (tak jak do hosta po IP).

## Restart po awarii i status na Discordzie

Obok `tldserver.exe` połóż `server_bot.py` i `start_server.bat` (oba są w tym samym wydaniu) i uruchamiaj serwer
przez `start_server.bat`. Potrzebny jest Python 3 (`winget install Python.Python.3.12`). Bez Pythona bat uruchomi
sam serwer.

- Gdy serwer padnie, bot uruchomi go ponownie. Po `stop` serwer zostaje wyłączony.
- Status na Discordzie: na kanale wybierz Ustawienia kanału → Integracje → Webhooki → Nowy webhook → Kopiuj URL
  webhooka i wklej ten adres do pliku `discord_webhook.txt` obok serwera. Bot trzyma tam jedną wiadomość ze stanem
  (kto jest i gdzie, godzina w świecie) i pisze krótką linię, gdy ktoś wchodzi albo wychodzi. Adresu webhooka nie
  udostępniaj nikomu: każdy, kto go ma, może pisać na tym kanale.
- **Aktualizacja**: nowa wersja zawsze pod tym samym linkiem:
  https://github.com/cgpt28641-ops/polar-link-server/releases/download/server-latest/tldserver.exe
  (podmień przy wyłączonym serwerze). Automatyczna aktualizacja przez `server_bot.py` jest WYŁĄCZONA; włącza ją pusty
  plik `auto_update.txt` obok (wtedy co 10 min pyta GitHuba i podmienia pliki, gdy nikt nie gra).
- Wyłączanie: `stop_server.bat` (leży obok). Serwer zapisuje świat i się zamyka, a bat sprząta bota Discorda i
  launcher. Bez menedżera zadań. `start_server.bat` nie uruchomi drugiego serwera, gdy jeden już działa.
- Żeby serwer startował razem z Windowsem: Win+R → `shell:startup` → wrzuć tam skrót do `start_server.bat`.
- Gdy serwer zniknie bez `stop` (np. zabije go antywirus), bot uzna to za awarię i uruchomi go ponownie.

## Bot czatu (#chat, lokalny model językowy)

`discord_chat.py` (w tym samym wydaniu, obok `server_bot.py`) czyta kanał `#chat` i odpowiada, gdy ktoś go oznaczy
(@bot) albo odpowie na jego wiadomość: wulgarnie, wprost, z polskimi memami i emotkami. Zna stan serwera (kto gra,
godzina w grze). Odpowiedzi pisze model językowy działający na tym komputerze (Ollama), nic nie idzie do żadnej
chmury poza samym Discordem. Uruchamia go `server_bot.py`, gdy obok leży `discord_bot_token.txt`.

1. Ollama i polski model (Bielik, ok. 4,5 GB, mieści się w karcie z 6 GB):
   `winget install Ollama.Ollama`, potem w nowym oknie cmd:
   `ollama pull SpeakLeash/bielik-minitron-7B-v3.0-instruct:Q4_K_M`
2. Biblioteka Discorda dla Pythona: `py -3 -m pip install discord.py`
3. Bot Discorda (webhook nie umie czytać kanału): discord.com/developers/applications → New Application → zakładka
   Bot → Reset Token → skopiuj token do pliku `discord_bot_token.txt` obok serwera. Na tej samej stronie włącz
   **MESSAGE CONTENT INTENT**. Potem OAuth2 → URL Generator: zaznacz `bot`, uprawnienia View Channels, Send
   Messages, Read Message History → otwórz wygenerowany link i dodaj bota do serwera Discord.
4. Uruchom serwer ponownie przez `start_server.bat`. Log bota: `discord_chat.log`.

Tokena nie udostępniaj nikomu: kto go ma, steruje botem. Opcjonalny plik `discord_chat.json` zmienia ustawienia, np.
`{"channels": ["chat"], "model": "...", "extra": "Szymon zawsze ginie pierwszy."}` (`extra`: dodatkowe teksty o
ekipie dla bota).

## Cały bot Discorda na tym komputerze (most + czat)

`setup_discord.bat` (w tym samym wydaniu) instaluje jednorazowo Git, biblioteki Pythona, Ollamę z modelem i klonuje
repozytorium mostu `tldcoop-discord` obok serwera. Potem wklej token bota do `discord_bot_token.txt` i uruchom
`start_server.bat`: `server_bot.py` sam startuje bota czatu (`discord_chat.py`) i most
(`tldcoop-discord\local\run_bridge.py`), i podnosi je, gdy padną. Logi: `discord_chat.log`, `discord_bridge.log`.

## Świat

Serwer trzyma świat w pliku `world.bin` obok exe (albo w pliku z `--world`). Zapisuje go co 30 s, jeśli coś się
zmieniło, oraz przy `stop`. Zapis idzie najpierw do `world.bin.tmp`, a potem plik jest podmieniany, więc po padzie
komputera zostaje stary albo nowy plik, nigdy uszkodzony.

Format jest ten sam co plik hosta z moda (`TLDCoop\worlds\<zapis>.bin`). Żeby zacząć od świata hosta, skopiuj ten
plik jako `world.bin`.

Przy każdym starcie serwer kopiuje `world.bin` do `backups\world-RRRRMMDD-GGMMSS.bin` (trzyma 20 ostatnich). Żeby
cofnąć świat: zatrzymaj serwer i skopiuj wybrany plik jako `world.bin`.

## Co działa

- Do 3 graczy naraz (tyle, ilu przyjaciół obsługuje dziś sesja moda).
- Gracze widzą się nawzajem. Serwer jest na liście graczy jako „Serwer”, ale nie ma postaci.
- Czat, ślady krwi, spray, światła w domach i podawanie przedmiotów są przekazywane dalej.
- Przedmioty na ziemi, kontenery (z blokadą „jeden gracz naraz”), rozebrane meble, zebrane rośliny, zamki, ciała,
  ogniska i źródła wody: serwer pamięta zmiany w każdej scenie, przekazuje je innym i wysyła graczowi, który wchodzi
  do sceny. Gdy dwóch graczy weźmie ten sam przedmiot, dostaje go jeden.
- **Jeszcze nie ma:** zegara i pogody z serwera, zwierząt, wspólnego snu, pociągu, dołączania kodem Steam. Te rzeczy
  wymagają też zmian w modzie.
- Sprawdzone tylko testami z udawanymi graczami, jeszcze nie z prawdziwą grą.

## Budowanie

Windows, MSVC i CMake:

```
git clone --recursive https://github.com/cgpt28641-ops/polar-link-server
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
build\server_test.exe
build\world_test.exe
```
