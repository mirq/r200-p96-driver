/* Exercise the real emitter with the existing CPU-only surface fixture. */
#define main tex_matrix_fixture_main
#include "tex_matrix_check.c"
#undef main

static void SerialSetup(BOOL commit, unsigned unit)
{
    Setup(commit, FALSE, FALSE, -1);
    Record[13] = 0x001fUL;
    if (unit) {
        Record[15] = 3;
        Record[17] = Record[12];
        Record[18] = 0x001fUL;
        Record[19] = RADEON3D_VERTEX_TEXTURE1;
    }
}

int main(void)
{
    static const ULONG serials[] = {0, 1, 5, 0xffff};
    static const ULONG invalid[] = {0x80, 0x1000, 0x2000, 0x4000, 0x8000};
    BOOL commit;
    unsigned unit, i;
    Checks = Failures = 0;
    for (commit = FALSE; commit <= TRUE; ++commit) {
        for (unit = 0; unit < 2; ++unit) {
            unsigned field = unit ? 18 : 13;
            SerialSetup(commit, unit);
            for (i = 0; i < sizeof(serials)/sizeof(serials[0]); ++i) {
                Record[field] = 0x001fUL | (serials[i] << RADEON3D_TEX_CONTENT_SHIFT);
                CHECK(Draw());
                CHECK((unit ? Emitter.Live->Texture1State : Emitter.Live->TextureState)
                      == Record[field]);
            }
            for (i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
                SerialSetup(commit, unit);
                Record[field] = 0x0005001fUL | invalid[i];
                CHECK(!Draw());
                CHECK(Emitter.Count == 0);
            }
            SerialSetup(commit, unit);
            Record[field] = 0x00050000UL | (6UL << RADEON3D_TEX_MIN_SHIFT);
            CHECK(!Draw()); /* serial must not bypass min-filter validation */
        }
    }
    printf("TEX_SERIAL checks=%u failures=%u\n", Checks, Failures);
    return Failures ? 5 : 0;
}
