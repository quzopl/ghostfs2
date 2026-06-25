# ghostfs — projekt własnego systemu plików

**Data:** 2026-06-22
**Status:** zatwierdzony projekt (przed planem implementacji)

## Cel

Własny, celowo **niekompatybilny** z istniejącymi system plików. Dane trzymane we
własnym pliku-kontenerze o autorskim formacie binarnym (z własnymi indeksami —
mapą bloków i tablicą i-węzłów). Standardowe narzędzia (`mount`, `file`, `blkid`)
nie rozpoznają formatu — do obsługi wymagany jest własny sterownik.

**Charakter ochrony:** security by obscurity (własny format), **nie** kryptografia.
Chroni przed standardowymi narzędziami i przypadkowym dostępem, nie przed
zdeterminowanym ekspertem od inżynierii wstecznej. Format zostawia zapas w
superbloku, by w przyszłości dało się dołożyć szyfrowanie.

## Decyzje projektowe (ustalone)

| Decyzja | Wybór |
|---|---|
| Język | C |
| Sterownik | libfuse (FUSE) — montowanie jako dysk |
| Dostęp | FUSE (montowany dysk) **oraz** narzędzie CLI |
| Składowanie | własny plik binarny z indeksami (bez gotowej bazy/SQLite) |
| Architektura on-disk | blokowa z i-węzłami (styl ext2 / xv6) |
| Wymagane funkcje v1 | katalogi (drzewo), duże pliki, metadane, edycja w miejscu |

## Architektura warstwowa

```
┌─────────────────────────────────────────────┐
│  ghostfs (FUSE)        │  ghostfs-cli         │   warstwa dostępu
│  montuje jako dysk     │  komendy w terminalu │
└───────────┬────────────┴──────────┬──────────┘
            └────────────┬───────────┘
                         ▼
┌─────────────────────────────────────────────┐
│  libghostfs — RDZEŃ (biblioteka C)            │   cała logika FS
│  open/read/write/mkdir/unlink/...             │
└───────────┬───────────────────────┬──────────┘
            ▼                        ▼
┌──────────────────────┐  ┌──────────────────────┐
│  Warstwa i-węzłów     │  │  Alokator bloków      │   struktury on-disk
│  (metadane, drzewo)   │  │  (mapa bitowa)        │
└───────────┬──────────┘  └──────────┬───────────┘
            └────────────┬───────────┘
                         ▼
┌─────────────────────────────────────────────┐
│  Warstwa bloków — czyta/pisze bloki 4 KB      │   surowy dostęp
│  do pliku-kontenera (np. ~/.ghost/store.gfs)  │
└─────────────────────────────────────────────┘
```

**Kluczowa zasada:** cała logika w `libghostfs`. FUSE i CLI to cienkie nakładki
wołające to samo API rdzenia — rdzeń testowalny niezależnie od FUSE, oba sposoby
dostępu zachowują się identycznie.

### Komponenty (każdy jedna odpowiedzialność)

| Komponent | Odpowiedzialność |
|---|---|
| `block.c` | czytanie/zapisywanie bloków 4 KB do pliku-kontenera |
| `alloc.c` | mapa bitowa — przydzielanie/zwalnianie bloków |
| `inode.c` | i-węzły: metadane + wskaźniki na bloki, odczyt/zapis treści pliku |
| `dir.c` | katalogi: wpisy `nazwa → i-węzeł`, wyszukiwanie po ścieżce |
| `super.c` | superblok: format/montowanie/odmontowanie kontenera |
| `fs.c` | publiczne API rdzenia (open/read/write/mkdir/...) |
| `fuse_main.c` | sterownik FUSE (mapuje wywołania systemu na API rdzenia) |
| `cli.c` | narzędzie CLI (format/mkfs, ls, put, get, rm, fsck) |

### Tworzenie kontenera (mkfs)

`ghostfs-cli format <plik.gfs> <rozmiar>` inicjalizuje nowy kontener: zapisuje
superblok z `magic`, wylicza i zeruje mapę bitową, tworzy tablicę i-węzłów i
zakłada pusty katalog główny (`root_inode`). Dopiero po sformatowaniu kontener da
się zamontować przez FUSE. Próba montowania niesformatowanego pliku → odmowa
(brak/niepoprawny `magic`).

## Format kontenera (on-disk)

Plik-kontener (`store.gfs`) to ciąg bloków po 4 KB:

```
blok 0        1            2 .. N           ...        ...
┌──────────┬─────────────┬───────────────┬──────────────┐
│ superblok│ mapa bitowa │ tablica       │  bloki        │
│          │ bloków      │ i-węzłów      │  danych       │
└──────────┴─────────────┴───────────────┴──────────────┘
```

### Superblok (blok 0)

```c
struct superblock {
    uint8_t  magic[8];      // "GHOSTFS\1" — celowo nietypowy
    uint32_t block_size;    // 4096
    uint64_t total_blocks;  // rozmiar kontenera w blokach
    uint64_t inode_count;   // ile i-węzłów w tablicy
    uint64_t bitmap_start;  // numer bloku startu mapy bitowej
    uint64_t inode_start;   // numer bloku tablicy i-węzłów
    uint64_t data_start;    // numer pierwszego bloku danych
    uint64_t root_inode;    // i-węzeł katalogu głównego (zwykle 1)
    uint8_t  reserved[];    // dopełnienie zerami do końca bloku 4096 B
};                          // (zapas na flagi snapshotów/szyfrowania)
```

Superblok zajmuje cały blok 0 (4096 B); pola po `root_inode` wyzerowane jako
rezerwa na przyszłe rozszerzenia.

### I-węzeł

```c
struct inode {
    uint16_t type;          // 0=wolny, 1=plik, 2=katalog
    uint16_t mode;          // uprawnienia (rwx) → ls -l
    uint32_t uid, gid;
    uint64_t size;          // rozmiar w bajtach
    uint64_t atime, mtime, ctime;
    uint32_t nlink;
    uint64_t direct[12];      // 12 wskaźników bezpośrednich (do 48 KB)
    uint64_t indirect;        // blok pośredni (512 wskaźników, +2 MB)
    uint64_t double_indirect; // podwójnie pośredni (do ~1 GB+)
};
```

**Duże pliki:** schemat wskaźników bezpośredni → pośredni → podwójnie pośredni.
**Edycja w miejscu:** nadpisanie konkretnego bloku danych bez ruszania reszty.

### Katalog

Zwykły plik, którego treść to tablica wpisów:

```c
struct dirent {
    uint64_t inode;     // numer i-węzła (0 = pusty/usunięty)
    uint16_t name_len;
    char     name[256];
};
```

Wyszukiwanie ścieżki `/a/b/c` = przejście od root katalog po katalogu z
dopasowaniem nazw.

### Mapa bitowa

1 bit na blok danych (0=wolny, 1=zajęty). Alokator szuka wolnego bitu, zaznacza,
zwraca numer bloku.

## Przepływ danych

### Montowanie (`ghostfs store.gfs /mnt/ghost`)
1. `super.c` czyta blok 0, sprawdza `magic` — zły magic = odmowa montowania.
2. Wczytanie superbloku, mapy bitowej i tablicy i-węzłów do cache.
3. FUSE nasłuchuje; `/mnt/ghost` pokazuje zawartość `root_inode`.

### Zapis (`echo "hej" > /mnt/ghost/notatka.txt`)
1. FUSE → `create("/notatka.txt")` → `fs.c`.
2. `dir.c` sprawdza brak istniejącego wpisu.
3. `inode.c` bierze wolny i-węzeł, ustawia typ/czasy/mode.
4. `dir.c` dopisuje wpis `notatka.txt → i-węzeł` do root.
5. `write`: `alloc.c` przydziela blok → dane do `direct[0]` → `block.c` zapisuje →
   `size` rośnie, `mtime` aktualizowany.
6. Zmienione struktury zapisane na dysk.

### Odczyt (`cat /mnt/ghost/notatka.txt`)
1. `lookup` → `dir.c` znajduje i-węzeł.
2. `read(offset,len)`: `blok_logiczny = offset/4096` → przez `direct[]`/`indirect`
   ustalany blok fizyczny.
3. `block.c` czyta bloki, zwraca wycinek bajtów.

### Edycja w miejscu
Lokalizacja właściwego bloku (jak w odczycie), modyfikacja tylko jego zawartości,
zapis z powrotem — reszta pliku nietknięta.

### Kolejność zapisów (spójność)
Bezpieczna kolejność: najpierw bloki danych → i-węzeł → mapa/superblok. Przerwanie
w połowie nie zostawia wiszących wskaźników. Pełny journaling pominięty w v1
(YAGNI); superblok ma zapas, by dodać go później.

## Obsługa błędów

### Walidacja przy montowaniu
- Zły `magic`/rozmiar → odmowa z czytelnym komunikatem (bez prób „naprawy").
- Niespójny superblok (np. `data_start` poza plikiem) → odmowa.

### Kody errno podczas operacji

| Sytuacja | Reakcja |
|---|---|
| Brak miejsca (mapa pełna) | `ENOSPC`, bez częściowo przydzielonych bloków |
| Brak wolnego i-węzła | `ENOSPC` |
| Ścieżka nie istnieje | `ENOENT` |
| Plik już istnieje (create) | `EEXIST` |
| `rmdir` na niepustym katalogu | `ENOTEMPTY` |
| Nazwa > 255 znaków | `ENAMETOOLONG` |
| Odczyt/zapis poza rozmiarem | przytnij do rozmiaru / rozszerz zerami |
| Uszkodzony wskaźnik bloku | `EIO` + log, przerwij |

### Atomowość operacji
Operacja, która nie może się dokończyć (np. brak miejsca w trakcie), **wycofuje**
już przydzielone bloki. Stan po nieudanej operacji = stan sprzed niej.

### Współbieżność
v1: jeden proces na raz. `flock` na kontenerze przy montowaniu — drugi proces
dostaje błąd zamiast uszkodzić dane. Wielodostęp = YAGNI na później.

### Awaria zasilania
Dzięki kolejności zapisów najgorszy skutek = osierocone zajęte bloki (zmarnowane
miejsce), nie uszkodzenie drzewa. CLI dostaje `fsck` — przejście po i-węzłach i
odbudowa mapy bitowej (wykrywa wycieki i podwójne przydziały).

### Tryb awaryjny
Każda niespójność do logu (stderr); kontener można otworzyć read-only (`-o ro`)
dla ratowania danych.

## Strategia testowania

Od dołu do góry, TDD (test-najpierw), kolejność: `block → alloc → inode → dir →
fs → fuse/cli`. Warstwa wyżej dopiero gdy niższa zielona.

1. **Jednostkowe rdzenia** (kontener = plik tymczasowy, bez FUSE):
   - `block.c`: zapis/odczyt bloku bajt w bajt.
   - `alloc.c`: przydział/zwolnienie, mapa się zgadza; zapełnienie → `ENOSPC`.
   - `inode.c`: dane przekraczające `direct[]` (blok pośredni) i podwójnie
     pośredni dla dużego pliku; odczyt = zapis.
   - `dir.c`: dodaj/znajdź/usuń, wyszukiwanie zagnieżdżonej ścieżki.
2. **Właściwości / odpornościowe:** losowa sekwencja operacji, po każdej `fsck`
   bez błędów; „zapis = odczyt" dla losowych długości.
3. **Symulacja awarii:** przerwanie po N zapisach (wstrzykiwany licznik), ponowne
   montowanie + `fsck` → drzewo spójne, najwyżej osierocone bloki.
4. **Integracyjne FUSE** (bash na `/mnt/ghost`): `cp` dużego pliku tam i z
   powrotem + `diff`; `mkdir -p a/b/c`, `ls -l`, `rm -r`; edycja w miejscu przez
   `dd`; realny `git clone` + kompilacja jako test obciążeniowy.
5. **Sanityzacja pamięci:** wszystkie testy pod Valgrind / ASan (nienegocjowalne).

## Świadome pominięcia w v1 (YAGNI)

- Journaling (dziennik) — zapas w superbloku na później.
- Szyfrowanie — zapas w superbloku na później.
- Wielodostęp/współbieżność — `flock` zabezpiecza przed równoległym użyciem.
- Dowiązania symboliczne, rozszerzone atrybuty (xattr) — poza zakresem v1.
