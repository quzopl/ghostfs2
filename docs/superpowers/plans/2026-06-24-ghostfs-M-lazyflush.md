# ghostfs pod-projekt M — leniwy flush: plan implementacji

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`).
> Bramka: crash-sweep `fsck==0` (zmiana dotyka protokołu dziennika) + regresja + integracja.

**Goal:** `gf_flush` (FUSE close) przestaje wymuszać trwały commit (POSIX: trwałość tylko na fsync/pojemność/unmount); redukcja fsync w `gh_jrnl_flush` (leniwe czyszczenie nagłówka, CRC-chronione). Usuwa burzę fsynców per-plik na USB. Zgodnie ze spec `docs/superpowers/specs/2026-06-24-ghostfs-M-lazyflush-design.md`.

**Tech Stack:** C11, libfuse3, mini-harness, ASan.

## Global Constraints
- Testy przez `make`; gołe `cc` wymagają `-D_POSIX_C_SOURCE=200809L -lcrypto -lpthread`.
- KRYTYCZNE: crash-consistency niezmieniona (crash-sweep `fsck==0`).

## File Structure
| Plik | Zmiana |
|---|---|
| `src/fuse_main.c` | `gf_flush` → no-op |
| `src/journal.c` | `gh_jrnl_flush` — usuń `fsync` po fazie 4 (leniwe czyszczenie nagłówka) |
| `tests/integration.sh` | trwałość przez unmount (cp bez fsync → remount → obecny) |

---

## Task 1: Leniwy flush + redukcja fsync

**Files:** Modify: `src/fuse_main.c`, `src/journal.c`, `tests/integration.sh`

- [ ] **Step 1: `gf_flush` no-op (`src/fuse_main.c`)**

```c
static int gf_flush(const char *path, struct fuse_file_info *fi) {
    (void)path; (void)fi;
    return 0;   /* leniwy: trwalosc tylko na fsync/pojemnosc/unmount (POSIX) */
}
```

(`gf_fsync` BEZ ZMIAN — nadal `GF_WR(gh_fs_sync(&g_fs))`, trwałe. Tablica `ops` bez zmian.)

- [ ] **Step 2: Usuń `fsync` po fazie 4 w `gh_jrnl_flush` (`src/journal.c`)**

Znajdź końcówkę sukcesu (po checkpoint i fsync fazy 3):

```c
    memset(blk, 0, sizeof(blk));
    if (raw_write(dev, js, blk)) { rc = -EIO; goto fail; }
    if (fsync(dev->fd)) { rc = -EIO; goto fail; }      /* <-- USUŃ TĘ LINIĘ */

    gh_discard_flush(dev, sb);
    t->n = 0; t->dirty = 0;
    return 0;
```

Zmień na (zostaw zapis wyczyszczonego nagłówka, usuń jego `fsync`):

```c
    memset(blk, 0, sizeof(blk));
    if (raw_write(dev, js, blk)) { rc = -EIO; goto fail; }
    /* leniwe czyszczenie naglowka: bez fsync — recover odtworzy idempotentnie,
       a CRC dziennika chroni przed rozdartym zapisem (J) */

    gh_discard_flush(dev, sb);
    t->n = 0; t->dirty = 0;
    return 0;
```

(Fazy 1, 2, 3 zachowują `fsync` — obrazy durable, punkt zatwierdzenia durable, checkpoint
durable. Tylko czyszczenie nagłówka jest leniwe.)

- [ ] **Step 3: Crash-sweep — bramka (`tests/test_crash.c` bez zmian, tylko uruchom)**

Run: `make clean && make test 2>&1 | grep -A2 'TEST test_crash'`
Expected: `test_crash` `0 failed` — sweep awarii N=1..40 daje `fsck==0` mimo leniwego
czyszczenia nagłówka (recover idempotentny + CRC). **Jeśli którekolwiek N daje issues>0 —
ZATRZYMAJ (BLOCKED): leniwe czyszczenie naruszyło atomowość.**

- [ ] **Step 4: Test trwałości przez unmount w `tests/integration.sh`**

Przed sekcją końcowego fsck dodaj:

```bash
# 17) trwalosc przez unmount (leniwy flush): cp bez fsync, unmount, remount -> obecny
echo "trwale-przez-unmount" > "$MNT/lazy.txt"     # close() nie wymusza commitu
fusermount3 -u "$MNT"; wait $FPID 2>/dev/null || true
"$GFS" "$CONT" "$MNT" -f &
FPID=$!; sleep 1
ok 'test "$(cat "$MNT/lazy.txt")" = "trwale-przez-unmount"' 'trwalosc po unmount (leniwy flush)'
```

(Po odmontowaniu running txn jest flushowany; remount widzi plik. `ok` istnieje od A11.)

- [ ] **Step 5: Build + pełna regresja + ASan**

Run: `make clean && make test && make test-asan`
Expected: wszystkie `0 failed` (w tym test_crash, test_journal — recover/CRC; reszta A–L).

- [ ] **Step 6: Integracja FUSE + urządzenie blokowe**

Run: `make cli fuse && ./tests/integration.sh && ./tests/integration_blockdev.sh`
Expected: WSZYSTKIE PRZESZŁY (w tym `OK: trwalosc po unmount (leniwy flush)`, `OK: remount
po kill -9 (recover)`, `OK: fsck clean`). Posprzątaj mounty.

- [ ] **Step 7: Commit**

```bash
git add src/fuse_main.c src/journal.c tests/integration.sh
git commit -m "perf: leniwy FUSE flush (trwalosc tylko na fsync/unmount) + 1 fsync mniej w dzienniku"
```

---

## Self-Review (przy pisaniu planu)

**Pokrycie spec M:** gf_flush no-op (Zmiana 1) ✓; redukcja fsync w gh_jrnl_flush (Zmiana 2) ✓;
test trwałości przez unmount + crash-sweep gate ✓.
**Placeholdery:** brak.
**Spójność:** `gf_flush` no-op; `gf_fsync` bez zmian (trwałe); faza 4 dziennika bez fsync.
**Ryzyka:** (1) KRYTYCZNE — leniwe czyszczenie nagłówka nie może naruszyć atomowości:
recover idempotentny (odtwarza ponownie zatwierdzoną paczkę) + CRC dziennika (J) chroni
rozdarcie → crash-sweep `fsck==0` to bramka; (2) trwałość po close zależna od fsync/unmount
— test unmount-durability + dokumentacja „odmontuj przed wyciągnięciem USB"; (3) brak
regresji A–L (gf_fsync i unmount nadal flushują, więc istniejące testy remount/at-rest/
crash działają).
