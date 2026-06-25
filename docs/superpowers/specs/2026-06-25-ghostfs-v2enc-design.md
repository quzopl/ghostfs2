# ghostfs v2 — szyfrowanie (reużycie krypto v1): design

**Data:** 2026-06-25 **Część:** v2 follow-up.

## Model
Reużycie krypto v1 (AES-256-XTS, PBKDF2). `dev->cipher` ustawiony → `gh_disk_read/write`
szyfrują/deszyfrują bloki ≥2 (dane+węzły drzew). Superbloki (slot 0,1) pisane SUROWYM
`pwrite` w gh2_super.c → NIESZYFROWANE (trzymają sól+weryfikator). Format z kluczem: sól+klucz+
weryfikator w SB, `GH2_SB_ENCRYPTED`. Mount z kluczem: derive+weryfikuj→`dev->cipher`.

## API
- `gh2_fs_format_key(dev, total, flags, passphrase/*NULL=bez szyfr.*/)`: jeśli passphrase →
  `gh_crypto_random(salt)`, `gh_crypto_derive`→key, `gh_crypto_verifier`→sb.enc_verifier,
  sb.enc_salt=sól, flaga GH2_SB_ENCRYPTED; **ustaw dev->cipher PRZED tworzeniem drzew** (drzewa
  szyfrowane); zapisz SB (raw). KDF iteracje: stała GH2_KDF_ITERS (lub reserved). (gh2_fs_format
  bez klucza = wrapper z NULL.)
- `gh2_fs_mount` / nowy `gh2_fs_mount_key(fs, dev, passphrase)`: po wczytaniu SB jeśli
  GH2_SB_ENCRYPTED → derive(passphrase, sb.enc_salt)→key, verifier==sb.enc_verifier? nie→-EACCES;
  malloc gh_cipher, dev->cipher=c; potem mark-sweep/load (deszyfr.). unmount: wipe+free cipher.
- Fasada `gfs_mount(gfs, path, key)`: v2 gałąź → gh2_fs_mount_key z key; -EACCES gdy zły/brak.
  gfs_unmount wipe. CLI/FUSE: GHOSTFS_KEY/prompt (już jest dla v1; gfs_mount(key) obejmie v2).
  CLI format2: jeśli GHOSTFS_KEY ustawione → gh2_fs_format_key.

## Testy (`tests/test_v2enc.c` + integration_v2.sh)
1. format2 z kluczem → mount z kluczem → round-trip (pliki/katalogi/dane) bajt-exact; fsck==0.
2. **At-rest:** zapisz rozpoznawalny tekst → surowe bajty kontenera (poza SB) NIE zawierają go.
3. **Zły/brak klucza:** mount złym hasłem → -EACCES; bez hasła zaszyfrowanego → -EACCES.
4. Integracja z snapshot/compress/dup: zaszyfrowany + --compress + snapshot → round-trip, izolacja,
   fsck; at-rest brak plaintextu (też skompresowanych — kompresja przed szyfrowaniem).
5. Persystencja remount; crash-sweep z szyfrowaniem fsck==0.
6. CLI: GHOSTFS_KEY format2 + put/get round-trip; zły klucz → błąd. Integracja FUSE: mount
   zaszyfrowanego v2 przez FUSE (prompt/env), round-trip.
7. Regresja: nieszyfrowany v2 + v1 nietknięte.
ASan czyste; cipher wipe przy unmount (brak wycieku klucza).
