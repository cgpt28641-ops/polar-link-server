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
- Żeby serwer startował razem z Windowsem: Win+R → `shell:startup` → wrzuć tam skrót do `start_server.bat`.

## Świat

Serwer trzyma świat w pliku `world.bin` obok exe (albo w pliku z `--world`). Zapisuje go co 30 s, jeśli coś się
zmieniło, oraz przy `stop`. Zapis idzie najpierw do `world.bin.tmp`, a potem plik jest podmieniany, więc po padzie
komputera zostaje stary albo nowy plik, nigdy uszkodzony.

Format jest ten sam co plik hosta z moda (`TLDCoop\worlds\<zapis>.bin`). Żeby zacząć od świata hosta, skopiuj ten
plik jako `world.bin`.

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
