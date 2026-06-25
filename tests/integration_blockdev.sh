#!/usr/bin/env bash
# Test ghostfs na realnym urzadzeniu blokowym (loop). Wymaga roota/sudo i losetup.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GFS="$ROOT/build/ghostfs"; CLI="$ROOT/build/ghostfs-cli"

if ! command -v losetup >/dev/null; then echo "POMINIETO: brak losetup"; exit 0; fi
SUDO=""
if [ "$(id -u)" != 0 ]; then
  if sudo -n true 2>/dev/null; then SUDO="sudo"; else echo "POMINIETO: brak uprawnien root"; exit 0; fi
fi

BACK=$(mktemp /tmp/ghost_loop.XXXXXX.img)
truncate -s 64M "$BACK"
LOOP=$($SUDO losetup --find --show "$BACK") || { echo "POMINIETO: losetup nieudany"; rm -f "$BACK"; exit 0; }
MNT=$(mktemp -d)
cleanup() {
  fusermount3 -u "$MNT" 2>/dev/null || true
  $SUDO losetup -d "$LOOP" 2>/dev/null || true
  rm -f "$BACK"; rmdir "$MNT" 2>/dev/null || true
}
trap cleanup EXIT

# format urzadzenia (autorozmiar: 0 blokow = caly device)
$SUDO chmod o+rw "$LOOP" 2>/dev/null || true
"$CLI" format "$LOOP" 0 1024 || { echo "FAIL: format urzadzenia"; exit 1; }
echo "OK: format urzadzenia blokowego $LOOP"

# mount FUSE na urzadzeniu
"$GFS" "$LOOP" "$MNT" -f &
GPID=$!; sleep 1
echo "test-na-ssd" > "$MNT/f.txt"
test "$(cat "$MNT/f.txt")" = "test-na-ssd" && echo "OK: round-trip na urzadzeniu"
head -c 1000000 /dev/urandom > "$MNT/big.bin"
cp "$MNT/big.bin" /tmp/ghost_bd.out
cmp -s "$MNT/big.bin" /tmp/ghost_bd.out && echo "OK: duzy plik na urzadzeniu"
rm -f "$MNT/big.bin" /tmp/ghost_bd.out
fusermount3 -u "$MNT"; wait $GPID 2>/dev/null || true

# fsck urzadzenia
"$CLI" fsck "$LOOP" | grep -q "0 niespójności" && echo "OK: fsck urzadzenia czysty"
echo "WSZYSTKIE TESTY URZADZENIA BLOKOWEGO PRZESZLY"
