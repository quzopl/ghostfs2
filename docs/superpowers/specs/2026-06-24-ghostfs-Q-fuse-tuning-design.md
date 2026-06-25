# ghostfs — pod-projekt Q: strojenie I/O FUSE

**Data:** 2026-06-24
**Status:** zatwierdzony (autonomia); diagnoza z `benchmark.md` (wniosek #4: zapis sekwencyjny
zdominowany narzutem FUSE).
**Część całości:** A–P ✓ → **Q (strojenie FUSE)**.

## Problem
Zapis sekwencyjny ~53–56 MiB/s (sufit nawet w RAM). `src/fuse_main.c` nie stroi FUSE —
domyślne libfuse3: `max_write` 128 KB (→ `bs=1M` cięte na 8 żądań = 8× przełączeń kontekstu),
brak splice (zbędne kopie), brak writeback cache (jądro nie scala zapisów).

## Rozwiązanie: callback `.init`
```c
static void *gf_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    (void)cfg;
    if (conn->capable & FUSE_CAP_WRITEBACK_CACHE) conn->want |= FUSE_CAP_WRITEBACK_CACHE;
    if (conn->capable & FUSE_CAP_SPLICE_WRITE)     conn->want |= FUSE_CAP_SPLICE_WRITE;
    if (conn->capable & FUSE_CAP_SPLICE_READ)      conn->want |= FUSE_CAP_SPLICE_READ;
    if (conn->capable & FUSE_CAP_SPLICE_MOVE)      conn->want |= FUSE_CAP_SPLICE_MOVE;
    conn->max_write = 1u << 20;   /* 1 MiB: 8x mniej round-tripow dla duzych zapisow */
    conn->max_read  = 1u << 20;
    return NULL;
}
```
`.init = gf_init` w tablicy `ops`. Negocjowane z jądrem (`want & capable`) — bezpieczne na
starszych jądrach (jeśli brak capability, nie włącza).

## Bezpieczeństwo / semantyka
- **Warstwa FUSE, rdzeń niezmieniony** — crash-consistency nietknięta.
- **writeback_cache**: jądro buforuje i scala zapisy (async). Większe okno utraty danych bez
  `fsync` (POSIX), ale `fsync`/unmount nadal trwałe (jądro flushuje page cache → ghostfs →
  commit). Wymaga weryfikacji integracją (rozmiar/mtime/round-trip). Jeśli psuje testy →
  wyłączyć writeback, zostawić max_write+splice (i tak zysk).
- **splice**: zero-copy jądro↔proces; transparentne.

## Testowanie
1. **Integracja (BRAMKA)** `tests/integration.sh`: round-trip dużych plików, edycja in-place,
   `mv`, df, chmod, links, xattr, **kill -9 + recover → fsck czysty**, **at-rest encryption**,
   współbieżność — wszystko zielone (semantyka niezmieniona mimo writeback/splice/max_write).
2. `tests/integration_blockdev.sh` zielony.
3. Regresja jednostkowa A–P `0 failed` (rdzeń nietknięty → bez zmian).
4. (Informacyjnie) większe `max_write` → mniej round-tripów; throughput zależny od jądra/nośnika.

## Ograniczenia
- Realny zysk zależy od jądra (max_pages) i nośnika; pomiar u użytkownika (fio).
- Nie zmienia narzutu CPU sum/krypto (osobny pod-projekt R).
