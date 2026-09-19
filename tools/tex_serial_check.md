# Texture-content serial validation regression

Quake's first gameplay state batch was rejected with fence `80090054`
(service stage 84) on the physical 68060/MPC7410/Prometheus/RV280 system,
at fullscreen 800x600x32. Loading-screen swaps succeeded. The captured
44-dword header contained `H13=0005001f`: legal sampler flags `001f` and
content serial 5 in the documented high 16 bits. The driver validated
against `RADEON3D_TEX_STATE_MASK` alone and rejected the serial.

The fix accepts `TEX_STATE_MASK | TEX_CONTENT_MASK` for both texture units,
without changing the sampler-only mask or any record/structure layout.
Reserved low bits remain rejected; the raw serial is retained in emitted
state for the existing cache-generation handling.

Run on the development host (no Amiga/GPU needed):

```
python3 tools/test_tex_serial.py
python3 tools/test_tex_matrix.py
make
```

The serial test compiles the real emitter under ASan/UBSan using the
existing CPU-only surface fixture. It covers serials 0, 1, 5 and 65535,
unit 0 and unit 1, inline and committed draws, preservation of serials
in live state, reserved low bits, and invalid minification filters.
It also builds the old-mask mutant and requires that version to fail.

Recorded host results: fixed 76 checks / 0 failures; old-mask mutant
76 checks / 24 failures. Existing matrix suite: 1188 checks / 0 failures.
68k driver build and WarpOS PPC emitter compilation succeeded.

Hardware validation of the corrected driver is still pending. Quake also
raised a PPC memory-corruption-during-freeing requester on its automatic
exit; that is a separate unresolved observation, not proven fixed here.
The basic standalone qpresent cases 0-9 passed on the original stack;
new resident-mip-update cases 10/11 target serial-bearing uploads.
