# ghostfs — pod-projekt R: szybszy CRC (slice-by-8)

**Data:** 2026-06-24
**Status:** zatwierdzony (autonomia); diagnoza z `benchmark.md` (sumy per blok = część kosztu zapisu).
**Część całości:** A–Q ✓ → **R (szybszy CRC)**.

## Problem
`gh_crc32_update` liczy CRC **bajt po bajcie** (~300–500 MB/s). Suma jest liczona nad każdym
blokiem 4 KB przy każdym zapisie (i weryfikowana przy odczycie) → kilkanaście–kilkadziesiąt %
kosztu ścieżki zapisu/odczytu.

## Rozwiązanie: slice-by-8
Przetwarzanie **8 bajtów na iterację** z 8 tablicami (256 wpisów każda) — standardowy
slice-by-8 dla odbitego CRC32. **Wynik IDENTYCZNY** (ten sam wielomian `0xEDB88320`, ta sama
inicjalizacja/finalny XOR) → **pełna kompatybilność formatu**: istniejące sumy na dysku
walidują się bez zmian. ~3–8× szybszy CRC.

Tablice: `t[0]` = obecna tablica; `t[k][i] = (t[k-1][i] >> 8) ^ t[0][t[k-1][i] & 0xFF]`.
Pętla: XOR 4 bajtów do `crc`, 4 kolejne do `hi`, 8 lookupów `t[7..0]`. Ogon (len%8) bajtowo
przez `t[0]` — identycznie jak dotąd. Zachowuje własność inkrementalną (CRC dziennika dzielony
na wywołania daje ten sam wynik).

## Bezpieczeństwo
- **Tylko implementacja CRC** — zero zmian formatu, zero zmian rdzenia/dziennika/szyfrowania.
- KRYTYCZNE: wynik musi być bit-w-bit identyczny ze starą implementacją dla KAŻDEGO wejścia.

## Testowanie (BRAMKA)
1. `tests/test_csum.c` cross-check: referencyjny CRC bajt-po-bajcie (inline, stary algorytm)
   vs `gh_crc32`/`gh_crc32_update` dla wielu długości (0,1,7,8,9,255,256,257,4095,4096,4097,
   losowe) i danych losowych — **identyczne**. Wektor znany: CRC32("123456789")==0xCBF43926.
   Inkrementalność: `update` w jednym wywołaniu == podzielony na kawałki.
2. Istniejące testy sum/dziennika (`test_csum`, `test_journal`, `test_crash`) `0 failed` —
   sumy na dysku zapisane starą metodą czytane nową (i odwrotnie) walidują się.
3. Regresja A–Q `0 failed`; ASan czysty.

## Ograniczenia
- Bez sprzętowego CRC32C (zmieniłby wielomian → niekompatybilne); slice-by-8 jest
  format-kompatybilne i przenośne (czysty C).
