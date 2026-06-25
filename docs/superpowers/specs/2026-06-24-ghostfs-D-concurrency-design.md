# ghostfs — pod-projekt D: współbieżność (bezpieczny wielowątkowy sterownik)

**Data:** 2026-06-24
**Status:** zatwierdzony projekt (autonomia użytkownika)
**Część całości:** A (POSIX) ✓ → B (journaling) ✓ → C (szyfrowanie) ✓ → **D (współbieżność)**.

## Motywacja (realny utajony błąd)

`fuse_main` jest uruchamiany bez `-s`, więc pętla zdarzeń FUSE jest **wielowątkowa**:
wiele żądań VFS może wykonywać się równolegle w osobnych wątkach. Rdzeń ghostfs nie ma
żadnej synchronizacji, a po pod-projekcie B istnieje **jeden, współdzielony na całe
urządzenie bufor transakcji** (`dev->txn`). Dwie współbieżne operacje modyfikujące
wywołałyby `gh_jrnl_begin` na tym samym `dev->txn`, nadpisując się i korumpując dane.
To utajony wyścig — testy sekwencyjne go nie wyzwalają, ale realny współbieżny dostęp
(np. równoległe `cp`/`make -j`) tak.

**Cel D:** uczynić wielowątkowy sterownik FUSE **poprawnym**, z realną współbieżnością
odczytów, oraz świadomie określić granicę dla wielodostępu międzyprocesowego.

## Zakres i decyzje

### W zakresie
- **Bezpieczeństwo wątkowe sterownika FUSE.** Blokada czytelnik-pisarz na poziomie
  sterownika serializuje operacje modyfikujące (wyłączny dostęp — bo współdzielą
  `dev->txn`, mapę bitową, dziennik) i dopuszcza **równoległe odczyty**.
- **Wielowątkowy FUSE pozostaje włączony** (bez `-s`) — teraz bezpieczny. Odczyty
  (`read`/`getattr`/`readdir`/`statfs`/`getxattr`/`listxattr`/`readlink`) biegną
  współbieżnie; zapisy są wzajemnie wykluczane i wykluczają odczyty.
- **Test obciążeniowy współbieżności** dowodzący braku korupcji (równoległe operacje +
  `fsck` czysty).

### Poza zakresem (świadomie, z uzasadnieniem)
- **Współdzielony zapis międzyprocesowy.** Dwa procesy piszące do tego samego kontenera
  wyścigowałyby na mapie bitowej (read-modify-write bitu) oraz na **jednym regionie
  dziennika** (oba procesy piszące w te same bloki dziennika → korupcja). Bezpieczne
  rozwiązanie wymaga współdzielonego menedżera blokad i koherencji cache między
  procesami — to osobny, badawczy projekt. Dlatego **`flock` pozostaje**: jeden proces
  na kontener (drugi dostaje błąd). To zgodne z pierwotnym założeniem spec
  („Wielodostęp = YAGNI na później"). D adresuje współbieżność WEWNĄTRZ procesu
  (wielowątkowy sterownik), nie MIĘDZY procesami.
- **Drobnoziarniste blokady per-i-węzeł** (równoległe zapisy różnych plików). Wymagałyby
  przeniesienia bufora transakcji z `dev` do kontekstu per-operacja/per-wątek (duży
  refactor B). Granica: D daje równoległe odczyty + serializowane zapisy; per-i-węzeł
  to możliwe rozszerzenie.

## Architektura: blokada na poziomie sterownika

Synchronizacja żyje w **sterowniku** (`fuse_main.c`), nie w rdzeniu. Rdzeń (CLI) jest
jednowątkowy i jednoprocesowy per komenda — nie potrzebuje blokad i pozostaje bez zmian
(zero narzutu, prostota). Tylko sterownik FUSE ma współbieżność, więc tam jest blokada.

```c
static pthread_rwlock_t g_lock;   /* "wielka blokada systemu plików" sterownika */
```

- Inicjalizacja w `main` po `gh_fs_mount` (`pthread_rwlock_init`), zniszczenie po
  `fuse_main` (`pthread_rwlock_destroy`).
- Każdy handler FUSE bierze blokadę na czas wywołania rdzenia:
  - **Odczyt (współdzielony, `rdlock`)**: `getattr`, `readdir`, `read`, `readlink`,
    `statfs`, `getxattr`, `listxattr`.
  - **Zapis (wyłączny, `wrlock`)**: `create`, `mkdir`, `unlink`, `rmdir`, `rename`,
    `truncate`, `write`, `chmod`, `chown`, `utimens`, `symlink`, `link`, `setxattr`,
    `removexattr`. Również `open` (może skracać przy `O_TRUNC`), `flush`, `fsync`
    (bezpiecznie pod `wrlock`).
- Wzorzec (przez makra pomocnicze, by uniknąć powtórzeń i wycieków blokady):

```c
#define GF_RD(expr) do { pthread_rwlock_rdlock(&g_lock); int _r = (expr); \
                         pthread_rwlock_unlock(&g_lock); return _r; } while (0)
#define GF_WR(expr) do { pthread_rwlock_wrlock(&g_lock); int _r = (expr); \
                         pthread_rwlock_unlock(&g_lock); return _r; } while (0)
```

(Dla handlerów zwracających przez `(int)ssize_t` — analogiczny wariant lub jawny
prolog/epilog. Każda ścieżka wyjścia handlera MUSI zwolnić blokadę.)

### Dlaczego rwlock wystarcza dla poprawności

- Zapis trzyma blokadę **wyłączną** → tylko jeden wątek dotyka `dev->txn`, mapy bitowej
  i dziennika naraz. Współdzielony stan B jest bezpieczny.
- Odczyty trzymają blokadę **współdzieloną** → biegną równolegle (czysty `pread`/
  deszyfrowanie są wątkowo-bezpieczne; każde wywołanie używa własnych buforów lokalnych
  i własnego kontekstu EVP w `crypto.c`).
- Odczyt nie może biec w trakcie zapisu (wrlock wyklucza rdlock) → czytelnik widzi stan
  sprzed operacji albo po zatwierdzeniu, nigdy w połowie transakcji.

### Wątkowa bezpieczność warstw niższych
- `crypto.c`: każde (de)szyfrowanie tworzy własny `EVP_CIPHER_CTX` (brak współdzielonego
  stanu) — bezpieczne równolegle. PBKDF2/derive wołane tylko przy montowaniu (1 wątek).
- `block.c`/`pread`/`pwrite`: pozycjonowane I/O (`pread`/`pwrite`) jest wątkowo-bezpieczne
  (nie używa współdzielonego offsetu pliku). Pod blokadą sterownika i tak nie ma
  równoległych zapisów.

## Zmiany plików

| Plik | Zmiana |
|---|---|
| `src/fuse_main.c` | `pthread_rwlock_t g_lock`; init/destroy; każdy handler pod rdlock/wrlock; `#include <pthread.h>` |
| `Makefile` | `-lpthread` do celu `fuse` |
| `tests/integration.sh` | test obciążeniowy współbieżności (równoległe operacje + fsck) |
| (rdzeń) | bez zmian — blokada wyłącznie w sterowniku |

## Strategia testowania

1. **Test obciążeniowy współbieżności (integracja FUSE)**: na zamontowanym FS uruchom
   N (np. 8) równoległych procesów/pętli, każda tworzy/zapisuje/czyta/usuwa wiele plików
   w osobnym podkatalogu; po `wait` odmontuj i `fsck` → **0 niespójności**. Dodatkowo
   wariant „wszyscy piszą do wspólnego katalogu" (stres na blok katalogu/mapę).
   Powtórzenie kilkukrotne (wyścigi są niedeterministyczne).
2. **Równoległy odczyt dużego pliku**: wielu czytelników `cat`/`cmp` tego samego dużego
   pliku równolegle == treść spójna (dowód, że odczyty współbieżne nie psują).
3. **Brak regresji**: wszystkie testy jednostkowe (A/B/C) i istniejące integracyjne dalej
   przechodzą; `make test-asan` = 0 failed. (Rdzeń niezmieniony, więc jednostkowe bez
   zmian.)
4. **flock nadal działa**: druga próba montowania tego samego kontenera → odmowa
   (jeden proces). (Istniejące zachowanie, potwierdzić w integracji.)
5. **Sanityzacja wątkowa (jeśli dostępna)**: budowa sterownika pod ThreadSanitizer
   (`-fsanitize=thread`) i krótki przebieg współbieżny — zero ostrzeżeń o wyścigach.
   (Best-effort; jeśli TSan koliduje z FUSE w środowisku — odnotować i polegać na teście
   obciążeniowym + fsck.)

## Obsługa błędów / przypadki brzegowe

| Sytuacja | Reakcja |
|---|---|
| Drugi proces montuje zajęty kontener | `flock` → odmowa (bez zmian) |
| Błąd `pthread_rwlock_init` | sterownik kończy z błędem przed montowaniem |
| Handler zwraca błąd | blokada zwalniana na każdej ścieżce (makra/epilog) |
| Operacja modyfikująca w trakcie odczytów | wrlock czeka aż odczyty zwolnią, potem wyłączność |

## Świadome ograniczenia D (YAGNI)

- Współbieżność **wewnątrz procesu** (wielowątkowy sterownik). Wielodostęp
  międzyprocesowy nadal wykluczany przez `flock` (jeden pisarz-proces) — pełen
  wielodostęp to osobny projekt (menedżer blokad + koherencja cache).
- Granularność: jedna „wielka blokada" sterownika (równoległe odczyty, serializowane
  zapisy). Drobnoziarniste blokady per-i-węzeł (równoległe zapisy różnych plików)
  wymagają per-operacyjnego bufora transakcji — możliwe rozszerzenie ponad D.
- Brak priorytetów/uczciwości ponad to, co daje `pthread_rwlock` implementacji.
