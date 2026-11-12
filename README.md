# polar-link-server

Serwer dedykowany do moda co-op TLDCoop_DLC ([polar-link](https://github.com/cgpt28641-ops/polar-link)) do The Long
Dark. Działa bez gry, 24/7, a wszyscy gracze łączą się z nim jako klienci.

Plan i etapy są w `mod/SERVER_PLAN.md`. Kod sieciowy (protokół, sesja) pochodzi z moda, który jest podpięty jako
submoduł `mod/`, więc serwer zawsze mówi tym samym protokołem co mod.

## Pobranie

Najnowszy `tldserver.exe` (budowany po każdym pushu na `main`):
https://github.com/cgpt28641-ops/polar-link-server/releases/download/server-latest/tldserver.exe

## Uruchomienie (etap 0)

1. Wrzuć `tldserver.exe` do dowolnego folderu.
2. Obok niego skopiuj `version.txt` z gry: `The Long Dark\tld_Data\StreamingAssets\version.txt`. Wersja gry musi
   się zgadzać z wersją u graczy.
3. Uruchom `tldserver.exe`. Domyślnie słucha na UDP 27015. Opcje: `--port`, `--name`, `--game-version`, `--game-dir`.
4. Komendy w konsoli: `players`, `kick <id>`, `say <tekst>`, `stop`. Wszystko trafia też do `tldserver.log`.

W grze: tryb `join` z IP serwera i portem, tak jak przy dołączaniu do hosta po IP.

## Etap 0: co działa

- Do 3 graczy naraz (tyle, ilu przyjaciół obsługuje dziś sesja moda).
- Gracze widzą się nawzajem. Serwer jest na liście graczy jako „Serwer”, ale nie ma postaci.
- Czat jest przekazywany dalej, a serwer może pisać komendą `say`.
- **Jeszcze nie ma:** zapisu świata (przedmioty, kontenery, ogniska itd.), zegara, pogody ani zwierząt. Kolejne
  etapy to dodadzą, więc na razie służy to do sprawdzenia połączenia, a nie do normalnej gry.

## Budowanie

Windows, MSVC i CMake:

```
git clone --recursive https://github.com/cgpt28641-ops/polar-link-server
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
build\server_test.exe
```
