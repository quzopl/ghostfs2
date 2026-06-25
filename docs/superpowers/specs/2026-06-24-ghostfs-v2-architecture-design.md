# ghostfs v2 — architektura (CoW B-tree): master design

**Data:** 2026-06-24
**Status:** zatwierdzony kierunek (użytkownik: CoW B-tree; snapshoty + samonaprawa +
kompresja; pełna autonomia, jakość przede wszystkim, bez skrótów).
**Relacja do v1:** v1 (A–R) pozostaje działający i wspierany. v2 to **nowy rdzeń** pod tym
samym publicznym API `gh_fs_*`, z nowym formatem on-disk `GHOSTFS\x02` (osobny magic →
współistnienie; brak migracji in-place — kopiowanie v1→v2 narzędziem).

## Cele
1. **Brak amplifikacji 2×** — CoW + ping-pong superbloku zastępują dziennik fizyczny; każdy
   blok zapisywany raz do nowej lokacji, commit atomowy przez podmianę korzenia.
2. **Snapshoty i klony zapisywalne** — O(1), współdzielenie bloków przez refcounty.
3. **Samonaprawa** — metadane DUP (dwie kopie); niezgodność sumy → czytaj duplikat.
4. **Kompresja** — per-ekstent (lz4/zstd) przed szyfrowaniem.
Jakość: TDD + ASan/UBSan + recenzja adwersaryjna per pod-projekt; crash-consistency to bramka.

## Format on-disk `GHOSTFS\x02`

### Bloki
4096 B (jak v1). Adres = numer bloku fizycznego (jedno urządzenie; format rezerwuje
`device_id` pod multi-device w przyszłości). **CoW**: bloki nigdy nie nadpisywane w miejscu,
**wyjątek**: dwa sloty superbloku (ping-pong).

### Superblok (podwójny, ping-pong)
Dwa sloty w stałych lokacjach (np. blok 0 i blok 1, lub 0 i N-1 dla rozłączności awarii).
Pola: `magic="GHOSTFS\x02"`, `generation` (u64, rośnie co commit), `root_tree_ptr`
(adres korzenia drzewa korzeni + jego suma + dup-adres), `total_blocks`, `block_size`,
flagi (encrypted/compress_default/dup_meta), `enc_salt`/`enc_verifier` (jak v1),
`uuid`, `sb_csum`. Mount: czytaj oba sloty, wybierz najwyższą `generation` z poprawną
`sb_csum`. Commit zapisuje slot `(generation & 1)` → nigdy nie żywy.

### Węzeł B-drzewa (leaf/internal)
Nagłówek: `{ csum, generation, owner_tree, level, nritems, flags }`. Klucz item:
`struct gh_key { u64 objectid; u8 type; u64 offset; }` (porządek leksykograficzny).
- **internal**: tablica `(key, child_block_ptr)` gdzie `child_block_ptr =
  {block, dup_block, csum, generation}`.
- **leaf**: itemy `(key, offset_w_bloku, size)` + dane itemów rosnące od końca bloku
  (układ btrfs leaf). CoW: modyfikacja → nowy blok (alokowany), rodzic dostaje nowy wskaźnik
  (też CoW), w górę do korzenia.

### Drzewa
- **drzewo korzeni** (root tree): itemy `(subvol_id, ROOT_ITEM, 0) → {fs_root_ptr,
  generation}` dla każdego subwolumenu/snapshotu; oraz `(EXTENT_TREE_OBJ, ROOT_ITEM,0)` i
  `(CSUM_TREE_OBJ, ROOT_ITEM,0)`. Superblok → korzeń tego drzewa.
- **drzewo FS** (per subwolumen): w jednym drzewie:
  - `(ino, INODE_ITEM, 0) → gh_inode2 {mode,uid,gid,size,nlink,atime,mtime,ctime,flags,
    generation,rdev}`,
  - `(parent_ino, DIR_ITEM, hash(name)) → {child_ino, type, name_len, name[]}` (kolizje
    hash → łańcuch w offsecie),
  - `(ino, INODE_REF, parent_ino) → {name}` (do nlink/rename/ścieżek wstecznych),
  - `(ino, EXTENT_DATA, file_off) → {disk_block, disk_nr, raw_len, comp_len, comp_algo,
    flags}` (flags: inline/regular; comp_algo: none/lz4/zstd). Dziura = brak itemu.
- **drzewo ekstentów** (alokacja+refcount): `(block, EXTENT_ITEM, len) → {refcount, flags
  (data/meta)}`. Alokacja = znajdź wolny zakres (free-space w pamięci budowany z drzewa /
  cache), wstaw item. Zwolnienie/CoW = dekrement refcount; 0 → wolny. Snapshot dzieli bloki
  (refcount++).
- **drzewo sum** (csum): `(LOGICAL=block, CSUM, 0) → crc32` (lub csum w ekstencie dla danych).
  Metadane: suma w nagłówku węzła. Samonaprawa: niezgodność → czytaj `dup_block`.

### CoW commit (transakcja, atomowy, bez dziennika)
1. Zmiany akumulowane w pamięci (brudne węzły, nowe ekstenty danych zapisane CoW od razu).
2. Commit: alokuj nowe bloki dla brudnych węzłów, zapisz **od liści w górę** (rodzic dostaje
   nowy wskaźnik dziecka), aż do nowych korzeni drzew FS/ekstentów/sum.
3. `fsync` wszystkich nowych bloków (dane+drzewa).
4. Zbuduj nowe drzewo korzeni wskazujące nowe korzenie; zapisz (CoW); `fsync`.
5. Zapisz superblok w slot `gen&1` z `root_tree_ptr` + `generation+1` + suma; `fsync`.
Awaria w 1–4 → stary superblok ważny → stary spójny stan. Awaria w 5 (rozdarcie) → drugi
slot (stary) ważny. **Zawsze spójne; każdy blok raz.**

### Samonaprawa (DUP metadane)
Domyślnie węzły drzew zapisywane w **dwóch** lokacjach (`block`+`dup_block`). Odczyt: czytaj
`block`, weryfikuj sumę; niezgodność → czytaj `dup_block`, weryfikuj; sukces → (opcjonalnie)
przepisz `block`. Dane: DUP opcjonalny (koszt 2×); pełna samonaprawa danych = multi-device
(rezerwa formatu, później).

### Kompresja
Zapis ekstentu: jeśli `compress_default` i blok się kompresuje (lz4/zstd) z zyskiem → zapisz
skompresowany, item `comp_len<raw_len`, `comp_algo`. Odczyt: dekompresja po deszyfrowaniu.
Kolejność: surowe → kompresja → szyfrowanie (XTS) → dysk. Suma liczona z surowego (jak v1).

### Szyfrowanie / sumy
Reużycie `crypto.c` (AES-256-XTS po numerze bloku fizycznego) i `csum.c` (CRC32 slice-by-8).
Sumy z plaintextu (przed kompresją? — z surowego, by samonaprawa wykrywała też błędy
dekompresji: suma surowego ekstentu + osobna suma bloku fizycznego). Decyzja w pod-projekcie.

## Reużycie v1 (warstwy nad rdzeniem)
`crypto.c`, `csum.c`, `block.c` (gh_dev_open/create/close, gh_disk_read/write — I/O+szyfr.),
`fuse_main.c`, `cli.c`. Publiczne API `gh_fs_*` (create/mkdir/read/write/...) **bez zmiany
sygnatur** → FUSE/CLI działają na v2 niemal bez zmian (mount wykrywa magic v1/v2 i kieruje do
odpowiedniego rdzenia).

## Dekompozycja na pod-projekty (każdy: spec→plan→TDD→ASan→recenzja→merge→push)
- **v2.0 — Format & superblok:** `GHOSTFS\x02`, podwójny superblok, generation+csum,
  `mkfs` v2, mount wybiera najnowszy ważny slot, commit ping-pong (stub alokatora). Bramka:
  awaria przy zapisie SB → mount wybiera stary, spójny.
- **v2.1 — CoW B-tree (prymityw):** generyczne drzewo (klucz, leaf/internal, lookup/insert/
  delete/split/merge, CoW, sumy węzłów). Najcięższe testy (korona). Bez I/O wyższego poziomu.
- **v2.2 — Alokator + drzewo ekstentów:** alokacja/zwolnienie bloków, refcounty, free-space.
- **v2.3 — Drzewo FS + i-węzły:** inode/dir/ref itemy; create/mkdir/lookup/readdir/getattr/
  unlink/rmdir/rename na API `gh_fs_*`.
- **v2.4 — Dane plików (ekstenty):** read/write/truncate przez EXTENT_DATA + CoW bloków.
- **v2.5 — Atomowy commit:** pełna sekwencja commit (CoW do korzeni + ping-pong SB);
  crash-consistency sweep (bramka).
- **v2.6 — Integracja FUSE/CLI:** wykrywanie v1/v2 po magicu; montowalny v2 end-to-end;
  integracja (round-trip, edycja, kill-9+recover, enc).
- **v2.7 — Snapshoty:** subwolumeny, snapshot O(1), refcount sharing, rozejście CoW; CLI/ioctl.
- **v2.8 — Samonaprawa:** DUP metadane, read-repair (wstrzykiwanie korupcji → naprawa z dup).
- **v2.9 — Kompresja:** lz4/zstd per-ekstent.
Kolejność = zależności (0→1→2→3→4→5→6, potem 7/8/9 niezależne). Po każdym: push (standing
instruction). `raport*`/`benchmark*` nietykalne.

## Świadome ryzyka / decyzje
- To duży, wieloetapowy projekt — budujemy fundament (0–2) solidnie, bo wszystko na nim stoi.
- Refcounty snapshotów: pełny btrfs ma skomplikowane backrefy; v2 zaczyna od prostego
  refcount per-ekstent (wystarcza dla snapshot/CoW; backrefy = ewentualne rozszerzenie).
- Format projektowany z zapasem (pola dup/comp/device_id) — by nie zamykać 7/8/9.
- Crash-consistency CoW jest *prostsza* niż dziennik (jeden punkt atomowy: superblok), ale
  ordering (dane+drzewo durable PRZED superblokiem) jest krytyczny → sweep awarii to bramka.
