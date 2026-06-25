#ifndef GH_JOURNAL_H
#define GH_JOURNAL_H
#include "ghostfs.h"
#include "block.h"
int  gh_jrnl_open(struct gh_dev*, const struct gh_superblock*);
void gh_jrnl_op_begin(struct gh_dev*);
void gh_jrnl_op_commit(struct gh_dev*);
void gh_jrnl_op_rollback(struct gh_dev*);
int  gh_jrnl_flush(struct gh_dev*, const struct gh_superblock*);
void gh_jrnl_close(struct gh_dev*);
int  gh_jrnl_recover(struct gh_dev*, const struct gh_superblock*);
#endif
