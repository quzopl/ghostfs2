#ifndef GH2_RCACHE_H
#define GH2_RCACHE_H
/* ghostfs v2 — read-side cache ZWERYFIKOWANYCH wezlow B-drzewa (csum-keyed, self-coherent).
 *
 * PROBLEM: gh2_node_read liczy gh_crc32(4096B) weryfikacji NA KAZDY wezel NA KAZDY descent.
 * Gorne wezly (korzen/wewnetrzne) sa czytane przy KAZDYM odczycie -> ta sama tresc weryfikowana
 * wielokrotnie. Losowy odczyt v2 -18% vs v1 (CRC ~22% czasu).
 *
 * ROZWIAZANIE: maly cache zweryfikowanych wezlow {block -> (buf 4096B, csum)} ograniczony
 * GH2_RCACHE_CAP. gh2_node_read: jesli rcache[block].csum == bptr.csum -> memcpy z cache, RETURN
 * (pomin gh_disk_read + gh_crc32). Inaczej istniejaca sciezka + put zweryfikowanej tresci.
 *
 * SPOJNOSC (csum-keyed, BEZ jawnej inwalidacji): csum dziala jak wersja tresci. Gdy blok zostanie
 * CoW-zwolniony i ponownie uzyty z INNA trescia, nowa tresc ma INNY csum; caller ma bptr.csum
 * dopasowany do NOWEJ tresci -> rcache[block].csum(stary) != bptr.csum(nowy) -> MISS -> re-read.
 * Nieaktualny wpis nieszkodliwy (slot do eksmisji). Self-healing: cache z DOBRA trescia (csum==bptr)
 * serwuje dobra mimo bitrotu na dysku.
 *
 * Per-montaz (gh_dev->v2_rcache). NULL gdy nieuzywane (v1). READ-ONLY: nie mutuje FS na dysku.
 * Eksmisja: CLOCK (second-chance) na tablicy o stalym rozmiarze. */
#include <stdint.h>

struct gh2_rcache;

/* pojemnosc (wpisy). 2048 * 4096B ~= 8 MB tresci + metadane. */
#define GH2_RCACHE_CAP 2048u

/* test-only licznik weryfikacji CRC WEZLOW (gh_crc32 w gh2_node_read). Zerowany przez test.
 * Inkrementowany TYLKO przy faktycznym gh_crc32 wezla (MISS); HIT z rcache go NIE rusza. */
extern unsigned long gh2_node_crc_verify_count;

/* utworz cache. Zwraca wskaznik lub NULL przy OOM. */
struct gh2_rcache *gh2_rcache_create(void);
/* zwolnij WSZYSTKIE bufory + strukture. */
void gh2_rcache_destroy(struct gh2_rcache *c);

/* HIT: block w cache I zapisany csum == csum -> kopiuj 4096B do buf_out, zwroc 1. Inaczej 0. */
int gh2_rcache_get(struct gh2_rcache *c, uint64_t block, uint32_t csum, void *buf_out);
/* wstaw/zaktualizuj block -> (kopia buf 4096B, csum). Eksmisja CLOCK gdy pelny. */
void gh2_rcache_put(struct gh2_rcache *c, uint64_t block, uint32_t csum, const void *buf);

/* liczba zajetych wpisow (diagnostyka/testy). */
uint64_t gh2_rcache_count(const struct gh2_rcache *c);

#endif
