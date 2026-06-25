#include "test.h"
#include "../src/fs.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

static void test_random_ops_fsck_clean(void) {
    char tmp[] = "/tmp/ghost_propXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 2048, 256), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);

    srand(12345);
    char name[64];
    for (int i = 0; i < 200; i++) {
        snprintf(name, sizeof(name), "/f%d", rand() % 40);
        int op = rand() % 3;
        if (op == 0) gh_fs_create(&fs, name, 0644);            /* moze EEXIST */
        else if (op == 1) {
            char data[300]; size_t len = rand() % sizeof(data);
            memset(data, 'x', len);
            gh_fs_write(&fs, name, data, len, 0);              /* moze ENOENT */
        } else gh_fs_unlink(&fs, name);                        /* moze ENOENT */

        int issues = -1;
        CHECK_EQ(gh_fsck(&fs, 0, &issues), 0);
        CHECK_EQ(issues, 0);                                   /* mapa zawsze spojna */
    }
    gh_fs_unmount(&fs); unlink(tmp);
}

static void test_write_equals_read(void) {
    char tmp[] = "/tmp/ghost_prop2XXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 4096, 64), 0);
    struct gh_fs fs; gh_fs_mount(&fs, tmp);
    gh_fs_create(&fs, "/big", 0644);
    for (int t = 0; t < 10; t++) {
        size_t len = (size_t)(rand() % 100000);
        char *src = malloc(len ? len : 1), *dst = malloc(len ? len : 1);
        for (size_t i = 0; i < len; i++) src[i] = (char)rand();
        CHECK_EQ(gh_fs_write(&fs, "/big", src, len, 0), (ssize_t)len);
        CHECK_EQ(gh_fs_read(&fs, "/big", dst, len, 0), (ssize_t)len);
        CHECK_EQ(len ? memcmp(src, dst, len) : 0, 0);
        free(src); free(dst);
    }
    gh_fs_unmount(&fs); unlink(tmp);
}

int main(void) {
    RUN_TEST(test_random_ops_fsck_clean);
    RUN_TEST(test_write_equals_read);
    return TEST_SUMMARY();
}
