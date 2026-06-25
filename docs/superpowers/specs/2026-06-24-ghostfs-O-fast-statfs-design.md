# ghostfs — pod-projekt O: szybki statfs

**Data:** 2026-06-24
**Status:** zatwierdzony (autonomia użytkownika); diagnoza z `raport.md`.
**Część całości:** A–N ✓ → **O (fast statfs)**.

## Problem
`gh_fs_statfs` (df, start `mc`) trwa ~16,8 s na 5 GB pendrivie. Pętle O(n) z pełnym
`gh_block_read` na każdą iterację: `gh_bitmap_test` per-blok czyta cały blok mapy (32768×
za dużo) i liczy CRC (J) za każdym razem; `gh_inode_read` per-i-węzeł czyta blok tablicy
(16× za dużo). ~1,3 mln odczytów.

## Rozwiązanie
Czytać każdy blok mapy bitowej i tablicy i-węzłów **raz** (keszowanie bieżącego bloku),
licząc bity/typy w pamięci. Ta sama matematyka bitów co `src/alloc.c`. ~1,3 mln → ~40+4tys
odczytów; statfs <0,1 s. Liczby identyczne jak dotąd.

## Testowanie
- `tests/test_fs.c` cross-check: `gh_fs_statfs.free_blocks/free_inodes` == brute-force
  (gh_bitmap_test/gh_inode_read per element) — dowód identycznych liczb przy nowej metodzie.
- Istniejący `test_statfs_sync` (free maleje po alloc) bez zmian.
- Regresja A–N `0 failed`; ASan czysty.

## Ograniczenia
- `gh_fsck` ma ten sam wzorzec O(n)-per-blok; opcjonalne ujednolicenie (fsck rzadko
  interaktywny) — poza twardym zakresem O, można dołożyć.
