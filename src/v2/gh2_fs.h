#ifndef GH2_FS_H
#define GH2_FS_H
/* ghostfs v2.3 — drzewo systemu plikow (i-wezly + katalogi) na fundamencie v2.0-2.2.
 *
 * Itemy drzewa FS (klucz = objectid,type,offset):
 *   INODE_ITEM   (ino, GH2_INODE_ITEM, 0)        -> struct gh2_inode
 *   DIR_ITEM     (parent_ino, GH2_DIR_ITEM, hash)-> spakowana lista wpisow (kolizje hash)
 *   EXTENT_DATA  (ino, GH2_EXTENT_DATA, off)      -> dane plikow (v2.4, zarezerwowane)
 *   SYMLINK_DATA (ino, GH2_SYMLINK_DATA, 0)       -> cel symlinka (v2.3 Task 2, zarezerwowane)
 *
 * Commit uproszczony (v2.3): kazda mutacja CoW -> nowy fs_root w pamieci; gh2_fs_commit
 * utrwala (fsync danych -> SB.root_tree -> txn_alloc_commit). Mount: mark-sweep z SB.root_tree.
 */
#include <stdint.h>
#include <sys/types.h>   /* ssize_t */
#include "v2/gh2_btree.h"
#include "v2/gh2_space.h"
#include "v2/gh2_super.h"
#include "v2/gh2_format.h"
#include "block.h"

/* ---- typy itemow drzewa FS (pole `type` w gh2_key) ---- */
#define GH2_INODE_ITEM    1
#define GH2_DIR_ITEM      2
#define GH2_EXTENT_DATA   3
#define GH2_SYMLINK_DATA  4
/* ---- v2.7: item drzewa korzeni (wpis subwolumenu) ----
 * Klucz: (subvol_id, GH2_ROOT_ITEM, 0) -> struct gh2_subvol_item.
 * sb.root_tree wskazuje DRZEWO KORZENI (gh2_btree), nie bezposrednio fs-tree. */
#define GH2_ROOT_ITEM     5
/* ---- v2xattr: rozszerzone atrybuty ----
 * Klucz: (ino, GH2_XATTR_ITEM, fnv1a64(name)) -> spakowana lista wpisow
 *   {uint16_t name_len; uint32_t value_len; char name[name_len]; uint8_t value[value_len];}
 * Kolizje hash -> wiele wpisow w jednej wartosci (lookup po DOKLADNEJ nazwie), jak DIR_ITEM. */
#define GH2_XATTR_ITEM    7

/* domyslny (glowny) subwolumen — id 1 */
#define GH2_SUBVOL_DEFAULT 1
#define GH2_SUBVOL_NAME_MAX 64

/* ---- wpis subwolumenu (wartosc GH2_ROOT_ITEM); enkod/dekod = memcpy (deterministyczny) ---- */
struct gh2_subvol_item {
    struct gh2_bptr fs_root;            /* korzen fs-tree tego subwolumenu */
    uint64_t        flags;             /* zarezerwowane (snapshot read-only itp.) */
    char            name[GH2_SUBVOL_NAME_MAX];  /* NUL-terminowana nazwa */
};

/* ---- typy plikow (gh2_inode.type / gh2_dirent.ftype), reuzycie semantyki v1 ---- */
#define GH2_FT_FILE     1
#define GH2_FT_DIR      2
#define GH2_FT_SYMLINK  3
#define GH2_FT_FIFO     4
#define GH2_FT_SOCK     5
#define GH2_FT_CHR      6
#define GH2_FT_BLK      7

/* korzen = ino 1 (DIR); numeracja od 2 (next_ino w SB.reserved[0]) */
#define GH2_ROOT_INO    1

/* ---- v2enc: szyfrowanie (reuzycie krypto v1: AES-256-XTS + PBKDF2) ----
 * liczba iteracji KDF (stala; jak v1 default). Mount re-derive z ta sama stala. */
#define GH2_KDF_ITERS   200000u

/* ---- i-wezel (wartosc INODE_ITEM); enkod/dekod = memcpy (deterministyczny) ---- */
struct gh2_inode {
    uint16_t type;       /* GH2_FT_* */
    uint16_t mode;       /* bity uprawnien (rwx) */
    uint32_t uid;
    uint32_t gid;
    uint32_t nlink;
    uint64_t size;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint64_t rdev;       /* dla urzadzen */
    uint64_t generation;
    uint32_t flags;
    uint32_t _pad;
};

/* ---- ekstent danych pliku (wartosc EXTENT_DATA), 1 item / blok 4 KB ---- */
/* Klucz: (ino, GH2_EXTENT_DATA, file_off) gdzie file_off = offset bloku (wielokrotnosc 4096).
 * disk_block = blok fizyczny z danymi; csum = crc32 plaintextu bloku; raw_len = wazne bajty
 * (ostatni blok < 4096); dup_block=0 (v2.8); comp_algo=0/comp_len=raw_len (v2.9).
 * Dziura = brak itemu -> odczyt zwraca zera. Enkod/dekod = memcpy (deterministyczny). */
struct gh2_extent {
    uint64_t disk_block;   /* blok fizyczny z danymi (0 = nieuzywany) */
    uint64_t dup_block;    /* kopia (samonaprawa) — 0 (zarezerwowane v2.8) */
    uint32_t csum;         /* crc32 plaintextu bloku */
    uint16_t comp_algo;    /* 0 = bez kompresji (zarezerwowane v2.9) */
    uint16_t flags;        /* zarezerwowane */
    uint32_t raw_len;      /* wazne bajty w bloku (<=4096) */
    uint32_t comp_len;     /* dlugosc danych skompresowanych (= raw_len gdy bez kompresji) */
};

/* ---- v2.9: naglowek chunk extentu (GH2_EXTENT_COMP); po nim uint64_t blocks[nblocks] ----
 * comp_algo: 0=raw, 1=zlib. raw_len = nieskompresowane bajty w chunku (<=GH2_COMP_CHUNK*4096;
 * ostatni czesciowy). comp_len = bajty (s)kompresowane zapisane (==raw_len gdy raw).
 * nblocks = ceil(comp_len/4096). csum = crc32 zapisanych (comp_len) bajtow. Enkod deterministyczny. */
struct gh2_cext_hdr {
    uint8_t  comp_algo;   /* 0 = raw, 1 = zlib */
    uint8_t  flags;       /* zarezerwowane */
    uint16_t nblocks;     /* liczba blokow fizycznych w blocks[] */
    uint32_t raw_len;     /* bajty nieskompresowane w chunku */
    uint32_t comp_len;    /* bajty zapisane (skompresowane lub raw) */
    uint32_t csum;        /* crc32 zapisanych danych (comp_len bajtow) */
};

/* maks. liczba blokow fizycznych w chunku (nigdy nie wieksza niz logicznych) */
#define GH2_CEXT_MAX_BLOCKS  GH2_COMP_CHUNK
/* maks. rozmiar zakodowanej wartosci chunk extentu (hdr + blocks[]) */
#define GH2_CEXT_MAX_VAL  ((uint32_t)sizeof(struct gh2_cext_hdr) + \
                           GH2_CEXT_MAX_BLOCKS * (uint32_t)sizeof(uint64_t))

/* dekoduj naglowek + liste blokow chunk extentu (uzywane tez przez mark-sweep w gh2_space.c).
 * Zwraca 0 i wypelnia *hdr + blocks[] (do max_blocks pozycji) lub -EIO przy niespojnosci. */
int gh2_cext_decode(const uint8_t *buf, uint32_t len, struct gh2_cext_hdr *hdr,
                    uint64_t *blocks, uint16_t max_blocks);

/* ---- wpis katalogu (naglowek; po nim name[name_len], bez NUL) ---- */
struct gh2_dirent {
    uint64_t ino;
    uint8_t  ftype;      /* GH2_FT_* */
    uint8_t  _pad;
    uint16_t name_len;
    /* uint8_t name[name_len]; */
};

/* ---- stan zamontowanego systemu plikow ---- */
struct gh2_fs {
    struct gh_dev          dev;
    struct gh2_superblock  sb;
    struct gh2_bptr        root_tree;    /* korzen DRZEWA KORZENI (= SB.root_tree po commicie) */
    uint64_t               active_subvol;/* id aktywnego subwolumenu (domyslnie 1) */
    struct gh2_bptr        fs_root;      /* korzen fs-tree aktywnego subwolumenu (mutowany) */
    struct gh2_space       space;        /* mapa wolnej przestrzeni (mark-sweep przy mount) */
    struct gh2_txn_alloc   alloc;        /* alokator transakcyjny */
    uint64_t               next_ino;     /* monotoniczny (SB.reserved[0]) */
    int                    compress;     /* v2.9: 1 = kontener --compress (sciezka chunk extentow) */
};

/* ---- callback readdir: nazwa (NUL-terminowana), dlugosc, ino, ftype ---- */
typedef int (*gh2_readdir_cb)(const char *name, uint16_t name_len, uint64_t ino,
                              uint8_t ftype, void *ctx);

/* ---- API (lustro v1 gh_fs_*, te same errno) ---- */

/* sformatuj urzadzenie jako ghostfs v2.3 i utworz korzen (ino 1, DIR, nlink 2, 0755) + commit. */
int gh2_fs_format(struct gh_dev *dev, uint64_t total_blocks, uint32_t flags);

/* v2enc: format z opcjonalnym szyfrowaniem. passphrase==NULL -> bez szyfrowania (== gh2_fs_format).
 * passphrase!=NULL -> gh_crypto_random(salt) + derive(klucz) + verifier do SB; flaga GH2_SB_ENCRYPTED;
 * dev->cipher ustawiony PRZED tworzeniem drzew (wezly+dane szyfrowane). SB pisany RAW (trzyma sol).
 * Po sukcesie dev->cipher zostaje ustawiony — caller (CLI/fasada) zarzadza wipe (np. gh2_fs_unmount
 * po mount, lub recznie). Puste haslo "" -> -EINVAL. */
int gh2_fs_format_key(struct gh_dev *dev, uint64_t total_blocks, uint32_t flags,
                      const char *passphrase);

/* zamontuj: gh2_mount + space_init + mark-sweep z SB.root_tree + next_ino z SB.reserved[0]. */
int gh2_fs_mount(struct gh2_fs *fs, struct gh_dev *dev);

/* v2enc: mount z opcjonalnym kluczem. Jesli SB ma GH2_SB_ENCRYPTED:
 *   passphrase==NULL/"" -> -EACCES; derive(passphrase, sb.enc_salt) -> klucz; verifier mismatch -> -EACCES;
 *   inaczej malloc gh_cipher, dev->cipher ustawiony PRZED mark-sweep (deszyfrowanie dziala).
 * Jesli SB nieszyfrowany -> jak gh2_fs_mount (cipher NULL). gh2_fs_mount = wrapper (NULL). */
int gh2_fs_mount_key(struct gh2_fs *fs, struct gh_dev *dev, const char *passphrase);

/* odmontuj: zwolnij alokator/mape; jesli dev->cipher ustawiony -> gh_crypto_wipe + free (brak wycieku). */
void gh2_fs_unmount(struct gh2_fs *fs);

/* utrwal stan: fsync danych -> SB.root_tree=fs_root + SB.reserved[0]=next_ino + commit_super
 * -> txn_alloc_commit (zwolnij stare bloki CoW). */
int gh2_fs_commit(struct gh2_fs *fs);

/* sciezka -> ino (od korzenia, komponenty, `.`/`..`); -ENOENT/-ENOTDIR. */
int gh2_path_resolve(struct gh2_fs *fs, const char *path, uint64_t *out_ino);

/* getattr: sciezka -> i-wezel (+ino). */
int gh2_fs_getattr(struct gh2_fs *fs, const char *path, struct gh2_inode *out, uint64_t *out_ino);

/* utworz zwykly plik / katalog. -EEXIST gdy istnieje; -ENOENT/-ENOTDIR dla sciezki rodzica. */
int gh2_fs_create(struct gh2_fs *fs, const char *path, uint16_t mode);
int gh2_fs_mkdir(struct gh2_fs *fs, const char *path, uint16_t mode);

/* readdir: emituj `.`, `..`, wpisy katalogu. -ENOTDIR gdy nie katalog. */
int gh2_fs_readdir(struct gh2_fs *fs, const char *path, gh2_readdir_cb cb, void *ctx);

/* ---- Task 2: operacje metadanych ---- */

/* usun zwykly plik / symlink / urzadzenie. nlink--; przy 0 usun INODE_ITEM (+SYMLINK_DATA).
 * -EISDIR gdy katalog; -ENOENT gdy brak. */
int gh2_fs_unlink(struct gh2_fs *fs, const char *path);

/* usun PUSTY katalog. -ENOTEMPTY gdy niepusty; -ENOTDIR gdy nie katalog; -ENOENT. */
int gh2_fs_rmdir(struct gh2_fs *fs, const char *path);

/* hardlink newpath -> ten sam i-wezel co oldpath. nlink++. -EPERM dla katalogu;
 * -EEXIST gdy newpath istnieje; -ENOENT. */
int gh2_fs_link(struct gh2_fs *fs, const char *oldpath, const char *newpath);

/* symlink: utworz i-wezel SYMLINK + SYMLINK_DATA(cel) + DIR_ITEM. -EEXIST/-ENOENT. */
int gh2_fs_symlink(struct gh2_fs *fs, const char *target, const char *path);

/* odczytaj cel symlinka do buf (NUL-terminowany). -EINVAL gdy nie symlink. Zwraca 0/-errno. */
int gh2_fs_readlink(struct gh2_fs *fs, const char *path, char *buf, size_t buflen);

/* utworz i-wezel specjalny (FIFO/SOCK/CHR/BLK). mode zawiera bity S_IF*. rdev dla CHR/BLK.
 * -EINVAL dla zlego typu; -EEXIST/-ENOENT. */
int gh2_fs_mknod(struct gh2_fs *fs, const char *path, uint32_t mode, uint64_t rdev);

/* przenies/zmien nazwe. flags: 0 (zwykly, nadpisuje cel). -EINVAL dla petli katalogu. */
int gh2_fs_rename(struct gh2_fs *fs, const char *oldpath, const char *newpath, uint32_t flags);

/* metadane */
int gh2_fs_chmod(struct gh2_fs *fs, const char *path, uint16_t mode);
int gh2_fs_chown(struct gh2_fs *fs, const char *path, uint32_t uid, uint32_t gid);
int gh2_fs_utimens(struct gh2_fs *fs, const char *path, uint64_t atime, uint64_t mtime);

/* obetnij rozmiar (v2.4): skroc -> zwolnij ekstenty o off>=align(size) + popraw ostatni
 * czesciowy (raw_len/wyzeruj ogon); rozszerz -> tylko gh2_inode.size (sparse). Tylko plik. */
int gh2_fs_truncate(struct gh2_fs *fs, const char *path, uint64_t size);

/* ---- v2.4: dane plikow (ekstenty) ---- */

/* zapisz `len` bajtow z `buf` na offset `off` w pliku `path`. Tylko GH2_FT_FILE
 * (-EISDIR/-EINVAL). inode.size=max(size,off+len), mtime. Atomowa per-wywolanie.
 * Zwraca liczbe zapisanych bajtow (>=0) lub -errno. */
ssize_t gh2_fs_write(struct gh2_fs *fs, const char *path, const void *buf, size_t len, uint64_t off);

/* odczytaj do `len` bajtow z offsetu `off` pliku `path` do `buf`. Ogranicz do inode.size;
 * dziury = zera; niezgodnosc csum -> dup_block lub -EIO. Zwraca liczbe bajtow lub -errno. */
ssize_t gh2_fs_read(struct gh2_fs *fs, const char *path, void *buf, size_t len, uint64_t off);

/* statystyki: wypelnia liczbe blokow i wolnych z mapy. */
struct gh2_statfs {
    uint64_t total_blocks;
    uint64_t free_blocks;
    uint64_t block_size;
};
int gh2_fs_statfs(struct gh2_fs *fs, struct gh2_statfs *out);

/* odczyt i-wezla po ino (pomocnik, eksponowany dla testow/fsck). */
int gh2_fs_read_inode(struct gh2_fs *fs, uint64_t ino, struct gh2_inode *out);

/* ---- v2.7 (Task 3): snapshoty ---- */

/* utworz snapshot aktywnego subwolumenu pod nazwa `name`:
 *  - nowy subvol_id (max istniejacy + 1);
 *  - wpis (new_id, GH2_ROOT_ITEM, 0) -> {fs_root = aktywny fs_root (TEN SAM bptr), name}
 *    do drzewa korzeni (CoW);
 *  - inc refcount WSZYSTKICH blokow osiagalnych z fs_root (wezly + bloki danych) — defer do commit;
 *  - cala operacja atomowa (savepoint/rollback); sukces -> commit.
 * Po snapshocie refcount kazdego wspoldzielonego bloku == 2 (default + snapshot).
 * -EEXIST gdy nazwa juz istnieje; -ENAMETOOLONG; -errno przy I/O. */
int gh2_fs_snapshot(struct gh2_fs *fs, const char *name);

/* ---- lista subwolumenow (iteracja wpisow GH2_ROOT_ITEM drzewa korzeni) ---- */
typedef int (*gh2_subvol_cb)(uint64_t id, const char *name, void *ctx);
int gh2_fs_subvol_list(struct gh2_fs *fs, gh2_subvol_cb cb, void *ctx);

/* ---- usun subwolumen (snapshot) ----
 *  - odmowa dla DOMYSLNEGO (id 1) i AKTYWNEGO -> -EINVAL/-EBUSY; nieistniejacy -> -ENOENT;
 *  - dekrementuj refcount WSZYSTKICH blokow osiagalnych z fs_root tego subwolumenu (wezly +
 *    EXTENT_DATA disk_block/dup_block) przez defer_dec; bloki WSPOLDZIELONE (rc>1) zostaja,
 *    WYLACZNE (rc==1) zwalniane przy commit;
 *  - usun wpis (subvol_id, GH2_ROOT_ITEM, 0) z drzewa korzeni (CoW);
 *  - atomowo (savepoint/rollback przy bledzie); sukces -> commit.
 * Zwraca 0 lub -errno. */
int gh2_fs_subvol_delete(struct gh2_fs *fs, uint64_t subvol_id);

/* ---- dostep READ do wybranego subwolumenu (snapshotu) — read-only, bez mutacji ---- */
/* getattr w kontekscie subwolumenu subvol_id (uzywa jego fs_root z drzewa korzeni). */
int gh2_fs_getattr_subvol(struct gh2_fs *fs, uint64_t subvol_id, const char *path,
                          struct gh2_inode *out, uint64_t *out_ino);
/* read w kontekscie subwolumenu subvol_id. Zwraca liczbe bajtow lub -errno. */
ssize_t gh2_fs_read_subvol(struct gh2_fs *fs, uint64_t subvol_id, const char *path,
                           void *buf, size_t len, uint64_t off);

/* ---- v2xattr: rozszerzone atrybuty (errno jak v1/POSIX) ----
 * flags: 0, GH2_XATTR_CREATE (zawiedz gdy istnieje -> -EEXIST),
 *        GH2_XATTR_REPLACE (zawiedz gdy brak -> -ENODATA). Atomowo per-op. */
#define GH2_XATTR_CREATE  1
#define GH2_XATTR_REPLACE 2

int gh2_fs_setxattr(struct gh2_fs *fs, const char *path, const char *name,
                    const void *value, size_t size, int flags);
ssize_t gh2_fs_getxattr(struct gh2_fs *fs, const char *path, const char *name,
                        void *buf, size_t size);
ssize_t gh2_fs_listxattr(struct gh2_fs *fs, const char *path, char *buf, size_t size);
int gh2_fs_removexattr(struct gh2_fs *fs, const char *path, const char *name);

/* ---- v2.5: fsck (read-only walidator spojnosci zatwierdzonego stanu) ---- */
/* Weryfikuje (z fs->fs_root): kazdy INODE_ITEM osiagalny z roota (brak sierot); kazdy
 * DIR_ITEM -> istniejacy INODE_ITEM; nlink == liczba dowiazan (katalog: 2 + podkatalogi);
 * kazdy disk_block ekstentu: w granicach + oznaczony w mapie + UNIKALNY (brak wspoldzielenia,
 * nie koliduje z blokiem-wezlem drzewa); size spojne z ekstentami (brak ekstentu off>=align(size)).
 * *issues = liczba niespojnosci (0 = OK). Zwraca 0 lub -errno przy bledzie I/O.
 *
 * repair: 0 = read-only (jak dotad, tylko zlicza). repair!=0 = po detekcji wykonaj pass
 * naprawczy w JEDNEJ transakcji na lokalnej kopii fs_root (savepoint na poczatku):
 *   (a) usun wiszace wpisy DIR_ITEM (child bez INODE_ITEM);
 *   (b) zwolnij sieroty (INODE_ITEM nieosiagalny) — wszystkie itemy i-wezla (ekstenty/chunki/
 *       xattr/symlink/wlasne DIR_ITEM jesli katalog);
 *   (c) popraw nlink i-wezlow osiagalnych z mismatch.
 * Przy bledzie -> rollback (fs_root NIETKNIETY), zwraca -errno. Sukces -> fs->fs_root = naprawiony
 * (caller robi gh2_fs_commit dla trwalosci). *issues = liczba WYKRYTYCH (przed naprawa). */
int gh2_fsck(struct gh2_fs *fs, int repair, int *issues);

/* ---- seam testowy: wymuszony hash DIR_ITEM (dowod pakowania kolizji) ---- */
int gh2_fs_test_dir_add(struct gh2_fs *fs, uint64_t parent, uint64_t hash,
                        const char *name, uint16_t nlen, uint64_t ino, uint8_t ftype);
int gh2_fs_test_dir_lookup(struct gh2_fs *fs, uint64_t parent, uint64_t hash,
                           const char *name, uint16_t nlen, uint64_t *out_ino);

/* ---- seamy testowe fsck --repair: usun wpis bez zwalniania i-wezla (sierota); wymus nlink ---- */
int gh2_fs_test_dir_remove(struct gh2_fs *fs, uint64_t parent, const char *name, uint16_t nlen);
int gh2_fs_test_set_nlink(struct gh2_fs *fs, uint64_t ino, uint32_t nlink);

/* ---- seam testowy: wymuszony hash XATTR_ITEM (dowod pakowania kolizji xattr) ---- */
int gh2_fs_test_xattr_set_hashed(struct gh2_fs *fs, uint64_t ino, uint64_t hash,
                                 const char *name, const void *value, size_t size);
ssize_t gh2_fs_test_xattr_get_hashed(struct gh2_fs *fs, uint64_t ino, uint64_t hash,
                                     const char *name, void *buf, size_t size);

#endif
