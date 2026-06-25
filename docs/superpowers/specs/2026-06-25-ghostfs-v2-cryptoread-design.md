# ghostfs — szybsze deszyfrowanie (thread-local EVP context): design

**Data:** 2026-06-25 **Część:** crypto (wspólne v1+v2). Diagnoza z benchmarkv2.md.

## Problem
Szyfrowany odczyt sekwencyjny −26% (po przyspieszeniu odczytu czyste AES-XTS widać). Przyczyna:
funkcja `xts` (`src/crypto.c`) robi NA KAŻDY BLOK 4 KB: `EVP_CIPHER_CTX_new()` + `EVP_*Init_ex(ctx,
EVP_aes_256_xts(), NULL, key, tweak)` (pełny **key-schedule AES-256** — ekspansja klucza) +
Update + Final + `EVP_CIPHER_CTX_free`. Odczyt sekwencyjny 256 bloków = 256 ekspansji klucza +
256 alloc/free kontekstu. Key-schedule dominuje (AES-NI samo szyfrowanie jest szybkie).

## Rozwiązanie: thread-local EVP context (re-key raz na wątek, re-tweak per blok)
Kontekst EVP **per-wątek** (thread-local), zainicjowany kluczem RAZ; per blok tylko ustaw tweak
(IV) bez re-key:
- TLS: `EVP_CIPHER_CTX *ctx` + znacznik klucza, którym zainicjowany (np. hash 64-bajtowego klucza
  lub wskaźnik+epoka). Pierwsze użycie w wątku LUB inny klucz → `EVP_*Init_ex(ctx, EVP_aes_256_xts,
  NULL, key, NULL)` (key-schedule RAZ). Każdy blok → `EVP_*Init_ex(ctx, NULL,NULL,NULL, tweak)`
  (ustaw tylko IV/tweak, BEZ re-key) → Update(4096) → Final.
- **Thread-safe BEZ locka**: każdy wątek ma własny ctx (równoległe odczyty zachowane — D rdlock).
- **ASan-clean**: `pthread_key_create` z destruktorem zwalniającym ctx przy wyjściu wątku;
  `pthread_once` do inicjalizacji klucza TLS.
- **Transparentne (BAJT-EXACT)**: ten sam klucz + ten sam tweak per blok → IDENTYCZNY szyfrogram/
  plaintext co pełny init per blok. XTS: IV = 16-bajtowy tweak z numeru bloku (LE) — bez zmian.

## API (`src/crypto.{h,c}`)
- Sygnatury `gh_crypto_encrypt_block`/`gh_crypto_decrypt_block` BEZ ZMIAN (transparentne).
- Wewnątrz `xts` zamiast new/init(key)/free per wywołanie → użyj TLS ctx (lazy re-key, re-tweak).
- Fallback: gdy `EVP_*Init_ex(tweak-only)` zawiedzie/ctx OOM → ścieżka jak dotąd (new+init+free)
  (poprawność > wydajność).

## Spójność klucza w TLS
Znacznik: 8-bajtowy hash (FNV-1a) z 64-bajtowego klucza zapisany przy re-key; jeśli kolejne
wywołanie ma inny hash → re-key. (Pojedynczy mount = stały klucz → re-key raz/wątek. Wiele
mountów/kluczy w procesie → re-key przy zmianie — rzadkie, poprawne.) Klucz NIE przechowywany w
TLS dłużej niż ctx; przy re-key tylko hash.

## Testy (`tests/test_cryptoreuse.c` + istniejące crypto/v1/v2 enc)
1. **BAJT-EXACT (BRAMKA):** encrypt/decrypt przez nowy `xts` == stary (per blok pełny init) dla
   losowych danych/bloków; round-trip dec(enc(x))==x; znane wektory (jeśli są) niezmienione.
2. **Wiele bloków różne tweaki:** sekwencja blkno 0..N — każdy poprawny (tweak per blok); blok N i
   M różne (różny tweak → różny szyfrogram dla tych samych danych).
3. **Zmiana klucza w wątku:** dwa różne klucze naprzemiennie w jednym wątku → każdy poprawny
   (re-key na zmianę hash); brak przecieku klucza między operacjami.
4. **Thread-safety (KRYTYCZNE):** N wątków równolegle enc/dec różne bloki tym samym kluczem →
   każdy bajt-exact (własny ctx per wątek, brak wyścigu); ASan/TSan czyste.
5. **Regresja:** test_crypto (v1), v1 fs zaszyfrowany, **test_v2enc** + v2 zaszyfrowany round-trip —
   wszystkie 0 failed; szyfrowanie at-rest zachowane (marker nieobecny w surowym kontenerze).
6. **ASan detect_leaks=1:** brak wycieku (pthread_key destruktor zwalnia ctx; pojedynczy wątek
   testowy → destruktor przy exit lub jawne czyszczenie). UBSan czyste.
7. **Dowód:** licznik `EVP_*Init_ex(key)` (re-key) — dla N bloków sekwencyjnych w 1 wątku == 1
   (nie N); per-blok init tweak-only.

## Świadome ograniczenia
- TLS ctx żyje do końca wątku (zwolniony destruktorem). Proces z wieloma wątkami FUSE → ctx/wątek.
- Dotyczy v1 i v2 (wspólne crypto) — oba korzystają.
- Nie zrównolegla AES przez rdzenie (osobny lewar); eliminuje narzut key-schedule (główny koszt).
