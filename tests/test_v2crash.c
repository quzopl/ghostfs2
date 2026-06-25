#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "test.h"
#include "v2/gh2_fs.h"
#include "v2/gh2_space.h"
#include "v2/gh2_btree.h"
#include "v2/gh2_format.h"
#include "block.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

/* ============================================================================
 * ghostfs v2.5 — crash-consistency sweep (atomowy commit).
 *
 * Dowodzimy atomowosci commitu CoW przez sweep awarii: dla N=1.. failujemy N-ty
 * zapis (wezel drzewa / blok danych / superblok — fail_after pokrywa wszystkie)
 * paczki ops + commit, porzucamy stan w pamieci, remountujemy SWIEZO z pliku i
 * weryfikujemy: gh2_fsck==0 (spojny) ORAZ stan to STARY albo NOWY — nigdy czesciowy.
 * ========================================================================== */

static const uint64_t NBLK = 8192;

/* dane kanarka: ~10 blokow z rozpoznawalnym wzorcem (zalezny od offsetu) */
#define CANARY_NBLK  10
#define CANARY_LEN   (CANARY_NBLK * 4096)

static void fill_canary(uint8_t *buf) {
    for (size_t i = 0; i < CANARY_LEN; i++)
        buf[i] = (uint8_t)((i * 31u + 7u) ^ (i >> 8));
}

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

/* Wykonaj reprezentatywna PACZKE operacji (mix metadanych i danych wielo-blokowych).
 * Wszystkie mutacje akumuluja sie w fs->fs_root (CoW, jeszcze nie utrwalone).
 * Zwraca 0 gdy cala paczka sie powiodla; <0 gdy ktoras operacja zwrocila blad
 * (awaria w srodku — wtedy commit i tak utrwali OLD lub czesc paczki w nieosiagalnych
 * blokach CoW; remount zobaczy OLD). */
static int do_pack(struct gh2_fs *fs, const uint8_t *canary) {
    int r;
    if ((r = gh2_fs_mkdir(fs, "/d", 0755)) != 0) return r;
    if ((r = gh2_fs_create(fs, "/d/f", 0644)) != 0) return r;
    /* zapis ~10 blokow danych z rozpoznawalnym wzorcem */
    ssize_t w = gh2_fs_write(fs, "/d/f", canary, CANARY_LEN, 0);
    if (w < 0) return (int)w;
    if (w != (ssize_t)CANARY_LEN) return -EIO;
    if ((r = gh2_fs_create(fs, "/g", 0600)) != 0) return r;
    if ((r = gh2_fs_rename(fs, "/g", "/h", 0)) != 0) return r;
    if ((r = gh2_fs_create(fs, "/tmpfile", 0644)) != 0) return r;
    if ((r = gh2_fs_unlink(fs, "/tmpfile")) != 0) return r;
    return 0;
}

/* Czy stan to pelny NOWY: /d, /d/f (z poprawnymi danymi), /h istnieja; /g i /tmpfile nie. */
static int is_new_state(struct gh2_fs *fs, const uint8_t *canary) {
    struct gh2_inode in; uint64_t ino = 0;
    if (gh2_fs_getattr(fs, "/d", &in, &ino) != 0 || in.type != GH2_FT_DIR) return 0;
    if (gh2_fs_getattr(fs, "/d/f", &in, &ino) != 0 || in.type != GH2_FT_FILE) return 0;
    if (in.size != CANARY_LEN) return 0;
    if (gh2_fs_getattr(fs, "/h", &in, &ino) != 0) return 0;
    if (gh2_fs_getattr(fs, "/g", &in, &ino) != -ENOENT) return 0;
    if (gh2_fs_getattr(fs, "/tmpfile", &in, &ino) != -ENOENT) return 0;
    /* dane kanarka poprawne w CALOSCI */
    uint8_t rd[CANARY_LEN];
    ssize_t rr = gh2_fs_read(fs, "/d/f", rd, CANARY_LEN, 0);
    if (rr != (ssize_t)CANARY_LEN) return 0;
    if (memcmp(rd, canary, CANARY_LEN) != 0) return 0;
    return 1;
}

/* Czy stan to STARY (paczka NIEOBECNA): /d, /d/f, /h nie istnieja. */
static int is_old_state(struct gh2_fs *fs) {
    struct gh2_inode in; uint64_t ino = 0;
    if (gh2_fs_getattr(fs, "/d", &in, &ino) != -ENOENT) return 0;
    if (gh2_fs_getattr(fs, "/d/f", &in, &ino) != -ENOENT) return 0;
    if (gh2_fs_getattr(fs, "/h", &in, &ino) != -ENOENT) return 0;
    if (gh2_fs_getattr(fs, "/g", &in, &ino) != -ENOENT) return 0;
    return 1;
}

/* ============================ Test 1: sweep awarii (BRAMKA) ============================ */
/* Dla N=1.. (az commit przejdzie bez awarii): swiezy kontener, format, mount, paczka ops,
 * fail_after=N, commit, close; remount SWIEZY; gh2_fsck==0; kanarek STARY albo NOWY. */
static void test_crash_sweep(void) {
    uint8_t canary[CANARY_LEN];
    fill_canary(canary);

    int max_n = 2000;        /* gorny limit bezpieczenstwa */
    int n;
    int covered = 0;
    int new_seen = 0, old_seen = 0;

    for (n = 1; n <= max_n; n++) {
        char tmp[] = "/tmp/gh2crash_sw_XXXXXX";
        int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);

        /* swiezy kontener + format + mount */
        struct gh_dev dev;
        CHECK_EQ(open_dev(&dev, tmp), 0);
        CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);

        struct gh2_fs fs;
        CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

        /* zbuduj CALA paczke w pamieci (CoW; bloki nowe zapisane, lecz nieosiagalne z
         * zatwierdzonego SB). Pakiet musi sie powiesc w calosci PRZED uzbrojeniem awarii —
         * inaczej commitowalibysmy spojny PREFIKS (kazda op atomowa), nie cala paczke. */
        CHECK_EQ(do_pack(&fs, canary), 0);

        /* uzbroj awarie N-tego zapisu commitu. Zapisy bloków danych/wezlow paczki juz
         * trafily na dysk (nieosiagalne); atomowy punkt to podmiana SB (sb_write_slot,
         * objety fail_after). Sweep dowodzi: kazdy prefiks zapisow commitu = STARY albo NOWY. */
        fs.dev.fail_after = n;

        /* commit (fsync danych -> atomowa podmiana SB -> zwolnienie starych);
         * moze zwrocic -EIO gdy awaria trafi w zapis SB. */
        int crashed = gh2_fs_commit(&fs);
        (void)crashed;

        /* czy commit przeszedl bez awarii? (licznik nie spadl do 0) */
        int commit_clean = (fs.dev.fail_after != 0);

        gh2_fs_unmount(&fs);
        close_dev(&dev);

        /* --- remount SWIEZY (czysty cache, czysty stan w pamieci) --- */
        struct gh_dev dev2;
        CHECK_EQ(reopen_dev(&dev2, tmp), 0);
        struct gh2_fs fs2;
        CHECK_EQ(gh2_fs_mount(&fs2, &dev2), 0);

        /* fsck MUSI byc czysty dla KAZDEGO N (bramka atomowosci) */
        int issues = -1;
        int fr = gh2_fsck(&fs2, 0, &issues);
        CHECK_EQ(fr, 0);
        if (issues != 0) {
            printf("  CRASH-SWEEP BLOCKED: N=%d fsck issues=%d (atomowosc zlamana)\n", n, issues);
        }
        CHECK_EQ(issues, 0);

        /* KANAREK: stan STARY albo NOWY — nigdy czesciowy */
        int is_new = is_new_state(&fs2, canary);
        int is_old = is_old_state(&fs2);
        if (!(is_new ^ is_old)) {
            printf("  CRASH-SWEEP BLOCKED: N=%d stan CZESCIOWY (new=%d old=%d)\n",
                   n, is_new, is_old);
        }
        CHECK(is_new ^ is_old);     /* dokladnie jeden z nich */
        if (is_new) new_seen = 1;
        if (is_old) old_seen = 1;

        gh2_fs_unmount(&fs2);
        close_dev(&dev2);
        unlink(tmp);

        if (commit_clean) { covered = n; break; }   /* pelne pokrycie punktow zapisu */
    }

    CHECK(covered > 0 && covered <= max_n);   /* sweep zakonczyl sie czystym commitem */
    CHECK_EQ(new_seen, 1);                     /* przynajmniej raz zaobserwowano NOWY */
    CHECK_EQ(old_seen, 1);                     /* i przynajmniej raz STARY */
    printf("  [sweep] pokryto N=1..%d; NOWY i STARY zaobserwowane; fsck==0 dla kazdego N\n",
           covered);
}

/* ============================ Test 1b: sweep POJEDYNCZEJ op (wszystkie zapisy) ========= */
/* Pojedyncza op (multi-blok write + commit) jako jednostka atomowa: awaria na KAZDYM jej
 * zapisie (bloki danych, wezly drzewa CoW, SB) musi dac STARY albo NOWY — nigdy czesciowy.
 * Op-level rollback gwarantuje, ze awaria w srodku pozostawia fs_root nietkniety -> commit
 * utrwala OLD. To pokrywa SZEROKI zbior punktow zapisu (dane+wezly+SB), nie tylko SB. */
static void test_crash_sweep_single_op(void) {
    uint8_t canary[CANARY_LEN];
    fill_canary(canary);

    int max_n = 4000;
    int covered = 0, new_seen = 0, old_seen = 0;

    for (int n = 1; n <= max_n; n++) {
        char tmp[] = "/tmp/gh2crash_so_XXXXXX";
        int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);

        struct gh_dev dev;
        CHECK_EQ(open_dev(&dev, tmp), 0);
        CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
        struct gh2_fs fs;
        CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

        /* stan BAZOWY trwaly: pusty plik /f (commit bez awarii) */
        CHECK_EQ(gh2_fs_create(&fs, "/f", 0644), 0);
        CHECK_EQ(gh2_fs_commit(&fs), 0);

        /* uzbroj awarie N-tego zapisu, potem JEDNA op (multi-blok write) + commit.
         * Awaria moze trafic w: zapis bloku danych, zapis wezla CoW B-drzewa, lub SB. */
        fs.dev.fail_after = n;
        ssize_t w = gh2_fs_write(&fs, "/f", canary, CANARY_LEN, 0);   /* moze -EIO */
        int commit_r = 0;
        if (w == (ssize_t)CANARY_LEN) commit_r = gh2_fs_commit(&fs);  /* moze -EIO (SB) */
        else (void)w;
        int clean = (fs.dev.fail_after != 0);
        (void)commit_r;

        gh2_fs_unmount(&fs);
        close_dev(&dev);

        struct gh_dev dev2;
        CHECK_EQ(reopen_dev(&dev2, tmp), 0);
        struct gh2_fs fs2;
        CHECK_EQ(gh2_fs_mount(&fs2, &dev2), 0);

        int issues = -1;
        CHECK_EQ(gh2_fsck(&fs2, 0, &issues), 0);
        if (issues != 0)
            printf("  SINGLE-OP SWEEP BLOCKED: N=%d fsck issues=%d\n", n, issues);
        CHECK_EQ(issues, 0);

        /* /f zawsze istnieje (stan bazowy). Kanarek: dane albo PUSTE (OLD, size 0) albo
         * PELNE i poprawne (NEW, size CANARY_LEN) — nigdy czesciowo zapisane. */
        struct gh2_inode in; uint64_t ino = 0;
        CHECK_EQ(gh2_fs_getattr(&fs2, "/f", &in, &ino), 0);
        int is_new = 0, is_old = 0;
        if (in.size == 0) {
            is_old = 1;
        } else if (in.size == CANARY_LEN) {
            uint8_t rd[CANARY_LEN];
            ssize_t rr = gh2_fs_read(&fs2, "/f", rd, CANARY_LEN, 0);
            is_new = (rr == (ssize_t)CANARY_LEN && memcmp(rd, canary, CANARY_LEN) == 0);
        }
        if (!(is_new ^ is_old))
            printf("  SINGLE-OP SWEEP BLOCKED: N=%d stan CZESCIOWY (size=%llu)\n",
                   n, (unsigned long long)in.size);
        CHECK(is_new ^ is_old);
        if (is_new) new_seen = 1;
        if (is_old) old_seen = 1;

        gh2_fs_unmount(&fs2);
        close_dev(&dev2);
        unlink(tmp);

        if (clean) { covered = n; break; }
    }

    CHECK(covered > 0 && covered <= max_n);
    CHECK_EQ(new_seen, 1);
    CHECK_EQ(old_seen, 1);
    printf("  [sweep-1op] pokryto N=1..%d (dane+wezly+SB); STARY i NOWY; fsck==0 kazdy N\n",
           covered);
}

/* ============================ Test 2: wielokrotny commit + awaria w 3-ciej ============= */
/* paczka1+commit, paczka2+commit, paczka3 z awaria -> remount: stan po paczce2 (spojny);
 * paczki 1-2 nienaruszone. */
static void test_multi_commit(void) {
    char tmp[] = "/tmp/gh2crash_mc_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);

    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    /* paczka 1 */
    CHECK_EQ(gh2_fs_mkdir(&fs, "/p1", 0755), 0);
    CHECK_EQ(gh2_fs_create(&fs, "/p1/a", 0644), 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    /* paczka 2 */
    CHECK_EQ(gh2_fs_mkdir(&fs, "/p2", 0755), 0);
    CHECK_EQ(gh2_fs_create(&fs, "/p2/b", 0644), 0);
    const char *msg = "committed-2";
    CHECK_EQ(gh2_fs_write(&fs, "/p2/b", msg, strlen(msg), 0), (ssize_t)strlen(msg));
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    /* paczka 3: mutacje + awaria w srodku commitu */
    CHECK_EQ(gh2_fs_mkdir(&fs, "/p3", 0755), 0);
    CHECK_EQ(gh2_fs_create(&fs, "/p3/c", 0644), 0);
    fs.dev.fail_after = 1;                 /* failuj pierwszy zapis commitu (bariera/SB) */
    int cr = gh2_fs_commit(&fs);
    CHECK(cr != 0);                        /* commit nieudany */

    gh2_fs_unmount(&fs);
    close_dev(&dev);

    /* remount: stan po paczce 2 */
    struct gh_dev dev2;
    CHECK_EQ(reopen_dev(&dev2, tmp), 0);
    struct gh2_fs fs2;
    CHECK_EQ(gh2_fs_mount(&fs2, &dev2), 0);

    int issues = -1;
    CHECK_EQ(gh2_fsck(&fs2, 0, &issues), 0);
    CHECK_EQ(issues, 0);

    struct gh2_inode in; uint64_t ino = 0;
    /* paczki 1-2 obecne i nienaruszone */
    CHECK_EQ(gh2_fs_getattr(&fs2, "/p1/a", &in, &ino), 0);
    CHECK_EQ(gh2_fs_getattr(&fs2, "/p2/b", &in, &ino), 0);
    char rd[32]; memset(rd, 0, sizeof(rd));
    CHECK_EQ(gh2_fs_read(&fs2, "/p2/b", rd, strlen(msg), 0), (ssize_t)strlen(msg));
    CHECK_EQ(memcmp(rd, msg, strlen(msg)), 0);
    /* paczka 3 nieobecna (awaria przed atomowa podmiana) */
    CHECK_EQ(gh2_fs_getattr(&fs2, "/p3", &in, &ino), -ENOENT);

    gh2_fs_unmount(&fs2);
    close_dev(&dev2);
    unlink(tmp);
}

/* ============================ Test 3: idempotencja remount ============================ */
/* Podwojny remount bez zmian -> identyczny stan, fsck==0 oba razy. */
static void test_remount_idempotent(void) {
    char tmp[] = "/tmp/gh2crash_id_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);

    uint8_t canary[CANARY_LEN];
    fill_canary(canary);

    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
    CHECK_EQ(do_pack(&fs, canary), 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    uint64_t ni0 = fs.next_ino;
    uint64_t free0 = fs.space.nfree;
    gh2_fs_unmount(&fs);
    close_dev(&dev);

    uint64_t ni_prev = 0, free_prev = 0;
    for (int pass = 0; pass < 2; pass++) {
        struct gh_dev d;
        CHECK_EQ(reopen_dev(&d, tmp), 0);
        struct gh2_fs f;
        CHECK_EQ(gh2_fs_mount(&f, &d), 0);
        int issues = -1;
        CHECK_EQ(gh2_fsck(&f, 0, &issues), 0);
        CHECK_EQ(issues, 0);
        CHECK(is_new_state(&f, canary));
        if (pass == 0) { ni_prev = f.next_ino; free_prev = f.space.nfree; }
        else { CHECK_EQ(f.next_ino, ni_prev); CHECK_EQ(f.space.nfree, free_prev); }
        CHECK_EQ(f.next_ino, ni0);
        CHECK_EQ(f.space.nfree, free0);
        gh2_fs_unmount(&f);
        close_dev(&d);
    }
    unlink(tmp);
}

/* ============================ Test 4: fsck wykrywa niespojnosc (negatywny) ============= */
/* Wstrzyknij niespojnosc seamem testowym: dodaj DIR_ITEM wskazujacy ino BEZ INODE_ITEM
 * (sierocy link). gh2_fsck musi zglosic issues>0 (dowod, ze fsck faktycznie sprawdza). */
static void test_fsck_detects(void) {
    char tmp[] = "/tmp/gh2crash_neg_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);

    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    /* normalny, spojny stan */
    CHECK_EQ(gh2_fs_mkdir(&fs, "/dir", 0755), 0);
    CHECK_EQ(gh2_fs_create(&fs, "/dir/real", 0644), 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    int issues = -1;
    CHECK_EQ(gh2_fsck(&fs, 0, &issues), 0);
    CHECK_EQ(issues, 0);                 /* przed wstrzyknieciem: czysto */

    /* wstrzyknij: wpis "ghost" w /dir wskazujacy ino bez INODE_ITEM (np. 999999) */
    uint64_t dino = 0;
    CHECK_EQ(gh2_path_resolve(&fs, "/dir", &dino), 0);
    const uint64_t FAKE_HASH = 0x1122334455667788ULL;
    const uint64_t ORPHAN_INO = 999999ULL;
    CHECK_EQ(gh2_fs_test_dir_add(&fs, dino, FAKE_HASH, "ghost", 5, ORPHAN_INO, GH2_FT_FILE), 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    /* remount swiezy + fsck -> wykrywa sierocy link (links_seen != 0 bez INODE_ITEM) */
    gh2_fs_unmount(&fs);
    close_dev(&dev);
    struct gh_dev dev2;
    CHECK_EQ(reopen_dev(&dev2, tmp), 0);
    struct gh2_fs fs2;
    CHECK_EQ(gh2_fs_mount(&fs2, &dev2), 0);
    issues = -1;
    CHECK_EQ(gh2_fsck(&fs2, 0, &issues), 0);
    CHECK(issues > 0);                   /* fsck FAKTYCZNIE wykrywa niespojnosc */

    gh2_fs_unmount(&fs2);
    close_dev(&dev2);
    unlink(tmp);
}

/* ============================ Test 5: SZEROKI sweep paczki (wszystkie zapisy) ========== */
/* Recenzent dowiodl szerokiego pokrycia: fail_after UZBROJONE PRZED budowa paczki ~12-15
 * mieszanych operacji z danymi. Awaria trafia w KAZDY zapis paczki (bloki danych + wezly
 * CoW B-drzewa) ORAZ w commit (SB). Dla KAZDEGO N remount musi byc spojny (fsck==0), mapa
 * spojna (brak wyciekow/zywy-wolny), a kazdy znany plik albo NIEOBECNY albo obecny z
 * POPRAWNA (nieuszkodzona) zawartoscia — nigdy obecny-ale-uszkodzony. To wbudowuje dowod
 * recenzenta (N=1..kilkadziesiat, fsck==0) jako test regresyjny.
 *
 * Roznica wobec test_crash_sweep: tam paczka budowana BEZ awarii (fail_after po do_pack),
 * wiec awaria pokrywa tylko zapis SB (N=1..2). Tu fail_after PRZED paczka -> N=1..kilkadziesiat
 * pokrywa wszystkie zapisy danych+wezlow+SB. */

#define WIDE_A_NBLK  8                     /* /d/a: ~8 blokow */
#define WIDE_A_LEN   (WIDE_A_NBLK * 4096)
#define WIDE_B_NBLK  3                     /* /b: ~3 bloki */
#define WIDE_B_LEN   (WIDE_B_NBLK * 4096)

/* wzorzec zalezny od (seed,offset) — kazdy plik rozpoznawalny i wewn. niejednorodny */
static void fill_pat(uint8_t *buf, size_t len, uint32_t seed) {
    for (size_t i = 0; i < len; i++)
        buf[i] = (uint8_t)((i * 131u + seed * 2654435761u + 17u) ^ (i >> 7));
}

/* Buduj paczke ~12-15 mieszanych operacji z danymi. fail_after juz uzbrojone -> dowolna
 * op moze zwrocic blad gdy awaria trafi w srodek; przerwij budowe przy pierwszym bledzie
 * (to OK — commit utrwali OLD lub spojny stan, remount to zweryfikuje). */
static void build_wide_pack(struct gh2_fs *fs, const uint8_t *pat_a, const uint8_t *pat_b) {
    if (gh2_fs_mkdir(fs, "/d", 0755) != 0) return;
    if (gh2_fs_create(fs, "/d/a", 0644) != 0) return;
    if (gh2_fs_write(fs, "/d/a", pat_a, WIDE_A_LEN, 0) != (ssize_t)WIDE_A_LEN) return;
    if (gh2_fs_create(fs, "/b", 0644) != 0) return;
    if (gh2_fs_write(fs, "/b", pat_b, WIDE_B_LEN, 0) != (ssize_t)WIDE_B_LEN) return;
    if (gh2_fs_symlink(fs, "/d/a", "/sym") != 0) return;
    if (gh2_fs_link(fs, "/d/a", "/c") != 0) return;          /* hardlink /d/a -> /c */
    if (gh2_fs_mknod(fs, "/fifo", S_IFIFO | 0644, 0) != 0) return;
    if (gh2_fs_create(fs, "/g", 0600) != 0) return;
    if (gh2_fs_rename(fs, "/g", "/h", 0) != 0) return;        /* /g -> /h */
    if (gh2_fs_truncate(fs, "/d/a", WIDE_A_LEN / 2) != 0) return;  /* skroc do 4 blokow */
    if (gh2_fs_create(fs, "/tmpfile", 0644) != 0) return;
    if (gh2_fs_unlink(fs, "/tmpfile") != 0) return;
    if (gh2_fs_mkdir(fs, "/rmd", 0755) != 0) return;
    if (gh2_fs_rmdir(fs, "/rmd") != 0) return;
}

/* KANAREK pliku: ENOENT (OLD/nieobecny) jest OK; gdy obecny, zawartosc MUSI byc poprawna.
 * exp_len<0 => oczekiwana dlugosc nieznana (po truncate niejednoznaczna) -> sprawdz tylko
 * spojnosc odczytu vs size i prefiks wzorca dla biezacego size. Zwraca 1 gdy OK. */
static int canary_file_ok(struct gh2_fs *fs, const char *path,
                          const uint8_t *pat, size_t pat_len) {
    struct gh2_inode in; uint64_t ino = 0;
    int r = gh2_fs_getattr(fs, path, &in, &ino);
    if (r == -ENOENT) return 1;                    /* nieobecny — dozwolone */
    if (r != 0) return 0;                           /* inny blad = niespojnosc */
    if (in.type != GH2_FT_FILE) return 1;           /* nie plik (np. symlink) — pomin tutaj */
    if (in.size > pat_len) return 0;                /* rozmiar poza znanym wzorcem = uszkodzenie */
    if (in.size == 0) return 1;                     /* pusty — dozwolone */
    uint8_t *rd = malloc(in.size);
    if (!rd) return 0;
    ssize_t rr = gh2_fs_read(fs, path, rd, in.size, 0);
    int ok = (rr == (ssize_t)in.size) && (memcmp(rd, pat, in.size) == 0);
    free(rd);
    return ok;
}

/* mapa zamontowana == niezalezna mark-sweep ze SWIEZEJ mapy (brak wyciekow / zywy-wolny). */
static int map_consistent(struct gh2_fs *fs) {
    struct gh2_space s2;
    if (gh2_space_init(&s2, fs->space.nblocks) != 0) return 0;
    int ok = 0;
    if (gh2_refmap_build_from_roots(&fs->dev, &s2, &fs->root_tree) == 0) {
        ok = (memcmp(fs->space.bits, s2.bits, (fs->space.nblocks + 7) / 8) == 0)
             && (fs->space.nfree == s2.nfree);
    }
    gh2_space_destroy(&s2);
    return ok;
}

static void test_crash_sweep_wide(void) {
    uint8_t pat_a[WIDE_A_LEN], pat_b[WIDE_B_LEN];
    fill_pat(pat_a, WIDE_A_LEN, 0xA1A1A1A1u);
    fill_pat(pat_b, WIDE_B_LEN, 0xB2B2B2B2u);

    /* kontener ~2048 blokow (lekki, by sweep kilkudziesieciu N byl szybki) */
    const uint64_t WBLK = 2048;

    int max_n = 4000;            /* gorny limit bezpieczenstwa */
    int covered = 0;
    int present_seen = 0, absent_seen = 0;

    for (int n = 1; n <= max_n; n++) {
        char tmp[] = "/tmp/gh2crash_wd_XXXXXX";
        int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);

        /* swiezy kontener + format + mount */
        struct gh_dev dev;
        CHECK_EQ(gh_dev_create(tmp, WBLK, &dev), 0);
        CHECK_EQ(gh_bcache_create(&dev), 0);
        CHECK_EQ(gh2_fs_format(&dev, WBLK, 0), 0);
        struct gh2_fs fs;
        CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

        /* UZBROJ awarie N-tego zapisu PRZED budowa paczki -> pokrywa zapisy danych+wezlow */
        fs.dev.fail_after = n;
        build_wide_pack(&fs, pat_a, pat_b);   /* moze przerwac w srodku przy bledzie */

        /* commit (moze zwrocic -EIO gdy awaria trafi w SB) */
        int cr = gh2_fs_commit(&fs);
        (void)cr;
        int commit_clean = (fs.dev.fail_after != 0);

        gh2_fs_unmount(&fs);
        close_dev(&dev);

        /* --- remount SWIEZY --- */
        struct gh_dev dev2;
        CHECK_EQ(reopen_dev(&dev2, tmp), 0);
        struct gh2_fs fs2;
        CHECK_EQ(gh2_fs_mount(&fs2, &dev2), 0);

        /* fsck czysty dla KAZDEGO N (bramka atomowosci) */
        int issues = -1;
        int fr = gh2_fsck(&fs2, 0, &issues);
        CHECK_EQ(fr, 0);
        if (issues != 0)
            printf("  WIDE-SWEEP BLOCKED: N=%d fsck issues=%d (atomowosc zlamana)\n", n, issues);
        CHECK_EQ(issues, 0);

        /* mapa spojna: brak wyciekow (zajety-niezywy) ani zywy-wolny */
        int mc = map_consistent(&fs2);
        if (!mc)
            printf("  WIDE-SWEEP BLOCKED: N=%d mapa niespojna (wyciek/zywy-wolny)\n", n);
        CHECK(mc);

        /* KANARKI: kazdy znany plik albo nieobecny albo z POPRAWNA zawartoscia.
         * /d/a po truncate ma rozmiar 0 / WIDE_A_LEN / WIDE_A_LEN/2 — kazdy z poprawnym
         * prefiksem wzorca; canary_file_ok sprawdza prefiks dla biezacego size. */
        int ca = canary_file_ok(&fs2, "/d/a", pat_a, WIDE_A_LEN);
        int cb = canary_file_ok(&fs2, "/b",   pat_b, WIDE_B_LEN);
        int cc = canary_file_ok(&fs2, "/c",   pat_a, WIDE_A_LEN);   /* hardlink do /d/a */
        if (!(ca && cb && cc))
            printf("  WIDE-SWEEP BLOCKED: N=%d kanarek USZKODZONY (a=%d b=%d c=%d)\n",
                   n, ca, cb, cc);
        CHECK(ca); CHECK(cb); CHECK(cc);

        /* obserwuj rozne wyniki: czasem paczka nieobecna (OLD), czasem czesciowo/calkiem obecna */
        struct gh2_inode in; uint64_t ino = 0;
        if (gh2_fs_getattr(&fs2, "/d", &in, &ino) == 0) present_seen = 1;
        else absent_seen = 1;

        gh2_fs_unmount(&fs2);
        close_dev(&dev2);
        unlink(tmp);

        if (commit_clean) { covered = n; break; }   /* pelne pokrycie wszystkich zapisow */
    }

    CHECK(covered >= 30);          /* >=30 punktow N (wszystkie zapisy paczki + commit) */
    CHECK_EQ(present_seen, 1);     /* przynajmniej raz paczka (czesciowo) obecna po remount */
    CHECK_EQ(absent_seen, 1);      /* i przynajmniej raz nieobecna (OLD) */
    printf("  [sweep-wide] pokryto N=1..%d (dane+wezly+SB); fsck==0 kazdy N; "
           "mapa spojna; kanarki nieuszkodzone\n", covered);
}

/* ============================ Test 6: crash-sweep z SNAPSHOT + SUBVOL_DELETE =========== */
/* Atomowosc operacji snapshotowych pod awaria. Stan bazowy: /f="base" + commit, snapshot "s0"
 * + modyfikacja /f (rozejscie) + commit -> trwaly snapshot s0. Potem dla N=1..: uzbroj
 * fail_after=N PRZED paczka {snapshot "s1"; subvol_delete s0}. Kazda z tych operacji robi
 * wlasny commit (atomowy punkt = podmiana SB). Po awarii remount SWIEZY:
 *   - gh2_fsck == 0 dla KAZDEGO N (spojnosc),
 *   - mapa/refcount spojne (map_consistent: zamontowana mapa == swieza mark-sweep),
 *   - snapshot s1 ALBO jest w calosci ALBO go nie ma; delete s0 ALBO wykonany ALBO nie
 *     (stan stary-albo-nowy na poziomie kazdej operacji — nigdy czesciowy).
 *   - dane (/f w aktywnym, oraz s0 jesli wciaz istnieje) czytelne i poprawne. */

/* cb: znajdz id subwolumenow po nazwie (s0, s1) */
struct snap_ids { uint64_t s0, s1; };
static int snap_ids_cb(uint64_t id, const char *name, void *ctx) {
    struct snap_ids *p = ctx;
    if (strcmp(name, "s0") == 0) p->s0 = id;
    if (strcmp(name, "s1") == 0) p->s1 = id;
    return 0;
}

static void test_crash_sweep_snapshot(void) {
    const char *BASE = "base-content-for-snapshot-crash-test";
    const char *MOD  = "modified-after-s0-divergence-longer-content-block";

    int max_n = 4000;
    int covered = 0;
    int s1_present_seen = 0, s1_absent_seen = 0;
    int s0_present_seen = 0, s0_absent_seen = 0;

    for (int n = 1; n <= max_n; n++) {
        char tmp[] = "/tmp/gh2crash_snap_XXXXXX";
        int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);

        struct gh_dev dev;
        CHECK_EQ(open_dev(&dev, tmp), 0);
        CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
        struct gh2_fs fs;
        CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

        /* stan bazowy trwaly: /f=BASE, snapshot s0, modyfikacja /f (rozejscie) */
        CHECK_EQ(gh2_fs_create(&fs, "/f", 0644), 0);
        CHECK_EQ(gh2_fs_write(&fs, "/f", BASE, strlen(BASE), 0), (ssize_t)strlen(BASE));
        CHECK_EQ(gh2_fs_commit(&fs), 0);
        CHECK_EQ(gh2_fs_snapshot(&fs, "s0"), 0);
        CHECK_EQ(gh2_fs_truncate(&fs, "/f", strlen(MOD)), 0);
        CHECK_EQ(gh2_fs_write(&fs, "/f", MOD, strlen(MOD), 0), (ssize_t)strlen(MOD));
        CHECK_EQ(gh2_fs_commit(&fs), 0);

        /* znajdz id s0 (do delete) PRZED uzbrojeniem awarii (czyste czytanie) */
        struct snap_ids pre = { 0, 0 };
        gh2_fs_subvol_list(&fs, snap_ids_cb, &pre);
        CHECK(pre.s0 != 0);

        /* uzbroj awarie N-tego zapisu, potem paczka snapshot+delete (kazda z commitem) */
        fs.dev.fail_after = n;
        int r_snap = gh2_fs_snapshot(&fs, "s1");                 /* moze -EIO */
        int r_del = gh2_fs_subvol_delete(&fs, pre.s0);           /* moze -EIO */
        (void)r_snap; (void)r_del;
        int clean = (fs.dev.fail_after != 0);

        gh2_fs_unmount(&fs);
        close_dev(&dev);

        /* --- remount SWIEZY --- */
        struct gh_dev dev2;
        CHECK_EQ(reopen_dev(&dev2, tmp), 0);
        struct gh2_fs fs2;
        CHECK_EQ(gh2_fs_mount(&fs2, &dev2), 0);

        /* fsck czysty dla KAZDEGO N */
        int issues = -1;
        CHECK_EQ(gh2_fsck(&fs2, 0, &issues), 0);
        if (issues != 0)
            printf("  SNAP-SWEEP BLOCKED: N=%d fsck issues=%d\n", n, issues);
        CHECK_EQ(issues, 0);

        /* mapa/refcount spojne (brak wyciekow ani premature-free) */
        int mc = map_consistent(&fs2);
        if (!mc)
            printf("  SNAP-SWEEP BLOCKED: N=%d mapa/refcount niespojne\n", n);
        CHECK(mc);

        /* aktywny /f zawsze MOD (modyfikacja bazowa zatwierdzona przed paczka) i czytelny */
        struct gh2_inode in; uint64_t ino = 0;
        CHECK_EQ(gh2_fs_getattr(&fs2, "/f", &in, &ino), 0);
        CHECK_EQ(in.size, (uint64_t)strlen(MOD));
        char buf[128]; memset(buf, 0, sizeof(buf));
        CHECK_EQ(gh2_fs_read(&fs2, "/f", buf, strlen(MOD), 0), (ssize_t)strlen(MOD));
        CHECK_EQ(memcmp(buf, MOD, strlen(MOD)), 0);

        /* stan s1 i s0: po remoncie ustal id po nazwie */
        struct snap_ids post = { 0, 0 };
        gh2_fs_subvol_list(&fs2, snap_ids_cb, &post);

        /* s1: albo jest (i wtedy czyta MOD bo snapshot zrobiony PO modyfikacji), albo go nie ma */
        if (post.s1 != 0) {
            s1_present_seen = 1;
            memset(buf, 0, sizeof(buf));
            ssize_t rd = gh2_fs_read_subvol(&fs2, post.s1, "/f", buf, strlen(MOD), 0);
            CHECK_EQ(rd, (ssize_t)strlen(MOD));
            CHECK_EQ(memcmp(buf, MOD, strlen(MOD)), 0);
        } else {
            s1_absent_seen = 1;
        }

        /* s0: albo jest (czyta BASE, stary stan) albo zostal usuniety */
        if (post.s0 != 0) {
            s0_present_seen = 1;
            memset(buf, 0, sizeof(buf));
            ssize_t rd = gh2_fs_read_subvol(&fs2, post.s0, "/f", buf, strlen(BASE), 0);
            CHECK_EQ(rd, (ssize_t)strlen(BASE));
            CHECK_EQ(memcmp(buf, BASE, strlen(BASE)), 0);
        } else {
            s0_absent_seen = 1;
        }

        gh2_fs_unmount(&fs2);
        close_dev(&dev2);
        unlink(tmp);

        if (clean) { covered = n; break; }
    }

    CHECK(covered > 0 && covered <= max_n);
    /* obserwujemy zarowno obecnosc jak i brak s1 oraz wykonanie/niewykonanie delete s0 */
    CHECK_EQ(s1_present_seen, 1);
    CHECK_EQ(s1_absent_seen, 1);
    CHECK_EQ(s0_present_seen, 1);
    CHECK_EQ(s0_absent_seen, 1);
    printf("  [sweep-snap] pokryto N=1..%d; snapshot+delete atomowe; fsck==0 i mapa spojna kazdy N\n",
           covered);
}

int main(void) {
    RUN_TEST(test_crash_sweep);
    RUN_TEST(test_crash_sweep_wide);
    RUN_TEST(test_crash_sweep_single_op);
    RUN_TEST(test_crash_sweep_snapshot);
    RUN_TEST(test_multi_commit);
    RUN_TEST(test_remount_idempotent);
    RUN_TEST(test_fsck_detects);
    return TEST_SUMMARY();
}
