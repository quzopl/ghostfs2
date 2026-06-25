# ghostfs — pod-projekt F: wydajność

**Data:** 2026-06-24
**Status:** zatwierdzony projekt (autonomia użytkownika)
**Część całości:** A–E ✓ → **F (wydajność)** → G (funkcje odłożone).

## Cel

Zredukować narzut I/O i CPU, szczególnie przy włączonym szyfrowaniu. Dwie główne dźwignie
plus pomiar:
1. **Hinty alokacji** — usunięcie skanów O(n) (alokacja bloku/i-węzła zaczyna od początku
   za każdym razem → O(n²) przy tworzeniu wielu plików).
2. **Cache bloków (in-memory)** — uniknięcie powtórnego `pread` i deszyfrowania gorących
   bloków (superblok-pochodne, mapa bitowa, tablica i-węzłów, katalogi).
3. **Benchmark** — zmierzyć i utrwalić poprawę; strażnik regresji.

Niezmiennik nadrzędny: **zero zmian semantyki**. Wszystkie testy A–E + integracja + stres
współbieżności (D) muszą dalej przechodzić; cache i hinty są przezroczyste.

## Część 1: Hinty alokacji

`struct gh_dev` zyskuje dwa hinty w pamięci (nie na dysku, liczone od zera przy każdym
otwarciu):

```c
struct gh_dev { ... ; uint64_t hint_block; uint64_t hint_inode; };
```

- **`gh_alloc_block`**: zacznij skan od `max(data_start, hint_block)`; jeśli dojdziesz do
  końca bez trafienia — zawiń i skanuj `[data_start, start)`. Po przydziale ustaw
  `hint_block = przydzielony + 1`. (Sekwencyjne tworzenie staje się amortyzowane O(1)
  zamiast O(n²).)
- **`gh_free_block`**: jeśli `zwalniany < hint_block` → `hint_block = zwalniany` (szybkie
  ponowne użycie).
- **`gh_inode_alloc`** (w `inode.c`): analogicznie z `hint_inode` (skan od
  `max(GH_ROOT_INO+1, hint_inode)`, zawijanie, aktualizacja po przydziale).
- **`gh_inode_free`**: jeśli `zwalniany < hint_inode` → `hint_inode = zwalniany`.

Inicjalizacja `hint_block = hint_inode = 0` w `gh_dev_create`/`gh_dev_open` (0 = „od
początku"). Pod blokadą sterownika (D) alokacja zachodzi pod `wrlock` (wyłącznie), więc
mutacja hintów jest bezpieczna wątkowo bez dodatkowych zamków.

**Poprawność:** hint to tylko punkt startu skanu — zawijanie gwarantuje, że żaden wolny
blok/i-węzeł nie zostanie pominięty; `-ENOSPC` tylko gdy pełen obrót bez trafienia.

## Część 2: Cache bloków (write-through, bezpieczny wątkowo)

`struct gh_dev` zyskuje wskaźnik na cache:

```c
struct gh_dev { ... ; struct gh_bcache *cache; };   /* NULL = wyłączony */
```

`struct gh_bcache` (direct-mapped, prosty i szybki):

```c
struct gh_bentry { uint64_t blkno; int valid; uint8_t data[GH_BLOCK_SIZE]; };
struct gh_bcache {
    pthread_mutex_t lock;
    uint32_t nslots;
    struct gh_bentry *slots;   /* nslots wpisów; slot = blkno % nslots */
};
```

- **Rozmiar:** `GH_BCACHE_SLOTS = 1024` (≈ 4 MB). Direct-mapped: `slot = blkno % nslots`.
  Eksmisja = nadpisanie slotu (bez LRU — prostota; gorące metadane i tak trafiają).
- **Treść:** zawsze **jawny** obraz bloku (po deszyfrowaniu przy odczycie, jawny bufor przy
  zapisie). Spójne z tym, co widzi rdzeń.
- **Blok 0 (superblok) NIE jest cache'owany** (rzadko czytany, specjalny).

### Integracja w `gh_disk_read`/`gh_disk_write` (`block.c`)

- `gh_disk_read(blkno)`: jeśli `cache && blkno!=0` → pod `lock`: trafienie (`valid &&
  blkno==`) → `memcpy` do bufora wywołującego, koniec. Pudło → (poza `lock`) `pread` +
  ewentualne deszyfrowanie do bufora; potem pod `lock` wstaw do slotu (`memcpy` z bufora).
- `gh_disk_write(blkno)`: po UDANYM `pwrite` (i ewentualnym szyfrowaniu) → jeśli `cache &&
  blkno!=0`, pod `lock` zaktualizuj slot jawnym buforem (write-through). Przy nieudanym
  zapisie (w tym hook `fail_after`) cache NIE jest aktualizowany.

### Cykl życia
- Cache jest **opcją montowania**: `gh_fs_mount`/`gh_fs_mount_key` alokuje cache
  (`calloc` slotów + `pthread_mutex_init`) i ustawia `dev.cache`; `gh_fs_unmount` niszczy
  mutex i zwalnia cache. `gh_dev_create`/`gh_dev_open` ustawiają `cache=NULL`.
- Narzędzia/testy używające surowego `gh_dev` (bez `gh_fs_mount`) mają `cache=NULL` →
  zachowanie bez zmian. Tylko ścieżka FS (CLI/FUSE/testy fs) korzysta z cache.

### Bezpieczeństwo wątkowe (interakcja z D)
- Blokada sterownika (D, rwlock) serializuje zapis-vs-odczyt: w trakcie zapisu (wrlock
  wyłączny) brak współbieżnych odczytów. Cache nie może być więc nieświeży względem
  dysku przy przejściu odczyt↔zapis.
- Mutex cache obsługuje wyłącznie wyścig **odczyt-vs-odczyt** (dwóch czytelników pod
  `rdlock` wstawiających ten sam blok) — krótka sekcja krytyczna (tylko manipulacja
  strukturą + `memcpy`, NIE `pread`/AES). Podwójne wstawienie = nadpisanie identyczną
  treścią (poprawne).
- Spójność z transakcją (B): w trakcie txn zapisy idą do bufora txn (nie `gh_disk_write`,
  więc nie do cache); odczyt sprawdza najpierw bufor txn, dopiero potem `gh_disk_read`
  (cache). Przy commit checkpoint woła `gh_disk_write` → aktualizuje cache zatwierdzoną
  treścią. Spójne na każdym etapie.

## Część 3: Benchmark

`tests/bench.c` (program pomiarowy, nie pass/fail w sensie wartości — raportuje czasy i
weryfikuje poprawność): utwórz/zapisz/odczytaj M plików (jawny i zaszyfrowany kontener),
zmierz `clock()`/wall-clock przed i po; wypisz przepustowość. Druga rundą (ciepły cache)
pokaż przyspieszenie odczytów. Asercje: poprawność danych (zapis==odczyt) i ukończenie w
rozsądnym czasie. Dodatkowo krótka notatka liczbowa w wyniku.

## Strategia testowania

1. `tests/test_alloc.c` / `tests/test_fs.c` (rozszerzenie): masowa alokacja/zwalnianie
   (np. utwórz 500 plików, usuń co drugi, utwórz kolejne) — poprawność + `fsck==0`; hinty
   nie pomijają wolnych zasobów (po zapełnieniu i częściowym zwolnieniu nowe alokacje
   trafiają w zwolnione miejsca).
2. `tests/test_fs.c` (cache transparentność): operacje przez `gh_fs` z włączonym cache
   dają identyczne wyniki; odczyt po zapisie spójny; remount (świeży cache) spójny.
3. `tests/bench.c`: pomiar + poprawność.
4. **Regresja:** wszystkie testy A–E `0 failed`; `make test-asan` = 0; integracja FUSE +
   **stres współbieżności (D) z włączonym cache** = bez korupcji, `fsck==0` (kluczowe — cache
   pod współbieżnością).
5. ThreadSanitizer (best-effort) na sterowniku z cache pod stresem — brak „data race".

## Obsługa błędów / przypadki brzegowe

| Sytuacja | Reakcja |
|---|---|
| `calloc` cache nieudany w mount | montowanie z `cache=NULL` (degradacja do bez-cache) lub `-ENOMEM` — wybór: `-ENOMEM` (jawnie) |
| Hook `fail_after` w zapisie | cache NIE aktualizowany (zapis nieudany) |
| Surowy `gh_dev` bez mount | `cache=NULL`, brak cache (jak dotąd) |
| Pełen obrót skanu alokacji | `-ENOSPC` |

## Świadome ograniczenia F (YAGNI)

- Cache direct-mapped (bez LRU/asocjacyjności) — prostota; gorące metadane trafiają, ale
  patologiczne wzorce (kolizje slotów) mają niższy hit-rate. LRU = możliwe rozszerzenie.
- Stały rozmiar cache (1024 bloki) — niekonfigurowalny w v1.
- Brak write-back/opóźnionego zapisu (write-through) — prostsze i bezpieczniejsze przy
  journalingu; write-back to przyszłość.
- Hinty nie są utrwalane na dysku (liczone od zera przy mount) — koszt: pierwszy skan po
  montowaniu jak dotąd; potem amortyzacja.
