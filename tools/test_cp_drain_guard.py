"""CPU guards for the interface-19 fence-coalescing life-cycle hooks.

python3 tools/test_cp_drain_guard.py

The first hardware attempt wedged because the per-dispatch fence had a
second, invisible job: serializing the 2D engine baseline rewrite
(HOST_PATH_CNTL/RB3D_CNTL/DP_DATATYPE in RestoreEngineState) against
in-flight CP fetches. No functional test caught that; these source-structure
guards encode the fix and the related W-80 bookkeeping so a refactor cannot
silently drop them.

Checks (all must be present, in order where ordering matters):

driver `src/radeon_accel.c`
  - SynchronizeEngine's CP branch calls RadeonCpWait(), then
    RadeonCpWaitDrained(), then RestoreEngineState();
  - RadeonPrepare3D's fast path does not restore the baseline itself.

driver `src/radeon_cp.c`
  - RadeonCpState has PendingUnfenced;
  - RadeonCpSubmitStream increments it for addFence==FALSE and clears it for
    a fenced submission;
  - RadeonCpWait clears it on a retired fence;
  - RadeonCpWaitDrained polls RB_RPTR and clears it when drained;
  - CP recovery clears it.

host `minigl_ppc_host.c`
  - FlushCommitAccumulator folds pendingSegMask into its fence record and
    resets pendingUnfenced/fenceGroupCount;
  - the DRAIN branch stamps ringLastGen[slot].
"""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
ACCEL = (ROOT / "src/radeon_accel.c").read_text()
CP = (ROOT / "src/radeon_cp.c").read_text()
HOST = (Path(__file__).resolve().parents[2] /
        "MiniGL_WOS_V19_mglQ3/backend_r200/minigl_ppc_host.c").read_text()

checks = 0
failures = 0


def require(condition, message):
    global checks, failures
    checks += 1
    if not condition:
        failures += 1
        print("FAIL:", message)


def block(text, signature, end="\n}\n"):
    start = text.index(signature)
    stop = text.index(end, start) + len(end)
    return text[start:stop]


# --- Driver: 2D transition drains fence-less submissions first ------------
sync = block(ACCEL, "static BOOL SynchronizeEngine(")
require("RADEON_PENDING_CP" in sync, "SynchronizeEngine still handles CP-pending")
wait_at = sync.find("RadeonCpWait(bi)")
drain_at = sync.find("RadeonCpWaitDrained(bi)")
restore_at = sync.find("RestoreEngineState(bi)")
require(wait_at >= 0, "SynchronizeEngine calls RadeonCpWait")
require(drain_at >= 0, "SynchronizeEngine calls RadeonCpWaitDrained")
require(restore_at >= 0, "SynchronizeEngine calls RestoreEngineState")
require(wait_at < drain_at < restore_at,
        "order must be fence wait -> ring drain -> baseline restore")

prepare = block(ACCEL, "BOOL RadeonPrepare3D(")
require("RestoreEngineState" not in prepare,
        "the CP->CP fast path must not rewrite the 2D baseline")

# --- Driver: PendingUnfenced bookkeeping ----------------------------------
state = block(CP, "struct RadeonCpState {", "\n};")
require("PendingUnfenced" in state, "RadeonCpState tracks PendingUnfenced")

submit = block(CP, "BOOL RadeonCpSubmitStream(")
require("++state->PendingUnfenced" in submit or
        "state->PendingUnfenced +=" in submit,
        "unfenced submissions are counted")
require("state->PendingUnfenced = 0" in submit,
        "a fenced submission clears the unfenced count")

wait = block(CP, "BOOL RadeonCpWait(")
require("state->PendingUnfenced = 0" in wait,
        "a retired fence covers the unfenced prefix")

drained = block(CP, "BOOL RadeonCpWaitDrained(")
require("CpWaitGuiIdle" in drained,
        "the drain wait uses full GUI idle, not RB_RPTR (the CP advances the "
        "read pointer before an indirect fetch completes)")
require("RADEON_CP_RB_RPTR" not in drained,
        "RB_RPTR must not be used as the fetch-complete signal")
require("PendingUnfenced" in drained,
        "the drain wait is a no-op without unfenced submissions and clears on success")

pending = block(CP, "BOOL RadeonCpUnfencedPending(")
require("PendingUnfenced" in pending,
        "the quiescence query reports in-flight fence-less submissions")

service = (ROOT / "src/radeon3d_service.c").read_text()
require(service.count("RadeonCpUnfencedPending(bi)") >= 2,
        "both MMIO guards (dispatch + fence-only kick) are gated on quiescence")
require("RADEON_CP_PACKET0(RADEON_SCRATCH_REG1, 0)" in service,
        "the fence-only kick uses a real register packet, not bare PACKET2s")

recover = block(CP, "BOOL RadeonCpRecover(")
require("state->PendingUnfenced = 0" in recover,
        "CP recovery resets the unfenced count")

# --- Host: group bookkeeping ----------------------------------------------
flush = block(HOST, "static BOOL FlushCommitAccumulator(")
require("folded" in flush and "pendingSegMask" in flush,
        "accumulator fence folds the pending segment mask")
require(flush.count("pendingUnfenced = 0") >= 1 and
        flush.count("fenceGroupCount = 0") >= 1,
        "accumulator fence closes the open run")

drain_branch = HOST[HOST.index("MGLPPC_SEMANTIC_FLAG_DRAIN)"):]
drain_branch = drain_branch[:drain_branch.index("} else if (entry.flags & MGLPPC_SEMANTIC_FLAG_CP_EMIT)")]
require("ringLastGen[entry.slot] = entry.generation" in drain_branch,
        "DRAIN entries advance the slot generation")

print(f"CP_DRAIN_GUARD checks={checks} failures={failures}")
sys.exit(1 if failures else 0)
