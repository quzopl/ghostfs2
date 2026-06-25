# ghostfs v2 — szyfrowanie: plan

> BRAMKA: at-rest brak plaintextu, zły klucz→-EACCES, round-trip, integracja snapshot/compress/dup,
> regresja (nieszyfrowany v2 + v1). Spec: `...v2enc-design.md`. Reużycie src/crypto.h.

## Task 1: szyfrowanie v2 (format/mount klucz + fasada + CLI/FUSE + testy)
- [ ] `gh2_fs_format_key(dev,total,flags,passphrase)` (gh2_fs.c/.h): passphrase→gh_crypto_random
  salt→sb.enc_salt, gh_crypto_derive→key, gh_crypto_verifier→sb.enc_verifier, flaga GH2_SB_ENCRYPTED;
  malloc gh_cipher, dev->cipher=c PRZED tworzeniem drzew (drzewa szyfrowane); zapisz SB (raw przez
  gh2_format — SB nieszyfrowany). KDF iter: stała GH2_KDF_ITERS (np. jak v1 default). gh2_fs_format
  = wrapper (passphrase NULL). Sprawdź gh_cipher (key[64]) i gh_crypto_* w crypto.h.
- [ ] `gh2_fs_mount_key(fs,dev,passphrase)` (gh2_fs.c/.h): wczytaj SB; jeśli GH2_SB_ENCRYPTED →
  derive(passphrase,sb.enc_salt)→key, verifier==sb.enc_verifier? nie/NULL→-EACCES; malloc cipher,
  dev->cipher; potem mark-sweep/load. gh2_fs_mount = wrapper (NULL; -EACCES gdy zaszyfrowany).
  gh2_fs_unmount: gh_crypto_wipe+free cipher.
- [ ] Fasada gh2_vfs: gfs_mount v2 gałąź → gh2_fs_mount_key(key); gfs format2 v2 → gh2_fs_format_key
  (key z gfs/CLI). gfs_unmount wipe. CLI cli.c: format2 + cli_mount używają GHOSTFS_KEY/prompt (jak
  v1) — przekaż key do gfs_mount/format. (gfs_mount już ma param key.)
- [ ] `tests/test_v2enc.c`: format2 klucz→mount klucz round-trip+fsck; at-rest (surowe bajty bez
  plaintextu); zły/brak klucz→-EACCES; +--compress (at-rest też skompresowane bez plaintextu) +
  snapshot (izolacja, round-trip) + --dup; persystencja remount; crash-sweep szyfr fsck==0;
  cipher wipe (brak wycieku). RUN_TEST.
- [ ] integration_v2.sh: GHOSTFS_KEY format2 + put/get round-trip + zły klucz błąd; mount FUSE
  zaszyfrowanego (env). `make build/test_v2enc && ./build/test_v2enc` 0 failed; ASan; regresja
  `make test` 0 failed; integracja zielona. Commit.
