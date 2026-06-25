# ghostfs — pod-projekt C: szyfrowanie at-rest

**Data:** 2026-06-23
**Status:** zatwierdzony projekt (autonomia użytkownika)
**Część całości:** A (POSIX) ✓ → B (journaling) ✓ → **C (szyfrowanie)** → D (współbieżność).

## Cel

Realna kryptografia danych w spoczynku (at-rest), ponad dotychczasowe „security by
obscurity". Po sformatowaniu z hasłem **cała zawartość kontenera poza superblokiem**
(mapa bitowa, i-węzły, dziennik, dane) jest zaszyfrowana. Bez poprawnego hasła
kontenera nie da się odczytać ani zamontować. Spec głównego projektu przewidział
zapas w superbloku na szyfrowanie — wykorzystujemy go.

Prymityw: **AES-256-XTS** (standard szyfrowania urządzeń blokowych) z biblioteki
OpenSSL libcrypto. Klucz wyprowadzany z hasła przez **PBKDF2-HMAC-SHA256**. Nie piszemy
własnej kryptografii.

## Zasada nadrzędna: szyfrowanie w warstwie I/O dysku

Szyfrowanie wpina się w **fizyczny dostęp do dysku** (najniższa warstwa), tak by było
przezroczyste dla całego rdzenia ORAZ dla dziennika (B). Wprowadzamy w `block.c`
publiczne, świadome szyfru funkcje fizycznego I/O: `gh_disk_read`/`gh_disk_write`.
Używa ich zarówno `gh_block_read`/`gh_block_write` (ścieżka bez transakcji), jak i
`journal.c` (zamiast własnego `pwrite`/`pread`). Dzięki temu na dysku zawsze leży
szyfrogram, a w pamięci (bufor transakcji, bufory rdzenia) — jawny tekst.

- **Superblok (blok 0) NIE jest szyfrowany** — przechowuje `magic`, sól i weryfikator
  hasła w jawnej postaci (muszą być czytelne przed wyprowadzeniem klucza).
- **Tweak XTS = numer bloku fizycznego** — identyczne bloki w różnych miejscach dają
  różny szyfrogram; brak wycieku wzorców między blokami.

## Zmiany formatu on-disk (kompatybilne wstecz)

### Superblok — pola szyfrowania (z rezerwy)

```c
struct gh_superblock {
    ... (bez zmian, do journal_blocks) ...
    uint64_t journal_blocks;
    uint32_t flags;             /* NOWE: bit GH_SB_ENCRYPTED */
    uint32_t enc_kdf_iters;     /* NOWE: iteracje PBKDF2 (np. 200000) */
    uint8_t  enc_salt[16];      /* NOWE: sól PBKDF2 (losowa) */
    uint8_t  enc_verifier[32];  /* NOWE: SHA256(klucz || sól) — weryfikacja hasła */
};
```

Stałe: `#define GH_SB_ENCRYPTED 0x1`. Pola w dotychczas zerowanej rezerwie → stare
kontenery: `flags==0` = brak szyfrowania (tryb jak dotąd). **Bez zmiany magic.**

## Klucz i jego cykl życia

- **Hasło** pochodzi ze zmiennej środowiskowej `GHOSTFS_KEY` (CLI i FUSE czytają ją
  przy montowaniu/formatowaniu). Proste do automatyzacji i testów; brak hasła w
  argumentach (nie trafia do `ps`/historii powłoki tak łatwo jak argument).
- **Wyprowadzenie:** `klucz[64] = PBKDF2-HMAC-SHA256(hasło, sól, iters)` (64 B = klucz
  AES-256-XTS, czyli dwa klucze 256-bit).
- **Weryfikacja:** `weryfikator = SHA256(klucz || sól)`. Przy montowaniu wyprowadzamy
  klucz i porównujemy z `enc_verifier`; niezgodność → `-EACCES` (złe hasło). Weryfikator
  nie ujawnia klucza.
- Klucz żyje wyłącznie w pamięci (`struct gh_dev.cipher`), nigdy na dysku.

## Nowy moduł `src/crypto.h` / `src/crypto.c` (opakowanie OpenSSL)

```c
struct gh_cipher { uint8_t key[64]; };   /* materiał klucza AES-256-XTS */

int  gh_crypto_derive(const char *passphrase, const uint8_t salt[16],
                      uint32_t iters, uint8_t key[64]);          /* PBKDF2 */
void gh_crypto_verifier(const uint8_t key[64], const uint8_t salt[16],
                        uint8_t out[32]);                        /* SHA256(key||salt) */
int  gh_crypto_random(uint8_t *buf, size_t n);                  /* RAND_bytes */
int  gh_crypto_encrypt_block(const struct gh_cipher*, uint64_t blkno,
                             const uint8_t *in, uint8_t *out);   /* AES-256-XTS, 4096 B */
int  gh_crypto_decrypt_block(const struct gh_cipher*, uint64_t blkno,
                             const uint8_t *in, uint8_t *out);
```

Implementacja przez EVP: `EVP_aes_256_xts()`, tweak = 16-bajtowy numer bloku (LE),
`PKCS5_PBKDF2_HMAC(..., EVP_sha256(), 64, key)`, `RAND_bytes`. Link `-lcrypto`.

## Warstwa bloków (`src/block.h` / `src/block.c`)

- `struct gh_dev` zyskuje `struct gh_cipher *cipher;` (NULL = brak szyfrowania).
  Inicjalizowane na NULL w `gh_dev_create`/`gh_dev_open`.
- Nowe publiczne, świadome szyfru funkcje fizycznego I/O:

```c
int gh_disk_read(struct gh_dev*, uint64_t blkno, void *buf);
int gh_disk_write(struct gh_dev*, uint64_t blkno, const void *buf);
```

Zachowanie: jeśli `dev->cipher && blkno != 0` → przy zapisie szyfruj do bufora tymcz.
i `pwrite`; przy odczycie `pread` i odszyfruj. Inaczej (brak szyfru lub blok 0) →
zwykły `pwrite`/`pread`. Bound-check `blkno < total_blocks` jak dotąd.

- `gh_block_read`/`gh_block_write`: logika transakcji bez zmian; ścieżka bez transakcji
  woła `gh_disk_read`/`gh_disk_write` zamiast bezpośredniego `pread`/`pwrite`.

## Dziennik (`src/journal.c`)

`raw_read`/`raw_write` przestają używać bezpośredniego `pread`/`pwrite` — wołają
`gh_disk_read`/`gh_disk_write`. Dzięki temu region dziennika też jest szyfrowany, a
obrazy (jawne w buforze txn) trafiają na dysk jako szyfrogram (tweak = numer bloku
dziennika), a przy checkpoint/recover są (od)szyfrowywane spójnie per-lokalizacja.

## Format i montowanie (`src/super.c`, `src/fs.c`)

### Format

```c
int gh_format_enc(const char *path, uint64_t total_blocks, uint64_t inode_count,
                  const char *passphrase);   /* passphrase==NULL -> bez szyfrowania */
int gh_format(const char *path, uint64_t total_blocks, uint64_t inode_count);
/* gh_format(...) == gh_format_enc(..., NULL) — zachowuje istniejące API/testy */
```

`gh_format_enc` z hasłem: losuje sól (`gh_crypto_random`), ustawia `iters`, wyprowadza
klucz, liczy weryfikator, ustawia `flags|=GH_SB_ENCRYPTED` i pola enc_* w superbloku,
ustawia `dev.cipher`, zapisuje superblok (blok 0, **jawnie**), a pozostałe bloki
(mapa/i-węzły/dziennik/root) **szyfrem**.

### Montowanie

```c
int gh_fs_mount_key(struct gh_fs*, const char *path, const char *passphrase);
int gh_fs_mount(struct gh_fs*, const char *path);
/* gh_fs_mount(...) == gh_fs_mount_key(..., getenv("GHOSTFS_KEY")) */
```

Przebieg `gh_fs_mount_key`: `gh_dev_open` → `gh_mount_sb` (czyta blok 0 jawnie) →
jeśli `flags & GH_SB_ENCRYPTED`: brak hasła → `-EACCES`; wyprowadź klucz; porównaj
weryfikator (niezgodność → `-EACCES`); `dev.cipher = nowy gh_cipher(klucz)` →
`gh_jrnl_recover` (czyta zaszyfrowany dziennik). `gh_fs_unmount` zwalnia `cipher`.

Stare/niezaszyfrowane kontenery (`flags==0`): hasło ignorowane, tryb jawny — pełna
kompatybilność wstecz; istniejące testy A/B działają bez zmian.

## Narzędzia (`src/cli.c`, `src/fuse_main.c`)

- CLI i FUSE czytają hasło z `GHOSTFS_KEY` (przez `gh_fs_mount` → `getenv`). `format`
  szyfruje, gdy `GHOSTFS_KEY` jest ustawione i niepuste (inaczej kontener jawny).
- Brak nowych argumentów — opcjonalność przez środowisko (łatwe testy/automatyzacja).
- Komunikat przy złym haśle: `-EACCES` → czytelny błąd „złe hasło / kontener
  zaszyfrowany".

## Strategia testowania (TDD + ASan)

1. **`tests/test_crypto.c`** (nowy):
   - derive deterministyczny (to samo hasło+sól+iters → ten sam klucz; inne hasło → inny).
   - encrypt→decrypt round-trip bloku 4096 B (jawne == odszyfrowane); szyfrogram ≠ jawne.
   - ten sam jawny blok pod różnym `blkno` → różny szyfrogram (tweak działa).
   - weryfikator: poprawne hasło zgodne, złe niezgodne.
2. **`tests/test_fs.c`** / nowy **`tests/test_enc.c`**:
   - `gh_format_enc(...,"haslo")` → `gh_fs_mount_key(...,"haslo")` OK; operacje
     (mkdir/create/write/read/rename/xattr) działają; `fsck`==0.
   - `gh_fs_mount_key(...,"zle")` → `-EACCES`; `gh_fs_mount_key(...,NULL)` → `-EACCES`.
   - **at-rest**: po zapisie pliku z jawną treścią, surowy odczyt bloku danych z pliku
     (przez `pread` na fd) NIE zawiera jawnego tekstu (szyfrogram).
   - kontener zaszyfrowany + journaling: operacja, remount z hasłem, recover, dane
     spójne; `fsck`==0.
3. **Kompatybilność**: wszystkie testy A/B (kontenery jawne) dalej `0 failed`.
4. **Integracja**: `tests/integration.sh` — wariant zaszyfrowany: `GHOSTFS_KEY=...`,
   `format`, montowanie FUSE z hasłem, round-trip pliku, `grep` surowego kontenera nie
   znajduje jawnego markera; montowanie bez/ze złym hasłem odmawia.
5. **ASan** — `make test-asan` = 0 failed; zero wycieków (cipher zwalniany w unmount,
   konteksty EVP zwalniane po użyciu).

## Obsługa błędów

| Sytuacja | Reakcja |
|---|---|
| Brak/zła wartość `GHOSTFS_KEY` na zaszyfrowanym kontenerze | `-EACCES` |
| Niezgodny weryfikator (złe hasło) | `-EACCES`, bez montowania |
| Błąd OpenSSL (derive/encrypt) | `-EIO` + log |
| `format` z hasłem, ale bez OpenSSL/RAND | `-EIO` |
| Operacje na jawnym kontenerze | tryb jawny (kompatybilność) |

## Świadome ograniczenia C (YAGNI)

- Brak uwierzytelniania treści (AEAD/MAC per blok) — XTS daje poufność, nie
  integralność kryptograficzną; wykrywanie modyfikacji to ewentualne rozszerzenie.
  (Spójność strukturalną dalej pilnuje `fsck`/journaling.)
- Jedno hasło na kontener; brak wielu kluczy/rotacji klucza.
- Superblok jawny (magic + sól + weryfikator) — celowo, by umożliwić wykrycie
  formatu i wyprowadzenie klucza. Sam fakt „to kontener ghostfs" nie jest tajny.
- Hasło przez `GHOSTFS_KEY` — wygodne, ale środowiskowe; interaktywny prompt to
  możliwe rozszerzenie.
- Rozmiar klucza/algorytm stałe (AES-256-XTS, PBKDF2-SHA256); brak negocjacji.
