# ghostfs v2

**ghostfs v2** is a from-scratch **copy-on-write (CoW) filesystem** written in C, in the spirit
of btrfs/ZFS but deliberately small and self-contained. It stores everything inside a single
container file *or a raw block device* using its own binary on-disk format (`GHOSTFS\x02`) and is
accessed through a **FUSE driver** or a **command-line tool** that share one core library.

It is an educational/experimental filesystem — capable and *very* heavily tested (≈9 million
assertions, AddressSanitizer/UBSan clean, every component adversarially reviewed) — but not a
drop-in replacement for a production filesystem.

> The repository also contains the original **v1** journaled core; the tools auto-detect the
> on-disk format by magic number and read both. New filesystems use the v2 (CoW) format.

---

## Why copy-on-write?

A CoW filesystem never overwrites live data. Every change writes new blocks and atomically
swaps a single root pointer. This buys three things almost for free, and ghostfs v2 ships all
three:

* **Crash safety without a journal.** The commit point is one atomic superblock write
  (double-buffered ping-pong). A power loss leaves you with either the old consistent state or
  the new one — never a torn in-between. There is no write-ahead log to replay.
* **Snapshots and writable clones.** Because untouched blocks are shared, a snapshot is just a
  second reference to the current tree. Divergence happens lazily as either side is modified.
* **Self-healing.** Metadata is stored in two copies; a read that fails its checksum transparently
  falls back to the duplicate and repairs the bad copy.

---

## Features

### Copy-on-write core
* **Atomic commits, no journal** — a generic CoW B-tree keyed by `(objectid, type, offset)`;
  changes copy the path to the root and a **double superblock (ping-pong)** swaps the root in a
  single, checksummed, read-back-verified write.
* **Crash-consistent by construction** — proven by an exhaustive fault-injection sweep
  (a write fault is injected at *every* write point of a commit; after a simulated crash and
  remount the filesystem is always `fsck`-clean and the state is strictly old-or-new).
* **Per-block CRC32 checksums** on every tree node and data block (corruption → `-EIO`, or a
  self-heal from the duplicate copy).

### Snapshots
* **Writable snapshots / subvolumes** with O(1)-divergence block sharing (eager reference
  counting; the refcount of a block is a pure function of the trees and is rebuilt at mount).
* `snapshot`, `subvol-list`, `subvol-del`. Modifying one subvolume never affects another; a
  snapshot deletion frees exactly the blocks it exclusively owned.

### Self-healing
* **DUP metadata** (`--dup`): every B-tree node is written twice. On a checksum mismatch the
  read returns the good copy and rewrites the corrupt one. A filesystem can survive bit-rot in
  any single copy of any metadata node and still read correctly.

### Transparent compression
* **zlib compression** (`--compress`): file data is stored in compressed chunk-extents (a chunk
  of logical blocks packed into fewer physical blocks). Incompressible data falls back to raw
  with no expansion. Compression happens before encryption.

### Encryption
* **AES-256-XTS at-rest encryption** (`GHOSTFS_KEY`): keys derived from a passphrase with
  PBKDF2-HMAC-SHA256, verified on mount, wiped from memory on unmount. Everything except the raw
  superblocks (which hold only the salt and a verifier) is encrypted on disk.

### POSIX surface
* Files, directories, symbolic links, hard links, and **special files** (`mknod`: FIFO, socket,
  char/block device nodes).
* `read`/`write`/`truncate`, sparse files (holes read as zeros), in-place editing.
* Full metadata: mode, uid/gid, atime/mtime/ctime, nlink — `chmod`/`chown`/`utimens`, `rename`.
* **Extended attributes** (xattr): `set`/`get`/`list`/`remove`, exposed through FUSE
  (`setfattr`/`getfattr`).
* **`fsck` with `--repair`** — verifies the tree (orphaned inodes, dangling directory entries,
  `nlink`) and repairs it atomically and durably.

### Drivers
* **FUSE driver** with a reader/writer lock (parallel reads, serialized writes), lazy `flush`
  (durability on `fsync`/unmount), and tuned I/O (1 MiB writes, splice, writeback cache).
* **CLI** (`ghostfs-cli`) for scripted access and offline operations.
* Runs on a container file **or a real block device**.

---

## On-disk format (`GHOSTFS\x02`)

A container/device is a sequence of 4096-byte blocks:

```
[ superblock A ][ superblock B ][ ............ CoW B-tree blocks ............ ]
   block 0         block 1        block 2 .. end  (nodes + data, never overwritten in place)
```

* **Dual superblock** (blocks 0 and 1): each carries a generation number and a self-checksum.
  A commit writes the *other* slot (`generation & 1`), so the last good state is never
  overwritten — this is the atomic commit point. Superblocks are written raw (unencrypted) and
  hold the encryption salt + verifier.
* **CoW B-tree(s)**: a generic copy-on-write B-tree with variable-length items. Trees:
  * a **root tree** of subvolumes/snapshots, each pointing to an **fs tree**;
  * each **fs tree** holds inodes, directory entries, file extents, xattrs, and symlink targets
    as keyed items;
  * the superblock points at the root tree.
* **Block pointers** carry a checksum and an optional duplicate location (self-heal).
* **Free space and reference counts** are derived from the trees by a mark-and-sweep at mount —
  there is no separate, self-referential extent tree.

Little-endian, x86-64 assumed.

---

## Building

**Dependencies:** a C11 compiler, GNU Make, `libfuse3` (+`pkg-config`), OpenSSL (`libcrypto`),
zlib (`-lz`), `pthreads`. For the test suite, `attr` (`setfattr`/`getfattr`) is optional.

```sh
# Debian/Ubuntu
sudo apt-get install -y build-essential pkg-config libfuse3-dev libssl-dev zlib1g-dev attr

make cli     # builds build/ghostfs-cli
make fuse    # builds build/ghostfs   (the FUSE driver)
make test    # builds and runs all unit tests
```

---

## Usage

### CLI

```sh
# create a v2 (copy-on-write) filesystem; 0 inodes = v2 has no fixed inode pool
./build/ghostfs-cli format2 /tmp/store.gfs 65536 0

# ...optionally with self-healing metadata and/or compression and/or encryption:
GHOSTFS_KEY="correct horse battery staple" \
  ./build/ghostfs-cli format2 /tmp/store.gfs 65536 0 --dup --compress

echo "hello" > /tmp/in.txt
./build/ghostfs-cli put   /tmp/store.gfs /tmp/in.txt /notes.txt
./build/ghostfs-cli mkdir /tmp/store.gfs /dir
./build/ghostfs-cli ls    /tmp/store.gfs /
./build/ghostfs-cli get   /tmp/store.gfs /notes.txt /tmp/out.txt
./build/ghostfs-cli df    /tmp/store.gfs
./build/ghostfs-cli fsck  /tmp/store.gfs            # add --repair to fix

# snapshots
./build/ghostfs-cli snapshot    /tmp/store.gfs before-change
./build/ghostfs-cli subvol-list /tmp/store.gfs
./build/ghostfs-cli subvol-del  /tmp/store.gfs 2
```

### Mounting via FUSE

```sh
mkdir -p /mnt/ghost
./build/ghostfs /tmp/store.gfs /mnt/ghost -f       # -f = foreground
# ... use /mnt/ghost like any directory; setfattr/getfattr work ...
fusermount3 -u /mnt/ghost
```

> 💾 **Durability.** Writes are committed on `fsync`/`fdatasync`, on buffer pressure, or at
> unmount — not on every `close()`. As with any filesystem, `umount` (or `sync`) before pulling a
> removable drive. Crash consistency is always preserved.

### Encryption

```sh
GHOSTFS_KEY="..." ./build/ghostfs-cli format2 /tmp/enc.gfs 65536 0
GHOSTFS_KEY="..." ./build/ghostfs        /tmp/enc.gfs /mnt/ghost -f
# wrong/missing key -> EACCES; raw bytes on disk reveal no plaintext.
```

---

## Design notes

ghostfs v2 was built as a chain of well-scoped, individually reviewed sub-projects. Three design
decisions kept the hardest parts tractable:

* **Eager reference counting instead of a self-referential extent tree.** A block's refcount is
  the number of subvolume trees that reach it — a pure function of the on-disk structure, rebuilt
  by mark-and-sweep at mount (like the free map). This avoids btrfs's hardest component entirely
  and, crucially, meant the **B-tree core was never modified** to add snapshots (only the
  allocator's "free" became "decrement, free at zero").
* **Per-container compression flag with chunk-extents**, leaving the uncompressed per-block data
  path untouched — no mixing of granularities within a container.
* **DUP at the node-I/O layer**, again leaving the B-tree algorithms untouched.

| | Sub-project |
|---|---|
| v2.0 | Format + dual ping-pong superblock (atomic commit, no journal) |
| v2.1 | Generic CoW B-tree (insert/lookup/delete/split/merge, variable-length items) |
| v2.2 | Allocator (mark-sweep free map + CoW deferred-free) |
| v2.3 | FS tree + inodes (full POSIX metadata, per-operation atomicity) |
| v2.4 | File data (extents, CoW, per-block checksums) |
| v2.5 | Atomic commit + crash-consistency fault-injection sweep |
| v2.6 | FUSE/CLI integration (mountable; v1/v2 auto-detect) |
| v2.7 | Snapshots (eager-refcount, block sharing) |
| v2.8 | Self-healing (DUP metadata + read-repair) |
| v2.9 | Compression (chunk-extents + zlib) |
| + | Encryption (AES-256-XTS), xattr, `fsck --repair` |

---

## Testing

```sh
make test        # unit suites — ≈9 million assertions
make test-asan   # everything under AddressSanitizer + UBSan
./tests/integration_v2.sh   # live FUSE v2: round-trip, edit, kill -9 + recovery, snapshots,
                            # compression, encryption, xattr
```

Every sub-project went through a dedicated **adversarial review** with independent fault-injection
and stress harnesses. These reviews found and fixed ~12 real critical bugs during development
(B-tree split overflow on variable-length items, per-operation atomicity on `ENOSPC`, rename of
two hardlinks to the same inode, premature-free of compressed-snapshot blocks, an `nlink`
corruption in `fsck --repair`, …). Design specs and plans live under `docs/`.

---

## Limitations / future work

* The on-disk format is little-endian/x86-64; cross-endian portability is not implemented.
* Snapshot creation is O(tree) (eager refcounting), not O(1) — simple and correct; lazy
  refcounting is future work.
* Compression is per-container (`--compress`), zlib only; data blocks are single-copy (DUP covers
  metadata). Multi-device / RAID is not implemented.
* It is a userspace (FUSE) filesystem, so it is not directly bootable as a Linux root filesystem.
* Inline code comments are in Polish (the development language); all documentation here is English.

---

## License

See the repository for license terms.
