# Native Texture Visibility Gate

Build on Linux, from the driver checkout:

```sh
make -C /home/mirek/p96-driver CROSS=/opt/amiga/bin/m68k-amigaos- r3dtexupdate
```

Output: `build/r3dtexupdate`, a native 68k Amiga executable (not a host test).
This optional target does not rebuild the driver or any existing probe.
No MiniGL library, PPC initialization, PCI access, MMIO, client hardware packets,
or client-side cache workaround is involved. All surfaces, including the
offscreen BGRA render target, come from `Radeon3DAllocSurface`.

## Coverage

- Both R5G6B5PC (little-endian 565 bytes) and B8G8R8A8 aux textures, 64x64.
- Sixteen writes/samples of one unchanged allocation and unchanged sampler state.
- Sixteen A-write/sample, B-write/sample, A-resample sequences with two live,
  non-overlapping CPU and GPU ranges and distinct handles.
- Eight release/reallocate cycles per format, with B retained and checked for
  corruption. A is last sampled before release and changes pixels on reuse.
  Replacement A uses phase `ROUNDS + i + 1` (17 + i), shared by its write
  and both readback expectations. The offset is not divisible by the four-color
  period: the first replacement changes phase 15 to 17, then each subsequent
  replacement advances by one. Untouched B retains phase `ROUNDS` (16).
  The exact old GPU address must recur; up to eight allocation attempts are
  permitted. A changed handle or CPU pointer alone is not evidence of reuse.
- Each draw checks all 2,304 interior pixels spanning four differently colored
  quadrants, including alpha. Saturated red/green/blue/white avoid 565 rounding
  ambiguities. No clear or unrelated draw is inserted into the in-place sequence.
- Exactly one outstanding Execute fence, waited for at most 1,000 ms, before
  readback or subsequent texture writes/releases. Failed submission/wait stops
  testing; cleanup uses service release/close, whose waits/recovery are bounded
  in the driver. Ctrl-C and a 30-second EClock budget are checked between cases
  and allocation retries. This is not a watchdog for a hung OS/service call;
  final cleanup can extend beyond the workload budget.

A complete PASS requires 176 verified draws, zero pixel failures, and 16 observed
address reuses. Pixel mismatches are logged and testing continues to collect
the remaining cases; service/allocation/budget failures stop the run. Exit codes:
`0` PASS, `5` pixel failure, `10` reuse not observed (incomplete, not PASS), `20`
setup/layout/overlap/service/timeout/abort failure. Earlier failure can prevent
the second format from running; use the stage logs and counts, not absent cases
as evidence of success.

## References And Limits

The local Mesa reference is
`mesa/src/mesa/drivers/dri/r200/r200_texstate.c:974` (`import_tex_obj_state`):
it explicitly avoids value-comparison state suppression to avoid stale texture
caches. `mesa/src/mesa/drivers/dri/r200/r200_tex.c:214` (`r200SetTexFilter`)
defines nearest filtering; this probe uses
the service's semantic nearest/clamp/replace state, not copied register code.
The reference motivates testing visibility, not assuming that any particular
register rewrite or cache flush fixes it.

The active native MiniGL consumer examined is
`/home/mirek/Downloads/PIStorm3D_V19_Classic/dev/MiniGL_Library_Source_Code/library/r200_texture.c`,
especially `SyncTextureDirty`, `TextureState`, and `BeginDirectTexture`, not the
older `minigl_ppc` backend. Its texture-use ordering and disabled aux path are
reasons to gate changes on measurements. This standalone service probe does not
validate MiniGL deferred uploads, PPC cacheability, mipmaps, dual-unit sampling,
streaming commits, or presentation.

Although `include/radeon3d.h` names `RADEON3D_TEX_CONTENT_*`, the current draw
validator rejects bits outside `RADEON3D_TEX_STATE_MASK`. The probe therefore
leaves those bits zero and makes no API/driver changes. An in-place visibility
failure is a gate failure, not permission to enable aux residency or claim that
the serial path works. A missing reuse observation is likewise not a pass.

## Hardware Measurement Protocol (Not Run During Implementation)

With an operator-approved, healthy, matched driver pair already installed,
the eventual Amiga shell test command is simply:

```text
r3dtexupdate
```

No deployment or hardware access is part of the build/host validation step.
Follow `Agents.md` for any later hardware run and recovery. Record driver/probe
hashes, commit and dirty state, CPU, bridge, board ID/revision, VRAM/ROM,
ToolTypes, display mode and cold/warm state, and retain the complete output.

Frequency is the **return value** of `ReadEClock`, not the counter low word.
Full 64-bit timestamps handle low-word rollover. Logs report this frequency,
the service's independent frequency field, and the minimum of 32 empty timing
pairs without subtracting it. Per-draw raw EClock intervals cover CPU texel
generation/stores, Execute, fence wait, and pixel verification separately.
`write_us = ticks_write * 1000000 / eclock_hz`; a no-write case measures only
the empty bracket. Record construction, allocation, printing, and cleanup are
outside these intervals; Execute includes the service's texture write drain.
The write timing is not upload bandwidth, GPU time, or application throughput.
Timer instrumentation and readback perturb cache behavior. No speedup or
performance claim is justified by a cross-build or this visibility test alone.
Any later comparison needs three independent runs under the prescribed
cold-boot protocol, all raw samples plus median, and one variable changed.
