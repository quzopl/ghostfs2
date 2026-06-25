# ghostfs pod-projekt P — ordered-data journaling: plan implementacji

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps `- [ ]`.
> KRYTYCZNE: crash-consistency (test_crash sweep `fsck==0`) to TWARDA BRAMKA — zmiana dotyka ścieżki zapisu danych.

**Goal:** Nowo-alokowane bloki zwykłych plików zapisywane bezpośrednio (nie przez dziennik); tylko metadane journalowane. Eliminuje 2× amplifikację + mniej fsync. Zgodnie ze spec `docs/superpowers/specs/2026-06-24-ghostfs-P-ordered-data-design.md`.

**Tech Stack:** C11, libfuse3, OpenSSL, mini-harness, ASan.

## Global Constraints
- Testy przez `make`; gołe `cc`: `-D_POSIX_C_SOURCE=200809L -lcrypto -lpthread`.
- Bramka: `test_crash` sweep `fsck==0`; regresja A–O `0 failed`; ASan = 0.
- Zakres: TYLKO `GH_FILE`, TYLKO nowo-alokowane bloki liści. Reszta bez zmian.

## File Structure
| Plik | Zmiana |
|---|---|
| `src/alloc.c`/`alloc.h` | `gh_alloc_block_nz` (alokacja bez zerowania); refaktor wspólnego impl |
| `src/block.c`/`block.h` | `gh_block_write_direct` (suma journalowana + dane bezpośrednio) |
| `src/inode.c` | `bmap` +`leaf_direct`/`out_newleaf`; `gh_inode_pwrite` kieruje nowe bloki pliku na direct |
| `tests/test_inode.c`, `tests/test_crash.c` | testy poprawności + crash |

---

## Task 1: Alokacja bez zerowania + zapis bezpośredni

**Files:** Modify `src/alloc.c`, `src/alloc.h`, `src/block.c`, `src/block.h`

- [ ] **Step 1: Refaktor `gh_alloc_block` + `gh_alloc_block_nz` (`src/alloc.c`)**

Zastąp ciało `gh_alloc_block` wspólnym helperem z flagą `zero`:

```c
static int alloc_impl(struct gh_dev *dev, const struct gh_superblock *sb, uint64_t *out, int zero) {
    if (!out) return -EINVAL;
    uint64_t start = dev->hint_block;
    if (start < sb->data_start || start >= sb->total_blocks) start = sb->data_start;
    for (int pass = 0; pass < 2; pass++) {
        uint64_t lo = (pass == 0) ? start : sb->data_start;
        uint64_t hi = (pass == 0) ? sb->total_blocks : start;
        for (uint64_t b = lo; b < hi; b++) {
            int set = 0; int r = gh_bitmap_test(dev, sb, b, &set); if (r) return r;
            if (!set) {
                r = bitmap_rw(dev, sb, b, 1, 1, NULL); if (r) return r;
                if (zero) {
                    uint8_t z[GH_BLOCK_SIZE]; memset(z, 0, sizeof(z));
                    r = gh_block_write(dev, b, z); if (r) return r;
                }
                dev->hint_block = b + 1;
                *out = b; return 0;
            }
        }
    }
    return -ENOSPC;
}
int gh_alloc_block(struct gh_dev *dev, const struct gh_superblock *sb, uint64_t *out) {
    return alloc_impl(dev, sb, out, 1);
}
int gh_alloc_block_nz(struct gh_dev *dev, const struct gh_superblock *sb, uint64_t *out) {
    return alloc_impl(dev, sb, out, 0);   /* bez zerowania: caller pisze pelny blok bezposrednio */
}
```

W `src/alloc.h` dodaj deklarację `gh_alloc_block_nz` obok `gh_alloc_block`.

- [ ] **Step 2: `gh_block_write_direct` (`src/block.c`)**

Dodaj po `gh_block_write` (kopiuje część sumy z `gh_block_write`, pisze dane bezpośrednio):

```c
/* Zapis bloku danych z pominieciem dziennika: SUMA journalowana (spojna po commicie),
   DANE bezposrednio na dysk (pwrite + cache). Tylko dla NOWO-alokowanych blokow plikow,
   ktore nie sa jeszcze referencyjne do czasu commitu metadanych (mapa/i-wezel). */
int gh_block_write_direct(struct gh_dev *dev, uint64_t blkno, const void *buf) {
    if (gh_is_csummed(dev, blkno)) {
        uint32_t crc = gh_crc32(buf, GH_BLOCK_SIZE);
        uint64_t cb = dev->csum_start + (blkno * 4) / GH_BLOCK_SIZE;
        uint32_t coff = (uint32_t)((blkno * 4) % GH_BLOCK_SIZE);
        uint8_t cbuf[GH_BLOCK_SIZE];
        int r = gh_block_read(dev, cb, cbuf); if (r) return r;
        memcpy(cbuf + coff, &crc, 4);
        r = gh_block_write(dev, cb, cbuf); if (r) return r;   /* suma: do txn (journalowana) */
    }
    return gh_disk_write(dev, blkno, buf);                     /* dane: bezposrednio */
}
```

W `src/block.h` dodaj deklarację `gh_block_write_direct` obok `gh_block_write`.

- [ ] **Step 3: Build sanity**

Run: `make cli 2>&1 | tail -2` → kompiluje się (nowe symbole użyte w Task 2).

---

## Task 2: Kierowanie nowych bloków plików na zapis bezpośredni

**Files:** Modify `src/inode.c`; Test `tests/test_inode.c`

- [ ] **Step 1: `bmap` +`leaf_direct`/`out_newleaf` (`src/inode.c`)**

Zmień sygnaturę i logikę alokacji liścia (bloki wskaźnikowe BEZ zmian — zawsze `gh_alloc_block`):

```c
static int bmap(struct gh_dev *dev, const struct gh_superblock *sb,
                struct gh_inode *node, uint64_t lbn, int alloc, uint64_t *out,
                int leaf_direct, int *out_newleaf) {
    if (out_newleaf) *out_newleaf = 0;
    if (lbn < GH_NDIRECT) {
        if (node->direct[lbn] == 0) {
            if (!alloc) { *out = 0; return 0; }
            int r = leaf_direct ? gh_alloc_block_nz(dev, sb, &node->direct[lbn])
                                : gh_alloc_block(dev, sb, &node->direct[lbn]);
            if (r) return r;
            if (out_newleaf) *out_newleaf = 1;
        }
        *out = node->direct[lbn]; return 0;
    }
    lbn -= GH_NDIRECT;
    if (lbn < GH_PTRS_PER_BLK) {
        if (node->indirect == 0) {
            if (!alloc) { *out = 0; return 0; }
            int r = gh_alloc_block(dev, sb, &node->indirect); if (r) return r;   /* wskaznikowy: zerowany */
        }
        uint64_t ptrs[GH_PTRS_PER_BLK];
        int r = gh_block_read(dev, node->indirect, ptrs); if (r) return r;
        if (ptrs[lbn] == 0) {
            if (!alloc) { *out = 0; return 0; }
            r = leaf_direct ? gh_alloc_block_nz(dev, sb, &ptrs[lbn])
                            : gh_alloc_block(dev, sb, &ptrs[lbn]);
            if (r) return r;
            if (out_newleaf) *out_newleaf = 1;
            r = gh_block_write(dev, node->indirect, ptrs); if (r) return r;
        }
        *out = ptrs[lbn]; return 0;
    }
    lbn -= GH_PTRS_PER_BLK;
    if (lbn < (uint64_t)GH_PTRS_PER_BLK * GH_PTRS_PER_BLK) {
        if (node->double_indirect == 0) {
            if (!alloc) { *out = 0; return 0; }
            int r = gh_alloc_block(dev, sb, &node->double_indirect); if (r) return r;   /* wskaznikowy */
        }
        uint64_t l1[GH_PTRS_PER_BLK];
        int r = gh_block_read(dev, node->double_indirect, l1); if (r) return r;
        uint64_t i1 = lbn / GH_PTRS_PER_BLK, i2 = lbn % GH_PTRS_PER_BLK;
        if (l1[i1] == 0) {
            if (!alloc) { *out = 0; return 0; }
            r = gh_alloc_block(dev, sb, &l1[i1]); if (r) return r;   /* wskaznikowy: zerowany */
            r = gh_block_write(dev, node->double_indirect, l1); if (r) return r;
        }
        uint64_t l2[GH_PTRS_PER_BLK];
        r = gh_block_read(dev, l1[i1], l2); if (r) return r;
        if (l2[i2] == 0) {
            if (!alloc) { *out = 0; return 0; }
            r = leaf_direct ? gh_alloc_block_nz(dev, sb, &l2[i2])
                            : gh_alloc_block(dev, sb, &l2[i2]);
            if (r) return r;
            if (out_newleaf) *out_newleaf = 1;
            r = gh_block_write(dev, l1[i1], l2); if (r) return r;
        }
        *out = l2[i2]; return 0;
    }
    return -EFBIG;
}
```

Zaktualizuj 2 istniejące wywołania (czytające, alloc=0) — `src/inode.c:117` (pread) i `:297`
(truncate): dodaj `, 0, NULL` na końcu:
```c
int r = bmap(dev, sb, node, lbn, 0, &phys, 0, NULL);
```

- [ ] **Step 2: `gh_inode_pwrite` — direct dla nowych bloków pliku (`src/inode.c`)**

```c
ssize_t gh_inode_pwrite(struct gh_dev *dev, const struct gh_superblock *sb,
                        uint64_t ino, struct gh_inode *node,
                        const void *buf, size_t n, uint64_t off) {
    size_t done = 0; uint8_t blk[GH_BLOCK_SIZE];
    int direct = (node->type == GH_FILE);     /* tylko dane zwyklych plikow */
    while (done < n) {
        uint64_t lbn = (off + done) / GH_BLOCK_SIZE;
        uint64_t boff = (off + done) % GH_BLOCK_SIZE;
        size_t chunk = GH_BLOCK_SIZE - boff;
        if (chunk > n - done) chunk = n - done;
        int newleaf = 0;
        uint64_t phys; int r = bmap(dev, sb, node, lbn, 1, &phys, direct, &newleaf);
        if (r) return r;
        if (direct && newleaf) {
            /* nowy blok pliku: zbuduj pelny blok (zera+dane), zapisz BEZPOSREDNIO (bez RMW) */
            if (chunk != GH_BLOCK_SIZE) memset(blk, 0, GH_BLOCK_SIZE);
            memcpy(blk + boff, (const uint8_t*)buf + done, chunk);
            r = gh_block_write_direct(dev, phys, blk); if (r) return r;
        } else {
            /* nadpisanie istniejacego / katalog / inne: jak dotad (journalowane) */
            if (chunk != GH_BLOCK_SIZE) { r = gh_block_read(dev, phys, blk); if (r) return r; }
            memcpy(blk + boff, (const uint8_t*)buf + done, chunk);
            r = gh_block_write(dev, phys, blk); if (r) return r;
        }
        done += chunk;
    }
    if (off + n > node->size) node->size = off + n;
    node->mtime = (uint64_t)time(NULL);
    int r = gh_inode_write(dev, sb, ino, node); if (r) return r;
    return (ssize_t)done;
}
```

- [ ] **Step 3: Test poprawności (`tests/test_inode.c`, +RUN_TEST)**

```c
static void test_ordered_data_write(void) {
    char tmp[] = "/tmp/ghost_odXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 4096, 64), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
    /* a) wielo-blokowy + czesciowy ostatni blok */
    CHECK_EQ(gh_fs_create(&fs, "/big", 0644), 0);
    size_t N = 4096*3 + 1234; char *w = malloc(N); for (size_t i=0;i<N;i++) w[i]=(char)(i*7+1);
    CHECK_EQ(gh_fs_write(&fs, "/big", w, N, 0), (ssize_t)N);
    char *rd = malloc(N); CHECK_EQ(gh_fs_read(&fs, "/big", rd, N, 0), (ssize_t)N);
    CHECK_EQ(memcmp(w, rd, N), 0);                    /* read-your-writes (direct) */
    /* b) dziura: zapis na offsecie 8192, [0,8192) to dziura -> zera */
    CHECK_EQ(gh_fs_create(&fs, "/sparse", 0644), 0);
    CHECK_EQ(gh_fs_write(&fs, "/sparse", "XYZ", 3, 8192), 3);
    char hole[8195]; CHECK_EQ(gh_fs_read(&fs, "/sparse", hole, sizeof(hole), 0), (ssize_t)sizeof(hole));
    for (int i=0;i<8192;i++) CHECK_EQ(hole[i], 0);    /* dziura = zera (nie smieci z nz) */
    CHECK_EQ(memcmp(hole+8192, "XYZ", 3), 0);
    /* c) nadpisanie istniejacego bloku (journalowana sciezka) */
    CHECK_EQ(gh_fs_write(&fs, "/big", "OVER", 4, 0), 4);
    CHECK_EQ(gh_fs_read(&fs, "/big", rd, 4, 0), 4); CHECK_EQ(memcmp(rd, "OVER", 4), 0);
    /* d) fsck czysty */
    int issues=-1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    free(w); free(rd);
    gh_fs_unmount(&fs); unlink(tmp);
}
```
W `main` dodaj `RUN_TEST(test_ordered_data_write);`. (Jeśli test_inode.c nie linkuje
`gh_fs_*`, użyj `tests/test_fs.c` zamiast tego — sprawdź nagłówki.)

- [ ] **Step 4: Crash + durability test (`tests/test_crash.c`, +RUN_TEST)**

Dodaj scenariusz: nowy plik z danymi, awaria PRZED commitem (fail_after w trakcie flushu) →
`fsck==0`, oraz po pełnym zapisie+sync+remount dane trwałe. Wzoruj się na istniejących testach
crash (fail_after na gh_disk_write, świeży remount, gh_fsck). KRYTYCZNE: po awarii brak bloku
referencyjnego z błędną sumą (żaden odczyt pliku po recover nie zwraca EIO).

- [ ] **Step 5: Pełna regresja + ASan + crash-sweep**

Run: `make clean && make test && make test-asan`
Expected: wszystkie `0 failed`. **`test_crash` (sweep N) `fsck==0` dla każdego N** — TWARDA
BRAMKA. Jeśli jakieś N daje issues>0 lub EIO na pliku po recover → ZATRZYMAJ (BLOCKED).

- [ ] **Step 6: Integracja FUSE + urządzenie blokowe + szyfrowanie**

Run: `make cli fuse && ./tests/integration.sh && ./tests/integration_blockdev.sh`
Expected: WSZYSTKIE PRZESZŁY (round-trip, edycja in-place, kill -9 + recover → fsck czysty,
at-rest encryption — direct zapis też szyfruje). Posprzątaj mounty.

- [ ] **Step 7: Commit**

```bash
git add src/alloc.c src/alloc.h src/block.c src/block.h src/inode.c tests/test_inode.c tests/test_crash.c
git commit -m "perf: ordered-data journaling (nowe bloki plikow zapisywane bezposrednio, tylko metadane w dzienniku)"
```

---

## Self-Review (przy pisaniu planu)
**Pokrycie spec P:** gh_alloc_block_nz (T1) ✓; gh_block_write_direct (T1) ✓; bmap leaf_direct/
newleaf (T2) ✓; gh_inode_pwrite direct dla nowych bloków pliku (T2) ✓; testy poprawności+
crash+szyfrowanie (T2) ✓.
**Placeholdery:** brak (kod podany; crash-test wzorowany na istniejących — implementer ma je
przeczytać).
**Spójność:** `leaf_direct`/`out_newleaf` w bmap i 3 wywołaniach; `direct=(type==GH_FILE)`;
bloki wskaźnikowe zawsze zerowane+journalowane; nowy liść pliku zawsze pełny-blok-direct (bez
RMW na śmieciach z nz).
**Ryzyka:** (1) KRYTYCZNE crash-consistency — direct dane trwałe przed commitem metadanych
(fsync fazy-1); awaria przed commitem → blok wolny/niereferencyjny → bramka test_crash
fsck==0 + brak EIO; (2) read-your-writes — blok nie w buforze txn → odczyt z cache/dysku
(gh_disk_write write-through aktualizuje cache); (3) nz-śmieci nigdy nie eksponowane — nowy
liść pliku zawsze zapisywany pełnym blokiem (zera w niezapisanych częściach); (4) katalogi/
nadpisania niezmienione (journalowane) — ograniczony obszar ryzyka.
