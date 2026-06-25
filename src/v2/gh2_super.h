#ifndef GH2_SUPER_H
#define GH2_SUPER_H
#include "v2/gh2_format.h"
#include "block.h"   /* struct gh_dev */

/* crc32 superbloku (bajty przed polem sb_csum) */
uint32_t gh2_sb_csum(const struct gh2_superblock *sb);

/* sformatuj urzadzenie jako ghostfs v2: zapisz SB gen=1 do obu slotow, next_free=GH2_DATA_START */
int gh2_format(struct gh_dev *dev, uint64_t total_blocks, uint32_t flags);

/* zamontuj: wczytaj oba sloty, wybierz najwyzsza wazna generacje (magic+csum); -EINVAL gdy brak */
int gh2_mount(struct gh_dev *dev, struct gh2_superblock *out);

/* atomowy commit superbloku: generation++, slot = generation&1, fsync, read-back verify.
   (v2.5: ostatni krok commitu drzewa.)
   KONTRAKT: zwraca 0 = stan trwale i zweryfikowany. Zwraca -EIO = commit NIEUDANY/rozdarty;
   `sb->generation` przywrocone do ostatniej trwalej generacji (drugi slot wciaz spojny).
   Przy bledzie commitu NIE kontynuuj — albo ponow commit (nadpisze rozdarty slot), albo
   remountuj (gh2_mount) i odrzuc lokalny `sb`. */
int gh2_commit_super(struct gh_dev *dev, struct gh2_superblock *sb);

#endif
