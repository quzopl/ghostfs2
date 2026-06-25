# ghostfs — pod-projekt N: trwałość napraw `fsck --repair`

**Data:** 2026-06-24
**Status:** zatwierdzony (autonomia użytkownika)
**Część całości:** A–M ✓ → **N (fsck repair durable)**.

## Problem (regresja z group commit, I)
`gh_fsck` w trybie `repair` zapisuje przez `gh_block_write`/`gh_inode_write`/`gh_free_block`,
które przy zamontowanym FS **buforują w bieżącej (running) transakcji** (group commit, I).
Ale `gh_fsck` nie wołało `gh_jrnl_op_begin`/`op_commit`, więc flaga `dirty` transakcji nie
była ustawiana. Przy `unmount` → `gh_jrnl_flush` widzi `dirty==0` → **pomija flush** →
naprawy **giną**. W tej samej sesji fsck „widzi" naprawy (read-your-writes z bufora) i
raportuje sukces, ale na dysku nic się nie zmienia (potwierdzone: `fsck --repair` → świeży
montaż → te same niespójności).

Wykryte testem `fsck --repair` na celowo uszkodzonym obrazie (zły nlink + sierota + wyciek
mapy). Istniejący `test_fsck_tree` nie remontował, więc brak trwałości przeszedł niezauważony.

## Rozwiązanie
Opakować naprawy fsck w transakcję operacji:
- na początku `gh_fsck`, gdy `repair` → `gh_jrnl_op_begin(&fs->dev)`,
- na końcu, gdy `repair` → `gh_jrnl_op_commit(&fs->dev)` (ustawia `dirty=1`).
Wtedy `gh_fs_unmount`/`gh_fs_sync` → `gh_jrnl_flush` utrwala paczkę napraw.
Tryb bez dziennika (`dev->txn==NULL`): `op_begin/commit` to no-op, a zapisy fsck idą i tak
bezpośrednio na dysk (jak dotąd) — bez regresji.

## Testowanie
- `tests/test_fs.c` `test_fsck_repair_persists` (NOWY, regresja): uszkodź obraz (nlink/
  sierota/wyciek) przez surowy dev, fsck wykrywa (issues>=3), `repair`, **UNMOUNT**,
  **REMOUNT**, fsck → `issues==0` (naprawy trwałe); nlink naprawiony, sierota zwolniona.
- Crash-sweep (`test_crash`) bez zmian — atomowość niezmieniona.
- Regresja A–M `0 failed`; ASan czysty.

## Ograniczenia
- Ścieżki błędu w `gh_fsck` (rzadkie `-ENOMEM` z `calloc`) mogą zostawić `op_active=1` bez
  commit — nieszkodliwe (kolejny `op_begin` resetuje; `dirty==0` → flush pomija; brak
  utrwalenia częściowej naprawy). fsck jest idempotentny (ponowne uruchomienie dokończy).
