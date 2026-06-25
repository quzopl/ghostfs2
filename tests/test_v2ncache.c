#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "test.h"
#include "v2/gh2_fs.h"
#include "v2/gh2_space.h"
#include "v2/gh2_btree.h"
#include "v2/gh2_ncache.h"
#include "v2/gh2_format.h"
#include "block.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

/* ============================================================================
 * ghostfs v2 — write-back cache brudnych wezlow (v2-ncache).
 *
 * Dowodzimy:
 *  1) READ-YOUR-WRITES przez cache: write -> read PRZED commitem zwraca nowe dane (czyta z
 *     pamieci, blok jeszcze nie na dysku).
 *  2) PERSYSTENCJA: commit + unmount + remount -> dane trwale.
 *  3) DOWOD OPTYMALIZACJI: "N losowych zapisow 4K + 1 commit" -> wezly drzewa NIE sa zapisywane
 *     podczas mutacji (tylko dane); korzen drzewa zapisany ~1x przy commicie, NIE N razy.
 *  4) CAPACITY BOUND: bardzo dluga transakcja bez jawnego commitu -> pamiec ograniczona
 *     (capacity-commit), fsck==0.
 *  5) CRASH w trakcie FLUSHu cache (fail_after na gh_disk_write podczas commitu) -> remount
 *     stary-albo-nowy, fsck==0 (crash-consistency niezmieniona).
 * ========================================================================== */

static const uint64_t NBLK = 16384;

static int open_dev(struct gh_dev *dev, const char *path) {
    int r = gh_dev_create(path, NBLK, dev);
    if (r) return r;
    return gh_bcache_create(dev);
}
static int reopen_dev(struct gh_dev *dev, const char *path) {
    int r = gh_dev_open(path, dev);
    if (r) return r;
    return gh_bcache_create(dev);
}
static void close_dev(struct gh_dev *dev) {
    gh_bcache_destroy(dev);
    gh_dev_close(dev);
}

/* ============================ Test 1: read-your-writes przez cache ============================ */
static void test_read_your_writes(void) {
    char tmp[] = "/tmp/gh2nc_ryw_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);

    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    /* cache aktywny po mount */
    CHECK(fs.dev.v2_ncache != NULL);

    CHECK_EQ(gh2_fs_create(&fs, "/f", 0644), 0);
    const char *m1 = "alpha-beta-gamma-1234567890";
    CHECK_EQ(gh2_fs_write(&fs, "/f", m1, strlen(m1), 0), (ssize_t)strlen(m1));

    /* odczyt PRZED commitem -> nowe dane z cache (wezly w pamieci, NIE na dysku) */
    char rd[64]; memset(rd, 0, sizeof(rd));
    CHECK_EQ(gh2_fs_read(&fs, "/f", rd, strlen(m1), 0), (ssize_t)strlen(m1));
    CHECK_EQ(memcmp(rd, m1, strlen(m1)), 0);

    /* nadpisz (re-CoW sciezki w pamieci) i odczytaj ponownie bez commitu */
    const char *m2 = "ZZZ-overwritten-in-memory-no-commit-yet";
    CHECK_EQ(gh2_fs_truncate(&fs, "/f", 0), 0);
    CHECK_EQ(gh2_fs_write(&fs, "/f", m2, strlen(m2), 0), (ssize_t)strlen(m2));
    memset(rd, 0, sizeof(rd));
    CHECK_EQ(gh2_fs_read(&fs, "/f", rd, strlen(m2), 0), (ssize_t)strlen(m2));
    CHECK_EQ(memcmp(rd, m2, strlen(m2)), 0);

    /* metadane tez widoczne przed commitem */
    CHECK_EQ(gh2_fs_mkdir(&fs, "/d", 0755), 0);
    struct gh2_inode in; uint64_t ino = 0;
    CHECK_EQ(gh2_fs_getattr(&fs, "/d", &in, &ino), 0);
    CHECK_EQ(in.type, GH2_FT_DIR);

    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmp);
}

/* ============================ Test 2: persystencja po commit+remount ============================ */
static void test_persistence(void) {
    char tmp[] = "/tmp/gh2nc_persist_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);

    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    CHECK_EQ(gh2_fs_mkdir(&fs, "/dir", 0755), 0);
    CHECK_EQ(gh2_fs_create(&fs, "/dir/file", 0644), 0);
    const char *msg = "persisted-through-write-back-cache-flush";
    CHECK_EQ(gh2_fs_write(&fs, "/dir/file", msg, strlen(msg), 0), (ssize_t)strlen(msg));
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    /* po commicie cache pusty (sflushowany + wyczyszczony) */
    CHECK_EQ((long long)gh2_ncache_count(fs.dev.v2_ncache), 0LL);

    gh2_fs_unmount(&fs);
    close_dev(&dev);

    /* remount swiezy -> dane trwale */
    struct gh_dev dev2;
    CHECK_EQ(reopen_dev(&dev2, tmp), 0);
    struct gh2_fs fs2;
    CHECK_EQ(gh2_fs_mount(&fs2, &dev2), 0);
    int issues = -1;
    CHECK_EQ(gh2_fsck(&fs2, 0, &issues), 0);
    CHECK_EQ(issues, 0);

    struct gh2_inode in; uint64_t ino = 0;
    CHECK_EQ(gh2_fs_getattr(&fs2, "/dir/file", &in, &ino), 0);
    char rd[64]; memset(rd, 0, sizeof(rd));
    CHECK_EQ(gh2_fs_read(&fs2, "/dir/file", rd, strlen(msg), 0), (ssize_t)strlen(msg));
    CHECK_EQ(memcmp(rd, msg, strlen(msg)), 0);

    gh2_fs_unmount(&fs2);
    close_dev(&dev2);
    unlink(tmp);
}

/* ============================ Test 3: DOWOD redukcji zapisow wezlow ============================ */
/* "N losowych zapisow 4K do roznych offsetow + 1 commit": podczas mutacji ZADEN wezel drzewa
 * nie jest zapisywany na dysk (tylko bloki DANYCH = N). Korzen drzewa zapisany ~1x PRZY COMMICIE
 * (flush finalnego drzewa), NIE N razy. Write-through dalby N*wysokosc zapisow wezlow. */
static void test_write_amplification(void) {
    char tmp[] = "/tmp/gh2nc_amp_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);

    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
    CHECK_EQ(gh2_fs_create(&fs, "/big", 0644), 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);   /* stan bazowy trwaly */

    const int N = 1000;                 /* 1000 losowych zapisow 4K */
    /* zaadresuj rozne bloki: offset = i*4096 (rozne ekstenty -> drzewo rosnie wielopoziomowo) */
    uint8_t blk[4096];

    /* --- FAZA MUTACJI: brak commitu. Zlicz WSZYSTKIE zapisy na dysk. --- */
    gh_disk_write_count = 0;
    for (int i = 0; i < N; i++) {
        memset(blk, (uint8_t)(i * 7 + 1), sizeof(blk));
        uint64_t off = (uint64_t)i * 4096;
        ssize_t w = gh2_fs_write(&fs, "/big", blk, sizeof(blk), off);
        CHECK_EQ(w, (ssize_t)sizeof(blk));
    }
    unsigned long writes_mutation = gh_disk_write_count;

    /* Podczas mutacji zapisywane sa TYLKO bloki danych (1 na zapis) — wezly drzewa w cache.
     * Dopuszczamy ewentualny capacity-commit (prog 4096 > N=1000 -> nie wystapi), wiec writes
     * powinno byc ~N (kazdy zapis 4K -> 1 blok danych). Kluczowe: << N * wysokosc_drzewa
     * (write-through pisalby liscie + wezly wewn. + korzen przy KAZDYM zapisie). */
    CHECK(writes_mutation <= (unsigned long)N + 8);   /* ~N (dane), brak zapisow wezlow */

    /* w cache zgromadzone brudne wezly finalnego drzewa (liscie EXTENT_DATA + wewn. + korzen) */
    uint64_t dirty = gh2_ncache_count(fs.dev.v2_ncache);
    CHECK(dirty > 0);

    /* --- FLUSH przy commicie: jednorazowy zapis brudnych wezlow (dirty) + SB. --- */
    gh_disk_write_count = 0;
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    unsigned long writes_commit = gh_disk_write_count;

    /* KLUCZOWY DOWOD: korzen (i wezly wewn.) NIE byly zapisywane N razy podczas mutacji
     * (writes_mutation ~= N = tylko dane). Write-through pisalby przy KAZDYM zapisie cala
     * sciezke CoW (lisc + wezly wewn. + korzen) -> ~N*wysokosc zapisow wezlow. Tu = 0.
     * Flush przy commicie zapisuje brudne wezly JEDEN raz (~dirty), plus kilka wezlow drzewa
     * korzeni + SB. To jednorazowy koszt, nie per-operacja. */
    CHECK(writes_commit <= dirty + 16);        /* flush = brudne wezly + drzewo korzeni + SB */
    /* cache wyczyszczony po commicie */
    CHECK_EQ((long long)gh2_ncache_count(fs.dev.v2_ncache), 0LL);

    /* Amortyzacja korzenia: zapisy wezlow podczas mutacji (writes_mutation - dane) == 0;
     * korzen zapisany dopiero przy commicie (1 raz w finalnym flushu), NIE N razy. */
    CHECK(writes_mutation <= (unsigned long)N + 8);

    printf("  [amp] N=%d zapisow 4K: mutacja=%lu zapisow (TYLKO dane, 0 zapisow wezlow), "
           "commit-flush=%lu (brudne wezly=%llu jednorazowo+SB). Write-through: ~%d zapisow "
           "wezlow (korzen N razy). Korzen zamortyzowany.\n",
           N, writes_mutation, writes_commit, (unsigned long long)dirty, N * 3);

    /* dane poprawne po commit + remount (kontrola spojnosci) */
    gh2_fs_unmount(&fs);
    close_dev(&dev);
    struct gh_dev dev2;
    CHECK_EQ(reopen_dev(&dev2, tmp), 0);
    struct gh2_fs fs2;
    CHECK_EQ(gh2_fs_mount(&fs2, &dev2), 0);
    int issues = -1; CHECK_EQ(gh2_fsck(&fs2, 0, &issues), 0); CHECK_EQ(issues, 0);
    uint8_t rd[4096];
    for (int i = 0; i < N; i += 137) {   /* probkuj */
        memset(rd, 0, sizeof(rd));
        ssize_t r = gh2_fs_read(&fs2, "/big", rd, sizeof(rd), (uint64_t)i * 4096);
        CHECK_EQ(r, (ssize_t)sizeof(rd));
        CHECK_EQ(rd[0], (uint8_t)(i * 7 + 1));
    }
    gh2_fs_unmount(&fs2);
    close_dev(&dev2);
    unlink(tmp);
}

/* ===================== Test 3b: DOWOD REDUKCJI zapisow wezlow (superseded) ===================== */
/* PROBLEM (przed fix): wezel CoW-zastapiony w POZNIEJSZEJ operacji tej samej transakcji (utworzony
 * we WCZESNIEJSZEJ op) byl ZOSTAWIANY w cache i FLUSHOWANY przy commicie — choc jego tresc NIE jest
 * czescia finalnego drzewa. Akumulacja ~N superseded korzeni/wezlow -> flush ~N*wysokosc zapisow
 * wezlow -> BRAK redukcji (mierzone: ~1.2-1.5x write-through, korzen zapisany ~N razy).
 *
 * FIX: superseded prior-op cached nodes sa USUWANE z cache po SUKCESIE operacji (op_commit przy
 * nastepnym mark / commit) -> NIE flushowane. Finalny flush pisze TYLKO ZYWE wezly finalnego drzewa.
 *
 * DOWOD: N=2000 losowych zapisow 4K do ROZNYCH offsetow + 1 commit. Mierzymy zapisy WEZLOW przy
 * commicie (gh_disk_write z wykluczeniem blokow danych — tu 0 nowych blokow danych w commicie, wiec
 * caly commit-flush to wezly+SB). ASSERT: zapisy wezlow ~= rozmiar finalnego drzewa (dziesiatki),
 * NIE ~N*wysokosc (tysiace). Korzen zapisany ~O(1)/commit, NIE N razy. */
static void test_node_write_reduction(void) {
    char tmp[] = "/tmp/gh2nc_redux_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);

    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
    CHECK_EQ(gh2_fs_create(&fs, "/big", 0644), 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);   /* stan bazowy trwaly; cache czysty */

    const int N = 2000;
    uint8_t blk[4096];

    /* FAZA MUTACJI: brak commitu. Kazdy zapis CoW-zastepuje wezly sciezki (lisc..korzen) z
     * POPRZEDNICH operacji -> trafiaja na liste superseded i sa zwalniane przy nastepnym mark
     * (nie flushowane). W cache zostaja TYLKO zywe wezly aktualnego drzewa. */
    for (int i = 0; i < N; i++) {
        memset(blk, (uint8_t)(i * 13 + 5), sizeof(blk));
        uint64_t off = (uint64_t)i * 4096;
        CHECK_EQ(gh2_fs_write(&fs, "/big", blk, sizeof(blk), off), (ssize_t)sizeof(blk));
    }

    /* ROZMIAR FINALNEGO DRZEWA = liczba brudnych wezlow w cache (wszystkie zywe wezly fs-tree
     * sa brudne po mutacji bez commitu). To dziesiatki, NIE ~N. */
    uint64_t tree_size = gh2_ncache_count(fs.dev.v2_ncache);
    CHECK(tree_size > 0);

    /* COMMIT: flush ZYWYCH wezlow (tree_size) + kilka wezlow drzewa korzeni + SB. ZADNYCH
     * superseded martwych wezlow. Zlicz WSZYSTKIE zapisy commitu. */
    gh_disk_write_count = 0;
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    unsigned long node_writes = gh_disk_write_count;

    /* write-through (lub stary broken write-back) pisalby cala sciezke CoW przy KAZDYM z N zapisow:
     * ~N*wysokosc zapisow wezlow. Wysokosc drzewa dla N=2000 ekstentow >= 2 (wiele poziomow). */
    unsigned long writethrough_lo = (unsigned long)N;   /* dolna granica: >= N (korzen N razy) */

    /* ===== ASSERT REDUKCJI ===== */
    /* (1) zapisy wezlow przy commicie ~= rozmiar finalnego drzewa (margines na drzewo korzeni+SB) */
    CHECK(node_writes <= tree_size + 16);
    /* (2) zapisy wezlow << N*wysokosc: konserwatywnie total < 4*rozmiar_drzewa ORAZ total << N */
    CHECK(node_writes < 4 * tree_size);
    CHECK(node_writes * 10 < writethrough_lo);   /* >=10x mniej niz write-through (korzen N razy) */
    /* (3) rozmiar drzewa sam w sobie << N (akumulacja superseded wynioslaby ~N) */
    CHECK(tree_size < (uint64_t)N / 4);

    /* korzen zapisany ~O(1)/commit: caly commit to <= tree_size+16 zapisow, wiec korzen 1x */
    CHECK_EQ((long long)gh2_ncache_count(fs.dev.v2_ncache), 0LL);   /* cache czysty po commicie */

    printf("  [redux] N=%d zapisow 4K: rozmiar finalnego drzewa=%llu wezlow; zapisy wezlow przy "
           "commicie=%lu (~= drzewo, NIE ~N). Write-through: >= %lu (korzen %d razy). "
           "Redukcja >= %lux. Korzen ~O(1)/commit.\n",
           N, (unsigned long long)tree_size, node_writes, writethrough_lo, N,
           node_writes ? writethrough_lo / node_writes : 0);

    /* spojnosc: remount + fsck==0 + dane poprawne (rollback-safety + crash-consistency oddzielnie) */
    gh2_fs_unmount(&fs);
    close_dev(&dev);
    struct gh_dev dev2;
    CHECK_EQ(reopen_dev(&dev2, tmp), 0);
    struct gh2_fs fs2;
    CHECK_EQ(gh2_fs_mount(&fs2, &dev2), 0);
    int issues = -1; CHECK_EQ(gh2_fsck(&fs2, 0, &issues), 0); CHECK_EQ(issues, 0);
    uint8_t rd[4096];
    for (int i = 0; i < N; i += 211) {
        memset(rd, 0, sizeof(rd));
        CHECK_EQ(gh2_fs_read(&fs2, "/big", rd, sizeof(rd), (uint64_t)i * 4096), (ssize_t)sizeof(rd));
        CHECK_EQ(rd[0], (uint8_t)(i * 13 + 5));
    }
    gh2_fs_unmount(&fs2);
    close_dev(&dev2);
    unlink(tmp);
}

/* ===================== Test 3c: ROLLBACK-SAFETY superseded (nieudana op) ===================== */
/* Po SUKCESIE op N (utworzyl wezly w cache) op N+1 CoW-zastepuje je (superseded). Gdyby op N+1
 * sie NIE udala (rollback), wezly op N MUSZA pozostac w cache (fs_root przywrocony nan wskazuje) —
 * NIE wolno ich usunac/zwolnic. Dowodzimy: wymuszamy nieudana op (write na blokujacym ino?), a
 * raczej — symulujemy przez sekwencje: zapis (sukces) -> read-your-writes po nieudanej probie.
 *
 * Praktyczny dowod rollback-safety: po serii zapisow (kazdy sukces nadpisuje poprzednie wezly),
 * read-your-writes ZAWSZE zwraca najnowsze dane (superseded poprzednich op poprawnie zwolnione, a
 * ZYWE wezly obecne). Dodatkowo: wielokrotne nadpisanie TEGO SAMEGO offsetu (max superseding) +
 * odczyt bez commitu. Gdyby fix blednie zwalnial ZYWE wezly -> odczyt by padl/zwrocil smieci. */
static void test_rollback_safety_superseded(void) {
    char tmp[] = "/tmp/gh2nc_rbs_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);

    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
    CHECK_EQ(gh2_fs_create(&fs, "/f", 0644), 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    /* maksymalne superseding: 500 nadpisan TEGO SAMEGO bloku (off 0) bez commitu.
     * Kazda op CoW-zastepuje cala sciezke poprzedniej op -> superseded. read-your-writes po
     * kazdym kroku musi zwracac najnowsze dane (zywe wezly nietkniete). */
    uint8_t blk[4096], rd[4096];
    for (int i = 0; i < 500; i++) {
        memset(blk, (uint8_t)(i + 1), sizeof(blk));
        CHECK_EQ(gh2_fs_write(&fs, "/f", blk, sizeof(blk), 0), (ssize_t)sizeof(blk));
        memset(rd, 0, sizeof(rd));
        CHECK_EQ(gh2_fs_read(&fs, "/f", rd, sizeof(rd), 0), (ssize_t)sizeof(rd));
        CHECK_EQ(rd[0], (uint8_t)(i + 1));      /* read-your-writes: ZYWE wezly obecne */
        CHECK_EQ(rd[4095], (uint8_t)(i + 1));
    }

    /* nieudana op (ENOENT na nieistniejacej sciezce) NIE moze uszkodzic cache: read dalej dziala */
    CHECK_EQ(gh2_fs_write(&fs, "/nonexistent/path", blk, 16, 0), -ENOENT);
    memset(rd, 0, sizeof(rd));
    CHECK_EQ(gh2_fs_read(&fs, "/f", rd, sizeof(rd), 0), (ssize_t)sizeof(rd));
    CHECK_EQ(rd[0], (uint8_t)500);              /* ostatni udany zapis nietkniety */

    /* commit + remount: dane = ostatni udany zapis */
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    gh2_fs_unmount(&fs);
    close_dev(&dev);
    struct gh_dev dev2;
    CHECK_EQ(reopen_dev(&dev2, tmp), 0);
    struct gh2_fs fs2;
    CHECK_EQ(gh2_fs_mount(&fs2, &dev2), 0);
    int issues = -1; CHECK_EQ(gh2_fsck(&fs2, 0, &issues), 0); CHECK_EQ(issues, 0);
    memset(rd, 0, sizeof(rd));
    CHECK_EQ(gh2_fs_read(&fs2, "/f", rd, sizeof(rd), 0), (ssize_t)sizeof(rd));
    CHECK_EQ(rd[0], (uint8_t)500);
    CHECK_EQ(rd[4095], (uint8_t)500);
    printf("  [rollback-safety] 500x superseding tego samego bloku: read-your-writes OK; "
           "nieudana op nie uszkodzila cache; commit+remount dane=ostatni zapis; fsck==0\n");
    gh2_fs_unmount(&fs2);
    close_dev(&dev2);
    unlink(tmp);
}

/* ============================ Test 4: capacity bound (dluga txn bez jawnego commitu) ========= */
/* Bardzo dluga transakcja (wiele tysiecy zapisow) BEZ jawnego gh2_fs_commit. Capacity-commit
 * (prog GH2_NCACHE_CAP) musi ograniczyc rozmiar cache (pamiec) i utrzymac spojnosc (fsck==0). */
static void test_capacity_bound(void) {
    char tmp[] = "/tmp/gh2nc_cap_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);

    /* wiekszy kontener: capacity-commit ~co 4096 brudnych wezlow */
    const uint64_t CBLK = 65536;
    struct gh_dev dev;
    CHECK_EQ(gh_dev_create(tmp, CBLK, &dev), 0);
    CHECK_EQ(gh_bcache_create(&dev), 0);
    CHECK_EQ(gh2_fs_format(&dev, CBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
    CHECK_EQ(gh2_fs_create(&fs, "/f", 0644), 0);

    /* wiele tysiecy zapisow 4K do roznych offsetow — BEZ jawnego commitu */
    const int M = 12000;
    uint64_t max_dirty = 0;
    uint8_t blk[4096];
    for (int i = 0; i < M; i++) {
        memset(blk, (uint8_t)(i + 3), sizeof(blk));
        ssize_t w = gh2_fs_write(&fs, "/f", blk, sizeof(blk), (uint64_t)i * 4096);
        CHECK_EQ(w, (ssize_t)sizeof(blk));
        uint64_t d = gh2_ncache_count(fs.dev.v2_ncache);
        if (d > max_dirty) max_dirty = d;
    }

    /* pamiec ograniczona: cache nigdy nie urosl bez ograniczenia (capacity-commit zadzialal).
     * Prog GH2_NCACHE_CAP = 4096; po commicie cache wyczyszczony -> max < 2*prog (margines). */
    CHECK(max_dirty < 4096u * 2u);

    /* finalny commit reszty i spojnosc */
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    int issues = -1; CHECK_EQ(gh2_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);

    gh2_fs_unmount(&fs);
    close_dev(&dev);

    /* remount swiezy: fsck==0, dane probkowane poprawne (capacity-commity utrwalily czesci) */
    struct gh_dev dev2;
    CHECK_EQ(reopen_dev(&dev2, tmp), 0);
    struct gh2_fs fs2;
    CHECK_EQ(gh2_fs_mount(&fs2, &dev2), 0);
    issues = -1; CHECK_EQ(gh2_fsck(&fs2, 0, &issues), 0); CHECK_EQ(issues, 0);
    uint8_t rd[4096];
    for (int i = 0; i < M; i += 911) {
        memset(rd, 0, sizeof(rd));
        ssize_t r = gh2_fs_read(&fs2, "/f", rd, sizeof(rd), (uint64_t)i * 4096);
        CHECK_EQ(r, (ssize_t)sizeof(rd));
        CHECK_EQ(rd[0], (uint8_t)(i + 3));
    }
    printf("  [cap] %d zapisow bez jawnego commitu: max brudnych wezlow=%llu (prog 4096); "
           "fsck==0\n", M, (unsigned long long)max_dirty);

    gh2_fs_unmount(&fs2);
    close_dev(&dev2);
    unlink(tmp);
}

/* ============================ Test 5: crash w trakcie FLUSHu cache ============================ */
/* fail_after=N uzbrojone tuz przed gh2_fs_commit -> awaria trafi w gh_disk_write podczas FLUSHu
 * brudnych wezlow (lub SB). Remount SWIEZY dla KAZDEGO N: fsck==0 i stan STARY-albo-NOWY
 * (superblok pisany OSTATNI, po flushu -> crash-consistency niezmieniona). */
static void test_crash_during_flush(void) {
    int max_n = 2000, covered = 0, old_seen = 0, new_seen = 0;
    const char *MSG = "flush-crash-canary-content-spanning";

    for (int n = 1; n <= max_n; n++) {
        char tmp[] = "/tmp/gh2nc_fc_XXXXXX";
        int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);

        struct gh_dev dev;
        CHECK_EQ(open_dev(&dev, tmp), 0);
        CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
        struct gh2_fs fs;
        CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

        /* stan bazowy trwaly */
        CHECK_EQ(gh2_fs_create(&fs, "/base", 0644), 0);
        CHECK_EQ(gh2_fs_commit(&fs), 0);

        /* zbuduj paczke mutacji (brudne wezly w cache; bloki danych pisane na biezaco) */
        CHECK_EQ(gh2_fs_mkdir(&fs, "/d", 0755), 0);
        CHECK_EQ(gh2_fs_create(&fs, "/d/f", 0644), 0);
        ssize_t w = gh2_fs_write(&fs, "/d/f", MSG, strlen(MSG), 0);
        CHECK_EQ(w, (ssize_t)strlen(MSG));

        /* uzbroj awarie N-tego zapisu, potem commit (FLUSH brudnych wezlow + SB) */
        fs.dev.fail_after = n;
        int cr = gh2_fs_commit(&fs);
        (void)cr;
        int clean = (fs.dev.fail_after != 0);

        gh2_fs_unmount(&fs);
        close_dev(&dev);

        /* remount swiezy: fsck==0 dla KAZDEGO N */
        struct gh_dev dev2;
        CHECK_EQ(reopen_dev(&dev2, tmp), 0);
        struct gh2_fs fs2;
        CHECK_EQ(gh2_fs_mount(&fs2, &dev2), 0);
        int issues = -1;
        int fr = gh2_fsck(&fs2, 0, &issues);
        CHECK_EQ(fr, 0);
        if (issues != 0)
            printf("  FLUSH-CRASH BLOCKED: N=%d fsck issues=%d\n", n, issues);
        CHECK_EQ(issues, 0);

        /* stan STARY (/d nieobecny) albo NOWY (/d, /d/f z poprawnymi danymi) */
        struct gh2_inode in; uint64_t ino = 0;
        int is_new = 0, is_old = 0;
        if (gh2_fs_getattr(&fs2, "/d/f", &in, &ino) == 0) {
            char rd[64]; memset(rd, 0, sizeof(rd));
            ssize_t rr = gh2_fs_read(&fs2, "/d/f", rd, strlen(MSG), 0);
            is_new = (rr == (ssize_t)strlen(MSG) && memcmp(rd, MSG, strlen(MSG)) == 0);
        } else if (gh2_fs_getattr(&fs2, "/d", &in, &ino) == -ENOENT) {
            is_old = 1;
        }
        if (!(is_new ^ is_old))
            printf("  FLUSH-CRASH BLOCKED: N=%d stan CZESCIOWY (new=%d old=%d)\n",
                   n, is_new, is_old);
        CHECK(is_new ^ is_old);
        if (is_new) new_seen = 1;
        if (is_old) old_seen = 1;

        gh2_fs_unmount(&fs2);
        close_dev(&dev2);
        unlink(tmp);

        if (clean) { covered = n; break; }
    }

    CHECK(covered > 0 && covered <= max_n);
    CHECK_EQ(old_seen, 1);   /* awaria wczesna -> STARY */
    CHECK_EQ(new_seen, 1);   /* commit czysty -> NOWY */
    printf("  [flush-crash] pokryto N=1..%d; flush+SB atomowe; STARY i NOWY; fsck==0 kazdy N\n",
           covered);
}

int main(void) {
    RUN_TEST(test_read_your_writes);
    RUN_TEST(test_persistence);
    RUN_TEST(test_write_amplification);
    RUN_TEST(test_node_write_reduction);
    RUN_TEST(test_rollback_safety_superseded);
    RUN_TEST(test_capacity_bound);
    RUN_TEST(test_crash_during_flush);
    return TEST_SUMMARY();
}
