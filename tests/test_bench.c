/* Benchmark create+write/odczyt z WLACZONYMI sumami kontrolnymi (J).
 * Format przez gh_format_enc -> GH_SB_CHECKSUMS default-on, wiec kazdy
 * gh_block_write robi read-modify-write bloku sum (CRC32), a gh_block_read
 * weryfikuje CRC. Narzut amortyzowany przez group commit (I: jeden gh_fs_sync
 * na koniec paczki) oraz cache bloku sum (F): bloki sum sa gorace, wiec RMW
 * trafia w cache zamiast w dysk. Asercje poprawnosci (zapis==odczyt) + fsck==0.
 *
 * Zmierzony narzut sum (create+write 300 plikow po group commit) jest
 * zauwazalny vs wariant bez sum (~0,117s w I): kazdy zapis bloku danych pociaga
 * dodatkowy RMW bloku sum przez warstwe txn/journal. Sumy pozostaja domyslnie
 * wlaczone (bezpieczenstwo > przepustowosc); narzut jest deterministyczny i
 * stabilny, a poprawnosc (fsck==0, zapis==odczyt) niezmieniona.
 *
 * K (katalogi haszowane): bench tworzy wszystkie M plikow w JEDNYM katalogu (/fN).
 * Przed K kazdy gh_fs_create robil liniowy gh_dir_add (skan calego katalogu) ->
 * tworzenie M plikow bylo O(M^2): w J ~4,7s/300 plikow. Po K gh_dir_add/lookup
 * jest O(1) srednio (open addressing), wiec faza create+write nie skaluje sie
 * kwadratowo i jest wyraznie szybsza. Wypisywany czas create+write to dowod. */
#include "test.h"
#include "../src/fs.h"
#include "../src/super.h"
#include "../src/ghostfs.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

static double secs(struct timespec a, struct timespec b) {
    return (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9;
}

static void bench_one(const char *label, const char *pass) {
    char tmp[] = "/tmp/ghost_bnXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format_enc(tmp, 16384, 2048, pass), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount_key(&fs, tmp, pass), 0);

    const int M = 300; const size_t SZ = 4000;
    char *data = malloc(SZ); memset(data, 'x', SZ);
    char name[64];
    struct timespec t0, t1, t2, t3;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < M; i++) {
        snprintf(name, sizeof(name), "/f%d", i);
        gh_fs_create(&fs, name, 0644);
        CHECK_EQ(gh_fs_write(&fs, name, data, SZ, 0), (ssize_t)SZ);
    }
    gh_fs_sync(&fs);   /* jeden trwaly flush calej paczki (group commit) zamiast M commitow */
    clock_gettime(CLOCK_MONOTONIC, &t1);
    /* dwukrotny odczyt wszystkich (druga runda = cieply cache) */
    char *rb = malloc(SZ);
    for (int i = 0; i < M; i++) {
        snprintf(name, sizeof(name), "/f%d", i);
        CHECK_EQ(gh_fs_read(&fs, name, rb, SZ, 0), (ssize_t)SZ);
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);
    for (int i = 0; i < M; i++) {
        snprintf(name, sizeof(name), "/f%d", i);
        CHECK_EQ(gh_fs_read(&fs, name, rb, SZ, 0), (ssize_t)SZ);
        CHECK_EQ(memcmp(rb, data, SZ), 0);   /* poprawnosc */
    }
    clock_gettime(CLOCK_MONOTONIC, &t3);
    printf("  [%s] create+write %d plikow: %.3fs | odczyt zimny: %.3fs | odczyt cieply: %.3fs\n",
           label, M, secs(t0, t1), secs(t1, t2), secs(t2, t3));
    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    free(data); free(rb);
    gh_fs_unmount(&fs); unlink(tmp);
}

static void test_bench(void) {
    bench_one("jawny", NULL);
    bench_one("szyfrowany", "haslo-benchmark");
}

int main(void) {
    RUN_TEST(test_bench);
    return TEST_SUMMARY();
}
