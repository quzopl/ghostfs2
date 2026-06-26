#!/usr/bin/env bash
# ghostfs v2.6 — integracja FUSE/CLI dla kontenera v2 (CoW B-tree, format2).
# Round-trip duzego pliku, edycja in-place, katalogi, ls -l, rm -r, mv, symlink,
# hardlink, df, chmod, mknod (FIFO), truncate, kill -9 + remount -> fsck czysty.
# CLI v2: format2/put/get round-trip/ls/mkdir/stat/df/fsck/rm.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GFS="$ROOT/build/ghostfs"; CLI="$ROOT/build/ghostfs-cli"
CONT=$(mktemp /tmp/ghost_v2.XXXXXX.gfs); MNT=$(mktemp -d)
cleanup() { fusermount3 -u "$MNT" 2>/dev/null || true; rm -f "$CONT"; rmdir "$MNT" 2>/dev/null || true; }
trap cleanup EXIT

ok() { if eval "$1"; then echo "OK: $2"; else echo "FAIL: $2"; exit 1; fi; }

# format2 -> kontener v2 (rozmiar musi pomiescic plik testowy >=64 MiB + metadane)
"$CLI" format2 "$CONT" 49152
ok 'true' 'format2 (v2 CoW B-tree)'

"$GFS" "$CONT" "$MNT" -f &
FPID=$!; sleep 1

# 1) duzy plik round-trip
head -c 5000000 /dev/urandom > /tmp/ghost_v2_big.bin
cp /tmp/ghost_v2_big.bin "$MNT/big.bin"
cp "$MNT/big.bin" /tmp/ghost_v2_big.out
ok 'diff -q /tmp/ghost_v2_big.bin /tmp/ghost_v2_big.out >/dev/null' 'big round-trip (v2 FUSE)'

# 1b) round-trip DUZEGO pliku >=64 MiB przez strojony mount (wymusza wielo-MiB
#     odczyty z nowym max_read=1MiB; sha256 zrodlo == odczyt, bajt-exact)
head -c 67108864 /dev/urandom > /tmp/ghost_v2_huge.bin
cp /tmp/ghost_v2_huge.bin "$MNT/huge.bin"
sync
SRC_SHA=$(sha256sum /tmp/ghost_v2_huge.bin | awk '{print $1}')
MNT_SHA=$(sha256sum "$MNT/huge.bin" | awk '{print $1}')
ok '[ "$SRC_SHA" = "$MNT_SHA" ]' 'big-file >=64MiB round-trip sha256 bajt-exact (max_read 1MiB)'

# 1c) re-odczyt: auto_cache koherencja — nadpisanie widzi swieza tresc (nie cache)
echo aaaa > "$MNT/rc"
ok '[ "$(cat "$MNT/rc")" = "aaaa" ]' 're-odczyt: pierwsza tresc (aaaa)'
echo bbbb > "$MNT/rc"
ok '[ "$(cat "$MNT/rc")" = "bbbb" ]' 're-odczyt: auto_cache koherentny po nadpisaniu (bbbb, nie cache)'

# 1d) user override max_read: mount z -o max_read=131072 NIE jest dublowany
#     i poprawnie montuje (init==session); osobny kontener/mount by nie kolidowac
OCONT=$(mktemp /tmp/ghost_v2ovr.XXXXXX.gfs); OMNT=$(mktemp -d)
"$CLI" format2 "$OCONT" 16384 >/dev/null
"$GFS" "$OCONT" "$OMNT" -o max_read=131072 -f &
OPID=$!; sleep 1
ok 'mountpoint -q "$OMNT"' 'override -o max_read=131072 montuje (respekt, brak dublowania)'
echo ovr > "$OMNT/o.txt"
ok '[ "$(cat "$OMNT/o.txt")" = "ovr" ]' 'override mount round-trip'
fusermount3 -u "$OMNT" 2>/dev/null || true; wait $OPID 2>/dev/null || true
rm -f "$OCONT"; rmdir "$OMNT" 2>/dev/null || true

# 1e) REGRESJA: mountpoint z podciagiem "max_read" w nazwie montuje DOMYSLNIE.
#     Dawniej skan strstr(argv,"max_read") falszywie wykrywal "user podal max_read"
#     na pozycyjnym mountpoincie -> POMIJAL wstrzykniecie -o max_read=1048576 ->
#     sesja max_read=0 vs gf_init conn->max_read=1MiB -> libfuse3 ODRZUCAL montaz
#     ("different maximum read size"). Teraz skanujemy tylko optstringi -o.
RCONT=$(mktemp /tmp/ghost_v2mr.XXXXXX.gfs)
RMNT="$(dirname "$(mktemp -d)")/ghost_max_read_dir.$$"; mkdir -p "$RMNT"
"$CLI" format2 "$RCONT" 16384 >/dev/null
"$GFS" "$RCONT" "$RMNT" -f &  # DOMYSLNIE: bez -o, injekcja MUSI zadzialac
RPID=$!; sleep 1
ok 'mountpoint -q "$RMNT"' 'mountpoint z "max_read" w nazwie montuje (regresja bug skanu)'
echo regres > "$RMNT/r.txt"
ok '[ "$(cat "$RMNT/r.txt")" = "regres" ]' 'mountpoint-"max_read" round-trip (brak different-read-size)'
fusermount3 -u "$RMNT" 2>/dev/null || true; wait $RPID 2>/dev/null || true
rm -f "$RCONT"; rmdir "$RMNT" 2>/dev/null || true

# 1f) override sklejony/combined: token max_read= rozpoznany w optstringu z lista
#     po przecinkach -> g_max_read poprawny, init==session, montuje (brak different
#     maximum read size). Linia mount pokazuje efektywny max_read przekazany sesji.
#     (default_permissions wlacza egzekucje uprawnien jadra — nie robimy zapisu w tym
#     wariancie; istotne jest, ze TOKEN max_read parsuje sie z combined optstringu.)
combo_mount() {  # $1=optstring  $2=oczekiwany max_read w linii mount  $3=etykieta
    local XCONT XMNT XPID LINE
    XCONT=$(mktemp /tmp/ghost_v2x.XXXXXX.gfs); XMNT=$(mktemp -d)
    "$CLI" format2 "$XCONT" 16384 >/dev/null
    "$GFS" "$XCONT" "$XMNT" -o "$1" -f &
    XPID=$!; sleep 1
    LINE=$(mount | grep " $XMNT " || true)
    ok 'mountpoint -q "$XMNT"' "$3 montuje (init==session)"
    ok "echo \"$LINE\" | grep -q 'max_read=$2'" "$3 -> g_max_read=$2 (token z optstringu)"
    fusermount3 -u "$XMNT" 2>/dev/null || true; wait $XPID 2>/dev/null || true
    rm -f "$XCONT"; rmdir "$XMNT" 2>/dev/null || true
}
combo_mount "max_read=262144"                     262144  "override -o max_read=262144"
combo_mount "max_read=262144,auto_unmount"        262144  "combined -o max_read=262144,auto_unmount"
combo_mount "default_permissions,max_read=131072" 131072  "combined -o default_permissions,max_read=131072"

# 1g) max_readahead NIE jest my3one z max_read: token max_read= wymaga '=' tuz po
#     nazwie, wiec "max_readahead=" NIE tlumi injekcji -> g_max_read pozostaje 1 MiB
#     (init==session). FUSE moze odrzucic samo max_readahead jako opcje -o, ale nasza
#     logika skanu NIE pudluje (brak "different maximum read size").
YCONT=$(mktemp /tmp/ghost_v2y.XXXXXX.gfs); YMNT=$(mktemp -d)
"$CLI" format2 "$YCONT" 16384 >/dev/null
"$GFS" "$YCONT" "$YMNT" -o max_readahead=1048576 -f >/tmp/ghost_v2y.log 2>&1 &
YPID=$!; sleep 1
# kluczowe: niezaleznie czy FUSE przyjmie max_readahead, NIE ma niespojnosci read-size
ok '! grep -qi "different maximum read size" /tmp/ghost_v2y.log' \
   'max_readahead NIE tlumi injekcji (brak different-read-size; g_max_read=1MiB)'
fusermount3 -u "$YMNT" 2>/dev/null || true; wait $YPID 2>/dev/null || true
rm -f "$YCONT" /tmp/ghost_v2y.log; rmdir "$YMNT" 2>/dev/null || true

# 2) katalogi zagniezdzone
mkdir -p "$MNT/a/b/c"
echo hello > "$MNT/a/b/c/f.txt"
ok '[ "$(cat "$MNT/a/b/c/f.txt")" = "hello" ]' 'nested dirs'
ok 'ls -l "$MNT/a/b/c" >/dev/null' 'ls -l'

# 3) edycja w miejscu
printf '0123456789' > "$MNT/edit.txt"
printf 'XY' | dd of="$MNT/edit.txt" bs=1 seek=4 conv=notrunc 2>/dev/null
ok '[ "$(cat "$MNT/edit.txt")" = "0123XY6789" ]' 'edycja w miejscu'

# 4) rm -r
rm -r "$MNT/a"
ok 'test ! -e "$MNT/a"' 'rm -r'

# 5) truncate do zera
printf '0123456789' > "$MNT/tr.txt"
: > "$MNT/tr.txt"
ok 'test ! -s "$MNT/tr.txt"' 'truncate do zera'

# 6) mv (plik i katalog)
echo hej > "$MNT/m1.txt"
mv "$MNT/m1.txt" "$MNT/m2.txt"
ok '[ "$(cat "$MNT/m2.txt")" = "hej" ]' 'mv pliku'
mkdir -p "$MNT/md/sub"
mv "$MNT/md/sub" "$MNT/sub2"
ok 'test -d "$MNT/sub2"' 'mv katalogu'

# 7) df (statfs)
ok 'df "$MNT" >/dev/null' 'df'

# 8) chmod + ls -l
echo x > "$MNT/perm.txt"
chmod 600 "$MNT/perm.txt"
ok 'ls -l "$MNT/perm.txt" | grep -q "^-rw-------"' 'chmod'

# 9) symlink
ln -s /jakis/cel "$MNT/symn"
ok '[ "$(readlink "$MNT/symn")" = "/jakis/cel" ]' 'symlink'

# 10) twardy link
echo wspolne > "$MNT/h1"
ln "$MNT/h1" "$MNT/h2"
ok '[ "$(cat "$MNT/h2")" = "wspolne" ]' 'hardlink'

# 11) mknod FIFO
mkfifo "$MNT/fifo1"
ok 'test -p "$MNT/fifo1"' 'mkfifo (FIFO)'
ls -l "$MNT/fifo1" | grep -q '^p' && echo "OK: ls -l FIFO (prw)" || echo "FAIL: FIFO ls"

# 12) truncate (rozszerz + skroc, sparse)
printf 'abc' > "$MNT/sz.txt"
truncate -s 100 "$MNT/sz.txt"
ok '[ "$(stat -c %s "$MNT/sz.txt")" = "100" ]' 'truncate rozszerz (sparse)'
truncate -s 2 "$MNT/sz.txt"
ok '[ "$(cat "$MNT/sz.txt")" = "ab" ]' 'truncate skroc'

# 12b) xattr round-trip przez FUSE (setfattr/getfattr) — jesli dostepne
if command -v setfattr >/dev/null && command -v getfattr >/dev/null; then
    echo xattrdata > "$MNT/xa.txt"
    setfattr -n user.foo -v bar "$MNT/xa.txt"
    ok 'getfattr -n user.foo --only-values "$MNT/xa.txt" | grep -q "^bar$"' 'xattr setfattr/getfattr (v2 FUSE)'
    setfattr -n user.second -v 123 "$MNT/xa.txt"
    ok 'getfattr -d "$MNT/xa.txt" | grep -q "user.foo"' 'xattr getfattr -d listuje user.foo'
    ok 'getfattr -d "$MNT/xa.txt" | grep -q "user.second"' 'xattr getfattr -d listuje user.second'
    setfattr -x user.foo "$MNT/xa.txt"
    ok '! getfattr -d "$MNT/xa.txt" | grep -q "user.foo"' 'xattr setfattr -x usuwa user.foo'
    ok 'getfattr -d "$MNT/xa.txt" | grep -q "user.second"' 'xattr -x nie ruszyl user.second'
else
    echo "SKIP: setfattr/getfattr niedostepne — pomijam xattr FUSE"
fi

# 13) symulacja awarii: fsync (durability point) -> kill -9 -> remount -> fsck czysty
echo zawartosc > "$MNT/crash.txt"
mkdir -p "$MNT/cd"
python3 - "$MNT" <<'PY'
import os, sys
m = sys.argv[1]
for p in (m + "/crash.txt", m + "/cd"):
    fd = os.open(p, os.O_RDONLY)
    os.fsync(fd)
    os.close(fd)
PY
sync
kill -9 "$FPID" 2>/dev/null || true
fusermount3 -u "$MNT" 2>/dev/null || true
wait "$FPID" 2>/dev/null || true
# remount (mark-sweep z SB.root_tree odbudowuje mape)
"$GFS" "$CONT" "$MNT" -f &
FPID=$!; sleep 1
ok 'test -f "$MNT/crash.txt"' 'remount po kill -9 (zatwierdzony stan)'
ok '[ "$(cat "$MNT/crash.txt")" = "zawartosc" ]' 'tresc po kill -9 bajt-exact'

# 14) trwalosc przez unmount (leniwy flush -> commit na unmount)
echo "trwale-przez-unmount" > "$MNT/lazy.txt"
fusermount3 -u "$MNT"; wait $FPID 2>/dev/null || true
"$GFS" "$CONT" "$MNT" -f &
FPID=$!; sleep 1
ok 'test "$(cat "$MNT/lazy.txt")" = "trwale-przez-unmount"' 'trwalosc po unmount (leniwy flush)'

fusermount3 -u "$MNT"; wait $FPID 2>/dev/null || true

# 15) fsck czysty po wszystkim (przez FUSE na v2)
ok '"$CLI" fsck "$CONT" | grep -q "0 niespójności"' 'fsck czysty (v2)'

# ====================== CLI v2 (bez FUSE) ======================
CCONT=$(mktemp /tmp/ghost_v2cli.XXXXXX.gfs)
trap 'fusermount3 -u "$MNT" 2>/dev/null || true; rm -f "$CONT" "$CCONT"; rmdir "$MNT" 2>/dev/null || true' EXIT
"$CLI" format2 "$CCONT" 16384
ok 'true' 'CLI format2'
head -c 300000 /dev/urandom > /tmp/ghost_v2cli.bin
"$CLI" put "$CCONT" /tmp/ghost_v2cli.bin /file.bin
"$CLI" get "$CCONT" /file.bin /tmp/ghost_v2cli.out
ok 'cmp -s /tmp/ghost_v2cli.bin /tmp/ghost_v2cli.out' 'CLI put/get round-trip'
"$CLI" mkdir "$CCONT" /sub
ok '"$CLI" ls "$CCONT" / | grep -q file.bin' 'CLI ls'
ok '"$CLI" ls "$CCONT" / | grep -q sub' 'CLI mkdir widoczny'
ok '"$CLI" stat "$CCONT" /file.bin | grep -q "rozmiar=300000"' 'CLI stat'
ok '"$CLI" df "$CCONT" | grep -q "blok=4096"' 'CLI df'
ok '"$CLI" fsck "$CCONT" | grep -q "0 niespójności"' 'CLI fsck czysty'
"$CLI" rm "$CCONT" /file.bin
ok '! "$CLI" ls "$CCONT" / | grep -q file.bin' 'CLI rm'
ok '"$CLI" fsck "$CCONT" | grep -q "0 niespójności"' 'CLI fsck po rm'
# fsck --repair na v2: juz nie blokowany (read-only); czysty FS -> 0 niespojnosci (naprawiono)
ok '"$CLI" fsck "$CCONT" --repair | grep -q "0 niespójności (naprawiono)"' 'CLI fsck --repair v2 (czysty)'
ok '"$CLI" fsck "$CCONT" | grep -q "0 niespójności"' 'CLI fsck po --repair (trwale czysty)'

# ====================== CLI v2 snapshoty (Task 4) ======================
SCONT=$(mktemp /tmp/ghost_v2snap.XXXXXX.gfs)
trap 'fusermount3 -u "$MNT" 2>/dev/null || true; rm -f "$CONT" "$CCONT" "$SCONT"; rmdir "$MNT" 2>/dev/null || true' EXIT
"$CLI" format2 "$SCONT" 16384
echo "tresc-oryginalu" > /tmp/ghost_v2snap.in
"$CLI" put "$SCONT" /tmp/ghost_v2snap.in /orig.txt
# snapshot
"$CLI" snapshot "$SCONT" snapA
ok '"$CLI" subvol-list "$SCONT" | grep -q "snapA"' 'CLI snapshot + subvol-list pokazuje snapA'
ok '"$CLI" subvol-list "$SCONT" | grep -q "default"' 'CLI subvol-list pokazuje default'
# drugi snapshot
"$CLI" snapshot "$SCONT" snapB
ok '[ "$("$CLI" subvol-list "$SCONT" | wc -l)" = "3" ]' 'CLI subvol-list: default+snapA+snapB'
# usun snapA po id
SID=$("$CLI" subvol-list "$SCONT" | awk '$2=="snapA"{print $1}')
ok '[ -n "$SID" ]' 'CLI subvol-list zwraca id snapA'
"$CLI" subvol-del "$SCONT" "$SID"
ok '! "$CLI" subvol-list "$SCONT" | grep -q "snapA"' 'CLI subvol-del usunal snapA'
ok '"$CLI" subvol-list "$SCONT" | grep -q "snapB"' 'CLI subvol-del nie ruszyl snapB'
# odmowa usuniecia domyslnego (id 1)
ok '! "$CLI" subvol-del "$SCONT" 1' 'CLI subvol-del odmawia domyslnego (id 1)'
# oryginal czytelny + fsck czysty po snapshotach/delete
"$CLI" get "$SCONT" /orig.txt /tmp/ghost_v2snap.out
ok 'cmp -s /tmp/ghost_v2snap.in /tmp/ghost_v2snap.out' 'CLI oryginal czytelny po snapshot/delete'
ok '"$CLI" fsck "$SCONT" | grep -q "0 niespójności"' 'CLI fsck czysty po snapshot/delete'

# ====================== CLI v2 szyfrowanie (v2enc) ======================
ECONT=$(mktemp /tmp/ghost_v2enc.XXXXXX.gfs)
trap 'fusermount3 -u "$MNT" 2>/dev/null || true; rm -f "$CONT" "$CCONT" "$SCONT" "$ECONT"; rmdir "$MNT" 2>/dev/null || true' EXIT
# format2 zaszyfrowany przez GHOSTFS_KEY
GHOSTFS_KEY="tajne-haslo-integ" "$CLI" format2 "$ECONT" 16384
ok 'true' 'CLI format2 zaszyfrowany (GHOSTFS_KEY)'
# at-rest: marker rozpoznawalny NIE moze byc widoczny w surowym kontenerze (poza SB)
printf 'ATREST_INTEG_MARKER_777\n' > /tmp/ghost_v2enc.in
GHOSTFS_KEY="tajne-haslo-integ" "$CLI" put "$ECONT" /tmp/ghost_v2enc.in /secret.txt
ok '! grep -aq "ATREST_INTEG_MARKER_777" "$ECONT"' 'CLI at-rest: brak plaintextu w kontenerze'
# round-trip z poprawnym kluczem
GHOSTFS_KEY="tajne-haslo-integ" "$CLI" get "$ECONT" /secret.txt /tmp/ghost_v2enc.out
ok 'cmp -s /tmp/ghost_v2enc.in /tmp/ghost_v2enc.out' 'CLI zaszyfrowany put/get round-trip'
# zly klucz (env) -> blad mountu (brak promptu bo stdin nie-tty); nie wypisuje sekretu
ok '! GHOSTFS_KEY="zly-klucz" "$CLI" ls "$ECONT" / </dev/null 2>/dev/null | grep -q secret.txt' \
   'CLI zly klucz -> brak dostepu'
# brak klucza -> brak dostepu
ok '! "$CLI" ls "$ECONT" / </dev/null 2>/dev/null | grep -q secret.txt' \
   'CLI brak klucza -> brak dostepu'
# fsck z kluczem czysty
ok 'GHOSTFS_KEY="tajne-haslo-integ" "$CLI" fsck "$ECONT" | grep -q "0 niespójności"' \
   'CLI fsck zaszyfrowany czysty'

# opcjonalnie: mount FUSE zaszyfrowanego v2 przez env
GHOSTFS_KEY="tajne-haslo-integ" "$GFS" "$ECONT" "$MNT" -f &
FPID=$!; sleep 1
ok '[ "$(GHOSTFS_KEY="tajne-haslo-integ" cat "$MNT/secret.txt")" = "ATREST_INTEG_MARKER_777" ]' \
   'FUSE mount zaszyfrowanego (GHOSTFS_KEY) round-trip'
echo "FUSE_ENC_NOWY=swieza-tresc" > "$MNT/new.txt"
fusermount3 -u "$MNT"; wait $FPID 2>/dev/null || true
ok 'GHOSTFS_KEY="tajne-haslo-integ" "$CLI" ls "$ECONT" / | grep -q new.txt' \
   'FUSE zaszyfrowany zapis trwaly'

rm -f /tmp/ghost_v2_big.bin /tmp/ghost_v2_big.out /tmp/ghost_v2_huge.bin /tmp/ghost_v2cli.bin /tmp/ghost_v2cli.out \
      /tmp/ghost_v2snap.in /tmp/ghost_v2snap.out /tmp/ghost_v2enc.in /tmp/ghost_v2enc.out

echo "WSZYSTKIE TESTY INTEGRACYJNE v2 PRZESZŁY"
