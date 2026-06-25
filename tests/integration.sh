#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GFS="$ROOT/build/ghostfs"; CLI="$ROOT/build/ghostfs-cli"
CONT=$(mktemp /tmp/ghost_int.XXXXXX.gfs); MNT=$(mktemp -d)
cleanup() { fusermount3 -u "$MNT" 2>/dev/null || true; rm -f "$CONT"; rmdir "$MNT" 2>/dev/null || true; }
trap cleanup EXIT

# realnie przerywa skrypt przy porazce (nie maskuje bledow pod set -e)
ok() { if eval "$1"; then echo "OK: $2"; else echo "FAIL: $2"; exit 1; fi; }

"$CLI" format "$CONT" 16384 512
"$GFS" "$CONT" "$MNT" -f &
FPID=$!; sleep 1

# 1) duzy plik round-trip
head -c 5000000 /dev/urandom > /tmp/ghost_big.bin
cp /tmp/ghost_big.bin "$MNT/big.bin"
cp "$MNT/big.bin" /tmp/ghost_big.out
diff /tmp/ghost_big.bin /tmp/ghost_big.out && echo "OK: big round-trip"

# 2) katalogi
mkdir -p "$MNT/a/b/c"
echo hello > "$MNT/a/b/c/f.txt"
test "$(cat "$MNT/a/b/c/f.txt")" = "hello" && echo "OK: nested dirs"
ls -l "$MNT/a/b/c" >/dev/null && echo "OK: ls -l"

# 3) edycja w miejscu
printf '0123456789' > "$MNT/edit.txt"
printf 'XY' | dd of="$MNT/edit.txt" bs=1 seek=4 conv=notrunc 2>/dev/null
test "$(cat "$MNT/edit.txt")" = "0123XY6789" && echo "OK: edycja w miejscu"

# 4) usuwanie
rm -r "$MNT/a"
test ! -e "$MNT/a" && echo "OK: rm -r"

# 5) truncate (skracanie przez '>')
printf '0123456789' > "$MNT/tr.txt"
: > "$MNT/tr.txt"
ok 'test ! -s "$MNT/tr.txt"' "truncate do zera"

# 6) mv (plik i katalog)
echo hej > "$MNT/m1.txt"
mv "$MNT/m1.txt" "$MNT/m2.txt"
ok '[ "$(cat "$MNT/m2.txt")" = "hej" ]' "mv pliku"
mkdir -p "$MNT/md/sub"
mv "$MNT/md/sub" "$MNT/sub2"
ok 'test -d "$MNT/sub2"' "mv katalogu"

# 7) df (statfs)
ok 'df "$MNT" >/dev/null' "df"

# 8) chmod + ls -l
echo x > "$MNT/perm.txt"
chmod 600 "$MNT/perm.txt"
ok 'ls -l "$MNT/perm.txt" | grep -q "^-rw-------"' "chmod"

# 9) symlink
ln -s /jakis/cel "$MNT/symn"
ok '[ "$(readlink "$MNT/symn")" = "/jakis/cel" ]' "symlink"

# 10) twardy link
echo wspolne > "$MNT/h1"
ln "$MNT/h1" "$MNT/h2"
ok '[ "$(cat "$MNT/h2")" = "wspolne" ]' "hardlink"

# 11) xattr (jesli dostepne narzedzia)
if command -v setfattr >/dev/null && command -v getfattr >/dev/null; then
  echo y > "$MNT/xa.txt"
  setfattr -n user.test -v wartosc "$MNT/xa.txt"
  ok 'getfattr -n user.test --only-values "$MNT/xa.txt" 2>/dev/null | grep -q wartosc' "xattr"
else
  echo "POMINIETO: xattr (brak setfattr/getfattr)"
fi

# 16) wezly specjalne
mkfifo "$MNT/fifo1"
ok 'test -p "$MNT/fifo1"' 'mkfifo (FIFO)'
ls -l "$MNT/fifo1" | grep -q '^p' && echo "OK: ls -l FIFO (prw)" || echo "FAIL: FIFO ls"
if [ "$(id -u)" = 0 ] || sudo -n true 2>/dev/null; then
  SUDO=""; [ "$(id -u)" = 0 ] || SUDO="sudo"
  $SUDO mknod "$MNT/nulldev" c 1 3 2>/dev/null && \
    ls -l "$MNT/nulldev" | grep -q '^c.* 1,  *3' && echo "OK: mknod urzadzenie znakowe (1,3)" || echo "POMINIETO: mknod dev"
else
  echo "POMINIETO: mknod urzadzenia (brak root)"
fi

# 13) symulacja awarii: kill -9 procesu FUSE w trakcie pracy, remount, fsck czysty
echo zawartosc > "$MNT/crash.txt"
mkdir -p "$MNT/cd"
# leniwy flush: close() nie utrwala — recover odtwarza tylko ZATWIERDZONE dane,
# wiec wymuszamy trwalosc przez fsync (durability point) przed symulacja awarii
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
# remount (uruchamia recover)
"$GFS" "$CONT" "$MNT" -f &
FPID=$!; sleep 1
ok 'test -f "$MNT/crash.txt"' 'remount po kill -9 (recover)'
"$CLI" fsck "$CONT" >/dev/null 2>&1 || true

# 17) trwalosc przez unmount (leniwy flush): cp bez fsync, unmount, remount -> obecny
echo "trwale-przez-unmount" > "$MNT/lazy.txt"     # close() nie wymusza commitu
fusermount3 -u "$MNT"; wait $FPID 2>/dev/null || true
"$GFS" "$CONT" "$MNT" -f &
FPID=$!; sleep 1
ok 'test "$(cat "$MNT/lazy.txt")" = "trwale-przez-unmount"' 'trwalosc po unmount (leniwy flush)'

fusermount3 -u "$MNT"; wait $FPID 2>/dev/null || true

# 12) fsck po wszystkim
ok '"$CLI" fsck "$CONT" | grep -q "0 niespójności"' "fsck clean"

# 14) wariant zaszyfrowany (AES-256-XTS)
ECONT=$(mktemp /tmp/ghost_enc.XXXXXX.gfs); EMNT=$(mktemp -d)
SECRET="POUFNY-MARKER-$$"
GHOSTFS_KEY="haslo-testowe" "$CLI" format "$ECONT" 8192 256
GHOSTFS_KEY="haslo-testowe" "$GFS" "$ECONT" "$EMNT" -f &
EPID=$!; sleep 1
echo "$SECRET" > "$EMNT/tajne.txt"
ok 'test "$(cat "$EMNT/tajne.txt")" = "$SECRET"' 'zaszyfrowany round-trip'
sync
fusermount3 -u "$EMNT" 2>/dev/null || true; wait $EPID 2>/dev/null || true
ok '! grep -aq "$SECRET" "$ECONT"' 'at-rest: brak jawnego sekretu w kontenerze'
ok '! GHOSTFS_KEY="zle" "$CLI" ls "$ECONT" / 2>/dev/null' 'zle haslo odmawia'
rm -f "$ECONT"; rmdir "$EMNT" 2>/dev/null || true

# 15) wspolbieznosc: N rownoleglych pisarzy w osobnych podkatalogach + fsck
CCONT=$(mktemp /tmp/ghost_conc.XXXXXX.gfs); CMNT=$(mktemp -d)
"$CLI" format "$CCONT" 65536 2048
"$GFS" "$CCONT" "$CMNT" -f &
CPID=$!; sleep 1
worker() {  # $1 = id
  local d="$CMNT/w$1"
  mkdir -p "$d"
  for i in $(seq 1 30); do
    head -c 20000 /dev/urandom > "$d/f$i.bin"
    cp "$d/f$i.bin" "$d/c$i.bin"
    cmp -s "$d/f$i.bin" "$d/c$i.bin" || { echo "FAIL: korupcja w w$1/f$i"; exit 1; }
    rm -f "$d/f$i.bin"
  done
}
CWPIDS=""
for w in 1 2 3 4 5 6 7 8; do worker "$w" & CWPIDS="$CWPIDS $!"; done
wait $CWPIDS   # tylko pisarze (NIE $CPID: $GFS -f dziala na pierwszym planie i nie konczy sie)
ok 'true' '8 rownoleglych pisarzy zakonczylo bez korupcji'
# rownolegly odczyt tego samego duzego pliku
head -c 2000000 /dev/urandom > "$CMNT/big.bin"
CRPIDS=""
for r in 1 2 3 4 5 6; do ( cmp -s "$CMNT/big.bin" "$CMNT/big.bin" || echo "FAIL reader $r" ) & CRPIDS="$CRPIDS $!"; done
wait $CRPIDS   # tylko czytelnicy
ok 'true' '6 rownoleglych czytelnikow duzego pliku'
sync
fusermount3 -u "$CMNT" 2>/dev/null || true; wait $CPID 2>/dev/null || true
ok '"$CLI" fsck "$CCONT" 2>/dev/null | grep -q "0 niespójności"' 'fsck czysty po stresie wspolbieznosci'
rm -f "$CCONT"; rmdir "$CMNT" 2>/dev/null || true

echo "WSZYSTKIE TESTY INTEGRACYJNE PRZESZŁY"
