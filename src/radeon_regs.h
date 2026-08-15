#ifndef RADEON9200_REGS_H
#define RADEON9200_REGS_H

/* PCI BAR layout for the pre-AVIVO Radeon family. */
#define RADEON_BAR_FRAMEBUFFER 0
#define RADEON_BAR_IO          1
#define RADEON_BAR_MMIO        2

#define RADEON_MMIO_MIN_SIZE       0x00010000UL
#define RADEON_FRAMEBUFFER_MIN_SIZE 0x00400000UL

/* Initial register subset from the Mesa R200 and NetBSD radeonfb headers. */
#define RADEON_MM_INDEX             0x0000UL
#define RADEON_MM_DATA              0x0004UL
#define RADEON_CLOCK_CNTL_INDEX     0x0008UL
#define RADEON_CLOCK_CNTL_DATA      0x000cUL
#define RADEON_BIOS_0_SCRATCH       0x0010UL
#define RADEON_BUS_CNTL             0x0030UL
#define RADEON_BUS_MASTER_DIS       (1UL << 6)
#define RADEON_BUS_BIOS_DIS_ROM     (1UL << 12)
#define RADEON_GEN_INT_CNTL         0x0040UL
#define RADEON_CRTC_GEN_CNTL        0x0050UL
#define RADEON_CRTC_CUR_EN          (1UL << 16)
#define RADEON_CRTC_CUR_MODE_MASK   (7UL << 20)
#define RADEON_CRTC_CUR_MODE_24BPP  (2UL << 20)
#define RADEON_CRTC_DBL_SCAN_EN     (1UL << 0)
#define RADEON_CRTC_INTERLACE_EN    (1UL << 1)
#define RADEON_CRTC_PIX_WIDTH_SHIFT 8
#define RADEON_CRTC_PIX_WIDTH_MASK  (0x0fUL << 8)
#define RADEON_CRTC_PIX_WIDTH_8BPP  (2UL << 8)
#define RADEON_CRTC_PIX_WIDTH_16BPP (4UL << 8)
#define RADEON_CRTC_PIX_WIDTH_32BPP (6UL << 8)
#define RADEON_CRTC_EXT_DISP_EN     (1UL << 24)
#define RADEON_CRTC_EN              (1UL << 25)
#define RADEON_CRTC_DISP_REQ_EN_B   (1UL << 26)
#define RADEON_CRTC_EXT_CNTL        0x0054UL
#define RADEON_VGA_ATI_LINEAR       (1UL << 3)
#define RADEON_XCRT_CNT_EN          (1UL << 6)
#define RADEON_CRTC_HSYNC_DIS       (1UL << 8)
#define RADEON_CRTC_VSYNC_DIS       (1UL << 9)
#define RADEON_CRTC_DISPLAY_DIS     (1UL << 10)
#define RADEON_CRTC_SYNC_TRISTAT    (1UL << 11)
#define RADEON_CRTC_CRT_ON          (1UL << 15)
#define RADEON_DAC_CNTL             0x0058UL
#define RADEON_DAC_RANGE_CNTL_MASK  0x00000003UL
#define RADEON_DAC_BLANKING         (1UL << 2)
#define RADEON_DAC_8BIT_EN          (1UL << 8)
#define RADEON_DAC_MASK_ALL         (0xffUL << 24)
#define RADEON_CRTC_STATUS          0x005cUL
#define RADEON_CRTC_VBLANK_SAVE     (1UL << 1)
#define RADEON_DAC_CNTL2            0x007cUL
#define RADEON_DAC2_DAC_CLK_SEL     (1UL << 0)
#define RADEON_DAC2_PALETTE_ACC_CTL (1UL << 5)
#define RADEON_I2C_CNTL_1           0x0094UL
#define RADEON_PALETTE_INDEX        0x00b0UL
#define RADEON_PALETTE_DATA         0x00b4UL
#define RADEON_PALETTE_30_DATA      0x00b8UL
#define RADEON_RBBM_SOFT_RESET      0x00f0UL
#define RADEON_SOFT_RESET_CP        (1UL << 0)
#define RADEON_SOFT_RESET_HI        (1UL << 1)
#define RADEON_SOFT_RESET_SE        (1UL << 2)
#define RADEON_SOFT_RESET_RE        (1UL << 3)
#define RADEON_SOFT_RESET_PP        (1UL << 4)
#define RADEON_SOFT_RESET_E2        (1UL << 5)
#define RADEON_SOFT_RESET_RB        (1UL << 6)
#define RADEON_CONFIG_MEMSIZE       0x00f8UL
#define RADEON_CONFIG_APER_0_BASE   0x0100UL
#define RADEON_CONFIG_APER_SIZE     0x0108UL
#define RADEON_HOST_PATH_CNTL       0x0130UL
#define RADEON_HDP_SOFT_RESET       (1UL << 26)
#define RADEON_HDP_READ_BUFFER_INVALIDATE (1UL << 27)
#define RADEON_MC_FB_LOCATION       0x0148UL
#define RADEON_MC_AGP_LOCATION      0x014cUL
#define RADEON_MEM_STR_CNTL         0x0150UL
#define RADEON_MEM_PWRUP_COMPLETE   0x00000003UL
#define RADEON_MC_IDLE              (1UL << 2)
#define RADEON_MEM_SDRAM_MODE_REG   0x0158UL
#define RADEON_SDRAM_MODE_MASK      0xffff0000UL
#define RADEON_B3MEM_RESET_MASK     0x6fffffffUL
#define RADEON_SEPROM_CNTL1         0x01c0UL
#define RADEON_SCK_PRESCALE_MASK    (0xffUL << 24)
#define RADEON_SCK_PRESCALE_SHIFT   24

#define RADEON_CRTC_H_TOTAL_DISP      0x0200UL
#define RADEON_CRTC_H_SYNC_STRT_WID   0x0204UL
#define RADEON_CRTC_H_SYNC_POL        (1UL << 23)
#define RADEON_CRTC_V_TOTAL_DISP      0x0208UL
#define RADEON_CRTC_V_SYNC_STRT_WID   0x020cUL
#define RADEON_CRTC_V_SYNC_POL        (1UL << 23)
#define RADEON_CRTC_VLINE_CRNT_VLINE  0x0210UL
#define RADEON_CRTC_CRNT_VLINE_MASK   (0x07ffUL << 16)
#define RADEON_CRTC_OFFSET            0x0224UL
#define RADEON_CRTC_OFFSET_CNTL       0x0228UL
#define RADEON_CRTC_PITCH             0x022cUL
#define RADEON_OVR_CLR                0x0230UL
#define RADEON_OVR_WID_LEFT_RIGHT     0x0234UL
#define RADEON_OVR_WID_TOP_BOTTOM     0x0238UL
#define RADEON_DISPLAY_BASE_ADDR      0x023cUL
#define RADEON_DAC_EXT_CNTL           0x0280UL
#define RADEON_DAC_FORCE_BLANK_OFF_EN (1UL << 4)
#define RADEON_DAC_FORCE_DATA_EN      (1UL << 5)
#define RADEON_DISPLAY2_BASE_ADDR     0x033cUL
#define RADEON_CRTC2_GEN_CNTL         0x03f8UL
#define RADEON_CRTC2_DISP_DIS         (1UL << 23)
#define RADEON_CRTC2_EN               (1UL << 25)
#define RADEON_CRTC2_DISP_REQ_EN_B    (1UL << 26)
#define RADEON_OV0_SCALE_CNTL          0x0420UL
#define RADEON_OV0_BASE_ADDR          0x043cUL
#define RADEON_SUBPIC_CNTL             0x0540UL
#define RADEON_CAP0_TRIG_CNTL          0x0950UL
#define RADEON_CAP1_TRIG_CNTL          0x09c0UL

#define RADEON_SURFACE_CNTL                 0x0b00UL
#define RADEON_NONSURF_AP0_SWP_16BPP       (1UL << 20)
#define RADEON_NONSURF_AP0_SWP_32BPP       (1UL << 21)
#define RADEON_SURFACE0_INFO                0x0b0cUL
#define RADEON_SURFACE1_INFO                0x0b1cUL
#define RADEON_SURFACE2_INFO                0x0b2cUL
#define RADEON_SURFACE3_INFO                0x0b3cUL
#define RADEON_SURFACE4_INFO                0x0b4cUL
#define RADEON_SURFACE5_INFO                0x0b5cUL
#define RADEON_SURFACE6_INFO                0x0b6cUL
#define RADEON_SURFACE7_INFO                0x0b7cUL
#define RADEON_VIPH_CONTROL                 0x0c40UL
#define RADEON_VIPH_EN                      (1UL << 21)
#define RADEON_DISP_MERGE_CNTL              0x0d60UL
#define RADEON_DISP_RGB_OFFSET_EN           (1UL << 8)
#define RADEON_DISP_OUTPUT_CNTL             0x0d64UL
#define RADEON_DISP_DAC_SOURCE_MASK         0x00000003UL

/* Direct-MMIO 2D engine registers. */
#define RADEON_RBBM_STATUS                  0x0e40UL
#define RADEON_RBBM_FIFOCNT_MASK            0x0000007fUL
#define RADEON_CP_CMDSTRM_BUSY               (1UL << 16)
#define RADEON_RBBM_ACTIVE                  (1UL << 31)
#define RADEON_DST_OFFSET                   0x1404UL
#define RADEON_DST_PITCH                    0x1408UL
#define RADEON_SRC_PITCH_OFFSET             0x1428UL
#define RADEON_DST_PITCH_OFFSET             0x142cUL
#define RADEON_SRC_Y_X                      0x1434UL
#define RADEON_DST_Y_X                      0x1438UL
#define RADEON_DST_HEIGHT_WIDTH             0x143cUL
#define RADEON_DP_GUI_MASTER_CNTL           0x146cUL
#define RADEON_BRUSH_Y_X                    0x1474UL
#define RADEON_DP_BRUSH_BKGD_CLR            0x1478UL
#define RADEON_DP_BRUSH_FRGD_CLR            0x147cUL
#define RADEON_BRUSH_DATA0                  0x1480UL
#define RADEON_BRUSH_DATA1                  0x1484UL
#define RADEON_DP_SRC_FRGD_CLR              0x15d8UL
#define RADEON_DP_SRC_BKGD_CLR              0x15dcUL
#define RADEON_DST_WIDTH_HEIGHT             0x1598UL
#define RADEON_DP_CNTL                      0x16c0UL
#define RADEON_DST_X_LEFT_TO_RIGHT          (1UL << 0)
#define RADEON_DST_Y_TOP_TO_BOTTOM          (1UL << 1)
#define RADEON_DP_DATATYPE                  0x16c4UL
#define RADEON_HOST_BIG_ENDIAN_EN           (1UL << 29)
#define RADEON_DP_WRITE_MASK                0x16ccUL
#define RADEON_DEFAULT_PITCH_OFFSET         0x16e0UL
#define RADEON_DEFAULT_SC_BOTTOM_RIGHT      0x16e8UL
#define RADEON_SC_TOP_LEFT                  0x16ecUL
#define RADEON_SC_BOTTOM_RIGHT              0x16f0UL
#define RADEON_DSTCACHE_CTLSTAT             0x1714UL
#define RADEON_RB2D_DC_FLUSH_ALL            0x00000005UL
#define RADEON_RB2D_DC_BUSY                 (1UL << 31)
#define RADEON_WAIT_UNTIL                   0x1720UL
#define RADEON_WAIT_DMA_GUI_IDLE            (1UL << 9)
#define RADEON_WAIT_2D_IDLECLEAN            (1UL << 16)
#define RADEON_WAIT_3D_IDLECLEAN            (1UL << 17)
#define RADEON_WAIT_HOST_IDLECLEAN          (1UL << 18)
#define RADEON_ISYNC_CNTL                   0x1724UL
#define RADEON_ISYNC_ANY2D_IDLE3D           (1UL << 0)
#define RADEON_ISYNC_ANY3D_IDLE2D           (1UL << 1)
#define RADEON_ISYNC_WAIT_IDLEGUI           (1UL << 4)
#define RADEON_ISYNC_CPSCRATCH_IDLEGUI      (1UL << 5)
#define RADEON_SCRATCH_REG0                 0x15e0UL
#define RADEON_SCRATCH_REG1                 0x15e4UL
#define RADEON_RBBM_GUICNTL                 0x172cUL
#define RADEON_HOST_DATA_SWAP_NONE          0UL
#define RADEON_DST_LINE_START               0x1600UL
#define RADEON_DST_LINE_END                 0x1604UL
#define RADEON_DST_LINE_PATCOUNT            0x1608UL
#define RADEON_HOST_DATA0                   0x17c0UL
#define RADEON_HOST_DATA7                   0x17dcUL
#define RADEON_HOST_DATA_LAST               0x17e0UL

#define RADEON_CUR_OFFSET                   0x0260UL
#define RADEON_CUR_HORZ_VERT_POSN           0x0264UL
#define RADEON_CUR_HORZ_VERT_OFF            0x0268UL
#define RADEON_CUR_CLR0                     0x026cUL
#define RADEON_CUR_CLR1                     0x0270UL
#define RADEON_CUR_LOCK                     (1UL << 31)
#define RADEON_RB3D_CNTL                    0x1c3cUL
#define R200_PP_CNTL                        0x1c38UL
#define R200_TEX_BLEND_0_ENABLE             0x00001000UL
#define R200_RB3D_BLENDCNTL                 0x1c20UL
#define R200_PP_MISC                        0x1c14UL
#define R200_ALPHA_TEST_NEVER               (0UL << 8)
#define R200_ALPHA_TEST_LESS                (1UL << 8)
#define R200_ALPHA_TEST_LEQUAL              (2UL << 8)
#define R200_ALPHA_TEST_EQUAL               (3UL << 8)
#define R200_ALPHA_TEST_GEQUAL              (4UL << 8)
#define R200_ALPHA_TEST_GREATER             (5UL << 8)
#define R200_ALPHA_TEST_NOTEQUAL            (6UL << 8)
#define R200_ALPHA_TEST_ALWAYS              (7UL << 8)
#define R200_SRC_BLEND_GL_ONE               (33UL << 16)
#define R200_DST_BLEND_GL_ZERO              (32UL << 24)
#define R200_SRC_BLEND_GL_SRC_ALPHA         (38UL << 16)
#define R200_DST_BLEND_GL_ONE_MINUS_SRC_ALPHA (39UL << 24)
#define R200_SRC_BLEND_GL_ZERO              (32UL << 16)
#define R200_SRC_BLEND_GL_SRC_COLOR         (34UL << 16)
#define R200_SRC_BLEND_GL_ONE_MINUS_SRC_COLOR (35UL << 16)
#define R200_SRC_BLEND_GL_DST_COLOR         (36UL << 16)
#define R200_SRC_BLEND_GL_ONE_MINUS_DST_COLOR (37UL << 16)
#define R200_SRC_BLEND_GL_DST_ALPHA         (40UL << 16)
#define R200_SRC_BLEND_GL_ONE_MINUS_DST_ALPHA (41UL << 16)
#define R200_SRC_BLEND_GL_SRC_ALPHA_SATURATE (42UL << 16)
#define R200_DST_BLEND_GL_ONE               (33UL << 24)
#define R200_DST_BLEND_GL_SRC_COLOR         (34UL << 24)
#define R200_DST_BLEND_GL_ONE_MINUS_SRC_COLOR (35UL << 24)
#define R200_DST_BLEND_GL_DST_COLOR         (36UL << 24)
#define R200_DST_BLEND_GL_ONE_MINUS_DST_COLOR (37UL << 24)
#define R200_DST_BLEND_GL_SRC_ALPHA         (38UL << 24)
#define R200_DST_BLEND_GL_DST_ALPHA         (40UL << 24)
#define R200_DST_BLEND_GL_ONE_MINUS_DST_ALPHA (41UL << 24)
#define R200_RB3D_DEPTHOFFSET               0x1c24UL
#define R200_RB3D_DEPTHPITCH                0x1c28UL
#define R200_DEPTHPITCH_MASK                0x00001ff8UL
#define R200_RB3D_ZSTENCILCNTL              0x1c2cUL
#define R200_DEPTH_FORMAT_16BIT_INT_Z        0UL
#define R200_Z_TEST_NEVER                    (0UL << 4)
#define R200_Z_TEST_LESS                     (1UL << 4)
#define R200_Z_TEST_LEQUAL                   (2UL << 4)
#define R200_Z_TEST_EQUAL                    (3UL << 4)
#define R200_Z_TEST_GEQUAL                   (4UL << 4)
#define R200_Z_TEST_GREATER                  (5UL << 4)
#define R200_Z_TEST_NOTEQUAL                 (6UL << 4)
#define R200_Z_TEST_ALWAYS                   (7UL << 4)
#define R200_STENCIL_TEST_ALWAYS             (7UL << 12)
#define R200_Z_WRITE_ENABLE                  (1UL << 30)
#define R200_RB3D_COLOROFFSET               0x1c40UL
#define R200_COLOROFFSET_MASK                0xfffffff0UL
#define R200_RE_WIDTH_HEIGHT                0x1c44UL
#define R200_RB3D_COLORPITCH                0x1c48UL
#define R200_COLORPITCH_MASK                 0x00001ff8UL
#define R200_SE_CNTL                        0x1c4cUL
#define R200_RE_CNTL                        0x1c50UL
#define R200_RB3D_PLANEMASK                 0x1d84UL
#define R200_COLOR_FORMAT_RGB565            (4UL << 10)
#define R200_ALPHA_BLEND_ENABLE              (1UL << 0)
#define R200_Z_ENABLE                        (1UL << 8)
#define R200_SE_VAP_CNTL                    0x2080UL
#define R200_VAP_FORCE_W_TO_ONE             0x00010000UL
#define R200_VAP_VF_MAX_VTX_NUM_SHIFT       18
#define R200_SE_VTX_FMT_0                   0x2088UL
#define R200_VTX_PK_RGBA                    1UL
#define R200_VTX_Z0                         (1UL << 0)
#define R200_VTX_DISCRETE_FOG               (1UL << 8)
#define R200_VTX_COLOR_0_SHIFT              11
#define R200_SE_VTX_FMT_1                   0x208cUL
#define R200_VTX_TEX0_COMP_CNT_SHIFT        0
#define R200_VTX_TEX1_COMP_CNT_SHIFT        3
#define R200_SE_VTE_CNTL                    0x20b0UL
#define R200_SE_VAP_CNTL_STATUS             0x2140UL
#define R200_SE_VTX_STATE_CNTL              0x2180UL
#define R200_RE_TOP_LEFT                    0x26c0UL
#define R200_RE_AUX_SCISSOR_CNTL            0x26f0UL
#define R200_SCISSOR_ENABLE                 0x00000002UL
#define R200_PP_TXFILTER_0                  0x2c00UL
#define R200_PP_TXFILTER_1                  0x2c20UL
#define R200_MAG_FILTER_LINEAR              (1UL << 0)
#define R200_MIN_FILTER_LINEAR              (1UL << 1)
#define R200_MIN_FILTER_NEAREST_MIP_NEAREST (2UL << 1)
#define R200_MIN_FILTER_LINEAR_MIP_NEAREST  (3UL << 1)
#define R200_MIN_FILTER_NEAREST_MIP_LINEAR  (6UL << 1)
#define R200_MIN_FILTER_LINEAR_MIP_LINEAR   (7UL << 1)
#define R200_MAX_MIP_LEVEL_SHIFT            16
#define R200_CLAMP_S_CLAMP_LAST             (2UL << 23)
#define R200_CLAMP_T_CLAMP_LAST             (2UL << 27)
#define R200_PP_TXFORMAT_0                  0x2c04UL
#define R200_PP_TXFORMAT_1                  0x2c24UL
#define R200_TXFORMAT_RGB565                4UL
#define R200_TXFORMAT_ARGB8888              6UL
#define R200_TXFORMAT_ALPHA_IN_MAP          (1UL << 6)
#define R200_TXFORMAT_NON_POWER2            (1UL << 7)
#define R200_TXFORMAT_WIDTH_SHIFT           8
#define R200_TXFORMAT_HEIGHT_SHIFT          12
#define R200_TXFORMAT_ST_ROUTE_STQ1          (1UL << 24)
#define R200_PP_TXFORMAT_X_0                0x2c08UL
#define R200_PP_TXFORMAT_X_1                0x2c28UL
#define R200_PP_TXSIZE_0                    0x2c0cUL
#define R200_PP_TXSIZE_1                    0x2c2cUL
#define R200_PP_TXPITCH_0                   0x2c10UL
#define R200_PP_TXPITCH_1                   0x2c30UL
#define R200_TXPITCH_MASK                   0x00001fe0UL
#define R200_PP_TXMULTI_CTL_0               0x2c1cUL
#define R200_PP_TXMULTI_CTL_1               0x2c3cUL
#define R200_PP_CNTL_X                      0x2cc4UL
#define R200_PP_TXOFFSET_0                  0x2d00UL
#define R200_PP_TXOFFSET_1                  0x2d18UL
#define R200_TXO_OFFSET_MASK                0xffffffe0UL
#define R200_TEX_0_ENABLE                   0x00000010UL
#define R200_TEX_1_ENABLE                   0x00000020UL
#define R200_TEX_BLEND_1_ENABLE             0x00002000UL
#define R200_FOG_ENABLE                     0x00400000UL
#define R200_ALPHA_TEST_ENABLE              0x00800000UL
#define R200_PP_TXCBLEND_0                  0x2f00UL
#define R200_PP_TXCBLEND2_0                 0x2f04UL
#define R200_PP_TXABLEND_0                  0x2f08UL
#define R200_PP_TXABLEND2_0                 0x2f0cUL
#define R200_PP_TXCBLEND_1                  0x2f10UL
#define R200_PP_TXCBLEND2_1                 0x2f14UL
#define R200_PP_TXABLEND_1                  0x2f18UL
#define R200_PP_TXABLEND2_1                 0x2f1cUL
#define R200_TXC_ARG_C_DIFFUSE_COLOR        (4UL << 10)
#define R200_TXA_ARG_C_DIFFUSE_ALPHA        (4UL << 10)
#define R200_TXC_ARG_C_R0_COLOR             (10UL << 10)
#define R200_TXA_ARG_C_R0_ALPHA             (10UL << 10)
#define R200_TXC_ARG_A_R0_COLOR             10UL
#define R200_TXC_ARG_B_DIFFUSE_COLOR        (4UL << 5)
#define R200_TXA_ARG_A_R0_ALPHA             10UL
#define R200_TXA_ARG_B_DIFFUSE_ALPHA        (4UL << 5)
#define R200_TXC_ARG_A_R1_COLOR             12UL
#define R200_TXC_ARG_B_R0_COLOR             (10UL << 5)
#define R200_TXC_ARG_C_R1_COLOR             (12UL << 10)
#define R200_TXA_ARG_A_R1_ALPHA             12UL
#define R200_TXA_ARG_B_R0_ALPHA             (10UL << 5)
#define R200_TXA_ARG_C_R1_ALPHA             (12UL << 10)
#define R200_TXC_CLAMP_0_1                  (1UL << 12)
#define R200_TXA_CLAMP_0_1                  (1UL << 12)
#define R200_TXC_OUTPUT_REG_R0              (1UL << 16)
#define R200_TXA_OUTPUT_REG_R0              (1UL << 16)
#define R200_PP_FOG_COLOR                   0x1c18UL
#define R200_FOG_USE_VTX_FOG                (4UL << 25)
#define R200_FOG_SHADE_GOURAUD              (2UL << 14)
#define R200_DISC_FOG_SHADE_GOURAUD         (2UL << 24)
#define R200_BFACE_SOLID                    (3UL << 1)
#define R200_FFACE_SOLID                    (3UL << 3)
#define R200_DIFFUSE_SHADE_GOURAUD          (2UL << 8)
#define R200_VTX_PIX_CENTER_OGL             (1UL << 27)
#define R200_ROUND_MODE_ROUND               (1UL << 28)
#define R200_ROUND_PREC_4TH_PIX             (2UL << 30)
#define RADEON_RB2D_DSTCACHE_MODE           0x3428UL

/* R200 command processor registers and packet encodings. */
#define RADEON_CP_RB_BASE                   0x0700UL
#define RADEON_CP_RB_CNTL                   0x0704UL
#define RADEON_RB_NO_UPDATE                 (1UL << 27)
#define RADEON_RB_RPTR_WR_ENA               (1UL << 31)
#define RADEON_CP_RB_RPTR_ADDR              0x070cUL
#define RADEON_CP_RB_RPTR                   0x0710UL
#define RADEON_CP_RB_WPTR                   0x0714UL
#define RADEON_CP_RB_WPTR_DELAY             0x0718UL
#define RADEON_CP_RB_RPTR_WR                0x071cUL
#define RADEON_CP_CSQ_CNTL                  0x0740UL
#define RADEON_CP_CSQ_MODE                  0x0744UL
#define RADEON_CSQ_PRIBM_INDBM              (4UL << 28)
#define RADEON_SCRATCH_UMSK                 0x0770UL
#define RADEON_SCRATCH_ADDR                 0x0774UL
#define RADEON_CP_ME_RAM_ADDR               0x07d4UL
#define RADEON_CP_ME_RAM_DATAH              0x07dcUL
#define RADEON_CP_ME_RAM_DATAL              0x07e0UL

#define RADEON_CP_PACKET0(reg, count) \
    (((ULONG)(count) << 16) | ((ULONG)(reg) >> 2))
#define RADEON_CP_PACKET2                   0x80000000UL
#define RADEON_CP_PACKET3(packet, count) \
    (0xc0000000UL | (ULONG)(packet) | ((ULONG)(count) << 16))
#define R200_CP_CMD_3D_DRAW_IMMD_2          0x00003500UL
#define R200_CP_VC_CNTL_PRIM_TYPE_TRI_LIST  0x00000004UL
#define R200_CP_VC_CNTL_PRIM_WALK_RING      0x00000030UL
#define RADEON_CNTL_PAINT_MULTI             0x00009a00UL

#define RADEON_GMC_SRC_PITCH_OFFSET_CNTL    (1UL << 0)
#define RADEON_GMC_DST_PITCH_OFFSET_CNTL    (1UL << 1)
#define RADEON_GMC_DST_CLIPPING             (1UL << 3)
#define RADEON_GMC_BRUSH_8X8_MONO_FG_BG     (0UL << 4)
#define RADEON_GMC_BRUSH_8X8_MONO_FG_LA     (1UL << 4)
#define RADEON_GMC_BRUSH_SOLID_COLOR        (13UL << 4)
#define RADEON_GMC_BRUSH_NONE               (15UL << 4)
#define RADEON_GMC_DST_8BPP_CI              (2UL << 8)
#define RADEON_GMC_DST_16BPP                (4UL << 8)
#define RADEON_GMC_DST_32BPP                (6UL << 8)
#define RADEON_GMC_SRC_DATATYPE_MONO_FG_BG  (0UL << 12)
#define RADEON_GMC_SRC_DATATYPE_MONO_FG_LA  (1UL << 12)
#define RADEON_GMC_SRC_DATATYPE_COLOR       (3UL << 12)
#define RADEON_GMC_BYTE_MSB_TO_LSB          (0UL << 14)
#define RADEON_GMC_BYTE_LSB_TO_MSB          (1UL << 14)
#define RADEON_ROP3_Dn                      0x00550000UL
#define RADEON_ROP3_S_XOR_D                 0x00660000UL
#define RADEON_ROP3_S                       0x00cc0000UL
#define RADEON_ROP3_P                       0x00f00000UL
#define RADEON_DP_SRC_SOURCE_MEMORY         (2UL << 24)
#define RADEON_DP_SRC_SOURCE_HOST_DATA      (3UL << 24)
#define RADEON_GMC_CLR_CMP_CNTL_DIS         (1UL << 28)

#define RADEON_SCISSOR_MAX                  0x1fff1fffUL

/* Indexed PLL registers. */
#define RADEON_CLK_PWRMGT_CNTL              0x0014U
#define RADEON_MC_BUSY                       (1UL << 16)
#define RADEON_DLL_READY                     (1UL << 19)
#define RADEON_CLK_PWRMGT_CNTL24             (1UL << 24)
#define RADEON_MCLK_CNTL                     0x0012U
#define RADEON_SET_ALL_SRCS_TO_PCI           0x00001111UL
#define RADEON_PPLL_CNTL                     0x0002U
#define RADEON_PPLL_RESET                    (1UL << 0)
#define RADEON_PPLL_SLEEP                    (1UL << 1)
#define RADEON_PPLL_REFCLK_SEL               (1UL << 4)
#define RADEON_PPLL_FBCLK_SEL                (1UL << 5)
#define RADEON_PPLL_ATOMIC_UPDATE_EN         (1UL << 16)
#define RADEON_PPLL_VGA_ATOMIC_UPDATE_EN     (1UL << 17)
#define RADEON_PPLL_REF_DIV                  0x0003U
#define RADEON_PPLL_REF_DIV_MASK             0x000003ffUL
#define RADEON_PPLL_ATOMIC_UPDATE_R          (1UL << 15)
#define RADEON_PPLL_ATOMIC_UPDATE_W          (1UL << 15)
#define RADEON_PPLL_DIV_0                    0x0004U
#define RADEON_PPLL_FB3_DIV_MASK             0x000007ffUL
#define RADEON_PPLL_POST3_DIV_MASK           0x00070000UL
#define RADEON_VCLK_ECP_CNTL                 0x0008U
#define RADEON_VCLK_SRC_SEL_MASK             0x00000003UL
#define RADEON_VCLK_SRC_SEL_CPUCLK           0x00000000UL
#define RADEON_VCLK_SRC_SEL_PPLLCLK          0x00000003UL
#define RADEON_PIXCLK_ALWAYS_ONb             (1UL << 6)
#define RADEON_PIXCLK_DAC_ALWAYS_ONb         (1UL << 7)
#define RADEON_HTOTAL_CNTL                   0x0009U
#define RADEON_PLL_WR_EN                     (1UL << 7)
#define RADEON_PLL_INDEX_MASK                0x3fUL
#define RADEON_PLL_DIV_SEL                   (3UL << 8)

#endif
