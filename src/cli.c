#include "fs.h"
#include "super.h"
#include "crypto.h"
#include "v2/gh2_vfs.h"
#include "v2/gh2_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

static int print_entry(const char *name, uint16_t type, void *ctx) {
    (void)ctx; (void)type; printf("%s\n", name); return 0;
}

/* mount przez fasade (auto-detect v1/v2). Najpierw GHOSTFS_KEY (v1+v2 zaszyfrowane), potem
 * prompt hasla przy -EACCES (zaszyfrowany kontener bez/zlym kluczem). */
static int cli_mount(struct gfs *g, const char *path) {
    const char *env = getenv("GHOSTFS_KEY");
    int r = gfs_mount(g, path, (env && env[0]) ? env : NULL);
    if (r == -EACCES) {
        char pw[256];
        if (gh_read_password("Haslo ghostfs: ", pw, sizeof(pw)) == 0)
            r = gfs_mount(g, path, pw);
    }
    return r;
}

static int cmd_format(int argc, char **argv) {
    /* format <plik> <bloki> <inody> [--v2] [--dup|--selfheal] */
    if (argc < 5) { fprintf(stderr, "format <plik> <bloki> <inody> [--v2] [--dup]\n"); return 2; }
    int v2 = 0; uint32_t v2flags = 0;
    for (int i = 5; i < argc; i++) {
        if (!strcmp(argv[i], "--v2")) v2 = 1;
        else if (!strcmp(argv[i], "--dup") || !strcmp(argv[i], "--selfheal"))
            v2flags |= GH2_SB_DUP_META;
        else if (!strcmp(argv[i], "--compress"))
            v2flags |= GH2_SB_COMPRESS;
    }
    uint64_t nblk = strtoull(argv[3], 0, 10);
    if (v2) {
        struct gh_dev dev;
        int r = gh_dev_create(argv[2], nblk, &dev);
        if (r) { fprintf(stderr, "format2: %s\n", strerror(-r)); return 1; }
        if (gh_bcache_create(&dev) != 0) { gh_dev_close(&dev); fprintf(stderr, "format2: ENOMEM\n"); return 1; }
        const char *fkey = getenv("GHOSTFS_KEY");
        r = gh2_fs_format_key(&dev, nblk, v2flags, (fkey && fkey[0]) ? fkey : NULL);
        if (dev.cipher) { gh_crypto_wipe(dev.cipher); free(dev.cipher); dev.cipher = NULL; }
        gh_bcache_destroy(&dev); gh_dev_close(&dev);
        if (r) { fprintf(stderr, "format2: %s\n", strerror(-r)); return 1; }
        return 0;
    }
    const char *key = getenv("GHOSTFS_KEY");
    int r = gh_format_enc(argv[2], nblk, strtoull(argv[4],0,10),
                          (key && key[0]) ? key : NULL);
    if (r) { fprintf(stderr, "format: %s\n", strerror(-r)); return 1; }
    return 0;
}

/* format2 <plik> <bloki> [<inody>] [--dup|--selfheal] [--compress] — tworzy kontener v2 (CoW B-tree).
 * --dup / --selfheal: wlacz samonaprawe (DUP metadane; flaga GH2_SB_DUP_META).
 * --compress: wlacz kompresje danych (chunk extenty + zlib; flaga GH2_SB_COMPRESS). Kombinowalne. */
static int cmd_format2(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "format2 <plik> <bloki> [<inody>] [--dup] [--compress]\n"); return 2; }
    uint64_t nblk = strtoull(argv[3], 0, 10);
    uint32_t flags = 0;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--dup") || !strcmp(argv[i], "--selfheal"))
            flags |= GH2_SB_DUP_META;
        else if (!strcmp(argv[i], "--compress"))
            flags |= GH2_SB_COMPRESS;
    }
    struct gh_dev dev;
    int r = gh_dev_create(argv[2], nblk, &dev);
    if (r) { fprintf(stderr, "format2: %s\n", strerror(-r)); return 1; }
    if (gh_bcache_create(&dev) != 0) { gh_dev_close(&dev); fprintf(stderr, "format2: ENOMEM\n"); return 1; }
    const char *fkey = getenv("GHOSTFS_KEY");
    r = gh2_fs_format_key(&dev, nblk, flags, (fkey && fkey[0]) ? fkey : NULL);
    if (dev.cipher) { gh_crypto_wipe(dev.cipher); free(dev.cipher); dev.cipher = NULL; }
    gh_bcache_destroy(&dev); gh_dev_close(&dev);
    if (r) { fprintf(stderr, "format2: %s\n", strerror(-r)); return 1; }
    return 0;
}

static int cmd_ls(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "ls <plik> <ścieżka>\n"); return 2; }
    struct gfs fs; int r = cli_mount(&fs, argv[2]);
    if (r) { fprintf(stderr, "mount: %s\n", strerror(-r)); return 1; }
    r = gfs_readdir(&fs, argv[3], print_entry, NULL);
    gfs_unmount(&fs);
    if (r) { fprintf(stderr, "ls: %s\n", strerror(-r)); return 1; }
    return 0;
}

static int cmd_put(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "put <plik> <src> <ścieżka>\n"); return 2; }
    struct gfs fs; if (cli_mount(&fs, argv[2])) return 1;
    int in = open(argv[3], O_RDONLY);
    if (in < 0) { perror("open src"); gfs_unmount(&fs); return 1; }
    gfs_create(&fs, argv[4], 0644);
    char buf[65536]; ssize_t k; uint64_t off = 0;
    while ((k = read(in, buf, sizeof(buf))) > 0) {
        if (gfs_write(&fs, argv[4], buf, (size_t)k, off) != k) {
            fprintf(stderr, "write failed\n"); close(in); gfs_unmount(&fs); return 1;
        }
        off += (uint64_t)k;
    }
    close(in); gfs_sync(&fs); gfs_unmount(&fs); return 0;
}

static int cmd_get(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "get <plik> <ścieżka> <dst>\n"); return 2; }
    struct gfs fs; if (cli_mount(&fs, argv[2])) return 1;
    struct gfs_attr st;
    if (gfs_getattr(&fs, argv[3], &st)) {
        fprintf(stderr, "brak: %s\n", argv[3]); gfs_unmount(&fs); return 1;
    }
    int out = open(argv[4], O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (out < 0) { perror("open dst"); gfs_unmount(&fs); return 1; }
    char buf[65536]; uint64_t off = 0;
    while (off < st.size) {
        ssize_t r = gfs_read(&fs, argv[3], buf, sizeof(buf), off);
        if (r <= 0) break;
        if (write(out, buf, (size_t)r) != r) break;
        off += (uint64_t)r;
    }
    close(out); gfs_unmount(&fs); return 0;
}

static int cmd_mkdir(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "mkdir <plik> <ścieżka>\n"); return 2; }
    struct gfs fs; if (cli_mount(&fs, argv[2])) return 1;
    int r = gfs_mkdir(&fs, argv[3], 0755);
    if (!r) gfs_sync(&fs);
    gfs_unmount(&fs);
    if (r) { fprintf(stderr, "mkdir: %s\n", strerror(-r)); return 1; }
    return 0;
}

static int cmd_rm(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "rm <plik> <ścieżka>\n"); return 2; }
    struct gfs fs; if (cli_mount(&fs, argv[2])) return 1;
    int r = gfs_unlink(&fs, argv[3]);
    if (r == -EISDIR) r = gfs_rmdir(&fs, argv[3]);
    if (!r) gfs_sync(&fs);
    gfs_unmount(&fs);
    if (r) { fprintf(stderr, "rm: %s\n", strerror(-r)); return 1; }
    return 0;
}

static int cmd_fsck(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "fsck <plik> [--repair]\n"); return 2; }
    int repair = 0;
    for (int i = 3; i < argc; i++)
        if (!strcmp(argv[i], "--repair")) repair = 1;
    struct gfs fs; if (cli_mount(&fs, argv[2])) return 1;
    /* v1 i v2 wykonuja trwale naprawy poziomu drzewa; po naprawie utrwal (gfs_sync). */
    int issues = 0;
    int r = gfs_fsck(&fs, repair, &issues);
    if (!r && repair) gfs_sync(&fs);
    gfs_unmount(&fs);
    if (r) { fprintf(stderr, "fsck: %s\n", strerror(-r)); return 1; }
    if (repair)
        printf("fsck: %d niespójności (naprawiono)\n", issues);
    else
        printf("fsck: %d niespójności\n", issues);
    return issues ? 1 : 0;
}

static int cmd_mv(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "mv <plik> <stara> <nowa>\n"); return 2; }
    struct gfs fs; if (cli_mount(&fs, argv[2])) return 1;
    int r = gfs_rename(&fs, argv[3], argv[4], 0);
    if (!r) gfs_sync(&fs);
    gfs_unmount(&fs);
    if (r) { fprintf(stderr, "mv: %s\n", strerror(-r)); return 1; }
    return 0;
}
static int cmd_truncate(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "truncate <plik> <ścieżka> <rozmiar>\n"); return 2; }
    struct gfs fs; if (cli_mount(&fs, argv[2])) return 1;
    int r = gfs_truncate(&fs, argv[3], strtoull(argv[4], 0, 10));
    if (!r) gfs_sync(&fs);
    gfs_unmount(&fs);
    if (r) { fprintf(stderr, "truncate: %s\n", strerror(-r)); return 1; }
    return 0;
}
static int cmd_ln(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "ln <plik> <cel> <link>\n"); return 2; }
    struct gfs fs; if (cli_mount(&fs, argv[2])) return 1;
    int r = gfs_link(&fs, argv[3], argv[4]);
    if (!r) gfs_sync(&fs);
    gfs_unmount(&fs);
    if (r) { fprintf(stderr, "ln: %s\n", strerror(-r)); return 1; }
    return 0;
}
static int cmd_lns(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "lns <plik> <cel> <link>\n"); return 2; }
    struct gfs fs; if (cli_mount(&fs, argv[2])) return 1;
    int r = gfs_symlink(&fs, argv[3], argv[4]);
    if (!r) gfs_sync(&fs);
    gfs_unmount(&fs);
    if (r) { fprintf(stderr, "lns: %s\n", strerror(-r)); return 1; }
    return 0;
}
static int cmd_chmod(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "chmod <plik> <ścieżka> <ósemkowo>\n"); return 2; }
    struct gfs fs; if (cli_mount(&fs, argv[2])) return 1;
    int r = gfs_chmod(&fs, argv[3], (uint16_t)strtol(argv[4], 0, 8));
    if (!r) gfs_sync(&fs);
    gfs_unmount(&fs);
    if (r) { fprintf(stderr, "chmod: %s\n", strerror(-r)); return 1; }
    return 0;
}
static int cmd_stat(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "stat <plik> <ścieżka>\n"); return 2; }
    struct gfs fs; if (cli_mount(&fs, argv[2])) return 1;
    struct gfs_attr st;
    int r = gfs_getattr(&fs, argv[3], &st);
    if (!r) {
        const char *t = st.type == GH_DIR ? "katalog"
                      : st.type == GH_SYMLINK ? "symlink"
                      : st.type == GH_FIFO ? "fifo"
                      : st.type == GH_SOCK ? "gniazdo"
                      : st.type == GH_CHR ? "urzadzenie-znakowe"
                      : st.type == GH_BLK ? "urzadzenie-blokowe" : "plik";
        if (st.type == GH_CHR || st.type == GH_BLK)
            printf("typ=%s rdev=%llu mode=%o nlink=%u uid=%u gid=%u\n",
                   t, (unsigned long long)st.rdev,
                   st.mode, st.nlink, st.uid, st.gid);
        else
            printf("typ=%s rozmiar=%llu mode=%o nlink=%u uid=%u gid=%u\n",
                   t, (unsigned long long)st.size,
                   st.mode, st.nlink, st.uid, st.gid);
    }
    gfs_unmount(&fs);
    if (r) { fprintf(stderr, "stat: %s\n", strerror(-r)); return 1; }
    return 0;
}
static int cmd_df(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "df <plik>\n"); return 2; }
    struct gfs fs; if (cli_mount(&fs, argv[2])) return 1;
    struct gfs_statfs s; int r = gfs_statfs(&fs, &s); gfs_unmount(&fs);
    if (r) { fprintf(stderr, "df: %s\n", strerror(-r)); return 1; }
    printf("bloki: %llu/%llu wolne, i-wezly: %llu/%llu wolne, blok=%u B\n",
           (unsigned long long)s.free_blocks, (unsigned long long)s.total_blocks,
           (unsigned long long)s.free_inodes, (unsigned long long)s.total_inodes,
           s.block_size);
    return 0;
}

static int cmd_mknod(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "mknod <plik> <ścieżka> <fifo|sock|chr|blk> [major minor]\n"); return 2; }
    uint16_t mode; uint64_t rdev = 0;
    const char *t = argv[4];
    if (!strcmp(t, "fifo")) mode = S_IFIFO | 0644;
    else if (!strcmp(t, "sock")) mode = S_IFSOCK | 0644;
    else if (!strcmp(t, "chr") || !strcmp(t, "blk")) {
        if (argc < 7) { fprintf(stderr, "chr/blk wymaga major minor\n"); return 2; }
        mode = (!strcmp(t, "chr") ? S_IFCHR : S_IFBLK) | 0666;
        rdev = makedev((unsigned)strtoul(argv[5], 0, 10), (unsigned)strtoul(argv[6], 0, 10));
    } else { fprintf(stderr, "nieznany typ: %s\n", t); return 2; }
    struct gfs fs; if (cli_mount(&fs, argv[2])) return 1;
    int r = gfs_mknod(&fs, argv[3], mode, rdev);
    if (!r) gfs_sync(&fs);
    gfs_unmount(&fs);
    if (r) { fprintf(stderr, "mknod: %s\n", strerror(-r)); return 1; }
    return 0;
}

/* ---- snapshoty (tylko v2) ---- */
static int cmd_snapshot(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "snapshot <kontener> <nazwa>\n"); return 2; }
    struct gfs fs; if (cli_mount(&fs, argv[2])) return 1;
    int r = gfs_snapshot(&fs, argv[3]);
    if (!r) gfs_sync(&fs);
    gfs_unmount(&fs);
    if (r) { fprintf(stderr, "snapshot: %s\n", strerror(-r)); return 1; }
    return 0;
}

static int snap_list_cb(uint64_t id, const char *name, void *ctx) {
    (void)ctx;
    printf("%llu\t%s\n", (unsigned long long)id, name);
    return 0;
}
static int cmd_subvol_list(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "subvol-list <kontener>\n"); return 2; }
    struct gfs fs; if (cli_mount(&fs, argv[2])) return 1;
    int r = gfs_subvol_list(&fs, snap_list_cb, NULL);
    gfs_unmount(&fs);
    if (r) { fprintf(stderr, "subvol-list: %s\n", strerror(-r)); return 1; }
    return 0;
}

static int cmd_subvol_del(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "subvol-del <kontener> <id>\n"); return 2; }
    struct gfs fs; if (cli_mount(&fs, argv[2])) return 1;
    int r = gfs_subvol_delete(&fs, strtoull(argv[3], 0, 10));
    if (!r) gfs_sync(&fs);
    gfs_unmount(&fs);
    if (r) { fprintf(stderr, "subvol-del: %s\n", strerror(-r)); return 1; }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "uzycie: ghostfs-cli <format|format2|ls|put|get|mkdir|rm|mv|truncate|ln|lns|chmod|mknod|stat|df|fsck|snapshot|subvol-list|subvol-del> ...\n");
        return 2;
    }
    const char *c = argv[1];
    if (!strcmp(c, "format"))  return cmd_format(argc, argv);
    if (!strcmp(c, "format2")) return cmd_format2(argc, argv);
    if (!strcmp(c, "ls"))     return cmd_ls(argc, argv);
    if (!strcmp(c, "put"))    return cmd_put(argc, argv);
    if (!strcmp(c, "get"))    return cmd_get(argc, argv);
    if (!strcmp(c, "mkdir"))  return cmd_mkdir(argc, argv);
    if (!strcmp(c, "rm"))     return cmd_rm(argc, argv);
    if (!strcmp(c, "mv"))       return cmd_mv(argc, argv);
    if (!strcmp(c, "truncate")) return cmd_truncate(argc, argv);
    if (!strcmp(c, "ln"))       return cmd_ln(argc, argv);
    if (!strcmp(c, "lns"))      return cmd_lns(argc, argv);
    if (!strcmp(c, "chmod"))    return cmd_chmod(argc, argv);
    if (!strcmp(c, "mknod"))    return cmd_mknod(argc, argv);
    if (!strcmp(c, "stat"))     return cmd_stat(argc, argv);
    if (!strcmp(c, "df"))       return cmd_df(argc, argv);
    if (!strcmp(c, "fsck"))   return cmd_fsck(argc, argv);
    if (!strcmp(c, "snapshot"))     return cmd_snapshot(argc, argv);
    if (!strcmp(c, "subvol-list"))  return cmd_subvol_list(argc, argv);
    if (!strcmp(c, "subvol-del"))   return cmd_subvol_del(argc, argv);
    fprintf(stderr, "nieznana komenda: %s\n", c);
    return 2;
}
