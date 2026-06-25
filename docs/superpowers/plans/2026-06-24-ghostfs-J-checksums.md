# ghostfs pod-projekt J — checksumy: plan implementacji

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`).
> Subagenci dozwoleni; sekwencyjnie. Bramka: zielone testy + ASan + crash-sweep fsck==0 + wykrycie korupcji.

**Goal:** Sumy kontrolne per blok (wykrywanie cichej korupcji, `-EIO`) + CRC dziennika (odporność na rozdarty zapis). Zgodnie ze spec `docs/superpowers/specs/2026-06-24-ghostfs-J-checksums-design.md`.

**Tech Stack:** C11, OpenSSL, pthreads, mini-harness, ASan.

## Global Constraints
- Testy przez `make`; gołe `cc` wymagają `-D_POSIX_C_SOURCE=200809L -lcrypto -lpthread`.
- Wszystkie testy A–I `0 failed`; sumy przezroczyste gdy brak korupcji.

## File Structure
| Plik | Zmiana |
|---|---|
| `src/csum.h/.c` | nowy: `gh_crc32` |
| `src/ghostfs.h` | superblok `csum_start`/`csum_blocks`/`sb_csum`; `GH_SB_CHECKSUMS`; `gh_jheader.csum` |
| `src/block.h` | pola `checksums`/`csum_start`/`csum_blocks`/`jrnl_start`/`jrnl_blocks` w `gh_dev` |
| `src/block.c` | (Task 2) sumy w `gh_block_read/write`; init pól |
| `src/super.c` | format alokuje region sum, default-on, `sb_csum`; `gh_mount_sb` ustawia pola dev + weryfikuje `sb_csum` |
| `src/journal.c` | (Task 2) CRC dziennika flush/recover |
| `Makefile` | `src/csum.c` do CORE |
| `tests/test_csum.c` | nowy |
| `tests/test_super.c`, `tests/test_fs.c`, `tests/test_journal.c` | testy |

---

## Task 1: Moduł CRC + region sum + format/mount (infrastruktura)

**Files:** Create: `src/csum.h`, `src/csum.c`, `tests/test_csum.c`; Modify: `src/ghostfs.h`, `src/block.h`, `src/block.c`, `src/super.c`, `Makefile`, `tests/test_super.c`

- [ ] **Step 1: `src/csum.h`**

```c
#ifndef GH_CSUM_H
#define GH_CSUM_H
#include <stdint.h>
#include <stddef.h>
uint32_t gh_crc32(const void *buf, size_t len);
#endif
```

- [ ] **Step 2: `src/csum.c` (CRC32 tablicowy)**

```c
#include "csum.h"
static uint32_t table[256];
static int built = 0;
static void build(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        table[i] = c;
    }
    built = 1;
}
uint32_t gh_crc32(const void *buf, size_t len) {
    if (!built) build();
    const uint8_t *p = buf;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
```

(Uwaga: `built` bez ochrony wątkowej — pierwsze wywołanie idempotentne; pod współbieżnością
najwyżej wielokrotne zbudowanie tej samej tablicy. Akceptowalne; alternatywnie zbuduj w
mount jednowątkowo. Pozostaw proste.)

- [ ] **Step 3: `tests/test_csum.c`**

```c
#include "test.h"
#include "../src/csum.h"
#include <string.h>
static void test_crc(void) {
    CHECK_EQ(gh_crc32("123456789", 9), 0xCBF43926u);   /* znana wartosc referencyjna */
    CHECK(gh_crc32("a", 1) != gh_crc32("b", 1));
    char z[4096]; memset(z, 0, sizeof(z));
    char o[4096]; memset(o, 0, sizeof(o)); o[100] = 1;
    CHECK(gh_crc32(z, 4096) != gh_crc32(o, 4096));     /* jednobitowa zmiana -> inny CRC */
}
int main(void) { RUN_TEST(test_crc); return TEST_SUMMARY(); }
```

- [ ] **Step 4: `src/ghostfs.h` — pola superbloku + flaga + jheader.csum**

Po `enc_verifier` w `struct gh_superblock`:

```c
    uint64_t csum_start;
    uint64_t csum_blocks;
    uint32_t sb_csum;
```

Stała przy innych `#define`: `#define GH_SB_CHECKSUMS 0x2u`.
W `struct gh_jheader` po `descriptor_blocks`: `uint32_t csum;`.

- [ ] **Step 5: `src/block.h` — pola dev**

```c
struct gh_dev { ... (istniejace) ... ;
                int checksums; uint64_t csum_start, csum_blocks, jrnl_start, jrnl_blocks; };
```

- [ ] **Step 6: `src/block.c` — init pól** w `gh_dev_create`/`gh_dev_open`:

```c
    dev->checksums = 0; dev->csum_start = 0; dev->csum_blocks = 0;
    dev->jrnl_start = 0; dev->jrnl_blocks = 0;
```

(W Task 1 `gh_block_read/write` POZOSTAJĄ bez zmian — pola nieużywane do Task 2.)

- [ ] **Step 7: `src/super.c` — format z regionem sum + mount ustawia pola**

`#include "csum.h"`. W `gh_format_enc` w wyliczaniu layoutu, PO `journal_blocks`, przed
`data_start`:

```c
    uint64_t csum_blocks = (total_blocks * 4 + GH_BLOCK_SIZE - 1) / GH_BLOCK_SIZE;
    uint64_t csum_start = journal_start + journal_blocks;
    uint64_t data_start = csum_start + csum_blocks;
    if (data_start >= total_blocks) return -EINVAL;
```

Ustaw w superbloku: `sb.csum_start = csum_start; sb.csum_blocks = csum_blocks;
sb.flags |= GH_SB_CHECKSUMS;`. Po otwarciu/utworzeniu `dev`, ustaw pola dev:
`dev.checksums = 1; dev.csum_start = csum_start; dev.csum_blocks = csum_blocks;
dev.jrnl_start = journal_start; dev.jrnl_blocks = journal_blocks;`.

Wyzeruj region sum (pętla zapisu zer do bloków `[csum_start, data_start)` — przez
`gh_block_write` lub `gh_disk_write`). Region sum zaznacz w mapie jako metadane (pętla bitów
`[0, data_start)` już to obejmuje, bo `data_start` uwzględnia region sum — bez zmian).

`sb_csum`: po wypełnieniu wszystkich pól sb, ustaw `sb.sb_csum = 0;` policz
`sb.sb_csum = gh_crc32(&sb, sizeof(sb));` PRZED zapisem bloku 0.

W `gh_mount_sb`, po walidacji magic i kopiowaniu sb: jeśli `sb->csum_blocks > 0`:
- sprawdź `sb_csum`: skopiuj sb, wyzeruj `sb_csum` w kopii, `gh_crc32(kopia, sizeof)` ==
  zapisany `sb->sb_csum`? Niezgodność → `-EINVAL`.
- ustaw `dev->checksums = 1; dev->csum_start = sb->csum_start; dev->csum_blocks =
  sb->csum_blocks; dev->jrnl_start = sb->journal_start; dev->jrnl_blocks =
  sb->journal_blocks;`.

(Pamiętaj: pętla zaznaczania bitów metadanych w formacie obejmuje `[0, data_start)` =
super+mapa+i-węzły+dziennik+sumy. Bez zmian, bo `data_start` rośnie.)

- [ ] **Step 8: `Makefile` — `src/csum.c` do CORE** (przed `src/fs.c`):

```make
CORE    := src/block.c src/alloc.c src/super.c src/inode.c src/dir.c src/xattr.c src/journal.c src/crypto.c src/csum.c src/fs.c
```

- [ ] **Step 9: `tests/test_super.c` — layout z regionem sum**

Dodaj/zmień asercje: `CHECK(sb.csum_blocks > 0); CHECK_EQ(sb.csum_start, sb.journal_start +
sb.journal_blocks); CHECK_EQ(sb.data_start, sb.csum_start + sb.csum_blocks);`. (Popraw
istniejące asercje `data_start == journal_start + journal_blocks` na nowy layout.)

- [ ] **Step 10: Build + testy**

Run: `make clean && make test`
Expected: wszystkie `0 failed` (test_csum, test_super z nowym layoutem; reszta przezroczyście
— `gh_block` jeszcze nie sumuje, więc zachowanie bez zmian; format alokuje region, mount
ustawia pola i weryfikuje sb_csum). `make test-asan` → `0 failed`.

- [ ] **Step 11: Commit**

```bash
git add src/csum.h src/csum.c tests/test_csum.c src/ghostfs.h src/block.h src/block.c src/super.c Makefile tests/test_super.c
git commit -m "feat: modul CRC32 + region sum w formacie + sb_csum + pola dev (infrastruktura J)"
```

---

## Task 2: Aktywacja sum w `gh_block_read/write` + CRC dziennika

**Files:** Modify: `src/block.c`, `src/journal.c`; Test: `tests/test_fs.c`, `tests/test_journal.c`

- [ ] **Step 1: Helper + suma w `gh_block_write`/`gh_block_read` (`src/block.c`)**

Dodaj `#include "csum.h"`. Dodaj helper i zmień obie funkcje:

```c
static int gh_is_csummed(struct gh_dev *dev, uint64_t blkno) {
    if (!dev->checksums) return 0;
    if (blkno == 0) return 0;
    if (dev->jrnl_blocks && blkno >= dev->jrnl_start && blkno < dev->jrnl_start + dev->jrnl_blocks) return 0;
    if (blkno >= dev->csum_start && blkno < dev->csum_start + dev->csum_blocks) return 0;
    return 1;
}
```

W `gh_block_write`, NA POCZĄTKU (przed logiką txn/disk):

```c
    if (gh_is_csummed(dev, blkno)) {
        uint32_t crc = gh_crc32(buf, GH_BLOCK_SIZE);
        uint64_t cb = dev->csum_start + (blkno * 4) / GH_BLOCK_SIZE;
        uint32_t coff = (uint32_t)((blkno * 4) % GH_BLOCK_SIZE);
        uint8_t cbuf[GH_BLOCK_SIZE];
        int r = gh_block_read(dev, cb, cbuf); if (r) return r;
        memcpy(cbuf + coff, &crc, 4);
        r = gh_block_write(dev, cb, cbuf); if (r) return r;   /* rekurencja bazowa: cb nie sumowany */
    }
    /* ... istniejaca logika txn/gh_disk_write ... */
```

W `gh_block_read`, zrestrukturyzuj na pojedynczy punkt weryfikacji na końcu:

```c
int gh_block_read(struct gh_dev *dev, uint64_t blkno, void *buf) {
    int r; int got = 0;
    if (dev->txn && dev->txn->active) {
        struct gh_txn *t = dev->txn;
        for (uint32_t i = 0; i < t->n; i++)
            if (t->blknos[i] == blkno) { memcpy(buf, t->images[i], GH_BLOCK_SIZE); got = 1; break; }
    }
    r = got ? 0 : gh_disk_read(dev, blkno, buf);
    if (r == 0 && gh_is_csummed(dev, blkno)) {
        uint32_t crc = gh_crc32(buf, GH_BLOCK_SIZE);
        uint64_t cb = dev->csum_start + (blkno * 4) / GH_BLOCK_SIZE;
        uint32_t coff = (uint32_t)((blkno * 4) % GH_BLOCK_SIZE);
        uint8_t cbuf[GH_BLOCK_SIZE];
        if (gh_block_read(dev, cb, cbuf) == 0) {     /* cb nie sumowany -> brak rekurencji */
            uint32_t stored; memcpy(&stored, cbuf + coff, 4);
            if (stored != 0 && stored != crc) return -EIO;   /* korupcja */
        }
    }
    return r;
}
```

(Suma liczona na jawnej treści `buf` — przed szyfrowaniem przy zapisie, po deszyfrowaniu
przy odczycie. Rekurencja `gh_block_read/write(cb)` jest bazowa, bo `gh_is_csummed(cb)==0`.)

- [ ] **Step 2: CRC dziennika w `src/journal.c`**

Dodaj `#include "csum.h"`. W `gh_jrnl_flush`, PO zapisaniu deskryptora i obrazów (przed
nagłówkiem committed=1), policz CRC nad obrazami i deskryptorem i wpisz do nagłówka:

```c
    /* CRC nad deskryptorem + obrazami (rozdarty zapis) */
    uint32_t jcsum = 0xFFFFFFFFu;  /* lub policz przyrostowo; prosciej: bufor-po-buforze */
    /* policz przez ponowne wczytanie z regionu dziennika (raw_read) lub z t->images/blknos w pamieci: */
    /* PROSTO: licz z pamieci — obrazy w t->images[], deskryptory odtworzone z t->blknos[] */
```

Implementacja (z pamięci, deterministyczna): policz CRC inkrementalnie nad bajtami
deskryptora i obrazów dokładnie tak, jak zapisane na dysku. Najprościej — policz CRC nad
**obrazami** (`t->images[0..n-1]`, każdy GH_BLOCK_SIZE) i nad **numerami bloków**
(`t->blknos[0..n-1]`), łącząc w jeden CRC. Użyj pomocniczego akumulatora; jeśli masz tylko
`gh_crc32(buf,len)` (bez stanu), policz CRC nad tymczasowym buforem reprezentującym
deskryptory ORAZ osobno nad obrazami i połącz przez XOR z długością — **najprościej i
jednoznacznie**: zbuduj CRC jako:

```c
    uint32_t jc = gh_crc32(t->blknos, (size_t)t->n * sizeof(uint64_t));
    for (uint32_t i = 0; i < t->n; i++)
        jc ^= gh_crc32(t->images[i], GH_BLOCK_SIZE);
    h.csum = jc;
```

(Zapisz `h.csum` w nagłówku przed `raw_write(js, blk)`.) W `gh_jrnl_recover`, po wczytaniu
nagłówka i walidacji (magic, committed, bounds), PRZED odtworzeniem: wczytaj deskryptor i
obrazy, przelicz `jc` w ten sam sposób (numery z deskryptora → tablica `targets`; obrazy z
`raw_read`), i jeśli `jc != h.csum` → **rozdarty zapis** → `return 0` (nie odtwarzaj).
Inaczej odtwórz jak dotąd.

(Uwaga: w recover numery bloków to `targets[]` wczytane z deskryptora — policz
`gh_crc32(targets, n*8)` analogicznie; obrazy `raw_read(js+1+dblocks+i)`.)

- [ ] **Step 3: Test wykrycia korupcji danych (`tests/test_fs.c`)** (+ RUN_TEST):

```c
#include <fcntl.h>
static void test_csum_detect(void) {
    char tmp[] = "/tmp/ghost_csXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 2048, 128), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
    CHECK_EQ(gh_fs_create(&fs, "/f", 0644), 0);
    const char *m = "dane-do-ochrony-checksumem-1234567890";
    CHECK_EQ(gh_fs_write(&fs, "/f", m, strlen(m), 0), (ssize_t)strlen(m));
    /* znajdz fizyczny blok danych: pierwszy blok pliku */
    struct gh_inode n; uint64_t ino; CHECK_EQ(gh_fs_getattr(&fs, "/f", &n, &ino), 0);
    uint64_t phys = n.direct[0];
    gh_fs_sync(&fs);          /* utrwal na dysk */
    gh_fs_unmount(&fs);

    /* zdrowy odczyt po remount */
    CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
    char buf[64] = {0};
    CHECK_EQ(gh_fs_read(&fs, "/f", buf, sizeof(buf), 0), (ssize_t)strlen(m));
    gh_fs_unmount(&fs);

    /* uszkodz surowo blok danych (1 bajt) */
    int rf = open(tmp, O_RDWR);
    uint8_t b; pread(rf, &b, 1, (off_t)phys * GH_BLOCK_SIZE + 10);
    b ^= 0xFF; pwrite(rf, &b, 1, (off_t)phys * GH_BLOCK_SIZE + 10);
    close(rf);

    /* odczyt uszkodzonego -> EIO (korupcja wykryta) */
    CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
    memset(buf, 0, sizeof(buf));
    CHECK_EQ(gh_fs_read(&fs, "/f", buf, sizeof(buf), 0), -EIO);
    gh_fs_unmount(&fs); unlink(tmp);
}
```

(Uwaga: kontener nieszyfrowany → blok danych na dysku to jawna treść; XOR bajtu psuje CRC.)

- [ ] **Step 4: Test CRC dziennika (`tests/test_journal.c`)** (+ RUN_TEST)

Rozszerz `test_recover_redo`-podobny: spreparuj nagłówek `committed=1` z **poprawnym**
`csum` (policz jak w flush) → recover odtwarza; z **błędnym** `csum` (po uszkodzeniu
obrazu w regionie dziennika) → recover NIE odtwarza (cel niezmieniony). Dodaj funkcję
`test_journal_csum` która: preparuje 1-blokową transakcję w dzienniku (deskryptor+obraz+
nagłówek z poprawnym csum), recover → cel ma obraz; potem psuje obraz w regionie dziennika
i ustawia committed=1 ze STARYM csum, recover → cel NIE zmieniony (rozdarcie wykryte).

- [ ] **Step 5: Build + testy + ASan**

Run: `make clean && make test && make test-asan`
Expected: wszystkie `0 failed` (test_csum_detect wykrywa korupcję; test_journal_csum;
regresja A–I przezroczysta — sumy spójne). Brak raportów ASan.

- [ ] **Step 6: Integracja + crash-sweep + urządzenie blokowe**

Run: `make cli fuse && ./tests/integration.sh && ./tests/integration_blockdev.sh`
Expected: wszystkie `OK:`/`WSZYSTKIE ... PRZESZŁY` (sumy journalowane → crash-sweep i recover
po kill -9 dają `fsck==0`, brak fałszywych alarmów korupcji). Posprzątaj mounty.

- [ ] **Step 7: Commit**

```bash
git add src/block.c src/journal.c tests/test_fs.c tests/test_journal.c
git commit -m "feat: sumy kontrolne per blok (wykrywanie korupcji, EIO) + CRC dziennika"
```

---

## Task 3: Regresja + benchmark narzutu

**Files:** Modify: `tests/test_bench.c`

- [ ] **Step 1: Benchmark z sumami** — `tests/test_bench.c` formatuje przez `gh_format_enc`
(sumy domyślnie on); zmierz create+write/odczyt z sumami (narzut amortyzowany przez group
commit + cache). Asercje poprawności bez zmian. Wypisz czasy (porównaj z I — narzut sum
powinien być umiarkowany).

- [ ] **Step 2: Pełna regresja**

Run: `make clean && make test && make test-asan`
Expected: wszystkie `0 failed`; czasy bench wypisane (narzut sum akceptowalny dzięki I+F).

- [ ] **Step 3: Commit**

```bash
git add tests/test_bench.c
git commit -m "test: benchmark z sumami kontrolnymi (narzut amortyzowany przez group commit + cache)"
```

---

## Self-Review (przy pisaniu planu)

**Pokrycie spec J:** CRC32 (Task 1) ✓; region sum + format/mount + sb_csum + jheader.csum
(Task 1) ✓; sumy w gh_block_read/write + CRC dziennika (Task 2) ✓; wykrycie korupcji
danych/dziennika (Task 2) ✓; benchmark narzutu (Task 3) ✓.
**Placeholdery:** brak; pełny kod crc32/gh_block/journal CRC.
**Spójność:** `gh_crc32` (csum.h); `csum_start`/`csum_blocks`/`sb_csum`/`GH_SB_CHECKSUMS`/
`gh_jheader.csum` (ghostfs.h); pola dev (block.h) ustawiane w format/mount; `gh_is_csummed`
+ rekurencja bazowa w block.c. Layout `[super][mapa][i-węzły][dziennik][sumy][dane]`.
**Ryzyka:** (1) rekurencja gh_block — region sum wyłączony (baza); (2) suma+dane atomowo
(oba przez gh_block_write→txn→journal) → brak desync po awarii (crash-sweep fsck==0); (3)
suma na jawnej treści (kompozycja z szyfrowaniem); (4) narzut: zapis sum amortyzowany (I),
odczyt sum cache'owany (F); (5) Task 1 zostawia build spójny (gh_block niezmienione), Task 2
aktywuje.
