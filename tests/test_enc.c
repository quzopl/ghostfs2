#include "test.h"
#include "../src/fs.h"
#include "../src/super.h"
#include "../src/ghostfs.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

static void test_encrypted_roundtrip(void) {
    char tmp[] = "/tmp/ghost_efsXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format_enc(tmp, 2048, 128, "tajne-haslo"), 0);

    /* zle/brak hasla -> EACCES */
    struct gh_fs fs;
    CHECK_EQ(gh_fs_mount_key(&fs, tmp, "zle"), -EACCES);
    CHECK_EQ(gh_fs_mount_key(&fs, tmp, NULL), -EACCES);

    /* poprawne haslo -> montuje */
    CHECK_EQ(gh_fs_mount_key(&fs, tmp, "tajne-haslo"), 0);
    CHECK_EQ(gh_fs_mkdir(&fs, "/d", 0755), 0);
    CHECK_EQ(gh_fs_create(&fs, "/d/f.txt", 0644), 0);
    const char *secret = "POUFNY-TEKST-1234567890";
    CHECK_EQ(gh_fs_write(&fs, "/d/f.txt", secret, strlen(secret), 0), (ssize_t)strlen(secret));
    char buf[64] = {0};
    CHECK_EQ(gh_fs_read(&fs, "/d/f.txt", buf, sizeof(buf), 0), (ssize_t)strlen(secret));
    CHECK_EQ(memcmp(buf, secret, strlen(secret)), 0);
    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    gh_fs_unmount(&fs);

    /* at-rest: surowe przeszukanie kontenera NIE zawiera jawnego sekretu */
    int rf = open(tmp, O_RDONLY);
    off_t sz = lseek(rf, 0, SEEK_END); lseek(rf, 0, SEEK_SET);
    char *all = malloc((size_t)sz); ssize_t got = read(rf, all, (size_t)sz); close(rf);
    CHECK(got == sz);
    int found = 0;
    for (off_t i = 0; i + (off_t)strlen(secret) <= sz; i++)
        if (memcmp(all + i, secret, strlen(secret)) == 0) { found = 1; break; }
    CHECK_EQ(found, 0);   /* sekretu nie ma jawnie na dysku */
    free(all);

    /* remount z haslem -> trwalosc */
    CHECK_EQ(gh_fs_mount_key(&fs, tmp, "tajne-haslo"), 0);
    memset(buf, 0, sizeof(buf));
    CHECK_EQ(gh_fs_read(&fs, "/d/f.txt", buf, sizeof(buf), 0), (ssize_t)strlen(secret));
    CHECK_EQ(memcmp(buf, secret, strlen(secret)), 0);
    issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    gh_fs_unmount(&fs); unlink(tmp);
}

static void test_plain_still_works(void) {
    char tmp[] = "/tmp/ghost_plXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format_enc(tmp, 1024, 64, NULL), 0);   /* NULL = jawny */
    struct gh_fs fs; CHECK_EQ(gh_fs_mount_key(&fs, tmp, NULL), 0);  /* haslo ignorowane */
    CHECK_EQ(gh_fs_create(&fs, "/x", 0644), 0);
    gh_fs_unmount(&fs); unlink(tmp);
}

static void test_empty_pass_rejected(void) {
    char tmp[] = "/tmp/ghost_epXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format_enc(tmp, 1024, 64, ""), -EINVAL);     /* puste -> blad */
    CHECK_EQ(gh_format_enc(tmp, 1024, 64, NULL), 0);         /* NULL -> jawny OK */
    unlink(tmp);
}

int main(void) {
    RUN_TEST(test_encrypted_roundtrip);
    RUN_TEST(test_plain_still_works);
    RUN_TEST(test_empty_pass_rejected);
    return TEST_SUMMARY();
}
