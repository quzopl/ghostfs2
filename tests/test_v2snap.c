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

/* ============================================================================
 * ghostfs v2.7 (Task 3) — testy snapshotow: wspoldzielenie (O(1) miejsca),
 * izolacja CoW (modyfikacja oryginalu nie zmienia snapshotu), refcount==mark-sweep
 * (BRAMKA), wiele snapshotow, persystencja po remount.
 * ========================================================================== */

static const uint64_t NBLK = 8192;

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

/* ---- BRAMKA: in-memory refs[] == gh2_refmap_build_from_roots (licznik po liczniku) ----
 * Zbuduj swiezy refmap z drzewa korzeni i porownaj z aktualnym fs->space.refs[]. */
static void check_refcount_eq_marksweep(struct gh2_fs *fs) {
    struct gh2_space ms;
    int r = gh2_space_init(&ms, fs->space.nblocks);
    CHECK_EQ(r, 0);
    r = gh2_refmap_build_from_roots(&fs->dev, &ms, &fs->root_tree);
    CHECK_EQ(r, 0);
    int mismatches = 0;
    for (uint64_t b = 0; b < fs->space.nblocks; b++) {
        if (gh2_ref_get(&fs->space, b) != gh2_ref_get(&ms, b)) {
            if (mismatches < 8)
                printf("    refmismatch blk=%llu inmem=%u marksweep=%u\n",
                       (unsigned long long)b, gh2_ref_get(&fs->space, b), gh2_ref_get(&ms, b));
            mismatches++;
        }
        /* niezmiennik used <=> refs>0 spojny w obu */
        CHECK_EQ(!!gh2_space_is_used(&fs->space, b), gh2_ref_get(&fs->space, b) > 0);
    }
    CHECK_EQ(mismatches, 0);
    gh2_space_destroy(&ms);
}

/* ---- zlicz bloki z refcount>=k (do dowodu wspoldzielenia) ---- */
static uint64_t count_refs_ge(struct gh2_fs *fs, uint16_t k) {
    uint64_t n = 0;
    for (uint64_t b = 0; b < fs->space.nblocks; b++)
        if (gh2_ref_get(&fs->space, b) >= k) n++;
    return n;
}

/* ---- lista subwolumenow: zbierz id+nazwy ---- */
struct sv_collect { int n; uint64_t ids[16]; char names[16][GH2_SUBVOL_NAME_MAX]; };
static int sv_collect_cb(uint64_t id, const char *name, void *ctx) {
    struct sv_collect *c = ctx;
    if (c->n < 16) {
        c->ids[c->n] = id;
        snprintf(c->names[c->n], GH2_SUBVOL_NAME_MAX, "%s", name);
        c->n++;
    }
    return 0;
}
static int sv_has_name(struct sv_collect *c, const char *name, uint64_t *out_id) {
    for (int i = 0; i < c->n; i++)
        if (strcmp(c->names[i], name) == 0) { if (out_id) *out_id = c->ids[i]; return 1; }
    return 0;
}

/* helper: pelny zapis pliku (utworz jesli brak, nadpisz tresc) */
static void put_file(struct gh2_fs *fs, const char *path, const char *data) {
    struct gh2_inode in;
    int r = gh2_fs_getattr(fs, path, &in, NULL);
    if (r == -ENOENT) { CHECK_EQ(gh2_fs_create(fs, path, 0644), 0); }
    /* przy nadpisaniu krotsza trescia obetnij stary ogon */
    CHECK_EQ(gh2_fs_truncate(fs, path, strlen(data)), 0);
    ssize_t w = gh2_fs_write(fs, path, data, strlen(data), 0);
    CHECK_EQ(w, (ssize_t)strlen(data));
}

/* ======================================================================== */

/* 1. Wspoldzielenie: snapshot nie podwaja danych/fs-tree; rc blokow == 2. */
static void test_sharing(void) {
    char tmpl[] = "/tmp/gh2snap_share_XXXXXX";
    int fd = mkstemp(tmpl); CHECK(fd >= 0); close(fd);

    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmpl), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);

    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    put_file(&fs, "/f", "hello world snapshot data");
    CHECK_EQ(gh2_fs_mkdir(&fs, "/d", 0755), 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    uint64_t used_before = fs.space.nblocks - fs.space.nfree;
    uint64_t rc2_before = count_refs_ge(&fs, 2);
    CHECK_EQ(rc2_before, 0);   /* brak wspoldzielenia przed snapshotem */
    check_refcount_eq_marksweep(&fs);

    CHECK_EQ(gh2_fs_snapshot(&fs, "s1"), 0);

    uint64_t used_after = fs.space.nblocks - fs.space.nfree;
    /* Wspoldzielenie: liczba zajetych blokow rosnie tylko o nowe wezly drzewa korzeni
     * (wpis subwolumenu). Dane (/f) i fs-tree NIE sa kopiowane. Niewielki przyrost. */
    CHECK(used_after - used_before <= 4);

    /* refcount fs_root i blokow danych == 2 (default + s1) */
    CHECK_EQ(gh2_ref_get(&fs.space, fs.fs_root.block), 2);
    /* wszystkie bloki osiagalne z fs_root teraz rc==2: co najmniej fs_root + blok danych /f.
     * (i-wezly i DIR_ITEM mieszcza sie w lisciu fs_root; nie sa osobnymi blokami.) */
    uint64_t rc2_after = count_refs_ge(&fs, 2);
    CHECK(rc2_after >= 2);   /* fs_root (leaf) + blok danych /f */
    printf("  sharing: used %llu->%llu (+%llu), shared(rc>=2)=%llu\n",
           (unsigned long long)used_before, (unsigned long long)used_after,
           (unsigned long long)(used_after - used_before), (unsigned long long)rc2_after);

    /* BRAMKA */
    check_refcount_eq_marksweep(&fs);

    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmpl);
}

/* 2. Izolacja CoW: modyfikuj ORYGINAL -> snapshot widzi STARY stan. */
static void test_isolation(void) {
    char tmpl[] = "/tmp/gh2snap_iso_XXXXXX";
    int fd = mkstemp(tmpl); CHECK(fd >= 0); close(fd);

    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmpl), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);

    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    put_file(&fs, "/f", "ORIGINAL");
    CHECK_EQ(gh2_fs_create(&fs, "/h", 0644), 0);   /* zostanie usuniety w oryginale */
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    CHECK_EQ(gh2_fs_snapshot(&fs, "s1"), 0);
    uint64_t s1 = 0;
    { struct sv_collect c = {0}; gh2_fs_subvol_list(&fs, sv_collect_cb, &c);
      CHECK(sv_has_name(&c, "s1", &s1)); }

    /* modyfikuj ORYGINAL (aktywny default): nowa tresc /f, utworz /g, usun /h */
    put_file(&fs, "/f", "MODIFIED-NEW-CONTENT");
    CHECK_EQ(gh2_fs_create(&fs, "/g", 0644), 0);
    CHECK_EQ(gh2_fs_unlink(&fs, "/h"), 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    /* aktywny (default) widzi NOWY stan */
    char buf[64]; memset(buf, 0, sizeof(buf));
    ssize_t rd = gh2_fs_read(&fs, "/f", buf, sizeof(buf) - 1, 0);
    CHECK(rd > 0); CHECK_EQ(strcmp(buf, "MODIFIED-NEW-CONTENT"), 0);
    struct gh2_inode in;
    CHECK_EQ(gh2_fs_getattr(&fs, "/g", &in, NULL), 0);   /* /g istnieje w aktywnym */
    CHECK_EQ(gh2_fs_getattr(&fs, "/h", &in, NULL), -ENOENT); /* /h usuniety w aktywnym */

    /* SNAPSHOT s1 widzi STARY stan (chwila snapshotu) */
    memset(buf, 0, sizeof(buf));
    rd = gh2_fs_read_subvol(&fs, s1, "/f", buf, sizeof(buf) - 1, 0);
    CHECK(rd > 0); CHECK_EQ(strcmp(buf, "ORIGINAL"), 0);     /* stara tresc */
    CHECK_EQ(gh2_fs_getattr_subvol(&fs, s1, "/h", &in, NULL), 0);  /* /h wciaz w snapshocie */
    CHECK_EQ(gh2_fs_getattr_subvol(&fs, s1, "/g", &in, NULL), -ENOENT); /* /g NIE w snapshocie */

    /* BRAMKA: brak premature-free (snapshot czytelny) ani wycieku */
    check_refcount_eq_marksweep(&fs);

    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmpl);
}

/* 3. Wiele snapshotow: s1, modyfikuj, s2, modyfikuj, s3 -> kazdy czyta swoj stan. */
static void test_multi_snapshot(void) {
    char tmpl[] = "/tmp/gh2snap_multi_XXXXXX";
    int fd = mkstemp(tmpl); CHECK(fd >= 0); close(fd);

    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmpl), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);

    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    /* wiele plikow -> fs-tree wielopoziomowe (internal root + wiele lisci). Plik /shared
     * w jednym lisciu NIE jest modyfikowany -> wspoldzielony przez WSZYSTKIE subwolumeny. */
    for (int i = 0; i < 60; i++) {
        char p[32]; snprintf(p, sizeof(p), "/file%02d", i);
        put_file(&fs, p, "stable-content-shared-across-snapshots");
    }
    put_file(&fs, "/shared", "NEVER-MODIFIED");
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    put_file(&fs, "/f", "v1");
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    CHECK_EQ(gh2_fs_snapshot(&fs, "s1"), 0);
    check_refcount_eq_marksweep(&fs);

    put_file(&fs, "/f", "v2");
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    CHECK_EQ(gh2_fs_snapshot(&fs, "s2"), 0);
    check_refcount_eq_marksweep(&fs);

    put_file(&fs, "/f", "v3");
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    CHECK_EQ(gh2_fs_snapshot(&fs, "s3"), 0);
    check_refcount_eq_marksweep(&fs);

    put_file(&fs, "/f", "v4-active");
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    check_refcount_eq_marksweep(&fs);

    /* kazdy snapshot czyta swoj stan */
    struct sv_collect c = {0}; gh2_fs_subvol_list(&fs, sv_collect_cb, &c);
    CHECK_EQ(c.n, 4);   /* default + s1 + s2 + s3 */
    uint64_t id1, id2, id3;
    CHECK(sv_has_name(&c, "s1", &id1));
    CHECK(sv_has_name(&c, "s2", &id2));
    CHECK(sv_has_name(&c, "s3", &id3));

    char buf[32];
    memset(buf, 0, sizeof(buf)); gh2_fs_read_subvol(&fs, id1, "/f", buf, 31, 0);
    CHECK_EQ(strcmp(buf, "v1"), 0);
    memset(buf, 0, sizeof(buf)); gh2_fs_read_subvol(&fs, id2, "/f", buf, 31, 0);
    CHECK_EQ(strcmp(buf, "v2"), 0);
    memset(buf, 0, sizeof(buf)); gh2_fs_read_subvol(&fs, id3, "/f", buf, 31, 0);
    CHECK_EQ(strcmp(buf, "v3"), 0);
    memset(buf, 0, sizeof(buf)); gh2_fs_read(&fs, "/f", buf, 31, 0);
    CHECK_EQ(strcmp(buf, "v4-active"), 0);

    /* najstarsze bloki wspoldzielone az do 4 subwolumenow (root_tree wezly itp.) -> rc do 4 */
    uint64_t maxrc = 0;
    for (uint64_t b = 0; b < fs.space.nblocks; b++) {
        uint16_t rc = gh2_ref_get(&fs.space, b);
        if (rc > maxrc) maxrc = rc;
    }
    CHECK(maxrc >= 2);   /* wspoldzielenie istnieje */
    CHECK(maxrc <= 4);   /* maks. 4 subwolumeny */
    printf("  multi: maxrc=%llu (oczekiwane <=4)\n", (unsigned long long)maxrc);

    check_refcount_eq_marksweep(&fs);

    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmpl);
}

/* 4. Persystencja: snapshot+commit+remount -> snapshot dostepny, refcounty odbudowane. */
static void test_persistence(void) {
    char tmpl[] = "/tmp/gh2snap_persist_XXXXXX";
    int fd = mkstemp(tmpl); CHECK(fd >= 0); close(fd);

    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmpl), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);

    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
    put_file(&fs, "/f", "persisted-data");
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    CHECK_EQ(gh2_fs_snapshot(&fs, "snap"), 0);
    /* modyfikuj oryginal po snapshocie (rozejscie) */
    put_file(&fs, "/f", "changed-after-snap");
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    check_refcount_eq_marksweep(&fs);
    gh2_fs_unmount(&fs);
    close_dev(&dev);

    /* remount */
    CHECK_EQ(reopen_dev(&dev, tmpl), 0);
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    /* wszystkie subwolumeny w liscie */
    struct sv_collect c = {0}; gh2_fs_subvol_list(&fs, sv_collect_cb, &c);
    CHECK_EQ(c.n, 2);   /* default + snap */
    uint64_t sid = 0;
    CHECK(sv_has_name(&c, "snap", &sid));
    CHECK(sv_has_name(&c, "default", NULL));

    /* snapshot dostepny (stary stan), aktywny nowy */
    char buf[32];
    memset(buf, 0, sizeof(buf)); gh2_fs_read_subvol(&fs, sid, "/f", buf, 31, 0);
    CHECK_EQ(strcmp(buf, "persisted-data"), 0);
    memset(buf, 0, sizeof(buf)); gh2_fs_read(&fs, "/f", buf, 31, 0);
    CHECK_EQ(strcmp(buf, "changed-after-snap"), 0);

    /* refcounty odbudowane mark-sweep == in-memory */
    check_refcount_eq_marksweep(&fs);

    /* fsck aktywnego: brak EIO/I/O errora (read-only walidator) */
    int issues = -1;
    int fr = gh2_fsck(&fs, 0, &issues);
    CHECK_EQ(fr, 0);

    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmpl);
}

/* 5. Bledy/duplikaty nazw. */
static void test_errors(void) {
    char tmpl[] = "/tmp/gh2snap_err_XXXXXX";
    int fd = mkstemp(tmpl); CHECK(fd >= 0); close(fd);

    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmpl), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);

    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
    put_file(&fs, "/f", "x");
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    CHECK_EQ(gh2_fs_snapshot(&fs, "s1"), 0);
    CHECK_EQ(gh2_fs_snapshot(&fs, "s1"), -EEXIST);   /* duplikat nazwy */
    CHECK_EQ(gh2_fs_snapshot(&fs, ""), -EINVAL);
    CHECK_EQ(gh2_fs_snapshot(&fs, NULL), -EINVAL);

    /* read z nieistniejacego subwolumenu */
    char buf[8];
    CHECK_EQ(gh2_fs_read_subvol(&fs, 999, "/f", buf, 8, 0), -ENOENT);

    check_refcount_eq_marksweep(&fs);

    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmpl);
}

/* 6. DELETE: snapshot, rozejscie CoW, delete -> zwolnione TYLKO bloki wylaczne snapshotu;
 *    oryginal + wspolne nietkniete (czytelne); refcount==mark-sweep; fsck==0; brak wycieku. */
static void test_delete(void) {
    char tmpl[] = "/tmp/gh2snap_del_XXXXXX";
    int fd = mkstemp(tmpl); CHECK(fd >= 0); close(fd);

    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmpl), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);

    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    /* dane bazowe: plik /shared (nigdy nie zmieniany) + /f (zmieniany po snapshocie). Wiele
     * plikow, by fs-tree mialo wezly internal -> czesc wspoldzielona, czesc wylaczna. */
    for (int i = 0; i < 40; i++) {
        char p[32]; snprintf(p, sizeof(p), "/keep%02d", i);
        put_file(&fs, p, "shared-content-stays-after-delete");
    }
    put_file(&fs, "/shared", "NEVER-MODIFIED-KEEPS");
    put_file(&fs, "/f", "before-snapshot");
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    CHECK_EQ(gh2_fs_snapshot(&fs, "s1"), 0);
    uint64_t s1 = 0;
    { struct sv_collect c = {0}; gh2_fs_subvol_list(&fs, sv_collect_cb, &c);
      CHECK(sv_has_name(&c, "s1", &s1)); }
    check_refcount_eq_marksweep(&fs);

    /* rozejscie CoW: modyfikuj /f w oryginale -> nowe wylaczne bloki w oryginale; snapshot
     * trzyma STARE wylaczne bloki (rc 1 po rozejsciu). Bloki /shared + /keep* nietkniete:
     * wspoldzielone rc 2. */
    put_file(&fs, "/f", "AFTER-snapshot-modified-content-much-longer-to-force-new-block");
    CHECK_EQ(gh2_fs_create(&fs, "/only-in-orig", 0644), 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    check_refcount_eq_marksweep(&fs);

    uint64_t free_before = fs.space.nfree;
    /* policz bloki WYLACZNE snapshotu: rc==1 ORAZ osiagalne TYLKO z fs_root snapshotu.
     * Prosciej: po delete oczekujemy, ze nfree wzrosnie o liczbe blokow rc==1 osiagalnych
     * wylacznie z snapshotu. Mierzymy roznice nfree i sprawdzamy refcount==mark-sweep. */

    /* delete snapshotu s1 */
    CHECK_EQ(gh2_fs_subvol_delete(&fs, s1), 0);

    uint64_t free_after = fs.space.nfree;
    CHECK(free_after > free_before);   /* cos zwolniono (wylaczne bloki snapshotu) */
    printf("  delete: nfree %llu->%llu (+%llu zwolnione, wylaczne snapshotu)\n",
           (unsigned long long)free_before, (unsigned long long)free_after,
           (unsigned long long)(free_after - free_before));

    /* s1 znika z listy */
    { struct sv_collect c = {0}; gh2_fs_subvol_list(&fs, sv_collect_cb, &c);
      CHECK(!sv_has_name(&c, "s1", NULL));
      CHECK_EQ(c.n, 1);   /* tylko default */ }

    /* ORYGINAL + wspolne nietkniete i czytelne */
    char buf[80]; memset(buf, 0, sizeof(buf));
    ssize_t rd = gh2_fs_read(&fs, "/f", buf, sizeof(buf) - 1, 0);
    CHECK(rd > 0);
    CHECK_EQ(strcmp(buf, "AFTER-snapshot-modified-content-much-longer-to-force-new-block"), 0);
    memset(buf, 0, sizeof(buf));
    rd = gh2_fs_read(&fs, "/shared", buf, sizeof(buf) - 1, 0);
    CHECK(rd > 0); CHECK_EQ(strcmp(buf, "NEVER-MODIFIED-KEEPS"), 0);
    struct gh2_inode in;
    CHECK_EQ(gh2_fs_getattr(&fs, "/only-in-orig", &in, NULL), 0);
    for (int i = 0; i < 40; i++) {
        char p[32]; snprintf(p, sizeof(p), "/keep%02d", i);
        CHECK_EQ(gh2_fs_getattr(&fs, p, &in, NULL), 0);
    }

    /* s1 niedostepny */
    CHECK_EQ(gh2_fs_getattr_subvol(&fs, s1, "/f", &in, NULL), -ENOENT);

    /* BRAMKA: refcount == mark-sweep (brak wycieku ani premature-free) */
    check_refcount_eq_marksweep(&fs);

    /* fsck czysty */
    int issues = -1;
    CHECK_EQ(gh2_fsck(&fs, 0, &issues), 0);
    CHECK_EQ(issues, 0);

    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmpl);
}

/* 7. Brak wycieku DOKLADNY: snapshot bez rozejscia (wszystko wspoldzielone) -> delete zwalnia
 *    DOKLADNIE 0 blokow danych/fs-tree (tylko wpisy drzewa korzeni CoW); oryginal pelny. */
static void test_delete_no_divergence(void) {
    char tmpl[] = "/tmp/gh2snap_delnd_XXXXXX";
    int fd = mkstemp(tmpl); CHECK(fd >= 0); close(fd);

    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmpl), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);

    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
    put_file(&fs, "/f", "data-that-is-shared");
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    uint64_t used_pre_snap = fs.space.nblocks - fs.space.nfree;
    CHECK_EQ(gh2_fs_snapshot(&fs, "s1"), 0);
    uint64_t s1 = 0;
    { struct sv_collect c = {0}; gh2_fs_subvol_list(&fs, sv_collect_cb, &c);
      CHECK(sv_has_name(&c, "s1", &s1)); }
    check_refcount_eq_marksweep(&fs);

    /* delete BEZ modyfikacji oryginalu: wszystkie bloki fs_root wspoldzielone (rc==2) -> dec
     * do rc==1, NIC nie zwolnione (poza ewentualnymi wezlami drzewa korzeni CoW). */
    CHECK_EQ(gh2_fs_subvol_delete(&fs, s1), 0);
    uint64_t used_post_del = fs.space.nblocks - fs.space.nfree;

    /* oryginal pelny i czytelny */
    char buf[32]; memset(buf, 0, sizeof(buf));
    ssize_t rd = gh2_fs_read(&fs, "/f", buf, sizeof(buf) - 1, 0);
    CHECK(rd > 0); CHECK_EQ(strcmp(buf, "data-that-is-shared"), 0);

    /* po delete uzycie wraca ~do stanu sprzed snapshotu (tylko male wahania wezlow drzewa
     * korzeni). Nie powinno byc premature-free oryginalu -> uzycie >= pliku oryginalu. */
    CHECK(used_post_del >= used_pre_snap - 2 && used_post_del <= used_pre_snap + 2);

    check_refcount_eq_marksweep(&fs);
    int issues = -1; CHECK_EQ(gh2_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);

    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmpl);
}

/* 8. Bledy delete: domyslny -> EINVAL, aktywny -> EBUSY, nieistniejacy -> ENOENT. */
static void test_delete_errors(void) {
    char tmpl[] = "/tmp/gh2snap_delerr_XXXXXX";
    int fd = mkstemp(tmpl); CHECK(fd >= 0); close(fd);

    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmpl), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);

    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
    put_file(&fs, "/f", "x");
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    CHECK_EQ(gh2_fs_snapshot(&fs, "s1"), 0);

    /* domyslny (id 1) -> EINVAL */
    CHECK_EQ(gh2_fs_subvol_delete(&fs, GH2_SUBVOL_DEFAULT), -EINVAL);
    /* aktywny == domyslny tu (id 1) -> juz pokryte; sztucznie aktywny != domyslny niemozliwy
     * bez przelaczania, wiec dowodzimy EBUSY przez ustawienie active_subvol na s1. */
    uint64_t s1 = 0;
    { struct sv_collect c = {0}; gh2_fs_subvol_list(&fs, sv_collect_cb, &c);
      CHECK(sv_has_name(&c, "s1", &s1)); }
    uint64_t saved_active = fs.active_subvol;
    fs.active_subvol = s1;
    CHECK_EQ(gh2_fs_subvol_delete(&fs, s1), -EBUSY);   /* aktywny -> EBUSY */
    fs.active_subvol = saved_active;
    /* nieistniejacy -> ENOENT */
    CHECK_EQ(gh2_fs_subvol_delete(&fs, 99999), -ENOENT);

    /* po nieudanych delete: stan niezmieniony, refcount==mark-sweep, s1 wciaz jest */
    check_refcount_eq_marksweep(&fs);
    { struct sv_collect c = {0}; gh2_fs_subvol_list(&fs, sv_collect_cb, &c);
      CHECK(sv_has_name(&c, "s1", NULL)); }

    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmpl);
}

/* 9. Sekwencja create/delete wielu snapshotow -> refcount==mark-sweep, brak wycieku/premature. */
static void test_create_delete_sequence(void) {
    char tmpl[] = "/tmp/gh2snap_seq_XXXXXX";
    int fd = mkstemp(tmpl); CHECK(fd >= 0); close(fd);

    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmpl), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);

    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    for (int i = 0; i < 50; i++) {
        char p[32]; snprintf(p, sizeof(p), "/base%02d", i);
        put_file(&fs, p, "stable-base-data-content");
    }
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    uint64_t free_baseline = fs.space.nfree;

    /* utworz s1,s2,s3 z modyfikacjami pomiedzy (rozejscia), potem usun w innej kolejnosci */
    put_file(&fs, "/f", "a"); CHECK_EQ(gh2_fs_commit(&fs), 0);
    CHECK_EQ(gh2_fs_snapshot(&fs, "s1"), 0); check_refcount_eq_marksweep(&fs);
    put_file(&fs, "/f", "bb-longer"); CHECK_EQ(gh2_fs_commit(&fs), 0);
    CHECK_EQ(gh2_fs_snapshot(&fs, "s2"), 0); check_refcount_eq_marksweep(&fs);
    put_file(&fs, "/f", "ccc-even-longer-content"); CHECK_EQ(gh2_fs_commit(&fs), 0);
    CHECK_EQ(gh2_fs_snapshot(&fs, "s3"), 0); check_refcount_eq_marksweep(&fs);

    uint64_t id1, id2, id3;
    { struct sv_collect c = {0}; gh2_fs_subvol_list(&fs, sv_collect_cb, &c);
      CHECK(sv_has_name(&c, "s1", &id1));
      CHECK(sv_has_name(&c, "s2", &id2));
      CHECK(sv_has_name(&c, "s3", &id3)); }

    /* usun w kolejnosci s2, s1, s3 -> po kazdym refcount==mark-sweep + fsck==0 */
    CHECK_EQ(gh2_fs_subvol_delete(&fs, id2), 0); check_refcount_eq_marksweep(&fs);
    { int is = -1; CHECK_EQ(gh2_fsck(&fs, 0, &is), 0); CHECK_EQ(is, 0); }
    CHECK_EQ(gh2_fs_subvol_delete(&fs, id1), 0); check_refcount_eq_marksweep(&fs);
    { int is = -1; CHECK_EQ(gh2_fsck(&fs, 0, &is), 0); CHECK_EQ(is, 0); }
    CHECK_EQ(gh2_fs_subvol_delete(&fs, id3), 0); check_refcount_eq_marksweep(&fs);
    { int is = -1; CHECK_EQ(gh2_fsck(&fs, 0, &is), 0); CHECK_EQ(is, 0); }

    /* wszystkie snapshoty usuniete -> tylko default; uzycie wraca ~do baseline (brak wycieku) */
    { struct sv_collect c = {0}; gh2_fs_subvol_list(&fs, sv_collect_cb, &c);
      CHECK_EQ(c.n, 1); }
    uint64_t free_end = fs.space.nfree;
    /* aktywny ma teraz /f (ostatnia tresc) + base* -> uzyje kilku blokow ponad baseline,
     * ale NIE moze byc wycieku setek blokow. Dopuszczamy maly narzut (nowe bloki /f). */
    printf("  seq: free baseline=%llu end=%llu (delta=%lld)\n",
           (unsigned long long)free_baseline, (unsigned long long)free_end,
           (long long)free_baseline - (long long)free_end);
    CHECK(free_end <= free_baseline);                 /* aktywny dolozyl /f */
    CHECK(free_baseline - free_end < 20);             /* brak wycieku (tylko bloki /f) */

    /* oryginal czytelny */
    char buf[32]; memset(buf, 0, sizeof(buf));
    gh2_fs_read(&fs, "/f", buf, 31, 0);
    CHECK_EQ(strcmp(buf, "ccc-even-longer-content"), 0);

    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmpl);
}

int main(void) {
    RUN_TEST(test_sharing);
    RUN_TEST(test_isolation);
    RUN_TEST(test_multi_snapshot);
    RUN_TEST(test_persistence);
    RUN_TEST(test_errors);
    RUN_TEST(test_delete);
    RUN_TEST(test_delete_no_divergence);
    RUN_TEST(test_delete_errors);
    RUN_TEST(test_create_delete_sequence);
    return TEST_SUMMARY();
}
