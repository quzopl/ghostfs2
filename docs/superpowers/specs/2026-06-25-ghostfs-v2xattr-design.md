# ghostfs v2 — xattr: design

**Data:** 2026-06-25 **Część:** v2 follow-up.

## Model
xattr przechowywane jako itemy w drzewie FS: klucz `(ino, GH2_XATTR_ITEM=7, name_hash)` →
spakowana lista wpisów `{u16 name_len; u32 value_len; name[]; value[]}` (kolizje hash → wiele
wpisów w jednej wartości, jak DIR_ITEM; lookup po dokładnej nazwie). name_hash = FNV-1a (jak dir).
Wartości inline w wartości B-drzewa (≤ GH2_LEAF_MAX_VAL ~2004 B na cały item; pojedynczy xattr
name+value musi się zmieścić). Większe → -E2BIG/-ENOSPC (block-backed = przyszłość).

## API (`gh2_fs_*`, fasada, errno jak v1/POSIX)
- `gh2_fs_setxattr(fs, path, name, value, size, flags)` — XATTR_CREATE/REPLACE flagi (jak v1);
  dodaj/zastąp wpis; -EEXIST (CREATE+istnieje), -ENODATA (REPLACE+brak). Atomowość per-op.
- `gh2_fs_getxattr(fs, path, name, buf, size)` → długość; size==0 → zwróć rozmiar; -ENODATA brak;
  -ERANGE za mały bufor.
- `gh2_fs_listxattr(fs, path, buf, size)` → wszystkie nazwy (null-separated), długość; -ERANGE.
- `gh2_fs_removexattr(fs, path, name)` — usuń wpis; usuń item gdy pusty; -ENODATA brak.
- Zwalnianie: przy usuwaniu i-węzła (unlink nlink→0, rmdir) usuń wszystkie XATTR_ITEM (ino,7,*).
- Fasada gh2_vfs: gfs_setxattr/getxattr/listxattr/removexattr v2 → core (dziś -ENOTSUP). v1→core.

## Testy (`tests/test_v2xattr.c` + integration_v2.sh setfattr/getfattr)
1. set/get round-trip (różne rozmiary name/value, binarne wartości); list; remove; -ENODATA brak.
2. wiele xattr na i-węźle; XATTR_CREATE/REPLACE flagi; nadpisanie; kolizja hash (pakowanie).
3. getxattr size==0→rozmiar; -ERANGE za mały; listxattr null-separated, -ERANGE.
4. zwalnianie: unlink pliku z xattr → XATTR_ITEM usunięte (mapa/fsck spójne, wyciek=0).
5. persystencja remount; spójność z snapshot (xattr w snapshocie izolowane), --compress, szyfr.
6. integracja CLI/FUSE: setfattr/getfattr na zamontowanym v2 (FUSE setxattr/getxattr/listxattr/
   removexattr → gfs → core). Regresja v1 xattr + nie-xattr v2 nietknięte. ASan czyste.
