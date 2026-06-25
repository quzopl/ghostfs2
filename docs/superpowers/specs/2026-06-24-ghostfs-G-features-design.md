# ghostfs — pod-projekt G: funkcje odłożone (domknięcie luk)

**Data:** 2026-06-24
**Status:** zatwierdzony projekt (autonomia użytkownika)
**Część całości:** A–F ✓ → **G (funkcje odłożone)**.

## Cel i dobór zakresu

Ścieżka 3 obejmowała: blokady per-i-węzeł, AEAD/integralność, prompt hasła,
RENAME_EXCHANGE/NOREPLACE, wielo-blokowe xattr, przenośność endianness. Po analizie
wykonalności i ryzyka G realizuje **trzy weryfikowalne, samodzielne funkcje** domykające
realne luki, a pozostałe odkłada z konkretnym uzasadnieniem (sekcja „Pozostaje na
przyszłość"). Niezmiennik nadrzędny: zero regresji A–F.

1. **UX i utwardzenie szyfrowania:** interaktywny prompt hasła (gdy brak `GHOSTFS_KEY`),
   odrzucenie pułapki „puste hasło = jawny kontener", wymazanie klucza z pamięci
   (`OPENSSL_cleanse`) przy odmontowaniu.
2. **`RENAME_NOREPLACE` i `RENAME_EXCHANGE`:** dwie flagi `rename(2)`, dziś zwracane jako
   `EINVAL` (luka wskazana w spec A).
3. **Wielo-blokowe xattr:** zniesienie limitu 1 bloku (4 KB) na atrybuty rozszerzone.

## Część 1: UX i utwardzenie szyfrowania

- **Prompt hasła:** gdy kontener jest zaszyfrowany, a `GHOSTFS_KEY` nieustawione/puste,
  CLI i FUSE pytają o hasło interaktywnie przez `getpass("Hasło ghostfs: ")` (terminal,
  bez echa). Jeśli terminal niedostępny (np. CI) i brak `GHOSTFS_KEY` → `-EACCES` jak
  dotąd. Rdzeń (`gh_fs_mount_key`) bez zmian — prompt jest w warstwie narzędzi
  (cli.c/fuse_main.c), które pozyskują hasło i przekazują do `gh_fs_mount_key`.
- **Pułapka pustego hasła:** `gh_format_enc` traktuje `passphrase == NULL` jako „jawny",
  ale `passphrase == ""` (puste) dziś też daje jawny kontener cicho. Zmiana: puste-lecz-
  -nie-NULL hasło → `-EINVAL` (jawny zamiar szyfrowania z pustym hasłem to błąd). Tryb
  jawny pozostaje przez `NULL`.
- **Wymazanie klucza:** `gh_fs_unmount` woła `OPENSSL_cleanse` na `cipher->key` przed
  `free` (klucz nie zostaje w zwolnionej pamięci). Wymaga, by `fs.c` znał `gh_cipher` —
  użyć `gh_crypto_wipe(struct gh_cipher*)` z `crypto.c` (opakowanie `OPENSSL_cleanse`),
  by nie wnosić OpenSSL do fs.c.

## Część 2: `RENAME_NOREPLACE` i `RENAME_EXCHANGE`

Nowe publiczne API rdzenia (zachowuje stare):

```c
#define GH_RENAME_NOREPLACE 0x1   /* = RENAME_NOREPLACE z linux/fs.h */
#define GH_RENAME_EXCHANGE  0x2   /* = RENAME_EXCHANGE */
int gh_fs_rename2(struct gh_fs*, const char *oldpath, const char *newpath, unsigned flags);
/* gh_fs_rename(...) == gh_fs_rename2(..., 0) — bez zmian zachowania */
```

- **`flags == 0`:** dotychczasowa semantyka (nadpisuje cel wg reguł).
- **`GH_RENAME_NOREPLACE`:** jeśli `newpath` istnieje → `-EEXIST`; inaczej jak zwykły
  rename. (Nie łączyć z EXCHANGE — `(flags & both)` → `-EINVAL`.)
- **`GH_RENAME_EXCHANGE`:** oba muszą istnieć (`oldpath` i `newpath`), inaczej `-ENOENT`.
  Atomowo zamień wpisy: w rodzicu starego `oldname → ino(new)`, w rodzicu nowego
  `newname → ino(old)`. Jeśli któryś jest katalogiem i rodzice różni — zaktualizuj jego
  `..` na nowego rodzica (oba kierunki). `nlink` rodziców bez zmian (model FS: stałe 2).
  Brak zwalniania i-węzłów (nic nie znika).
- **FUSE `gf_rename`:** przekaż `flags` do `gh_fs_rename2` (zamiast `if (flags) return
  -EINVAL`). Nieznane flagi (poza tymi dwiema) → `-EINVAL`.
- **CLI:** komenda `mv` pozostaje `flags=0`; (opcjonalnie warianty — poza zakresem,
  flagi głównie z VFS przez FUSE).

## Część 3: Wielo-blokowe xattr (łańcuch)

Dziś atrybuty mieszczą się w jednym bloku (`inode.xattr_block`). Wprowadzamy **łańcuch
bloków xattr**:

- Każdy blok xattr: **pierwsze 8 bajtów = numer następnego bloku** (`0` = koniec
  łańcucha); pozostałe `GH_BLOCK_SIZE - 8` bajtów = rekordy w dotychczasowym formacie
  (`[u16 name_len][u16 value_len][name][value]`, terminator `name_len==0` lub koniec
  obszaru rekordów bloku).
- `inode.xattr_block` = głowa łańcucha (`0` = brak xattr).
- **Limit:** `GH_XATTR_MAX` rośnie do wielu bloków; pojedyncza para `name+value` nadal
  musi zmieścić się w jednym bloku rekordów (`<= GH_BLOCK_SIZE - 8 - 4`). Łączna liczba
  atrybutów ograniczona dostępnymi blokami (alokator).
- **Operacje (`xattr.c`):**
  - `set`: znajdź nazwę w łańcuchu (usuń stary rekord jeśli jest), dopisz nowy do
    pierwszego bloku z miejscem; gdy brak miejsca w istniejących blokach → alokuj nowy
    blok, podłącz do łańcucha. Pierwsza alokacja ustawia `inode.xattr_block`.
  - `get`/`list`: przejdź łańcuch.
  - `remove`: usuń rekord; jeśli blok stał się pusty i nie jest głową → odepnij i zwolnij;
    jeśli cały łańcuch pusty → zwolnij głowę, `xattr_block = 0`.
- **`gh_inode_free`:** zwalnia CAŁY łańcuch (idąc po `next`).
- **`gh_fsck`:** zaznacza WSZYSTKIE bloki łańcucha jako osiągalne (idąc po `next`), nie
  tylko głowę.

## Strategia testowania (TDD + ASan)

1. `tests/test_enc.c` (rozszerzenie): puste hasło w `gh_format_enc` → `-EINVAL`; (prompt
   testowany pośrednio — `getpass` wymaga TTY, więc test jednostkowy sprawdza tylko
   politykę pustego hasła i ścieżkę `GHOSTFS_KEY`). Wymazanie klucza: po unmount brak
   asercji na pamięci (trudne) — weryfikacja przez ASan (brak wycieku) + przegląd kodu.
2. `tests/test_fs.c` (rozszerzenie): `RENAME_NOREPLACE` (cel istnieje → EEXIST; nie
   istnieje → przenosi); `RENAME_EXCHANGE` (zamiana dwóch plików; zamiana plik↔katalog z
   aktualizacją `..`; brak jednego → ENOENT; obie flagi naraz → EINVAL); `fsck==0` po.
3. `tests/test_links_xattr.c` (rozszerzenie): xattr przekraczające 1 blok — ustaw wiele
   atrybutów sumarycznie > 4 KB; `get`/`list`/`remove` poprawne; `fsck==0`; `gh_inode_free`
   zwalnia łańcuch (po usunięciu pliku `fsck==0`, brak wycieku bloków).
4. **Regresja:** wszystkie testy A–F `0 failed`; `make test-asan` = 0; integracja FUSE
   (w tym `mv` i xattr) zielona.

## Obsługa błędów

| Sytuacja | Reakcja |
|---|---|
| Zaszyfrowany kontener, brak `GHOSTFS_KEY`, brak TTY | `-EACCES` |
| `gh_format_enc` z pustym (nie-NULL) hasłem | `-EINVAL` |
| `RENAME_NOREPLACE` + cel istnieje | `-EEXIST` |
| `RENAME_EXCHANGE` + brak źródła lub celu | `-ENOENT` |
| `RENAME_NOREPLACE \| RENAME_EXCHANGE` | `-EINVAL` |
| nieznana flaga rename | `-EINVAL` |
| pojedynczy atrybut > blok rekordów | `-E2BIG`/`-ENOSPC` |
| brak wolnych bloków na łańcuch xattr | `-ENOSPC` |

## Pozostaje na przyszłość (świadomie poza G — z uzasadnieniem)

- **Blokady per-i-węzeł (równoległe zapisy różnych plików):** wymagają nie tylko zamków
  per-i-węzeł, ale i **partycjonowania dziennika** (dziś jeden region/jedna transakcja na
  urządzenie — dwa równoległe zapisy nie mogą zatwierdzać niezależnie). To redesign B, nie
  pojedyncza funkcja. Obecny rwlock (D) daje poprawność + równoległe odczyty.
- **AEAD / integralność kryptograficzna:** XTS daje poufność, nie wykrywa modyfikacji.
  Pełna integralność wymaga przechowywania tagu MAC per blok (dedykowany region tagów +
  atomowość tagu z danymi przez dziennik) — istotny redesign formatu i ścieżki I/O.
  Strukturalną spójność dalej pilnują `fsck`/journaling.
- **Przenośność endianness:** mechaniczna, ale **nieweryfikowalna na hoście little-endian**
  (testy nie wykryją błędów serializacji) — wymaga środowiska big-endian lub asercji
  bajtowych; ryzykowna „w ciemno". Format zakłada x86-64 (świadome YAGNI z v1).
