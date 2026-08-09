# Validated libnix Runtime Members

The pinned Amiga GCC 6.5 release image reproduces the hardware-tested card
driver byte-for-byte. Its `libnix.a` differs from the runtime used to build the
validated `p96screen` utility in three small stdio members. The release workflow
replaces only these members and verifies every object, the reconstructed
archive, and all final artifacts by SHA-256.

The objects were extracted from:

```text
/opt/amiga/m68k-amigaos/libnix/lib/libm020/libnix.a
```

Their corresponding sources are from `AmigaPorts/libnix` commit
`e1ba6cfae3d39c560f975bc2e44d9e8e99b42868`:

- `sources/nix/stdio/__bufsiz.c`
- `sources/nix/stdio/fopen.c`
- `sources/nix/stdio/setbuf.c`

The [libnix README](https://github.com/AmigaPorts/libnix) declares the library
Public Domain. The object files are base64-encoded so this small runtime
snapshot remains reviewable and portable through ordinary Git checkouts.

Validated archive:

```text
SHA256  1ffa3b50cb5d81b3022aa20f0543d4632d3705e49d298dc743c8469796fdce1d
Size    75828 bytes
```
