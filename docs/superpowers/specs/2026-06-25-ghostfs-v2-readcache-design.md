# ghostfs v2 — read-side node cache: design

**Data:** 2026-06-25 **Część:** v2 optymalizacja (losowy odczyt, diagnoza benchmarkv2.md).

## Problem
Losowy odczyt v2 −18% vs v1. Każdy odczyt = descent B-drzewa (czyta węzły ścieżki). Bloki węzłów
są w cache bloków (F), więc gh_disk_read = memcpy, ALE `gh2_node_read` liczy `gh_crc32` (4096 B)
weryfikacji NA KAŻDY węzeł NA KAŻDY descent. Górne węzły (korzeń/wewn.) czytane przy KAŻDYM
odczycie → wielokrotna ponowna weryfikacja CRC tych samych węzłów. To ~znaczna część czasu
losowego odczytu (CRC ~22% przy 22k IOPS).

## Rozwiązanie: read-side node cache (csum-keyed, self-coherent)
Mały cache ZWERYFIKOWANYCH węzłów: `block -> {bufor 4096, csum}` (LRU/hash, ograniczony np.
2048 węzłów = 8 MB). `gh2_node_read(bptr)`: po sprawdzeniu dirty-ncache (read-your-writes), jeśli
`readcache[bptr.block].csum == bptr.csum` → **memcpy z cache, RETURN (pomiń gh_disk_read + gh_crc32
weryfikację)**; inaczej gh_disk_read + weryfikuj (+dup/repair jak dotąd) + wstaw {bufor, bptr.csum}.
Gorące węzły (korzeń/wewn.) zawsze w cache → CRC pominięty; tylko liść (inny per losowy odczyt)
weryfikowany.

## Spójność (csum-keyed → BEZ jawnej inwalidacji)
csum działa jako wersja treści: gdy blok zostanie CoW-zwolniony i ponownie użyty z INNĄ treścią,
nowa treść ma INNY csum; caller (bieżące drzewo) ma bptr.csum dopasowany do NOWEJ treści →
`readcache[block].csum`(stary) != bptr.csum(nowy) → MISS → re-read. Nieaktualny wpis nieszkodliwy
(zajmuje slot do eksmisji LRU). Self-healing: jeśli cache ma DOBRĄ treść (csum==bptr) a dysk
zbitrotował → cache serwuje dobrą (omija korupcję) — POMAGA samonaprawie. Snapshoty: czytają stare
węzły przez bptr o starym csum → cache hit poprawny.

## Interakcje
- Dirty write-back ncache sprawdzany PIERWSZY (read-your-writes); read-cache dla CZYSTYCH węzłów.
- Init przy mount, free przy unmount; ograniczony LRU (eksmisja). NIE wpływa na v1 (NULL).
- Szyfrowanie: gh_disk_read deszyfruje; read-cache trzyma PLAINTEXT zweryfikowany (csum z plaintextu).

## Testy
1. **Korektność (BRAMKA):** WSZYSTKIE testy v2 0 failed — read-cache zwraca tę samą zweryfikowaną
   treść; round-trip bajt-exact; persystencja; snapshot; crash-sweep niezmieniony (read-only).
2. **Spójność csum-keyed:** CoW blok reused → read inny bptr/csum → poprawna treść (nie stara);
   napisz węzeł, czytaj (cache), CoW (nowy blok), czytaj nowy → nowy; stary bptr (snapshot) → stary.
3. **Self-healing z cache:** cache ma dobry węzeł, zbitrotuj disk-block → read przez cache = dobry
   (lub gdy nie w cache: dup/repair jak dotąd); oba złe + brak w cache → -EIO.
4. **Dowód:** licznik gh_crc32 weryfikacji węzłów dla N losowych odczytów → << N×wysokość (górne
   węzły pomijane); hit-rate górnych węzłów ~100%.
5. read-only (mapa/refcount niezmienione); ASan brak wycieku (cache zwolniony przy unmount).

## Świadome ograniczenia
- Cache plaintext zweryfikowanych węzłów w pamięci (LRU bounded). Pomija CRC+disk-read dla hot.
- Dane (bloki plików) NIE cache'owane tu (to bcache F + range-read).
