/* CPU-only attribution: fake surfaces, no Radeon service or GPU submission. */
#include <devices/timer.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/timer.h>
#include <stdio.h>

#include "radeon3d_emit.h"

#define REPEATS 200UL
#define HEADER_WORDS 110UL
#ifndef EMIT_MEMORY_FLAGS
#define EMIT_MEMORY_FLAGS MEMF_PUBLIC
#endif

struct Device *TimerBase;

struct Fixture {
    struct Radeon3DEmitter Emitter;
    ULONG Words[RADEON3D_MAX_BATCH_DWORDS];
    ULONG Header[HEADER_WORDS];
    ULONG Offset;
    UBYTE Texture[64UL * 64UL * 4UL];
};

static struct Radeon3DEmitSurface *Resolve(void *user, ULONG handle,
                                         struct Radeon3DEmitSurface *surface)
{
    struct Fixture *fixture = user;

    if (!handle || handle > 3UL)
        return NULL;
    surface->CpuAddress = fixture->Texture;
    surface->GpuAddress = handle * 0x00200000UL;
    surface->Width = handle == 3UL ? 64UL : 640UL;
    surface->Height = handle == 3UL ? 64UL : 480UL;
    surface->Format = handle == 2UL ? RADEON3D_FORMAT_R5G6B5PC :
                                     RADEON3D_FORMAT_B8G8R8A8;
    surface->Pitch = surface->Width * (handle == 2UL ? 2UL : 4UL);
    return surface;
}

static void Reset(struct Fixture *fixture)
{
    struct Radeon3DEmitter *emitter = &fixture->Emitter;

    emitter->Words = fixture->Words;
    emitter->Count = 0;
    Radeon3DEmitResetCaptures(emitter);
    emitter->Live->TextureState = 0;
    emitter->Live->Texture1State = 0;
    emitter->GuardClipEmitted = FALSE;
    emitter->MatrixValid = FALSE;
    emitter->TexGenMatrixValid[0] = FALSE;
    emitter->TexGenMatrixValid[1] = FALSE;
    emitter->ModelViewValid = FALSE;
    emitter->InvModelViewValid = FALSE;
    emitter->CommitVbuf = TRUE;
    emitter->CommitSegmentGpuBase = 0x00800000UL;
    emitter->CommitSegmentBytes = 65536UL;
    emitter->CommitVertexOffsets = &fixture->Offset;
    emitter->CommitDrawIndex = 0;
}

static void BuildHeader(struct Fixture *fixture, BOOL textured, ULONG opcode)
{
    ULONG *header = fixture->Header;
    ULONG index;

    for (index = 0; index < HEADER_WORDS; ++index)
        header[index] = 0;
    header[0] = opcode;
    header[1] = textured ? HEADER_WORDS :
                          RADEON3D_EXEC_DRAW_HW_TCL_HEADER_DWORDS;
    header[2] = 1;
    header[3] = 2;
    header[5] = RADEON3D_DRAW_FRAGMENT_STATE | RADEON3D_DRAW_EXTENDED_VERTEX |
                RADEON3D_DRAW_HW_TCL | RADEON3D_DRAW_DEPTH_LESS |
                RADEON3D_DRAW_DEPTH_WRITE;
    header[8] = 640;
    header[9] = 480;
    header[10] = 6;
    for (index = 0; index < 16UL; index += 5UL)
        header[21UL + index] = 0x3f800000UL;
    header[37] = UnsignedFloatBits(320);
    header[38] = UnsignedFloatBits(320);
    header[39] = UnsignedFloatBits(240);
    header[40] = UnsignedFloatBits(240);
    header[41] = 0x3f000000UL;
    header[42] = 0x3f000000UL;
    header[43] = 16UL << RADEON3D_TRANSFORM_POINT_SIZE_SHIFT;
    if (textured) {
        header[4] = 3;
        header[5] |= RADEON3D_DRAW_TEXTURED | RADEON3D_DRAW_NORMALS |
                     RADEON3D_DRAW_TEXGEN;
        header[12] = 63UL | (63UL << 16);
        header[44] = RADEON3D_TEXGEN_MODE_SPHERE_MAP |
                     RADEON3D_TEXGEN_GEN_S | RADEON3D_TEXGEN_GEN_T;
        for (index = 0; index < 16UL; index += 5UL) {
            header[46UL + index] = 0x3f800000UL;
            header[78UL + index] = 0x3f800000UL;
            header[94UL + index] = 0x3f800000UL;
        }
    }
}

static BOOL Run(struct Fixture *fixture, BOOL textured, ULONG draws,
                ULONG opcode)
{
    struct Radeon3DEmitter *emitter = &fixture->Emitter;
    struct EClockVal start, end;
    ULONG primitive, repeat, draw, index, crc = ~0UL;
    ULONG hz;

    BuildHeader(fixture, textured, opcode);
    if (!Radeon3DEmitPrimitiveType(opcode, &primitive))
        return FALSE;
    hz = ReadEClock(&start);
    for (repeat = 0; repeat < REPEATS; ++repeat) {
        Reset(fixture);
        if (!Radeon3DEmitDraw(emitter, fixture->Header, fixture->Header[1],
                              primitive))
            return FALSE;
        for (draw = 1; draw < draws; ++draw)
            if (!Radeon3DEmitVbufDraw(emitter, draw * 512UL, 6UL, primitive))
                return FALSE;
    }
    ReadEClock(&end);
    /* Hash and output stay outside the interval. Process dwords MSB first. */
    for (index = 0; index < emitter->Count; ++index) {
        ULONG byte;
        for (byte = 0; byte < 4UL; ++byte) {
            ULONG bit;
            crc ^= (emitter->Words[index] >> (24UL - byte * 8UL)) & 0xffUL;
            for (bit = 0; bit < 8UL; ++bit)
                crc = (crc >> 1) ^ ((crc & 1UL) ? 0xedb88320UL : 0UL);
        }
    }
    printf("EMIT_RUN texture=%lu primitive=%lu draws=%lu repeats=%lu "
           "ticks=%lu hz=%lu words=%lu crc=%08lx\n",
           (unsigned long)textured, (unsigned long)opcode,
           (unsigned long)draws, (unsigned long)REPEATS,
           (unsigned long)(end.ev_lo - start.ev_lo), (unsigned long)hz,
           (unsigned long)emitter->Count, (unsigned long)~crc);
    return TRUE;
}

int main(void)
{
    struct Fixture *fixture = NULL;
    struct MsgPort *port = NULL;
    struct timerequest *timer = NULL;
    struct MemHeader *memory;
    struct {
        APTR Lower, Upper;
        ULONG Attributes;
        LONG Priority;
        char Name[40];
    } regions[8];
    ULONG textured, count, regionCount = 0;
    static const ULONG draws[] = {1, 5, 20};
    int result = 20;

    fixture = AllocMem(sizeof(*fixture), EMIT_MEMORY_FLAGS | MEMF_CLEAR);
    port = CreateMsgPort();
    if (port)
        timer = (struct timerequest *)CreateIORequest(port, sizeof(*timer));
    if (!fixture || !timer ||
        OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_ECLOCK,
                   (struct IORequest *)timer, 0))
        goto out;
    TimerBase = timer->tr_node.io_Device;
    fixture->Emitter.InterfaceVersion = RADEON3D_IFACE_VERSION;
    fixture->Emitter.Resolve = Resolve;
    fixture->Emitter.ResolveUser = fixture;
    printf("EMIT_CPU attn=%04lx fixture=%08lx bytes=%lu type=%08lx flags=%08lx\n",
           (unsigned long)SysBase->AttnFlags, (unsigned long)fixture,
           (unsigned long)sizeof(*fixture), (unsigned long)TypeOfMem(fixture),
           (unsigned long)EMIT_MEMORY_FLAGS);
    /* Snapshot before any output can reschedule us during the list walk. */
    Forbid();
    for (memory = (struct MemHeader *)SysBase->MemList.lh_Head;
         memory->mh_Node.ln_Succ && regionCount < 8UL;
         memory = (struct MemHeader *)memory->mh_Node.ln_Succ) {
        ULONG index = 0;
        const char *name = (const char *)memory->mh_Node.ln_Name;
        regions[regionCount].Lower = memory->mh_Lower;
        regions[regionCount].Upper = memory->mh_Upper;
        regions[regionCount].Attributes = memory->mh_Attributes;
        regions[regionCount].Priority = memory->mh_Node.ln_Pri;
        if (name)
            while (index + 1UL < sizeof(regions[regionCount].Name) && name[index]) {
                regions[regionCount].Name[index] = name[index];
                ++index;
            }
        regions[regionCount++].Name[index] = 0;
    }
    Permit();
    for (count = 0; count < regionCount; ++count)
        printf("EMIT_MEM lower=%08lx upper=%08lx attr=%04lx pri=%ld name=%s\n",
               (unsigned long)regions[count].Lower,
               (unsigned long)regions[count].Upper,
               (unsigned long)regions[count].Attributes,
               (long)regions[count].Priority, regions[count].Name);
    for (textured = 0; textured < 2UL; ++textured)
        for (count = 0; count < sizeof(draws) / sizeof(draws[0]); ++count) {
            if (!Run(fixture, textured != 0, draws[count],
                     RADEON3D_EXEC_DRAW_TRIANGLES) ||
                !Run(fixture, textured != 0, draws[count],
                     RADEON3D_EXEC_DRAW_TRI_STRIP))
                goto out;
        }
    result = 0;
out:
    printf("EMIT_END result=%d stage=%lu\n", result,
           fixture ? (unsigned long)fixture->Emitter.FailStage : 0UL);
    if (TimerBase)
        CloseDevice((struct IORequest *)timer);
    if (timer)
        DeleteIORequest((struct IORequest *)timer);
    if (port)
        DeleteMsgPort(port);
    if (fixture)
        FreeMem(fixture, sizeof(*fixture));
    return result;
}
