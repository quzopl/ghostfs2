# ghostfs — pod-projekt J: sumy kontrolne (integralność danych) + CRC dziennika

**Data:** 2026-06-24
**Status:** zatwierdzony projekt (autonomia użytkownika)
**Część całości:** A–I ✓ → **J (checksumy)** → K (katalogi haszowane).

## Cel

Wykrywanie **cichej korupcji** (bit-rot, read-disturb na flashu) danych i metadanych +
odporność dziennika na **rozdarty zapis** przy utracie zasilania.

1. **Sumy kontrolne per blok** (dane + metadane): CRC32 każdego bloku w
   **journalowanym regionie sum**; weryfikacja przy odczycie → `-EIO` przy niezgodności.
2. **CRC dziennika**: suma nagłówka+obrazów; recover odrzuca rozdartą paczkę (nie odtwarza
   śmieci).

## Decyzja architektoniczna: checksumy w warstwie logicznej

Sumy liczone i sprawdzane w `gh_block_read`/`gh_block_write` (warstwa logiczna, **jawna
treść** przed szyfrowaniem). Suma bloku `B` trzymana w **regionie sum** (osobny region
on-disk), aktualizowana przez `gh_block_write(blok_sum)` — czyli **journalowana i
cache'owana** jak każdy blok. Rekurencja ograniczona: bloki regionu sum **nie są
sumowane** (przypadek bazowy).

**Kompozycja z I i F (niski narzut):**
- Wiele bloków danych dzieli jeden blok sum (1024 sum/blok). Group commit (I) zapisuje ten
  blok sum **raz na flush** (amortyzacja), mimo wielu aktualizacji w pamięci.
- Cache bloków (F) czyni odczyt bloku sum przy weryfikacji **tanim** (trafienie po
  pierwszym dostępie).
- Journaling regionu sum (przez bufor txn) → suma i dane zapisywane **atomowo razem** →
  brak desynchronizacji po awarii (żadnych fałszywych alarmów korupcji).

## Zmiany formatu on-disk (kompatybilne wstecz)

### Superblok — region sum + flaga + self-suma

```c
struct gh_superblock {
    ... (do enc_verifier) ...
    uint64_t csum_start;    /* NOWE: pierwszy blok regionu sum (0 = brak) */
    uint64_t csum_blocks;   /* NOWE: rozmiar regionu sum (0 = brak) */
    uint32_t sb_csum;       /* NOWE: CRC32 superbloku (pola, z sb_csum=0) */
};
```

- Flaga `#define GH_SB_CHECKSUMS 0x2u` w `flags`.
- Region sum: `csum_blocks = ceil(total_blocks * 4 / GH_BLOCK_SIZE)` (4 B CRC/blok).
- Układ po J: `[super][mapa][i-węzły][dziennik][SUMY][dane]`. `data_start` rośnie o
  `csum_blocks`.
- Stare kontenery: `csum_blocks==0`, `flags & GH_SB_CHECKSUMS == 0` → brak sum (tryb jak
  dotąd). **Domyślnie WŁĄCZONE** przy nowym formacie (cecha niezawodnościowa).
- `sb_csum`: CRC superbloku (z `sb_csum=0` w trakcie liczenia); sprawdzany w
  `gh_mount_sb` → `-EINVAL` przy niezgodności (gdy `csum_blocks>0`).

### Nagłówek dziennika — CRC

```c
struct gh_jheader {
    ... (bez zmian) ...
    uint64_t descriptor_blocks;
    uint32_t csum;          /* NOWE: CRC32 deskryptora+obrazów (rozdarty zapis) */
};
```

## Nowy moduł `src/csum.h` / `src/csum.c`

```c
uint32_t gh_crc32(const void *buf, size_t len);   /* tablicowy CRC32 (poly 0xEDB88320) */
```

Implementacja tablicowa (256-elementowa tablica, init `0xFFFFFFFF`, final XOR
`0xFFFFFFFF`). Szybki, bez zależności. Nie kryptograficzny — do wykrywania bit-rotu
wystarcza.

## Pola `gh_dev` (cache układu)

`gh_block_read`/`write` biorą tylko `(dev, blkno, buf)` — bez `sb`. Cache'ujemy układ sum w
`gh_dev` (ustawiany przy mount/format):

```c
struct gh_dev { ... ; int checksums;
                uint64_t csum_start, csum_blocks, jrnl_start, jrnl_blocks; };
```

`is_checksummed(dev, blkno)` = `dev->checksums && blkno!=0 && blkno∉[jrnl_start,jrnl_start+
jrnl_blocks) && blkno∉[csum_start,csum_start+csum_blocks)`. Pokrywa mapę bitową, i-węzły,
katalogi, bloki pośrednie, dane. Wyłącza: superblok, dziennik, region sum.

## Integracja w `block.c`

- **`gh_block_write(blkno, buf)`**: jeśli `is_checksummed` → policz `crc = gh_crc32(buf)`;
  zlokalizuj blok sum (`csum_start + blkno*4/BS`, offset `blkno*4 % BS`); `gh_block_read`
  bloku sum → wstaw `crc` (4 B) → `gh_block_write` bloku sum (rekurencja bazowa). Potem
  istniejąca logika (bufor txn / `gh_disk_write`).
- **`gh_block_read(blkno, buf)`**: po wczytaniu (bufor txn lub `gh_disk_read` z
  deszyfrowaniem) jeśli `is_checksummed` → policz `crc` z `buf`; wczytaj zapisaną sumę z
  regionu (`gh_block_read` bloku sum); jeśli `stored != 0 && stored != crc` → `-EIO`
  (korupcja). (`stored==0` = blok jeszcze niesumowany — pomiń; defensywnie.)

Suma liczona na **jawnej** treści (przed szyfrowaniem). Korupcja szyfrogramu → błędne
odszyfrowanie → inne `crc` → wykryte.

## CRC dziennika (`journal.c`)

- **`gh_jrnl_flush`**: po zapisaniu deskryptora i obrazów do regionu dziennika, policz
  `csum = gh_crc32` nad konkatenacją bloków deskryptora + obrazów; zapisz w `h.csum`.
- **`gh_jrnl_recover`**: po wczytaniu nagłówka (magic+committed+bounds OK), wczytaj
  deskryptor+obrazy, przelicz CRC; jeśli `!= h.csum` → **rozdarty zapis** → potraktuj jak
  niezatwierdzony (nie odtwarzaj). Inaczej odtwórz jak dotąd.

## Format i montowanie

- **`gh_format_enc`**: wylicz `csum_blocks`, wstaw region sum (po dzienniku, przed danymi),
  ustaw `flags |= GH_SB_CHECKSUMS`, `csum_start`/`csum_blocks`; ustaw `dev.checksums` +
  pola układu PRZED zapisem metadanych (by mapa/i-węzły/root dostały sumy); wyzeruj region
  sum; policz `sb_csum`. Region sum zaznaczony w mapie jako metadane (jak dziennik).
- **`gh_mount_sb`/`gh_fs_mount_key`**: wczytaj superblok; jeśli `csum_blocks>0` → sprawdź
  `sb_csum`; ustaw `dev.checksums=1` + `csum_start`/`csum_blocks`/`jrnl_start`/
  `jrnl_blocks` z superbloku. Stare kontenery (`csum_blocks==0`) → `dev.checksums=0`.

## Strategia testowania (TDD + ASan)

1. `tests/test_csum.c` (nowy): `gh_crc32` deterministyczny; różne dane → różne CRC; znana
   wartość referencyjna (np. CRC32 „123456789" = `0xCBF43926`).
2. `tests/test_fs.c`: **wykrycie korupcji danych** — zapisz plik, odmontuj, **uszkodź
   surowo** blok danych w kontenerze (`pwrite` 1 bajt), zamontuj, `gh_fs_read` → `-EIO`.
   Analogicznie korupcja bloku i-węzłów/katalogu → operacja `-EIO`. Zdrowy odczyt → OK.
3. `tests/test_journal.c`: **CRC dziennika** — spreparuj nagłówek `committed=1` z
   poprawnym CRC → recover odtwarza; z BŁĘDNYM CRC (uszkodzone obrazy) → recover NIE
   odtwarza (rozdarty zapis).
4. `tests/test_super.c`: layout z regionem sum (`csum_start`, `csum_blocks>0`, `data_start`
   przesunięty); `sb_csum` weryfikowany (uszkodzony superblok → mount `-EINVAL`).
5. **Regresja:** wszystkie testy A–I `0 failed` (sumy przezroczyste gdy brak korupcji);
   `make test-asan` = 0; integracja FUSE + urządzenie blokowe zielone; crash-sweep
   `fsck==0` (sumy journalowane → spójne po awarii, brak fałszywych alarmów).
6. **Scrub:** `gh_fsck` automatycznie korzysta z weryfikacji przy odczycie (czyta bloki
   przez `gh_block_read` → korupcja = `-EIO` w trakcie fsck). (Pełny scrub-pass opcjonalny.)

## Obsługa błędów

| Sytuacja | Reakcja |
|---|---|
| Niezgodna suma bloku przy odczycie | `-EIO` (korupcja wykryta) |
| Uszkodzony superblok (`sb_csum`) | `gh_mount_sb` → `-EINVAL` |
| Rozdarty zapis dziennika (CRC) | recover traktuje jak niezatwierdzony (nie odtwarza) |
| Stary kontener bez sum | `dev.checksums=0`, tryb bez sum |

## Świadome ograniczenia J (YAGNI)

- CRC32 (nie kryptograficzny) — wykrywa losową korupcję/bit-rot, nie złośliwą modyfikację.
  Integralność kryptograficzna = AEAD (oddzielnie, przyszłość).
- Region sum nie jest replikowany — korupcja samego bloku sum daje fałszywy alarm na
  blokach, które obejmuje (1024 bloki). Ditto-block dla regionu sum = możliwe rozszerzenie.
- Brak automatycznej naprawy (self-healing) — wykrycie tylko (brak redundancji; patrz
  wcześniejsza analiza: healing wymaga ditto/mirror).
- Narzut: +`csum_blocks` regionu; +1 odczyt bloku sum przy odczycie (cache'owany); zapis
  sum amortyzowany przez group commit. Akceptowalny dla niezawodności.
- Domyślnie włączone przy formacie; wyłączenie = osobny tryb (nieobjęte v1 — można dodać
  flagę formatu).
