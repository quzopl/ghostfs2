#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#include "fs.h"
#include "v2/gh2_vfs.h"
#include "crypto.h"
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <pthread.h>

static struct gfs g_fs;
static int g_lock_fd = -1;
/* Efektywny max_read przekazany do fuse_session_new (nasz domyslny 1 MiB
   LUB user override z -o max_read=N). gf_init MUSI ustawic conn->max_read na
   te sama wartosc — libfuse3 wymaga zgodnosci init() i fuse_session_new(). */
static unsigned g_max_read = 1u << 20;

static pthread_rwlock_t g_lock;

#define GF_RD(call) do { pthread_rwlock_rdlock(&g_lock); \
    int _r = (call); pthread_rwlock_unlock(&g_lock); return _r; } while (0)
#define GF_WR(call) do { pthread_rwlock_wrlock(&g_lock); \
    int _r = (call); pthread_rwlock_unlock(&g_lock); return _r; } while (0)

struct rd_ctx { void *buf; fuse_fill_dir_t filler; };
static int rd_cb(const char *name, uint16_t type, void *c) {
    (void)type;
    struct rd_ctx *x = c;
    x->filler(x->buf, name, NULL, 0, 0);
    return 0;
}

/* --- ODCZYT: rdlock --- */
static int gf_getattr(const char *path, struct stat *st, struct fuse_file_info *fi) {
    (void)fi;
    pthread_rwlock_rdlock(&g_lock);
    memset(st, 0, sizeof(*st));
    struct gfs_attr n;
    int r = gfs_getattr(&g_fs, path, &n);
    if (r == 0) {
        mode_t t;
        switch (n.type) {
            case GH_DIR:     t = S_IFDIR;  break;
            case GH_SYMLINK: t = S_IFLNK;  break;
            case GH_FIFO:    t = S_IFIFO;  break;
            case GH_SOCK:    t = S_IFSOCK; break;
            case GH_CHR:     t = S_IFCHR;  break;
            case GH_BLK:     t = S_IFBLK;  break;
            default:         t = S_IFREG;  break;
        }
        st->st_mode = t | n.mode;
        if (n.type == GH_CHR || n.type == GH_BLK) st->st_rdev = (dev_t)n.rdev;
        st->st_nlink = n.nlink; st->st_size = (off_t)n.size;
        st->st_uid = n.uid; st->st_gid = n.gid;
        st->st_atime = n.atime; st->st_mtime = n.mtime; st->st_ctime = n.ctime;
    }
    pthread_rwlock_unlock(&g_lock);
    return r;
}

static int gf_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      off_t off, struct fuse_file_info *fi, enum fuse_readdir_flags fl) {
    (void)off; (void)fi; (void)fl;
    struct rd_ctx x = { buf, filler };
    GF_RD(gfs_readdir(&g_fs, path, rd_cb, &x));
}

static int gf_read(const char *path, char *buf, size_t n, off_t off,
                   struct fuse_file_info *fi) {
    (void)fi;
    pthread_rwlock_rdlock(&g_lock);
    ssize_t r = gfs_read(&g_fs, path, buf, n, (uint64_t)off);
    pthread_rwlock_unlock(&g_lock);
    return (int)r;
}

static int gf_readlink(const char *path, char *buf, size_t size) {
    if (size == 0) return 0;
    pthread_rwlock_rdlock(&g_lock);
    ssize_t r = gfs_readlink(&g_fs, path, buf, size - 1);
    if (r >= 0) buf[r] = '\0';
    pthread_rwlock_unlock(&g_lock);
    return r < 0 ? (int)r : 0;
}

static int gf_statfs(const char *path, struct statvfs *st) {
    (void)path;
    pthread_rwlock_rdlock(&g_lock);
    struct gfs_statfs s; int r = gfs_statfs(&g_fs, &s);
    if (r == 0) {
        memset(st, 0, sizeof(*st));
        st->f_bsize = s.block_size; st->f_frsize = s.block_size;
        st->f_blocks = s.total_blocks; st->f_bfree = s.free_blocks; st->f_bavail = s.free_blocks;
        st->f_files = s.total_inodes; st->f_ffree = s.free_inodes; st->f_favail = s.free_inodes;
        st->f_namemax = s.name_max;
    }
    pthread_rwlock_unlock(&g_lock);
    return r;
}

static int gf_getxattr(const char *path, const char *name, char *value, size_t size) {
    pthread_rwlock_rdlock(&g_lock);
    ssize_t r = gfs_getxattr(&g_fs, path, name, value, size);
    pthread_rwlock_unlock(&g_lock);
    return (int)r;
}

static int gf_listxattr(const char *path, char *list, size_t size) {
    pthread_rwlock_rdlock(&g_lock);
    ssize_t r = gfs_listxattr(&g_fs, path, list, size);
    pthread_rwlock_unlock(&g_lock);
    return (int)r;
}

/* --- ZAPIS: wrlock --- */
static int gf_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)fi; GF_WR(gfs_create(&g_fs, path, mode & 0777));
}
static int gf_mkdir(const char *path, mode_t mode) {
    GF_WR(gfs_mkdir(&g_fs, path, mode & 0777));
}
static int gf_mknod(const char *path, mode_t mode, dev_t rdev) {
    /* mode zawiera bity typu (S_IFMT) — rdzen ich potrzebuje, NIE maskuj */
    GF_WR(gfs_mknod(&g_fs, path, (uint32_t)mode, (uint64_t)rdev));
}
static int gf_unlink(const char *path) { GF_WR(gfs_unlink(&g_fs, path)); }
static int gf_rmdir(const char *path)  { GF_WR(gfs_rmdir(&g_fs, path)); }

static int gf_open(const char *path, struct fuse_file_info *fi) {
    pthread_rwlock_wrlock(&g_lock);          /* wrlock: moze skracac przy O_TRUNC */
    struct gfs_attr n;
    int r = gfs_getattr(&g_fs, path, &n);
    if (r == 0 && (fi->flags & O_TRUNC) && n.type == GH_FILE)
        r = gfs_truncate(&g_fs, path, 0);
    pthread_rwlock_unlock(&g_lock);
    return r;
}

static int gf_write(const char *path, const char *buf, size_t n, off_t off,
                    struct fuse_file_info *fi) {
    (void)fi;
    pthread_rwlock_wrlock(&g_lock);
    ssize_t r = gfs_write(&g_fs, path, buf, n, (uint64_t)off);
    pthread_rwlock_unlock(&g_lock);
    return (int)r;
}

static int gf_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
    (void)fi; GF_WR(gfs_truncate(&g_fs, path, (uint64_t)size));
}

static int gf_utimens(const char *path, const struct timespec tv[2],
                      struct fuse_file_info *fi) {
    (void)fi;
    pthread_rwlock_wrlock(&g_lock);
    uint64_t now = (uint64_t)time(NULL), at = now, mt = now;
    struct gfs_attr n;
    int r = gfs_getattr(&g_fs, path, &n);
    if (r == 0) {
        if (tv) {
            at = (tv[0].tv_nsec == UTIME_NOW) ? now
               : (tv[0].tv_nsec == UTIME_OMIT) ? n.atime : (uint64_t)tv[0].tv_sec;
            mt = (tv[1].tv_nsec == UTIME_NOW) ? now
               : (tv[1].tv_nsec == UTIME_OMIT) ? n.mtime : (uint64_t)tv[1].tv_sec;
        }
        r = gfs_utimens(&g_fs, path, at, mt);
    }
    pthread_rwlock_unlock(&g_lock);
    return r;
}

static int gf_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)fi; GF_WR(gfs_chmod(&g_fs, path, (uint16_t)(mode & 0777)));
}
static int gf_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi) {
    (void)fi; GF_WR(gfs_chown(&g_fs, path, (uint32_t)uid, (uint32_t)gid));
}
static int gf_rename(const char *from, const char *to, unsigned int flags) {
    GF_WR(gfs_rename(&g_fs, from, to, flags));
}
static int gf_flush(const char *path, struct fuse_file_info *fi) {
    (void)path; (void)fi;
    return 0;   /* leniwy: trwalosc tylko na fsync/pojemnosc/unmount (POSIX) */
}
static int gf_fsync(const char *path, int datasync, struct fuse_file_info *fi) {
    (void)path; (void)datasync; (void)fi; GF_WR(gfs_sync(&g_fs));
}
static int gf_symlink(const char *target, const char *linkpath) {
    GF_WR(gfs_symlink(&g_fs, target, linkpath));
}
static int gf_link(const char *from, const char *to) {
    GF_WR(gfs_link(&g_fs, from, to));
}
static int gf_setxattr(const char *path, const char *name, const char *value,
                       size_t size, int flags) {
    GF_WR(gfs_setxattr(&g_fs, path, name, value, size, flags));
}
static int gf_removexattr(const char *path, const char *name) {
    GF_WR(gfs_removexattr(&g_fs, path, name));
}

/* strojenie I/O: wieksze zapisy (mniej round-tripow), splice (zero-copy),
   writeback cache (jadro scala zapisy sekwencyjne). Negocjowane z jadrem (want & capable). */
static void *gf_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    if (conn->capable & FUSE_CAP_WRITEBACK_CACHE) conn->want |= FUSE_CAP_WRITEBACK_CACHE;
    if (conn->capable & FUSE_CAP_SPLICE_WRITE)     conn->want |= FUSE_CAP_SPLICE_WRITE;
    if (conn->capable & FUSE_CAP_SPLICE_READ)      conn->want |= FUSE_CAP_SPLICE_READ;
    if (conn->capable & FUSE_CAP_SPLICE_MOVE)      conn->want |= FUSE_CAP_SPLICE_MOVE;
    conn->max_write = 1u << 20;   /* 1 MiB: 8x mniej round-tripow dla duzych zapisow. */
    conn->max_read = g_max_read;  /* tnie round-tripy ODCZYTU (jadro domyslnie ~128 KiB);
       == wartosc -o max_read (nasz 1 MiB lub user override) — libfuse3 wymaga zgodnosci. */
    conn->max_readahead = 8u << 20; /* 8 MiB prefetch: pipeline'uje sekwencyjny odczyt
       (nakladanie latencji FUSE na konsumpcje aplikacji). */
    cfg->auto_cache = 1;          /* retencja page-cache + auto-inwalidacja po mtime/size:
       koherentny re-odczyt (single-writer + flock — swieza tresc po nadpisaniu). */
    return NULL;
}

static const struct fuse_operations ops = {
    .init = gf_init,
    .getattr = gf_getattr, .readdir = gf_readdir, .create = gf_create,
    .mkdir = gf_mkdir, .mknod = gf_mknod, .unlink = gf_unlink, .rmdir = gf_rmdir,
    .open = gf_open, .read = gf_read, .write = gf_write,
    .truncate = gf_truncate, .utimens = gf_utimens,
    .chmod = gf_chmod, .chown = gf_chown, .rename = gf_rename,
    .statfs = gf_statfs, .flush = gf_flush, .fsync = gf_fsync,
    .symlink = gf_symlink, .readlink = gf_readlink, .link = gf_link,
    .setxattr = gf_setxattr, .getxattr = gf_getxattr,
    .listxattr = gf_listxattr, .removexattr = gf_removexattr,
};

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "uzycie: ghostfs <kontener> <punkt_montowania> [opcje fuse]\n");
        return 2;
    }
    const char *container = argv[1];
    /* blokada: tylko jeden proces na kontener */
    g_lock_fd = open(container, O_RDWR);
    if (g_lock_fd < 0) { perror("open kontener"); return 1; }
    if (flock(g_lock_fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "kontener juz w uzyciu (flock)\n"); return 1;
    }
    const char *env_key = getenv("GHOSTFS_KEY");
    int r = gfs_mount(&g_fs, container, (env_key && env_key[0]) ? env_key : NULL);
    if (r == -EACCES) {
        char pw[256];
        if (gh_read_password("Haslo ghostfs: ", pw, sizeof(pw)) == 0)
            r = gfs_mount(&g_fs, container, pw);
    }
    if (r) { fprintf(stderr, "mount: %s\n", strerror(-r)); return 1; }

    if (pthread_rwlock_init(&g_lock, NULL) != 0) {
        fprintf(stderr, "pthread_rwlock_init nieudane\n");
        gfs_unmount(&g_fs); flock(g_lock_fd, LOCK_UN); close(g_lock_fd); return 1;
    }

    /* przekaz do FUSE argv bez nazwy kontenera (argv[1]).
       Jesli user nie podal max_read -> wstrzykujemy -o max_read=1048576 (1 MiB,
       zgodne z conn->max_read w gf_init): ustawia rozmiar zadan odczytu w
       fuse_session_new. Jesli podal -> uszanuj override, nie dubluj. */
    /* Skanuj WYLACZNIE optstringi FUSE (-o ...), NIE argumenty pozycyjne
       (mountpoint moze zawierac podciag "max_read") ani inne flagi.
       Optstring to lista po przecinkach; szukamy TOKENU "max_read=" (z '='
       zaraz po nazwie, by NIE lapac "max_readahead="). */
    int user_max_read = 0;
    for (int i = 2; i < argc; i++) {
        const char *opt = NULL;
        if (strcmp(argv[i], "-o") == 0) {       /* -o <optstring>: optstring = nastepny argv */
            if (i + 1 >= argc) break;           /* NULL-safe: brak wartosci po -o */
            opt = argv[++i];
        } else if (argv[i][0] == '-' && argv[i][1] == 'o' && argv[i][2] != '\0') {
            opt = argv[i] + 2;                  /* -o<optstring> sklejone */
        } else {
            continue;                           /* mountpoint / -f / -s / -d ... pomijamy */
        }
        /* przejdz tokeny optstringu rozdzielone przecinkami */
        const char *tok = opt;
        while (tok) {
            if (strncmp(tok, "max_read=", 9) == 0) {  /* dokladny token, NIE max_readahead= */
                user_max_read = 1;
                unsigned long v = strtoul(tok + 9, NULL, 10);
                if (v > 0) g_max_read = (unsigned)v; /* zgodnosc gf_init <-> sesja */
            }
            const char *c = strchr(tok, ',');
            tok = c ? c + 1 : NULL;
        }
    }

    int fargc = argc - 1 + (user_max_read ? 0 : 2);
    char **fargv = malloc(sizeof(char*) * (size_t)fargc);
    fargv[0] = argv[0];
    for (int i = 2; i < argc; i++) fargv[i-1] = argv[i];
    if (!user_max_read) {           /* domyslnie 1 MiB (g_max_read juz = 1<<20) */
        fargv[argc-1] = "-o";
        fargv[argc]   = "max_read=1048576";
    }

    int rc = fuse_main(fargc, fargv, &ops, NULL);
    gfs_sync(&g_fs);          /* utrwal stan przed odmontowaniem (leniwy flush) */
    gfs_unmount(&g_fs);
    pthread_rwlock_destroy(&g_lock);
    free(fargv);
    flock(g_lock_fd, LOCK_UN); close(g_lock_fd);
    return rc;
}
