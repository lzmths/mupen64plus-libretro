#include "z64.h"
#include "vi.h"
#include "rdp.h"

static int cmd_cur;
static int cmd_ptr; /* for 64-bit elements, always <= +0x7FFF */

/* static DP_FIFO cmd_fifo; */
static DP_FIFO cmd_data[0x0003FFFF/sizeof(i64) + 1];

static void invalid(void);
static void noop(void);
static void tri_noshade(void);
static void tri_noshade_z(void);
static void tri_tex(void);
static void tri_tex_z(void);
static void tri_shade(void);
static void tri_shade_z(void);
static void tri_texshade(void);
static void tri_texshade_z(void);
static void tex_rect(void);
static void tex_rect_flip(void);
static void sync_load(void);
static void sync_pipe(void);
static void sync_tile(void);
static void sync_full(void);
static void set_key_gb(void);
static void set_key_r(void);
static void set_convert(void);
static void set_scissor(void);
static void set_prim_depth(void);
static void set_other_modes(void);
static void set_tile_size(void);
static void load_block(void);
static void load_tlut(void);
static void load_tile(void);
static void set_tile(void);
static void fill_rect(void);
static void set_fill_color(void);
static void set_fog_color(void);
static void set_blend_color(void);
static void set_prim_color(void);
static void set_env_color(void);
static void set_combine(void);
static void set_texture_image(void);
static void set_mask_image(void);
static void set_color_image(void);

static NOINLINE void draw_triangle(int shade, int texture, int zbuffer);
NOINLINE static void render_spans(
    int yhlimit, int yllimit, int tilenum, int flip);
STRICTINLINE static u16 normalize_dzpix(u16 sum);

static void (*const rdp_command_table[64])(void) = {
    noop              ,invalid           ,invalid           ,invalid           ,
    invalid           ,invalid           ,invalid           ,invalid           ,
    tri_noshade       ,tri_noshade_z     ,tri_tex           ,tri_tex_z         ,
    tri_shade         ,tri_shade_z       ,tri_texshade      ,tri_texshade_z    ,

    invalid           ,invalid           ,invalid           ,invalid           ,
    invalid           ,invalid           ,invalid           ,invalid           ,
    invalid           ,invalid           ,invalid           ,invalid           ,
    invalid           ,invalid           ,invalid           ,invalid           ,

    invalid           ,invalid           ,invalid           ,invalid           ,
    tex_rect          ,tex_rect_flip     ,sync_load         ,sync_pipe         ,
    sync_tile         ,sync_full         ,set_key_gb        ,set_key_r         ,
    set_convert       ,set_scissor       ,set_prim_depth    ,set_other_modes   ,

    load_tlut         ,invalid           ,set_tile_size     ,load_block        ,
    load_tile         ,set_tile          ,fill_rect         ,set_fill_color    ,
    set_fog_color     ,set_blend_color   ,set_prim_color    ,set_env_color     ,
    set_combine       ,set_texture_image ,set_mask_image    ,set_color_image   ,
};

static const int DP_CMD_LEN_W[64] = { /* command length, in DP FIFO words */
    (8) / 8          ,(8) / 8          ,(8) / 8          ,(8) / 8          ,
    (8) / 8          ,(8) / 8          ,(8) / 8          ,(8) / 8          ,
    (32) / 8         ,(32+16) / 8      ,(32+64) / 8      ,(32+64+16) / 8   ,
    (32+64) / 8      ,(32+64+16) / 8   ,(32+64+64) / 8   ,(32+64+64+16) / 8,

    (8) / 8          ,(8) / 8          ,(8) / 8          ,(8) / 8          ,
    (8) / 8          ,(8) / 8          ,(8) / 8          ,(8) / 8          ,
    (8) / 8          ,(8) / 8          ,(8) / 8          ,(8) / 8          ,
    (8) / 8          ,(8) / 8          ,(8) / 8          ,(8) / 8          ,

    (8) / 8          ,(8) / 8          ,(8) / 8          ,(8) / 8          ,
    (16) / 8         ,(16) / 8         ,(8) / 8          ,(8) / 8          ,
    (8) / 8          ,(8) / 8          ,(8) / 8          ,(8) / 8          ,
    (8) / 8          ,(8) / 8          ,(8) / 8          ,(8) / 8          ,

    (8) / 8          ,(8) / 8          ,(8) / 8          ,(8) / 8          ,
    (8) / 8          ,(8) / 8          ,(8) / 8          ,(8) / 8          ,
    (8) / 8          ,(8) / 8          ,(8) / 8          ,(8) / 8          ,
    (8) / 8          ,(8) / 8          ,(8) / 8          ,(8) / 8          ,
};

#ifdef TRACE_DP_COMMANDS
static long cmd_count[64];

static const char DP_command_names[64][18] = {
    "NOOP             ",
    "reserved         ",
    "reserved         ",
    "reserved         ",
    "reserved         ",
    "reserved         ",
    "reserved         ",
    "reserved         ",
    "TRIFILL          ",
    "TRIFILLZBUFF     ",
    "TRITXTR          ",
    "TRITXTRZBUFF     ",
    "TRISHADE         ",
    "TRISHADEZBUFF    ",
    "TRISHADETXTR     ",
    "TRISHADETXTRZBUFF",
    "reserved         ",
    "reserved         ",
    "reserved         ",
    "reserved         ",
    "reserved         ",
    "reserved         ",
    "reserved         ",
    "reserved         ",
    "reserved         ",
    "reserved         ",
    "reserved         ",
    "reserved         ",
    "reserved         ",
    "reserved         ",
    "reserved         ",
    "reserved         ",
    "reserved         ",
    "reserved         ",
    "reserved         ",
    "reserved         ",
    "TEXRECT          ",
    "TEXRECTFLIP      ",
    "LOADSYNC         ",
    "PIPESYNC         ",
    "TILESYNC         ",
    "FULLSYNC         ",
    "SETKEYGB         ",
    "SETKEYR          ",
    "SETCONVERT       ",
    "SETSCISSOR       ",
    "SETPRIMDEPTH     ",
    "SETRDPOTHER      ",
    "LOADTLUT         ",
    "reserved         ",
    "SETTILESIZE      ",
    "LOADBLOCK        ",
    "LOADTILE         ",
    "SETTILE          ",
    "FILLRECT         ",
    "SETFILLCOLOR     ",
    "SETFOGCOLOR      ",
    "SETBLENDCOLOR    ",
    "SETPRIMCOLOR     ",
    "SETENVCOLOR      ",
    "SETCOMBINE       ",
    "SETTIMG          ",
    "SETMIMG          ",
    "SETCIMG          "
};

void count_DP_commands(void)
{
    FILE * stream;
    long total;
    register int i;

    total = 0;
    for (i = 0; i < 64; i++)
        total += cmd_count[i];
    stream = fopen("dp_count.txt", "w");
    for (i = 0; i < 64; i++)
        fprintf(
            stream, "%s:  %ld (%f%%)\n", DP_command_names[i], cmd_count[i],
            (float)(cmd_count[i])/(float)(total) * 100.F);
    fclose(stream);
    for (i = 0; i < 64; i++)
        cmd_count[i] = 0; /* reset for fresh data */
    return;
}
#endif

void process_RDP_list(void)
{
    int length;
    unsigned int offset;
    const u32 DP_CURRENT = *GET_GFX_INFO(DPC_CURRENT_REG) & 0x00FFFFF8;
    const u32 DP_END     = *GET_GFX_INFO(DPC_END_REG)     & 0x00FFFFF8;

    *GET_GFX_INFO(DPC_STATUS_REG) &= ~DP_STATUS_FREEZE;

    length = DP_END - DP_CURRENT;
    if (length <= 0)
        return;
    length = (unsigned)(length) / sizeof(i64);
    if ((cmd_ptr + length) & ~(0x0003FFFF / sizeof(i64)))
    {
        DisplayError("ProcessRDPList\nOut of command cache memory.");
        return;
    }

    --length; /* filling in cmd data in backwards order for performance */
    offset = (DP_END - sizeof(i64)) / sizeof(i64);
    if (*GET_GFX_INFO(DPC_STATUS_REG) & DP_STATUS_XBUS_DMA)
        do
        {
            offset &= 0xFFF / sizeof(i64);
            BUFFERFIFO(cmd_ptr + length, SP_DMEM, offset);
            offset -= 0x001 * sizeof(i8);
        } while (--length >= 0);
    else
        if (DP_END > plim || DP_CURRENT > plim)
        {
            DisplayError("DRAM access violation overrides");
            return;
        }
        else
        {
            do
            {
                offset &= 0xFFFFFF / sizeof(i64);
                BUFFERFIFO(cmd_ptr + length, DRAM, offset);
                offset -= 0x000001 * sizeof(i8);
            } while (--length >= 0);
        }
#ifdef USE_MMX_DECODES
    _mm_empty();
#endif
    cmd_ptr += (DP_END - DP_CURRENT) / sizeof(i64); /* += length */
    if (rdp_pipeline_crashed != 0)
        goto exit_a;

    while (cmd_cur - cmd_ptr < 0)
    {
        int command, cmd_length;

        command = (cmd_data[cmd_cur + 0].UW32[0] >> 24) % 64;
        cmd_length = sizeof(i64)/sizeof(i64) * DP_CMD_LEN_W[command];
#ifdef TRACE_DP_COMMANDS
        ++cmd_count[command];
#endif
        if (cmd_ptr - cmd_cur - cmd_length < 0)
            goto exit_b;
        rdp_command_table[command]();
        cmd_cur += cmd_length;
    };
exit_a:
    cmd_ptr = 0;
    cmd_cur = 0;
exit_b:
    *GET_GFX_INFO(DPC_START_REG)
  = *GET_GFX_INFO(DPC_CURRENT_REG)
  = *GET_GFX_INFO(DPC_END_REG);
    return;
}

static char invalid_command[] = "00\nDP reserved command.";
static void invalid(void)
{
    const unsigned int command
      = (cmd_data[cmd_cur + 0].UW32[0] & 0x3F000000) >> 24;

    invalid_command[0] = '0' | command >> 3;
    invalid_command[1] = '0' | command & 07;
    DisplayError(invalid_command);
    return;
}

static void noop(void)
{
    return;
}

static void tri_noshade(void)
{
    draw_triangle(SHADE_NO, TEXTURE_NO, ZBUFFER_NO);
    return;
}

static void tri_noshade_z(void)
{
    draw_triangle(SHADE_NO, TEXTURE_NO, ZBUFFER_YES);
    return;
}

static void tri_tex(void)
{
    draw_triangle(SHADE_NO, TEXTURE_YES, ZBUFFER_NO);
    return;
}

static void tri_tex_z(void)
{
    draw_triangle(SHADE_NO, TEXTURE_YES, ZBUFFER_YES);
    return;
}

static void tri_shade(void)
{
    draw_triangle(SHADE_YES, TEXTURE_NO, ZBUFFER_NO);
    return;
}

static void tri_shade_z(void)
{
    draw_triangle(SHADE_YES, TEXTURE_NO, ZBUFFER_YES);
    return;
}

static void tri_texshade(void)
{
    draw_triangle(SHADE_YES, TEXTURE_YES, ZBUFFER_NO);
    return;
}

static void tri_texshade_z(void)
{
    draw_triangle(SHADE_YES, TEXTURE_YES, ZBUFFER_YES);
    return;
}

static void tex_rect(void)
{
    int tilenum;
    int xl, yl, xh, yh;
    int s, t, dsdx, dtdy;
    int xlint, xhint;

    int ycur, ylfar;
    int yllimit, yhlimit;
    int invaly;
    int curcross;
    int allover, allunder, curover, curunder;
    int allinval;
    register int j, k;
    i32 stwz[4];
    i32 d_stwz_dx[4];
    i32 d_stwz_de[4];
    i32 d_stwz_dy[4];
    i32 d_stwz_dxh[4];
    i32 xleft, xright;
    u8 xfrac;
    const i32 clipxlshift = __clip.xl << 1;
    const i32 clipxhshift = __clip.xh << 1;

    xl      = (cmd_data[cmd_cur + 0].UW32[0] & 0x00FFF000) >> 12;
    yl      = (cmd_data[cmd_cur + 0].UW32[0] & 0x00000FFF) >>  0;
    tilenum = (cmd_data[cmd_cur + 0].UW32[1] & 0x07000000) >> 24;
    xh      = (cmd_data[cmd_cur + 0].UW32[1] & 0x00FFF000) >> 12;
    yh      = (cmd_data[cmd_cur + 0].UW32[1] & 0x00000FFF) >>  0;

    yl |= (other_modes.cycle_type & 2) ? 3 : 0; /* FILL OR COPY */

    s    = (cmd_data[cmd_cur + 1].UW32[0] & 0xFFFF0000) >> 16;
    t    = (cmd_data[cmd_cur + 1].UW32[0] & 0x0000FFFF) >>  0;
    dsdx = (cmd_data[cmd_cur + 1].UW32[1] & 0xFFFF0000) >> 16;
    dtdy = (cmd_data[cmd_cur + 1].UW32[1] & 0x0000FFFF) >>  0;
    
    dsdx = SIGN16(dsdx);
    dtdy = SIGN16(dtdy);

    xlint = (unsigned)(xl) >> 2;
    xhint = (unsigned)(xh) >> 2;

 /* edgewalker for primitives */
    max_level = 0;
    xl = (xlint << 16) | (xl & 3)<<14;
    xl = SIGN(xl, 30);
    xh = (xhint << 16) | (xh & 3)<<14;
    xh = SIGN(xh, 30);

    stwz[0] = s << 16;
    stwz[1] = t << 16;
    d_stwz_dx[0] = dsdx << 11;
    d_stwz_dx[1] = 0x00000000;
    d_stwz_de[0] = 0x00000000;
    d_stwz_de[1] = dtdy << 11;
    d_stwz_dy[0] = 0x00000000;
    d_stwz_dy[1] = dtdy << 11;

    setzero_si128(spans_d_rgba);
    spans_d_stwz[0] = d_stwz_dx[0] & ~0x0000001F;
    spans_d_stwz[1] = d_stwz_dx[1] & ~0x0000001F;
    spans_d_stwz[2] = 0x00000000;
    spans_d_stwz[3] = 0x00000000;

    setzero_si128(spans_d_rgba_dy);
    spans_d_stwz_dy[0] = d_stwz_dy[0] & ~0x00007FFF;
    spans_d_stwz_dy[1] = d_stwz_dy[1] & ~0x00007FFF;
    spans_d_stwz_dy[2] = 0x00000000;
    spans_d_stwz_dy[3] = 0x00000000;

    setzero_si128(spans_cd_rgba);
    spans_cdz = 0x00000000;
    spans_dzpix = normalize_dzpix(0);

    d_stwz_dxh[1] = 0x00000000;
    if (other_modes.cycle_type == CYCLE_TYPE_COPY)
        d_stwz_dxh[0] = 0x00000000;
    else
        d_stwz_dxh[0] = (d_stwz_dx[0] >> 8) & ~0x00000001;

    invaly = 1;
    yllimit = (yl < __clip.yl) ? yl : __clip.yl;

    ycur = yh & ~3;
    ylfar = yllimit | 3;
    if ((yl >> 2) > (ylfar >> 2))
        ylfar += 4;
    else if ((yllimit >> 2) >= 0 && (yllimit >> 2) < 1023)
        span[(yllimit >> 2) + 1].validline = 0;

    yhlimit = (yh >= __clip.yh) ? yh : __clip.yh;

    xleft = xl & ~0x00000001;
    xright = xh & ~0x00000001;
    xfrac = (xright >> 8) & 0xFF;

    stwz[0] &= ~0x000001FF;
    stwz[1] &= ~0x000001FF;
    allover = 1;
    allunder = 1;
    curover = 0;
    curunder = 0;
    allinval = 1;
    for (k = ycur; k <= ylfar; k++)
    {
        static int maxxmx, minxhx;
        int xrsc, xlsc, stickybit;
        const int yhclose = yhlimit & ~3;
        const int spix = k & 3;

        if (k < yhclose)
            { /* branch */ }
        else
        {
            invaly = (u32)(k - yhlimit)>>31 | (u32)~(k - yllimit)>>31;
            j = k >> 2;
            if (spix == 0)
            {
                maxxmx = 0x000;
                minxhx = 0xFFF;
                allover = allunder = 1;
                allinval = 1;
            }

            stickybit = (xright & 0x00003FFF) - 1; /* xright/2 & 0x1FFF */
            stickybit = (u32)~(stickybit) >> 31; /* (stickybit >= 0) */
            xrsc = (xright >> 13)&0x1FFE | stickybit;
            curunder = !!(xright & 0x08000000);
            curunder = curunder | (u32)(xrsc - clipxhshift)>>31;
            xrsc = curunder ? clipxhshift : (xright>>13)&0x3FFE | stickybit;
            curover  = !!(xrsc & 0x00002000);
            xrsc = xrsc & 0x1FFF;
            curover |= (u32)~(xrsc - clipxlshift) >> 31;
            xrsc = curover ? clipxlshift : xrsc;
            span[j].majorx[spix] = xrsc & 0x1FFF;
            allover &= curover;
            allunder &= curunder;

            stickybit = (xleft & 0x00003FFF) - 1; /* xleft/2 & 0x1FFF */
            stickybit = (u32)~(stickybit) >> 31; /* (stickybit >= 0) */
            xlsc = (xleft >> 13)&0x1FFE | stickybit;
            curunder = !!(xleft & 0x08000000);
            curunder = curunder | (u32)(xlsc - clipxhshift)>>31;
            xlsc = curunder ? clipxhshift : (xleft>>13)&0x3FFE | stickybit;
            curover  = !!(xlsc & 0x00002000);
            xlsc &= 0x1FFF;
            curover |= (u32)~(xlsc - clipxlshift) >> 31;
            xlsc = curover ? clipxlshift : xlsc;
            span[j].minorx[spix] = xlsc & 0x1FFF;
            allover &= curover;
            allunder &= curunder;

            curcross = ((xleft&0x0FFFC000 ^ 0x08000000)
                     < (xright&0x0FFFC000 ^ 0x08000000));
            invaly |= curcross;
            span[j].invalyscan[spix] = invaly;
            allinval &= invaly;
            if (invaly != 0)
                { /* branch */ }
            else
            {
                xlsc = (xlsc >> 3) & 0xFFF;
                xrsc = (xrsc >> 3) & 0xFFF;
                maxxmx = (xlsc > maxxmx) ? xlsc : maxxmx;
                minxhx = (xrsc < minxhx) ? xrsc : minxhx;
            }

            if (spix == 0)
            {
                span[j].unscrx = xright >> 16;
                setzero_si128(span[j].rgba);
                span[j].stwz[0] = (stwz[0] - xfrac*d_stwz_dxh[0]) & ~0x000003FF;
                span[j].stwz[1] = stwz[1];
                span[j].stwz[2] = 0x00000000;
                span[j].stwz[3] = 0x00000000;
            }
            else if (spix == 3)
            {
                const int invalidline = (sckeepodd ^ j) & scfield
                                      | (allinval | allover | allunder);
                span[j].lx = maxxmx;
                span[j].rx = minxhx;
                span[j].validline = invalidline ^ 1;
             /* stwz[0] = (stwz[0] + 0x00000000) & ~0x000001FF; */
                stwz[1] = (stwz[1] + d_stwz_de[1]) & ~0x000003FF;
            }
        }
    }
    render_spans(yhlimit >> 2, yllimit >> 2, tilenum, 1);
    return;
}

static void tex_rect_flip(void)
{
    int tilenum;
    int xl, yl, xh, yh;
    int s, t, dsdx, dtdy;
    int dd_swap;
    int xlint, xhint;

    int ycur, ylfar;
    int yllimit, yhlimit;
    int invaly;
    int curcross;
    int allover, allunder, curover, curunder;
    int allinval;
    register int j, k;
    i32 stwz[4];
    i32 d_stwz_dx[4];
    i32 d_stwz_de[4];
    i32 d_stwz_dy[4];
    i32 d_stwz_dxh[4];
    i32 xleft, xright;
    u8 xfrac;
    const i32 clipxlshift = __clip.xl << 1;
    const i32 clipxhshift = __clip.xh << 1;

    xl      = (cmd_data[cmd_cur + 0].UW32[0] & 0x00FFF000) >> 12;
    yl      = (cmd_data[cmd_cur + 0].UW32[0] & 0x00000FFF) >>  0;
    tilenum = (cmd_data[cmd_cur + 0].UW32[1] & 0x07000000) >> 24;
    xh      = (cmd_data[cmd_cur + 0].UW32[1] & 0x00FFF000) >> 12;
    yh      = (cmd_data[cmd_cur + 0].UW32[1] & 0x00000FFF) >>  0;

    yl |= (other_modes.cycle_type & 2) ? 3 : 0; /* FILL OR COPY */

    s    = (cmd_data[cmd_cur + 1].UW32[0] & 0xFFFF0000) >> 16;
    t    = (cmd_data[cmd_cur + 1].UW32[0] & 0x0000FFFF) >>  0;
    dsdx = (cmd_data[cmd_cur + 1].UW32[1] & 0xFFFF0000) >> 16;
    dtdy = (cmd_data[cmd_cur + 1].UW32[1] & 0x0000FFFF) >>  0;
    
    dsdx = SIGN16(dsdx);
    dtdy = SIGN16(dtdy);

/*
 * unique work to tex_rect_flip
 */
    dd_swap = dsdx;
    dsdx = dtdy;
    dtdy = dd_swap;

    xlint = (unsigned)(xl) >> 2;
    xhint = (unsigned)(xh) >> 2;

 /* edgewalker for primitives */
    max_level = 0;
    xl = (xlint << 16) | (xl & 3)<<14;
    xl = SIGN(xl, 30);
    xh = (xhint << 16) | (xh & 3)<<14;
    xh = SIGN(xh, 30);

    stwz[0] = s << 16;
    stwz[1] = t << 16;
    d_stwz_dx[0] = dsdx << 11;
    d_stwz_dx[1] = 0x00000000;
    d_stwz_de[0] = 0x00000000;
    d_stwz_de[1] = dtdy << 11;
    d_stwz_dy[0] = 0x00000000;
    d_stwz_dy[1] = dtdy << 11;

    setzero_si128(spans_d_rgba);
    spans_d_stwz[0] = d_stwz_dx[0] & ~0x0000001F;
    spans_d_stwz[1] = d_stwz_dx[1] & ~0x0000001F;
    spans_d_stwz[2] = 0x00000000;
    spans_d_stwz[3] = 0x00000000;

    setzero_si128(spans_d_rgba_dy);
    spans_d_stwz_dy[0] = d_stwz_dy[0] & ~0x00007FFF;
    spans_d_stwz_dy[1] = d_stwz_dy[1] & ~0x00007FFF;
    spans_d_stwz_dy[2] = 0x00000000;
    spans_d_stwz_dy[3] = 0x00000000;

    setzero_si128(spans_cd_rgba);
    spans_cdz = 0x00000000;
    spans_dzpix = normalize_dzpix(0);

    d_stwz_dxh[1] = 0x00000000;
    if (other_modes.cycle_type == CYCLE_TYPE_COPY)
        d_stwz_dxh[0] = 0x00000000;
    else
        d_stwz_dxh[0] = (d_stwz_dx[0] >> 8) & ~0x00000001;

    invaly = 1;
    yllimit = (yl < __clip.yl) ? yl : __clip.yl;

    ycur = yh & ~3;
    ylfar = yllimit | 3;
    if ((yl >> 2) > (ylfar >> 2))
        ylfar += 4;
    else if ((yllimit >> 2) >= 0 && (yllimit >> 2) < 1023)
        span[(yllimit >> 2) + 1].validline = 0;

    yhlimit = (yh >= __clip.yh) ? yh : __clip.yh;

    xleft = xl & ~0x00000001;
    xright = xh & ~0x00000001;
    xfrac = (xright >> 8) & 0xFF;

    stwz[0] &= ~0x000001FF;
    stwz[1] &= ~0x000001FF;
    allover = 1;
    allunder = 1;
    curover = 0;
    curunder = 0;
    allinval = 1;
    for (k = ycur; k <= ylfar; k++)
    {
        static int maxxmx, minxhx;
        int xrsc, xlsc, stickybit;
        const int yhclose = yhlimit & ~3;
        const int spix = k & 3;

        if (k < yhclose)
            { /* branch */ }
        else
        {
            invaly = (u32)(k - yhlimit)>>31 | (u32)~(k - yllimit)>>31;
            j = k >> 2;
            if (spix == 0)
            {
                maxxmx = 0x000;
                minxhx = 0xFFF;
                allover = allunder = 1;
                allinval = 1;
            }

            stickybit = (xright & 0x00003FFF) - 1; /* xright/2 & 0x1FFF */
            stickybit = (u32)~(stickybit) >> 31; /* (stickybit >= 0) */
            xrsc = (xright >> 13)&0x1FFE | stickybit;
            curunder = !!(xright & 0x08000000);
            curunder = curunder | (u32)(xrsc - clipxhshift)>>31;
            xrsc = curunder ? clipxhshift : (xright>>13)&0x3FFE | stickybit;
            curover  = !!(xrsc & 0x00002000);
            xrsc = xrsc & 0x1FFF;
            curover |= (u32)~(xrsc - clipxlshift) >> 31;
            xrsc = curover ? clipxlshift : xrsc;
            span[j].majorx[spix] = xrsc & 0x1FFF;
            allover &= curover;
            allunder &= curunder;

            stickybit = (xleft & 0x00003FFF) - 1; /* xleft/2 & 0x1FFF */
            stickybit = (u32)~(stickybit) >> 31; /* (stickybit >= 0) */
            xlsc = (xleft >> 13)&0x1FFE | stickybit;
            curunder = !!(xleft & 0x08000000);
            curunder = curunder | (u32)(xlsc - clipxhshift)>>31;
            xlsc = curunder ? clipxhshift : (xleft>>13)&0x3FFE | stickybit;
            curover  = !!(xlsc & 0x00002000);
            xlsc &= 0x1FFF;
            curover |= (u32)~(xlsc - clipxlshift) >> 31;
            xlsc = curover ? clipxlshift : xlsc;
            span[j].minorx[spix] = xlsc & 0x1FFF;
            allover &= curover;
            allunder &= curunder;

            curcross = ((xleft&0x0FFFC000 ^ 0x08000000)
                     < (xright&0x0FFFC000 ^ 0x08000000));
            invaly |= curcross;
            span[j].invalyscan[spix] = invaly;
            allinval &= invaly;
            if (invaly != 0)
                { /* branch */ }
            else
            {
                xlsc = (xlsc >> 3) & 0xFFF;
                xrsc = (xrsc >> 3) & 0xFFF;
                maxxmx = (xlsc > maxxmx) ? xlsc : maxxmx;
                minxhx = (xrsc < minxhx) ? xrsc : minxhx;
            }

            if (spix == 0)
            {
                span[j].unscrx = xright >> 16;
                setzero_si128(span[j].rgba);
                span[j].stwz[0] = (stwz[0] - xfrac*d_stwz_dxh[0]) & ~0x000003FF;
                span[j].stwz[1] = stwz[1];
                span[j].stwz[2] = 0x00000000;
                span[j].stwz[3] = 0x00000000;
            }
            else if (spix == 3)
            {
                const int invalidline = (sckeepodd ^ j) & scfield
                                      | (allinval | allover | allunder);
                span[j].lx = maxxmx;
                span[j].rx = minxhx;
                span[j].validline = invalidline ^ 1;
             /* stwz[0] = (stwz[0] + 0x00000000) & ~0x000001FF; */
                stwz[1] = (stwz[1] + d_stwz_de[1]) & ~0x000003FF;
            }
        }
    }
    render_spans(yhlimit >> 2, yllimit >> 2, tilenum, 1);
    return;
}

static void sync_load(void)
{
    return;
}

static void sync_pipe(void)
{
    return;
}

static void sync_tile(void)
{
    return;
}

static void sync_full(void)
{
    z64gl_command = 0; /* wtf is this for */
    *gfx_info.MI_INTR_REG |= DP_INTERRUPT;
    gfx_info.CheckInterrupts();
    return;
}

static void set_key_gb(void)
{
    key_width.g  = (cmd_data[cmd_cur + 0].UW32[0] & 0x00FFF000) >> 12;
    key_width.b  = (cmd_data[cmd_cur + 0].UW32[0] & 0x00000FFF) >>  0;
    key_center.g = (cmd_data[cmd_cur + 0].UW32[1] & 0xFF000000) >> 24;
    key_scale.g  = (cmd_data[cmd_cur + 0].UW32[1] & 0x00FF0000) >> 16;
    key_center.b = (cmd_data[cmd_cur + 0].UW32[1] & 0x0000FF00) >>  8;
    key_scale.b  = (cmd_data[cmd_cur + 0].UW32[1] & 0x000000FF) >>  0;
    return;
}

static void set_key_r(void)
{
    key_width.r  = (cmd_data[cmd_cur + 0].UW32[1] & 0x0FFF0000) >> 16;
    key_center.r = (cmd_data[cmd_cur + 0].UW32[1] & 0x0000FF00) >>  8;
    key_scale.r  = (cmd_data[cmd_cur + 0].UW32[1] & 0x000000FF) >>  0;
    return;
}

static void set_convert(void)
{
   UINT32 w1 = cmd_data[cmd_cur + 0].UW32[0];
   UINT32 w2 = cmd_data[cmd_cur + 0].UW32[1];

   INT32 k0 = (w1 >> 13) & 0x1ff;
   INT32 k1 = (w1 >> 4) & 0x1ff;
   INT32 k2 = ((w1 & 0xf) << 5) | ((w2 >> 27) & 0x1f);
   INT32 k3 = (w2 >> 18) & 0x1ff;
   k0_tf = (SIGN(k0, 9) << 1) + 1;
   k1_tf = (SIGN(k1, 9) << 1) + 1;
   k2_tf = (SIGN(k2, 9) << 1) + 1;
   k3_tf = (SIGN(k3, 9) << 1) + 1;
   k4 = (w2 >> 9) & 0x1ff;
 	k5 = w2 & 0x1ff;
   return;
}

static void set_scissor(void)
{
    __clip.xh   = (cmd_data[cmd_cur + 0].UW32[0] & 0x00FFF000) >> (44 - 32);
    __clip.yh   = (cmd_data[cmd_cur + 0].UW32[0] & 0x00000FFF) >> (32 - 32);
    scfield   = (cmd_data[cmd_cur + 0].UW32[1] & 0x02000000) >> (25 -  0);
    sckeepodd = (cmd_data[cmd_cur + 0].UW32[1] & 0x01000000) >> (24 -  0);
    __clip.xl   = (cmd_data[cmd_cur + 0].UW32[1] & 0x00FFF000) >> (12 -  0);
    __clip.yl   = (cmd_data[cmd_cur + 0].UW32[1] & 0x00000FFF) >> ( 0 -  0);
    return;
}

static void set_prim_depth(void)
{
    primitive_z       = (cmd_data[cmd_cur + 0].UW32[1] & 0xFFFF0000) >> 16;
    primitive_delta_z = (cmd_data[cmd_cur + 0].UW32[1] & 0x0000FFFF) >>  0;
    primitive_z = (primitive_z & 0x7FFF) << 16; /* angrylion does this why? */
    return;
}

static void set_other_modes(void)
{
    const DP_FIFO cmd_fifo = cmd_data[cmd_cur + 0];

 /* K:  atomic_prim              = (cmd_fifo.UW & 0x0080000000000000) >> 55; */
 /* j:  reserved for future use -- (cmd_fifo.UW & 0x0040000000000000) >> 54 */
    other_modes.cycle_type       = (cmd_fifo.UW32[0] & 0x00300000) >> (52 - 32);
    other_modes.persp_tex_en     = !!(cmd_fifo.UW32[0] & 0x00080000); /* 51 */
    other_modes.detail_tex_en    = !!(cmd_fifo.UW32[0] & 0x00040000); /* 50 */
    other_modes.sharpen_tex_en   = !!(cmd_fifo.UW32[0] & 0x00020000); /* 49 */
    other_modes.tex_lod_en       = !!(cmd_fifo.UW32[0] & 0x00010000); /* 48 */
    other_modes.en_tlut          = !!(cmd_fifo.UW32[0] & 0x00008000); /* 47 */
    other_modes.tlut_type        = !!(cmd_fifo.UW32[0] & 0x00004000); /* 46 */
    other_modes.sample_type      = !!(cmd_fifo.UW32[0] & 0x00002000); /* 45 */
    other_modes.mid_texel        = !!(cmd_fifo.UW32[0] & 0x00001000); /* 44 */
    other_modes.bi_lerp0         = !!(cmd_fifo.UW32[0] & 0x00000800); /* 43 */
    other_modes.bi_lerp1         = !!(cmd_fifo.UW32[0] & 0x00000400); /* 42 */
    other_modes.convert_one      = !!(cmd_fifo.UW32[0] & 0x00000200); /* 41 */
    other_modes.key_en           = !!(cmd_fifo.UW32[0] & 0x00000100); /* 40 */
    other_modes.rgb_dither_sel   = (cmd_fifo.UW32[0] & 0x000000C0) >> (38 - 32);
    other_modes.alpha_dither_sel = (cmd_fifo.UW32[0] & 0x00000030) >> (36 - 32);
 /* reserved for future, def:15 -- (cmd_fifo.UW & 0x0000000F00000000) >> 32 */
    other_modes.blend_m1a_0      = (cmd_fifo.UW32[1] & 0xC0000000) >> (30 -  0);
    other_modes.blend_m1a_1      = (cmd_fifo.UW32[1] & 0x30000000) >> (28 -  0);
    other_modes.blend_m1b_0      = (cmd_fifo.UW32[1] & 0x0C000000) >> (26 -  0);
    other_modes.blend_m1b_1      = (cmd_fifo.UW32[1] & 0x03000000) >> (24 -  0);
    other_modes.blend_m2a_0      = (cmd_fifo.UW32[1] & 0x00C00000) >> (22 -  0);
    other_modes.blend_m2a_1      = (cmd_fifo.UW32[1] & 0x00300000) >> (20 -  0);
    other_modes.blend_m2b_0      = (cmd_fifo.UW32[1] & 0x000C0000) >> (18 -  0);
    other_modes.blend_m2b_1      = (cmd_fifo.UW32[1] & 0x00030000) >> (16 -  0);
 /* N:  reserved for future use -- (cmd_fifo.UW & 0x0000000000008000) >> 15 */
    other_modes.force_blend      = !!(cmd_fifo.UW32[1] & 0x00004000); /* 14 */
    other_modes.alpha_cvg_select = !!(cmd_fifo.UW32[1] & 0x00002000); /* 13 */
    other_modes.cvg_times_alpha  = !!(cmd_fifo.UW32[1] & 0x00001000); /* 12 */
    other_modes.z_mode           = (cmd_fifo.UW32[1] & 0x00000C00) >> (10 -  0);
    other_modes.cvg_dest         = (cmd_fifo.UW32[1] & 0x00000300) >> ( 8 -  0);
    other_modes.color_on_cvg     = !!(cmd_fifo.UW32[1] & 0x00000080); /*  7 */
    other_modes.image_read_en    = !!(cmd_fifo.UW32[1] & 0x00000040); /*  6 */
    other_modes.z_update_en      = !!(cmd_fifo.UW32[1] & 0x00000020); /*  5 */
    other_modes.z_compare_en     = !!(cmd_fifo.UW32[1] & 0x00000010); /*  4 */
    other_modes.antialias_en     = !!(cmd_fifo.UW32[1] & 0x00000008); /*  3 */
    other_modes.z_source_sel     = !!(cmd_fifo.UW32[1] & 0x00000004); /*  2 */
    other_modes.dither_alpha_en  = !!(cmd_fifo.UW32[1] & 0x00000002); /*  1 */
    other_modes.alpha_compare_en = !!(cmd_fifo.UW32[1] & 0x00000001); /*  0 */

    SET_BLENDER_INPUT(
        0, 0, &blender1a_r[0], &blender1a_g[0], &blender1a_b[0],
        &blender1b_a[0], other_modes.blend_m1a_0, other_modes.blend_m1b_0);
    SET_BLENDER_INPUT(
        0, 1, &blender2a_r[0], &blender2a_g[0], &blender2a_b[0],
        &blender2b_a[0], other_modes.blend_m2a_0, other_modes.blend_m2b_0);
    SET_BLENDER_INPUT(
        1, 0, &blender1a_r[1], &blender1a_g[1], &blender1a_b[1],
        &blender1b_a[1], other_modes.blend_m1a_1, other_modes.blend_m1b_1);
    SET_BLENDER_INPUT(
        1, 1, &blender2a_r[1], &blender2a_g[1], &blender2a_b[1],
        &blender2b_a[1], other_modes.blend_m2a_1, other_modes.blend_m2b_1);

    other_modes.f.stalederivs = 1;
    return;
}

static void set_tile_size(void)
{
    int sl, tl, tilenum, sh, th;

    sl      = (cmd_data[cmd_cur + 0].UW32[0] & 0x00FFF000) >> (44 - 32);
    tl      = (cmd_data[cmd_cur + 0].UW32[0] & 0x00000FFF) >> (32 - 32);
    tilenum = (cmd_data[cmd_cur + 0].UW32[1] & 0x07000000) >> (24 -  0);
    sh      = (cmd_data[cmd_cur + 0].UW32[1] & 0x00FFF000) >> (12 -  0);
    th      = (cmd_data[cmd_cur + 0].UW32[1] & 0x00000FFF) >> ( 0 -  0);

    tile[tilenum].sl = sl;
    tile[tilenum].tl = tl;
    tile[tilenum].sh = sh;
    tile[tilenum].th = th;
    calculate_clamp_diffs(tilenum);
    return;
}

static void load_block(void)
{
    INT32 lewdata[10];
    const int command = (cmd_data[cmd_cur + 0].UW32[0] & 0xFF000000) >> (56-32);
    const int sl      = (cmd_data[cmd_cur + 0].UW32[0] & 0x00FFF000) >> (44-32);
    const int tl      = (cmd_data[cmd_cur + 0].UW32[0] & 0x00000FFF) >> (32-32);
    const int tilenum = (cmd_data[cmd_cur + 0].UW32[1] & 0x07000000) >> (24- 0);
    const int sh      = (cmd_data[cmd_cur + 0].UW32[1] & 0x00FFF000) >> (12- 0);
    const int dxt     = (cmd_data[cmd_cur + 0].UW32[1] & 0x00000FFF) >> ( 0- 0);
    const int tlclamped = tl & 0x3FF;

    tile[tilenum].sl = sl;
    tile[tilenum].tl = tl;
    tile[tilenum].sh = sh;
    tile[tilenum].th = dxt;

    calculate_clamp_diffs(tilenum);

    lewdata[0] =
        (command << 24)
      | (0x10 << 19)
      | (tilenum << 16)
      | ((tlclamped << 2) | 3);
    lewdata[1] = (((tlclamped << 2) | 3) << 16) | (tlclamped << 2);
    lewdata[2] = sh << 16;
    lewdata[3] = sl << 16;
    lewdata[4] = sh << 16;
    lewdata[5] = ((sl << 3) << 16) | (tl << 3);
    lewdata[6] = (dxt & 0xff) << 8;
    lewdata[7] = ((0x80 >> ti_size) << 16) | (dxt >> 8);
    lewdata[8] = 0x20;
    lewdata[9] = 0x20;

    edgewalker_for_loads(lewdata);
    return;
}

static void load_tlut(void)
{
    tile_tlut_common_cs_decoder(
        cmd_data[cmd_cur + 0].UW32[0], cmd_data[cmd_cur + 0].UW32[1]);
    return;
}

static void load_tile(void)
{
    tile_tlut_common_cs_decoder(
        cmd_data[cmd_cur + 0].UW32[0], cmd_data[cmd_cur + 0].UW32[1]);
    return;
}

static void set_tile(void)
{
    const DP_FIFO cmd_fifo = cmd_data[cmd_cur + 0];
    const int tilenum     = (cmd_fifo.UW32[1] & 0x07000000) >> 24;
    
    tile[tilenum].format  = (cmd_fifo.UW32[0] & 0x00E00000) >> (53 - 32);
    tile[tilenum].size    = (cmd_fifo.UW32[0] & 0x00180000) >> (51 - 32);
    tile[tilenum].line    = (cmd_fifo.UW32[0] & 0x0003FE00) >> (41 - 32);
    tile[tilenum].tmem    = (cmd_fifo.UW32[0] & 0x000001FF) >> (32 - 32);
 /* tilenum               = (cmd_fifo.UW & 0x0000000007000000) >> 24; */
    tile[tilenum].palette = (cmd_fifo.UW32[1] & 0x00F00000) >> (20 -  0);
    tile[tilenum].ct      = (cmd_fifo.UW32[1] & 0x00080000) >> (19 -  0);
    tile[tilenum].mt      = (cmd_fifo.UW32[1] & 0x00040000) >> (18 -  0);
    tile[tilenum].mask_t  = (cmd_fifo.UW32[1] & 0x0003C000) >> (14 -  0);
    tile[tilenum].shift_t = (cmd_fifo.UW32[1] & 0x00003C00) >> (10 -  0);
    tile[tilenum].cs      = (cmd_fifo.UW32[1] & 0x00000200) >> ( 9 -  0);
    tile[tilenum].ms      = (cmd_fifo.UW32[1] & 0x00000100) >> ( 8 -  0);
    tile[tilenum].mask_s  = (cmd_fifo.UW32[1] & 0x000000F0) >> ( 4 -  0);
    tile[tilenum].shift_s = (cmd_fifo.UW32[1] & 0x0000000F) >> ( 0 -  0);

    calculate_tile_derivs(tilenum);
    return;
}

static void fill_rect(void)
{
    int xl, yl, xh, yh;
    int xlint, xhint;

    int ycur, ylfar;
    int yllimit, yhlimit;
    int invaly;
    int curcross;
    int allover, allunder, curover, curunder;
    int allinval;
    register int j, k;
    const i32 clipxlshift = __clip.xl << 1;
    const i32 clipxhshift = __clip.xh << 1;

    xl = (cmd_data[cmd_cur + 0].UW32[0] & 0x00FFF000) >> (44 - 32);
    yl = (cmd_data[cmd_cur + 0].UW32[0] & 0x00000FFF) >> (32 - 32);
    xh = (cmd_data[cmd_cur + 0].UW32[1] & 0x00FFF000) >> (12 -  0);
    yh = (cmd_data[cmd_cur + 0].UW32[1] & 0x00000FFF) >> ( 0 -  0);

    yl |= (other_modes.cycle_type & 2) ? 3 : 0; /* FILL or COPY */

    xlint = (unsigned)(xl) >> 2;
    xhint = (unsigned)(xh) >> 2;

    max_level = 0;
    xl = (xlint << 16) | (xl & 3)<<14;
    xl = SIGN(xl, 30);
    xh = (xhint << 16) | (xh & 3)<<14;
    xh = SIGN(xh, 30);

    setzero_si128(spans_d_rgba);
    setzero_si128(spans_d_stwz);

    setzero_si128(spans_d_rgba_dy);
    setzero_si128(spans_d_stwz_dy);

    setzero_si128(spans_cd_rgba);
    spans_cdz = 0;

    spans_dzpix = normalize_dzpix(0);

    invaly = 1;
    yllimit = (yl < __clip.yl) ? yl : __clip.yl;

    ycur = yh & ~3;
    ylfar = yllimit | 3;
    if (yl >> 2 > ylfar >> 2)
        ylfar += 4;
    else if (yllimit >> 2 >= 0 && yllimit>>2 < 1023)
        span[(yllimit >> 2) + 1].validline = 0;
    yhlimit = (yh >= __clip.yh) ? yh : __clip.yh;

    allover = 1;
    allunder = 1;
    curover = 0;
    curunder = 0;
    allinval = 1;
    for (k = ycur; k <= ylfar; k++)
    {
        static int maxxmx, minxhx;
        int xrsc, xlsc, stickybit;
        const i32 xleft = xl & ~0x00000001, xright = xh & ~0x00000001;
        const int yhclose = yhlimit & ~3;
        const int spix = k & 3;

        if (k < yhclose)
            continue;
        invaly = (u32)(k - yhlimit)>>31 | (u32)~(k - yllimit)>>31;
        j = k >> 2;
        if (spix == 0)
        {
            maxxmx = 0x000;
            minxhx = 0xFFF;
            allover = allunder = 1;
            allinval = 1;
        }

        stickybit = (xright & 0x00003FFF) - 1; /* xright/2 & 0x1FFF */
        stickybit = (u32)~(stickybit) >> 31; /* (stickybit >= 0) */
        xrsc = (xright >> 13)&0x1FFE | stickybit;
        curunder = !!(xright & 0x08000000);
        curunder = curunder | (u32)(xrsc - clipxhshift)>>31;
        xrsc = curunder ? clipxhshift : (xright>>13)&0x3FFE | stickybit;
        curover  = !!(xrsc & 0x00002000);
        xrsc = xrsc & 0x1FFF;
        curover |= (u32)~(xrsc - clipxlshift) >> 31;
        xrsc = curover ? clipxlshift : xrsc;
        span[j].majorx[spix] = xrsc & 0x1FFF;
        allover &= curover;
        allunder &= curunder;

        stickybit = (xleft & 0x00003FFF) - 1; /* xleft/2 & 0x1FFF */
        stickybit = (u32)~(stickybit) >> 31; /* (stickybit >= 0) */
        xlsc = (xleft >> 13)&0x1FFE | stickybit;
        curunder = !!(xleft & 0x08000000);
        curunder = curunder | (u32)(xlsc - clipxhshift)>>31;
        xlsc = curunder ? clipxhshift : (xleft>>13)&0x3FFE | stickybit;
        curover  = !!(xlsc & 0x00002000);
        xlsc &= 0x1FFF;
        curover |= (u32)~(xlsc - clipxlshift) >> 31;
        xlsc = curover ? clipxlshift : xlsc;
        span[j].minorx[spix] = xlsc & 0x1FFF;
        allover &= curover;
        allunder &= curunder;

        curcross = ((xleft&0x0FFFC000 ^ 0x08000000)
                 < (xright&0x0FFFC000 ^ 0x08000000));
        invaly |= curcross;
        span[j].invalyscan[spix] = invaly;
        allinval &= invaly;
        if (invaly != 0)
            { /* branch */ }
        else
        {
            xlsc = (xlsc >> 3) & 0xFFF;
            xrsc = (xrsc >> 3) & 0xFFF;
            maxxmx = (xlsc > maxxmx) ? xlsc : maxxmx;
            minxhx = (xrsc < minxhx) ? xrsc : minxhx;
        }

        if (spix == 0)
        {
            span[j].unscrx = xright >> 16;
            setzero_si128(span[j].rgba);
            setzero_si128(span[j].stwz);
        }
        else if (spix == 3)
        {
            const int invalidline = (sckeepodd ^ j) & scfield
                                  | (allinval | allover | allunder);
            span[j].lx = maxxmx;
            span[j].rx = minxhx;
            span[j].validline = invalidline ^ 1;
        }
    }
    render_spans(yhlimit >> 2, yllimit >> 2, 0, 1);
    return;
}

static void set_fill_color(void)
{
    fill_color = cmd_data[cmd_cur + 0].UW32[1];
    return;
}

static void set_fog_color(void)
{
    fog_color.r = (cmd_data[cmd_cur + 0].UW32[1] & 0xFF000000) >> 24;
    fog_color.g = (cmd_data[cmd_cur + 0].UW32[1] & 0x00FF0000) >> 16;
    fog_color.b = (cmd_data[cmd_cur + 0].UW32[1] & 0x0000FF00) >>  8;
    fog_color.a = (cmd_data[cmd_cur + 0].UW32[1] & 0x000000FF) >>  0;
    return;
}

static void set_blend_color(void)
{
    blend_color.r = (cmd_data[cmd_cur + 0].UW32[1] & 0xFF000000) >> 24;
    blend_color.g = (cmd_data[cmd_cur + 0].UW32[1] & 0x00FF0000) >> 16;
    blend_color.b = (cmd_data[cmd_cur + 0].UW32[1] & 0x0000FF00) >>  8;
    blend_color.a = (cmd_data[cmd_cur + 0].UW32[1] & 0x000000FF) >>  0;
    return;
}

static void set_prim_color(void)
{
    min_level          = (cmd_data[cmd_cur + 0].UW32[0] & 0x00001F00) >>(40-32);
    primitive_lod_frac = (cmd_data[cmd_cur + 0].UW32[0] & 0x000000FF) >>(32-32);
    prim_color.r       = (cmd_data[cmd_cur + 0].UW32[1] & 0xFF000000) >> 24;
    prim_color.g       = (cmd_data[cmd_cur + 0].UW32[1] & 0x00FF0000) >> 16;
    prim_color.b       = (cmd_data[cmd_cur + 0].UW32[1] & 0x0000FF00) >>  8;
    prim_color.a       = (cmd_data[cmd_cur + 0].UW32[1] & 0x000000FF) >>  0;
    return;
}

static void set_env_color(void)
{
    env_color.r = (cmd_data[cmd_cur + 0].UW32[1] & 0xFF000000) >> 24;
    env_color.g = (cmd_data[cmd_cur + 0].UW32[1] & 0x00FF0000) >> 16;
    env_color.b = (cmd_data[cmd_cur + 0].UW32[1] & 0x0000FF00) >>  8;
    env_color.a = (cmd_data[cmd_cur + 0].UW32[1] & 0x000000FF) >>  0;
    return;
}

static void set_combine(void)
{
    combine.sub_a_rgb0 = (cmd_data[cmd_cur + 0].UW32[0] & 0x00F00000) >>(52-32);
    combine.mul_rgb0   = (cmd_data[cmd_cur + 0].UW32[0] & 0x000F8000) >>(47-32);
    combine.sub_a_a0   = (cmd_data[cmd_cur + 0].UW32[0] & 0x00007000) >>(44-32);
    combine.mul_a0     = (cmd_data[cmd_cur + 0].UW32[0] & 0x00000E00) >>(41-32);
    combine.sub_a_rgb1 = (cmd_data[cmd_cur + 0].UW32[0] & 0x000001E0) >>(37-32);
    combine.mul_rgb1   = (cmd_data[cmd_cur + 0].UW32[0] & 0x0000001F) >>(32-32);
    combine.sub_b_rgb0 = (cmd_data[cmd_cur + 0].UW32[1] & 0xF0000000) >> 28;
    combine.sub_b_rgb1 = (cmd_data[cmd_cur + 0].UW32[1] & 0x0F000000) >> 24;
    combine.sub_a_a1   = (cmd_data[cmd_cur + 0].UW32[1] & 0x00E00000) >> 21;
    combine.mul_a1     = (cmd_data[cmd_cur + 0].UW32[1] & 0x001C0000) >> 18;
    combine.add_rgb0   = (cmd_data[cmd_cur + 0].UW32[1] & 0x00038000) >> 15;
    combine.sub_b_a0   = (cmd_data[cmd_cur + 0].UW32[1] & 0x00007000) >> 12;
    combine.add_a0     = (cmd_data[cmd_cur + 0].UW32[1] & 0x00000E00) >>  9;
    combine.add_rgb1   = (cmd_data[cmd_cur + 0].UW32[1] & 0x000001C0) >>  6;
    combine.sub_b_a1   = (cmd_data[cmd_cur + 0].UW32[1] & 0x00000038) >>  3;
    combine.add_a1     = (cmd_data[cmd_cur + 0].UW32[1] & 0x00000007) >>  0;

    SET_SUBA_RGB_INPUT(
        &combiner_rgbsub_a_r[0], &combiner_rgbsub_a_g[0],
        &combiner_rgbsub_a_b[0], combine.sub_a_rgb0);
    SET_SUBB_RGB_INPUT(
        &combiner_rgbsub_b_r[0], &combiner_rgbsub_b_g[0],
        &combiner_rgbsub_b_b[0], combine.sub_b_rgb0);
    SET_MUL_RGB_INPUT(
        &combiner_rgbmul_r[0], &combiner_rgbmul_g[0], &combiner_rgbmul_b[0],
        combine.mul_rgb0);
    SET_ADD_RGB_INPUT(
        &combiner_rgbadd_r[0], &combiner_rgbadd_g[0], &combiner_rgbadd_b[0],
        combine.add_rgb0);
    SET_SUB_ALPHA_INPUT(&combiner_alphasub_a[0], combine.sub_a_a0);
    SET_SUB_ALPHA_INPUT(&combiner_alphasub_b[0], combine.sub_b_a0);
    SET_MUL_ALPHA_INPUT(&combiner_alphamul[0], combine.mul_a0);
    SET_SUB_ALPHA_INPUT(&combiner_alphaadd[0], combine.add_a0);

    SET_SUBA_RGB_INPUT(
        &combiner_rgbsub_a_r[1], &combiner_rgbsub_a_g[1],
        &combiner_rgbsub_a_b[1], combine.sub_a_rgb1);
    SET_SUBB_RGB_INPUT(
        &combiner_rgbsub_b_r[1], &combiner_rgbsub_b_g[1],
        &combiner_rgbsub_b_b[1], combine.sub_b_rgb1);
    SET_MUL_RGB_INPUT(
        &combiner_rgbmul_r[1], &combiner_rgbmul_g[1], &combiner_rgbmul_b[1],
        combine.mul_rgb1);
    SET_ADD_RGB_INPUT(
        &combiner_rgbadd_r[1], &combiner_rgbadd_g[1], &combiner_rgbadd_b[1],
        combine.add_rgb1);
    SET_SUB_ALPHA_INPUT(&combiner_alphasub_a[1], combine.sub_a_a1);
    SET_SUB_ALPHA_INPUT(&combiner_alphasub_b[1], combine.sub_b_a1);
    SET_MUL_ALPHA_INPUT(&combiner_alphamul[1], combine.mul_a1);
    SET_SUB_ALPHA_INPUT(&combiner_alphaadd[1], combine.add_a1);

    other_modes.f.stalederivs = 1;
    return;
}

static void set_texture_image(void)
{
    ti_format  = (cmd_data[cmd_cur + 0].UW32[0] & 0x00E00000) >> (53 - 32);
    ti_size    = (cmd_data[cmd_cur + 0].UW32[0] & 0x00180000) >> (51 - 32);
    ti_width   = (cmd_data[cmd_cur + 0].UW32[0] & 0x000003FF) >> (32 - 32);
    ti_address = (cmd_data[cmd_cur + 0].UW32[1] & 0x03FFFFFF) >> ( 0 -  0);
 /* ti_address &= 0x00FFFFFF; // physical memory limit, enforced later */
    ++ti_width;
    return;
}

static void set_mask_image(void)
{
    zb_address = cmd_data[cmd_cur + 0].UW32[1] & 0x03FFFFFF;
 /* zb_address &= 0x00FFFFFF; */
    return;
}

static void set_color_image(void)
{
    fb_format  = (cmd_data[cmd_cur + 0].UW32[0] & 0x00E00000) >> (53 - 32);
    fb_size    = (cmd_data[cmd_cur + 0].UW32[0] & 0x00180000) >> (51 - 32);
    fb_width   = (cmd_data[cmd_cur + 0].UW32[0] & 0x000003FF) >> (32 - 32);
    fb_address = (cmd_data[cmd_cur + 0].UW32[1] & 0x03FFFFFF) >> ( 0 -  0);
    ++fb_width;
 /* fb_address &= 0x00FFFFFF; */
    return;
}

static NOINLINE void draw_triangle(int shade, int texture, int zbuffer)
{
    register int base;
    int lft, level, tile;
    s32 yl, ym, yh; /* triangle edge y-coordinates */
    s32 xl, xh, xm; /* triangle edge x-coordinates */
    s32 DxLDy, DxHDy, DxMDy; /* triangle edge inverse-slopes */
    int tilenum, flip;

    i32 rgba[4]; /* RGBA color components */
    i32 d_rgba_dx[4]; /* RGBA delda per x-coordinate delta */
    i32 d_rgba_de[4]; /* RGBA delta along the edge */
    i32 d_rgba_dy[4]; /* RGBA delta per y-coordinate delta */
    i16 rgba_int[4], rgba_frac[4];
    i16 d_rgba_dx_int[4], d_rgba_dx_frac[4];
    i16 d_rgba_de_int[4], d_rgba_de_frac[4];
    i16 d_rgba_dy_int[4], d_rgba_dy_frac[4];

    i32 stwz[4];
    i32 d_stwz_dx[4];
    i32 d_stwz_de[4];
    i32 d_stwz_dy[4];
    i16 stwz_int[4], stwz_frac[4];
    i16 d_stwz_dx_int[4], d_stwz_dx_frac[4];
    i16 d_stwz_de_int[4], d_stwz_de_frac[4];
    i16 d_stwz_dy_int[4], d_stwz_dy_frac[4];

    i32 d_rgba_dxh[4];
    i32 d_stwz_dxh[4];
    i32 d_rgba_diff[4], d_stwz_diff[4];
    i32 xlr[2], xlr_inc[2];
    u8 xfrac;
#ifdef USE_SSE_SUPPORT
    __m128i xmm_d_rgba_de, xmm_d_stwz_de;
#endif
    int sign_dxhdy;
    int ycur, ylfar;
    int yllimit, yhlimit;
    int ldflag;
    int invaly;
    int curcross;
    int allover, allunder, curover, curunder;
    int allinval;
    register int j, k;
    const i32 clipxlshift = __clip.xl << 1;
    const i32 clipxhshift = __clip.xh << 1;

    base = cmd_cur + 0;
    setzero_si64(rgba_int);
    setzero_si64(rgba_frac);
    setzero_si64(d_rgba_dx_int);
    setzero_si64(d_rgba_dx_frac);
    setzero_si64(d_rgba_de_int);
    setzero_si64(d_rgba_de_frac);
    setzero_si64(d_rgba_dy_int);
    setzero_si64(d_rgba_dy_frac);
    setzero_si64(stwz_int);
    setzero_si64(stwz_frac);
    setzero_si64(d_stwz_dx_int);
    setzero_si64(d_stwz_dx_frac);
    setzero_si64(d_stwz_de_int);
    setzero_si64(d_stwz_de_frac);
    setzero_si64(d_stwz_dy_int);
    setzero_si64(d_stwz_dy_frac);

/*
 * Edge Coefficients
 */
    lft   = (cmd_data[base + 0].UW32[0] & 0x00800000) >> (55 - 32);
 /* unused  (cmd_data[base + 0].UW32[0] & 0x00400000) >> (54 - 32) */
    level = (cmd_data[base + 0].UW32[0] & 0x00380000) >> (51 - 32);
    tile  = (cmd_data[base + 0].UW32[0] & 0x00070000) >> (48 - 32);
    flip = lft;
    max_level = level;
    tilenum = tile;

    yl = (cmd_data[base + 0].UW32[0] & 0x0000FFFF) >> (32 - 32); /* & 0x3FFF */
    yl = SIGN(yl, 14);
    ym = (cmd_data[base + 0].UW32[1] & 0xFFFF0000) >> (16 -  0); /* & 0x3FFF */
    ym = SIGN(ym, 14);
    yh = (cmd_data[base + 0].UW32[1] & 0x0000FFFF) >> ( 0 -  0); /* & 0x3FFF */
    yh = SIGN(yh, 14);

    xl = cmd_data[base + 1].UW32[0];
    xl = SIGN(xl, 30);
    DxLDy = cmd_data[base + 1].UW32[1];
    xh = cmd_data[base + 2].UW32[0];
    xh = SIGN(xh, 30);
    DxHDy = cmd_data[base + 2].UW32[1];
    xm = cmd_data[base + 3].UW32[0];
    xm = SIGN(xm, 30);
    DxMDy = cmd_data[base + 3].UW32[1];

/*
 * Shade Coefficients
 */
    if (shade == 0) /* branch unlikely */
        goto no_read_shade_coefficients;
#ifdef USE_MMX_DECODES
    *(__m64 *)rgba_int       = *(__m64 *)&cmd_data[base +  4];
    *(__m64 *)d_rgba_dx_int  = *(__m64 *)&cmd_data[base +  5];
    *(__m64 *)rgba_frac      = *(__m64 *)&cmd_data[base +  6];
    *(__m64 *)d_rgba_dx_frac = *(__m64 *)&cmd_data[base +  7];
    *(__m64 *)d_rgba_de_int  = *(__m64 *)&cmd_data[base +  8];
    *(__m64 *)d_rgba_dy_int  = *(__m64 *)&cmd_data[base +  9];
    *(__m64 *)d_rgba_de_frac = *(__m64 *)&cmd_data[base + 10];
    *(__m64 *)d_rgba_dy_frac = *(__m64 *)&cmd_data[base + 11];
    *(__m64 *)rgba_int       = _mm_shuffle_pi16(*(__m64 *)rgba_int, 0xB1);
    *(__m64 *)d_rgba_dx_int  = _mm_shuffle_pi16(*(__m64 *)d_rgba_dx_int, 0xB1);
    *(__m64 *)rgba_frac      = _mm_shuffle_pi16(*(__m64 *)rgba_frac, 0xB1);
    *(__m64 *)d_rgba_dx_frac = _mm_shuffle_pi16(*(__m64 *)d_rgba_dx_frac, 0xB1);
    *(__m64 *)d_rgba_de_int  = _mm_shuffle_pi16(*(__m64 *)d_rgba_de_int, 0xB1);
    *(__m64 *)d_rgba_dy_int  = _mm_shuffle_pi16(*(__m64 *)d_rgba_dy_int, 0xB1);
    *(__m64 *)d_rgba_de_frac = _mm_shuffle_pi16(*(__m64 *)d_rgba_de_frac, 0xB1);
    *(__m64 *)d_rgba_dy_frac = _mm_shuffle_pi16(*(__m64 *)d_rgba_dy_frac, 0xB1);
#else
    rgba_int[0] = (cmd_data[base + 4].UW32[0] >> 16) & 0xFFFF;
    rgba_int[1] = (cmd_data[base + 4].UW32[0] >>  0) & 0xFFFF;
    rgba_int[2] = (cmd_data[base + 4].UW32[1] >> 16) & 0xFFFF;
    rgba_int[3] = (cmd_data[base + 4].UW32[1] >>  0) & 0xFFFF;
    d_rgba_dx_int[0] = (cmd_data[base + 5].UW32[0] >> 16) & 0xFFFF;
    d_rgba_dx_int[1] = (cmd_data[base + 5].UW32[0] >>  0) & 0xFFFF;
    d_rgba_dx_int[2] = (cmd_data[base + 5].UW32[1] >> 16) & 0xFFFF;
    d_rgba_dx_int[3] = (cmd_data[base + 5].UW32[1] >>  0) & 0xFFFF;
    rgba_frac[0] = (cmd_data[base + 6].UW32[0] >> 16) & 0xFFFF;
    rgba_frac[1] = (cmd_data[base + 6].UW32[0] >>  0) & 0xFFFF;
    rgba_frac[2] = (cmd_data[base + 6].UW32[1] >> 16) & 0xFFFF;
    rgba_frac[3] = (cmd_data[base + 6].UW32[1] >>  0) & 0xFFFF;
    d_rgba_dx_frac[0] = (cmd_data[base + 7].UW32[0] >> 16) & 0xFFFF;
    d_rgba_dx_frac[1] = (cmd_data[base + 7].UW32[0] >>  0) & 0xFFFF;
    d_rgba_dx_frac[2] = (cmd_data[base + 7].UW32[1] >> 16) & 0xFFFF;
    d_rgba_dx_frac[3] = (cmd_data[base + 7].UW32[1] >>  0) & 0xFFFF;
    d_rgba_de_int[0] = (cmd_data[base + 8].UW32[0] >> 16) & 0xFFFF;
    d_rgba_de_int[1] = (cmd_data[base + 8].UW32[0] >>  0) & 0xFFFF;
    d_rgba_de_int[2] = (cmd_data[base + 8].UW32[1] >> 16) & 0xFFFF;
    d_rgba_de_int[3] = (cmd_data[base + 8].UW32[1] >>  0) & 0xFFFF;
    d_rgba_dy_int[0] = (cmd_data[base + 9].UW32[0] >> 16) & 0xFFFF;
    d_rgba_dy_int[1] = (cmd_data[base + 9].UW32[0] >>  0) & 0xFFFF;
    d_rgba_dy_int[2] = (cmd_data[base + 9].UW32[1] >> 16) & 0xFFFF;
    d_rgba_dy_int[3] = (cmd_data[base + 9].UW32[1] >>  0) & 0xFFFF;
    d_rgba_de_frac[0] = (cmd_data[base + 10].UW32[0] >> 16) & 0xFFFF;
    d_rgba_de_frac[1] = (cmd_data[base + 10].UW32[0] >>  0) & 0xFFFF;
    d_rgba_de_frac[2] = (cmd_data[base + 10].UW32[1] >> 16) & 0xFFFF;
    d_rgba_de_frac[3] = (cmd_data[base + 10].UW32[1] >>  0) & 0xFFFF;
    d_rgba_dy_frac[0] = (cmd_data[base + 11].UW32[0] >> 16) & 0xFFFF;
    d_rgba_dy_frac[1] = (cmd_data[base + 11].UW32[0] >>  0) & 0xFFFF;
    d_rgba_dy_frac[2] = (cmd_data[base + 11].UW32[1] >> 16) & 0xFFFF;
    d_rgba_dy_frac[3] = (cmd_data[base + 11].UW32[1] >>  0) & 0xFFFF;
#endif
    base += 8;
no_read_shade_coefficients:
    base -= 8;
#ifdef USE_MMX_DECODES
    *(__m64 *)(rgba + (0 ^ 2))
      = _mm_unpackhi_pi16(*(__m64 *)rgba_frac, *(__m64 *)rgba_int);
    *(__m64 *)(rgba + (2 ^ 2))
      = _mm_unpacklo_pi16(*(__m64 *)rgba_frac, *(__m64 *)rgba_int);
    *(__m64 *)(d_rgba_dx + (0 ^ 2))
      = _mm_unpackhi_pi16(*(__m64 *)d_rgba_dx_frac, *(__m64 *)d_rgba_dx_int);
    *(__m64 *)(d_rgba_dx + (2 ^ 2))
      = _mm_unpacklo_pi16(*(__m64 *)d_rgba_dx_frac, *(__m64 *)d_rgba_dx_int);
    *(__m64 *)(d_rgba_de + (0 ^ 2))
      = _mm_unpackhi_pi16(*(__m64 *)d_rgba_de_frac, *(__m64 *)d_rgba_de_int);
    *(__m64 *)(d_rgba_de + (2 ^ 2))
      = _mm_unpacklo_pi16(*(__m64 *)d_rgba_de_frac, *(__m64 *)d_rgba_de_int);
    *(__m64 *)(d_rgba_dy + (0 ^ 2))
      = _mm_unpackhi_pi16(*(__m64 *)d_rgba_dy_frac, *(__m64 *)d_rgba_dy_int);
    *(__m64 *)(d_rgba_dy + (2 ^ 2))
      = _mm_unpacklo_pi16(*(__m64 *)d_rgba_dy_frac, *(__m64 *)d_rgba_dy_int);
#else
    rgba[0] = (rgba_int[0] << 16) | (u16)(rgba_frac[0]);
    rgba[1] = (rgba_int[1] << 16) | (u16)(rgba_frac[1]);
    rgba[2] = (rgba_int[2] << 16) | (u16)(rgba_frac[2]);
    rgba[3] = (rgba_int[3] << 16) | (u16)(rgba_frac[3]);
    d_rgba_dx[0] = (d_rgba_dx_int[0] << 16) | (u16)(d_rgba_dx_frac[0]);
    d_rgba_dx[1] = (d_rgba_dx_int[1] << 16) | (u16)(d_rgba_dx_frac[1]);
    d_rgba_dx[2] = (d_rgba_dx_int[2] << 16) | (u16)(d_rgba_dx_frac[2]);
    d_rgba_dx[3] = (d_rgba_dx_int[3] << 16) | (u16)(d_rgba_dx_frac[3]);
    d_rgba_de[0] = (d_rgba_de_int[0] << 16) | (u16)(d_rgba_de_frac[0]);
    d_rgba_de[1] = (d_rgba_de_int[1] << 16) | (u16)(d_rgba_de_frac[1]);
    d_rgba_de[2] = (d_rgba_de_int[2] << 16) | (u16)(d_rgba_de_frac[2]);
    d_rgba_de[3] = (d_rgba_de_int[3] << 16) | (u16)(d_rgba_de_frac[3]);
    d_rgba_dy[0] = (d_rgba_dy_int[0] << 16) | (u16)(d_rgba_dy_frac[0]);
    d_rgba_dy[1] = (d_rgba_dy_int[1] << 16) | (u16)(d_rgba_dy_frac[1]);
    d_rgba_dy[2] = (d_rgba_dy_int[2] << 16) | (u16)(d_rgba_dy_frac[2]);
    d_rgba_dy[3] = (d_rgba_dy_int[3] << 16) | (u16)(d_rgba_dy_frac[3]);
#endif
/*
 * Texture Coefficients
 */
    if (texture == 0)
        goto no_read_texture_coefficients;
#ifdef USE_MMX_DECODES
    *(__m64 *)stwz_int       = *(__m64 *)&cmd_data[base + 12];
    *(__m64 *)d_stwz_dx_int  = *(__m64 *)&cmd_data[base + 13];
    *(__m64 *)stwz_frac      = *(__m64 *)&cmd_data[base + 14];
    *(__m64 *)d_stwz_dx_frac = *(__m64 *)&cmd_data[base + 15];
    *(__m64 *)d_stwz_de_int  = *(__m64 *)&cmd_data[base + 16];
    *(__m64 *)d_stwz_dy_int  = *(__m64 *)&cmd_data[base + 17];
    *(__m64 *)d_stwz_de_frac = *(__m64 *)&cmd_data[base + 18];
    *(__m64 *)d_stwz_dy_frac = *(__m64 *)&cmd_data[base + 19];
    *(__m64 *)stwz_int       = _mm_shuffle_pi16(*(__m64 *)stwz_int, 0xB1);
    *(__m64 *)d_stwz_dx_int  = _mm_shuffle_pi16(*(__m64 *)d_stwz_dx_int, 0xB1);
    *(__m64 *)stwz_frac      = _mm_shuffle_pi16(*(__m64 *)stwz_frac, 0xB1);
    *(__m64 *)d_stwz_dx_frac = _mm_shuffle_pi16(*(__m64 *)d_stwz_dx_frac, 0xB1);
    *(__m64 *)d_stwz_de_int  = _mm_shuffle_pi16(*(__m64 *)d_stwz_de_int, 0xB1);
    *(__m64 *)d_stwz_dy_int  = _mm_shuffle_pi16(*(__m64 *)d_stwz_dy_int, 0xB1);
    *(__m64 *)d_stwz_de_frac = _mm_shuffle_pi16(*(__m64 *)d_stwz_de_frac, 0xB1);
    *(__m64 *)d_stwz_dy_frac = _mm_shuffle_pi16(*(__m64 *)d_stwz_dy_frac, 0xB1);
#else
    stwz_int[0] = (cmd_data[base + 12].UW32[0] >> 16) & 0xFFFF;
    stwz_int[1] = (cmd_data[base + 12].UW32[0] >>  0) & 0xFFFF;
    stwz_int[2] = (cmd_data[base + 12].UW32[1] >> 16) & 0xFFFF;
 /* stwz_int[3] = (cmd_data[base + 12].UW32[1] >>  0) & 0xFFFF; */
    d_stwz_dx_int[0] = (cmd_data[base + 13].UW32[0] >> 16) & 0xFFFF;
    d_stwz_dx_int[1] = (cmd_data[base + 13].UW32[0] >>  0) & 0xFFFF;
    d_stwz_dx_int[2] = (cmd_data[base + 13].UW32[1] >> 16) & 0xFFFF;
 /* d_stwz_dx_int[3] = (cmd_data[base + 13].UW32[1] >>  0) & 0xFFFF; */
    stwz_frac[0] = (cmd_data[base + 14].UW32[0] >> 16) & 0xFFFF;
    stwz_frac[1] = (cmd_data[base + 14].UW32[0] >>  0) & 0xFFFF;
    stwz_frac[2] = (cmd_data[base + 14].UW32[1] >> 16) & 0xFFFF;
 /* stwz_frac[3] = (cmd_data[base + 14].UW32[1] >>  0) & 0xFFFF; */
    d_stwz_dx_frac[0] = (cmd_data[base + 15].UW32[0] >> 16) & 0xFFFF;
    d_stwz_dx_frac[1] = (cmd_data[base + 15].UW32[0] >>  0) & 0xFFFF;
    d_stwz_dx_frac[2] = (cmd_data[base + 15].UW32[1] >> 16) & 0xFFFF;
 /* d_stwz_dx_frac[3] = (cmd_data[base + 15].UW32[1] >>  0) & 0xFFFF; */
    d_stwz_de_int[0] = (cmd_data[base + 16].UW32[0] >> 16) & 0xFFFF;
    d_stwz_de_int[1] = (cmd_data[base + 16].UW32[0] >>  0) & 0xFFFF;
    d_stwz_de_int[2] = (cmd_data[base + 16].UW32[1] >> 16) & 0xFFFF;
 /* d_stwz_de_int[3] = (cmd_data[base + 16].UW32[1] >>  0) & 0xFFFF; */
    d_stwz_dy_int[0] = (cmd_data[base + 17].UW32[0] >> 16) & 0xFFFF;
    d_stwz_dy_int[1] = (cmd_data[base + 17].UW32[0] >>  0) & 0xFFFF;
    d_stwz_dy_int[2] = (cmd_data[base + 17].UW32[1] >> 16) & 0xFFFF;
 /* d_stwz_dy_int[3] = (cmd_data[base + 17].UW32[1] >>  0) & 0xFFFF; */
    d_stwz_de_frac[0] = (cmd_data[base + 18].UW32[0] >> 16) & 0xFFFF;
    d_stwz_de_frac[1] = (cmd_data[base + 18].UW32[0] >>  0) & 0xFFFF;
    d_stwz_de_frac[2] = (cmd_data[base + 18].UW32[1] >> 16) & 0xFFFF;
 /* d_stwz_de_frac[3] = (cmd_data[base + 18].UW32[1] >>  0) & 0xFFFF; */
    d_stwz_dy_frac[0] = (cmd_data[base + 19].UW32[0] >> 16) & 0xFFFF;
    d_stwz_dy_frac[1] = (cmd_data[base + 19].UW32[0] >>  0) & 0xFFFF;
    d_stwz_dy_frac[2] = (cmd_data[base + 19].UW32[1] >> 16) & 0xFFFF;
 /* d_stwz_dy_frac[3] = (cmd_data[base + 19].UW32[1] >>  0) & 0xFFFF; */
#endif
    base += 8;
no_read_texture_coefficients:
    base -= 8;

/*
 * Z-Buffer Coefficients
 */
    if (zbuffer == 0) /* branch unlikely */
        goto no_read_zbuffer_coefficients;
    stwz_int[3]       = (cmd_data[base + 20].UW32[0] >> 16) & 0xFFFF;
    stwz_frac[3]      = (cmd_data[base + 20].UW32[0] >>  0) & 0xFFFF;
    d_stwz_dx_int[3]  = (cmd_data[base + 20].UW32[1] >> 16) & 0xFFFF;
    d_stwz_dx_frac[3] = (cmd_data[base + 20].UW32[1] >>  0) & 0xFFFF;
    d_stwz_de_int[3]  = (cmd_data[base + 21].UW32[0] >> 16) & 0xFFFF;
    d_stwz_de_frac[3] = (cmd_data[base + 21].UW32[0] >>  0) & 0xFFFF;
    d_stwz_dy_int[3]  = (cmd_data[base + 21].UW32[1] >> 16) & 0xFFFF;
    d_stwz_dy_frac[3] = (cmd_data[base + 21].UW32[1] >>  0) & 0xFFFF;
    base += 8;
no_read_zbuffer_coefficients:
    base -= 8;
#ifdef USE_MMX_DECODES
    *(__m64 *)(stwz + (0 ^ 2))
      = _mm_unpackhi_pi16(*(__m64 *)stwz_frac, *(__m64 *)stwz_int);
    *(__m64 *)(stwz + (2 ^ 2))
      = _mm_unpacklo_pi16(*(__m64 *)stwz_frac, *(__m64 *)stwz_int);
    *(__m64 *)(d_stwz_dx + (0 ^ 2))
      = _mm_unpackhi_pi16(*(__m64 *)d_stwz_dx_frac, *(__m64 *)d_stwz_dx_int);
    *(__m64 *)(d_stwz_dx + (2 ^ 2))
      = _mm_unpacklo_pi16(*(__m64 *)d_stwz_dx_frac, *(__m64 *)d_stwz_dx_int);
    *(__m64 *)(d_stwz_de + (0 ^ 2))
      = _mm_unpackhi_pi16(*(__m64 *)d_stwz_de_frac, *(__m64 *)d_stwz_de_int);
    *(__m64 *)(d_stwz_de + (2 ^ 2))
      = _mm_unpacklo_pi16(*(__m64 *)d_stwz_de_frac, *(__m64 *)d_stwz_de_int);
    *(__m64 *)(d_stwz_dy + (0 ^ 2))
      = _mm_unpackhi_pi16(*(__m64 *)d_stwz_dy_frac, *(__m64 *)d_stwz_dy_int);
    *(__m64 *)(d_stwz_dy + (2 ^ 2))
      = _mm_unpacklo_pi16(*(__m64 *)d_stwz_dy_frac, *(__m64 *)d_stwz_dy_int);
#else
    stwz[0] = (stwz_int[0] << 16) | (u16)(stwz_frac[0]);
    stwz[1] = (stwz_int[1] << 16) | (u16)(stwz_frac[1]);
    stwz[2] = (stwz_int[2] << 16) | (u16)(stwz_frac[2]);
    stwz[3] = (stwz_int[3] << 16) | (u16)(stwz_frac[3]);
    d_stwz_dx[0] = (d_stwz_dx_int[0] << 16) | (u16)(d_stwz_dx_frac[0]);
    d_stwz_dx[1] = (d_stwz_dx_int[1] << 16) | (u16)(d_stwz_dx_frac[1]);
    d_stwz_dx[2] = (d_stwz_dx_int[2] << 16) | (u16)(d_stwz_dx_frac[2]);
    d_stwz_dx[3] = (d_stwz_dx_int[3] << 16) | (u16)(d_stwz_dx_frac[3]);
    d_stwz_de[0] = (d_stwz_de_int[0] << 16) | (u16)(d_stwz_de_frac[0]);
    d_stwz_de[1] = (d_stwz_de_int[1] << 16) | (u16)(d_stwz_de_frac[1]);
    d_stwz_de[2] = (d_stwz_de_int[2] << 16) | (u16)(d_stwz_de_frac[2]);
    d_stwz_de[3] = (d_stwz_de_int[3] << 16) | (u16)(d_stwz_de_frac[3]);
    d_stwz_dy[0] = (d_stwz_dy_int[0] << 16) | (u16)(d_stwz_dy_frac[0]);
    d_stwz_dy[1] = (d_stwz_dy_int[1] << 16) | (u16)(d_stwz_dy_frac[1]);
    d_stwz_dy[2] = (d_stwz_dy_int[2] << 16) | (u16)(d_stwz_dy_frac[2]);
    d_stwz_dy[3] = (d_stwz_dy_int[3] << 16) | (u16)(d_stwz_dy_frac[3]);
#endif
#ifdef USE_SSE_SUPPORT
    xmm_d_rgba_de = _mm_load_si128((__m128i *)d_rgba_de);
    xmm_d_stwz_de = _mm_load_si128((__m128i *)d_stwz_de);
#endif

/*
 * rest of edgewalker algorithm
 */
    spans_d_rgba[0] = d_rgba_dx[0] & ~0x0000001F;
    spans_d_rgba[1] = d_rgba_dx[1] & ~0x0000001F;
    spans_d_rgba[2] = d_rgba_dx[2] & ~0x0000001F;
    spans_d_rgba[3] = d_rgba_dx[3] & ~0x0000001F;
    spans_d_stwz[0] = d_stwz_dx[0] & ~0x0000001F;
    spans_d_stwz[1] = d_stwz_dx[1] & ~0x0000001F;
    spans_d_stwz[2] = d_stwz_dx[2] & ~0x0000001F;
    spans_d_stwz[3] = d_stwz_dx[3];

    spans_d_rgba_dy[0] = d_rgba_dy[0] >> 14;
    spans_d_rgba_dy[1] = d_rgba_dy[1] >> 14;
    spans_d_rgba_dy[2] = d_rgba_dy[2] >> 14;
    spans_d_rgba_dy[3] = d_rgba_dy[3] >> 14;
    spans_d_rgba_dy[0] = SIGN(spans_d_rgba_dy[0], 13);
    spans_d_rgba_dy[1] = SIGN(spans_d_rgba_dy[1], 13);
    spans_d_rgba_dy[2] = SIGN(spans_d_rgba_dy[2], 13);
    spans_d_rgba_dy[3] = SIGN(spans_d_rgba_dy[3], 13);

    spans_cd_rgba[0] = spans_d_rgba[0] >> 14;
    spans_cd_rgba[1] = spans_d_rgba[1] >> 14;
    spans_cd_rgba[2] = spans_d_rgba[2] >> 14;
    spans_cd_rgba[3] = spans_d_rgba[3] >> 14;
    spans_cd_rgba[0] = SIGN(spans_cd_rgba[0], 13);
    spans_cd_rgba[1] = SIGN(spans_cd_rgba[1], 13);
    spans_cd_rgba[2] = SIGN(spans_cd_rgba[2], 13);
    spans_cd_rgba[3] = SIGN(spans_cd_rgba[3], 13);
    spans_cdz = spans_d_stwz[3] >> 10;
    spans_cdz = SIGN(spans_cdz, 22);

    spans_d_stwz_dy[0] = d_stwz_dy[0] & ~0x00007FFF;
    spans_d_stwz_dy[1] = d_stwz_dy[1] & ~0x00007FFF;
    spans_d_stwz_dy[2] = d_stwz_dy[2] & ~0x00007FFF;
    spans_d_stwz_dy[3] = d_stwz_dy[3] >> 10;
    spans_d_stwz_dy[3] = SIGN(spans_d_stwz_dy[3], 22);

    d_stwz_dx_int[3] ^= (d_stwz_dx_int[3] < 0) ? ~0 : 0;
    d_stwz_dy_int[3] ^= (d_stwz_dy_int[3] < 0) ? ~0 : 0;
    spans_dzpix = normalize_dzpix(d_stwz_dx_int[3] + d_stwz_dy_int[3]);

    sign_dxhdy = (DxHDy < 0);
    if (sign_dxhdy ^ flip) /* !do_offset */
    {
        setzero_si128(d_rgba_diff);
        setzero_si128(d_stwz_diff);
    }
    else
    {
        i32 d_rgba_deh[4], d_stwz_deh[4];
        i32 d_rgba_dyh[4], d_stwz_dyh[4];

        d_rgba_deh[0] = d_rgba_de[0] & ~0x000001FF;
        d_rgba_deh[1] = d_rgba_de[1] & ~0x000001FF;
        d_rgba_deh[2] = d_rgba_de[2] & ~0x000001FF;
        d_rgba_deh[3] = d_rgba_de[3] & ~0x000001FF;
        d_stwz_deh[0] = d_stwz_de[0] & ~0x000001FF;
        d_stwz_deh[1] = d_stwz_de[1] & ~0x000001FF;
        d_stwz_deh[2] = d_stwz_de[2] & ~0x000001FF;
        d_stwz_deh[3] = d_stwz_de[3] & ~0x000001FF;

        d_rgba_dyh[0] = d_rgba_dy[0] & ~0x000001FF;
        d_rgba_dyh[1] = d_rgba_dy[1] & ~0x000001FF;
        d_rgba_dyh[2] = d_rgba_dy[2] & ~0x000001FF;
        d_rgba_dyh[3] = d_rgba_dy[3] & ~0x000001FF;
        d_stwz_dyh[0] = d_stwz_dy[0] & ~0x000001FF;
        d_stwz_dyh[1] = d_stwz_dy[1] & ~0x000001FF;
        d_stwz_dyh[2] = d_stwz_dy[2] & ~0x000001FF;
        d_stwz_dyh[3] = d_stwz_dy[3] & ~0x000001FF;

        d_rgba_diff[0] = d_rgba_deh[0] - d_rgba_dyh[0];
        d_rgba_diff[1] = d_rgba_deh[1] - d_rgba_dyh[1];
        d_rgba_diff[2] = d_rgba_deh[2] - d_rgba_dyh[2];
        d_rgba_diff[3] = d_rgba_deh[3] - d_rgba_dyh[3];
        d_rgba_diff[0] -= (d_rgba_diff[0] >> 2);
        d_rgba_diff[1] -= (d_rgba_diff[1] >> 2);
        d_rgba_diff[2] -= (d_rgba_diff[2] >> 2);
        d_rgba_diff[3] -= (d_rgba_diff[3] >> 2);
        d_stwz_diff[0] = d_stwz_deh[0] - d_stwz_dyh[0];
        d_stwz_diff[1] = d_stwz_deh[1] - d_stwz_dyh[1];
        d_stwz_diff[2] = d_stwz_deh[2] - d_stwz_dyh[2];
        d_stwz_diff[3] = d_stwz_deh[3] - d_stwz_dyh[3];
        d_stwz_diff[0] -= (d_stwz_diff[0] >> 2);
        d_stwz_diff[1] -= (d_stwz_diff[1] >> 2);
        d_stwz_diff[2] -= (d_stwz_diff[2] >> 2);
        d_stwz_diff[3] -= (d_stwz_diff[3] >> 2);
    }

    if (other_modes.cycle_type == CYCLE_TYPE_COPY)
    {
        setzero_si128(d_rgba_dxh);
        setzero_si128(d_stwz_dxh);
    }
    else
    {
        d_rgba_dxh[0] = (d_rgba_dx[0] >> 8) & ~0x00000001;
        d_rgba_dxh[1] = (d_rgba_dx[1] >> 8) & ~0x00000001;
        d_rgba_dxh[2] = (d_rgba_dx[2] >> 8) & ~0x00000001;
        d_rgba_dxh[3] = (d_rgba_dx[3] >> 8) & ~0x00000001;
        d_stwz_dxh[0] = (d_stwz_dx[0] >> 8) & ~0x00000001;
        d_stwz_dxh[1] = (d_stwz_dx[1] >> 8) & ~0x00000001;
        d_stwz_dxh[2] = (d_stwz_dx[2] >> 8) & ~0x00000001;
        d_stwz_dxh[3] = (d_stwz_dx[3] >> 8) & ~0x00000001;
    }

    ldflag = (sign_dxhdy ^ flip) ? 0 : 3;
    invaly = 1;
    yllimit = (yl - __clip.yl < 0) ? yl : __clip.yl; /* clip.yl always &= 0xFFF */

    ycur = yh & ~3;
    ylfar = yllimit | 3;
    if (yl >> 2 > ylfar >> 2)
        ylfar += 4;
    else if (yllimit >> 2 >= 0 && yllimit >> 2 < 1023)
        span[(yllimit >> 2) + 1].validline = 0;

    yhlimit = (yh - __clip.yh >= 0) ? yh : __clip.yh; /* clip.yh always &= 0xFFF */

    xlr_inc[0] = (DxMDy >> 2) & ~0x00000001;
    xlr_inc[1] = (DxHDy >> 2) & ~0x00000001;
    xlr[0] = xm & ~0x00000001;
    xlr[1] = xh & ~0x00000001;
    xfrac = (xlr[1] >> 8) & 0xFF;

    allover = 1;
    allunder = 1;
    curover = 0;
    curunder = 0;
    allinval = 1;

    for (k = ycur; k <= ylfar; k++)
    {
        static int minmax[2];
        int stickybit;
        int xlrsc[2];
        const int spix = k & 3;
        const int yhclose = yhlimit & ~3;

        if (k == ym)
        {
            xlr[0] = xl & ~0x00000001;
            xlr_inc[0] = (DxLDy >> 2) & ~0x00000001;
        }

        if (k < yhclose)
            { /* branch */ }
        else
        {
            invaly = (u32)(k - yhlimit)>>31 | (u32)~(k - yllimit)>>31;
            j = k >> 2;
            if (spix == 0)
            {
                minmax[1] = 0x000;
                minmax[0] = 0xFFF;
                allover = allunder = 1;
                allinval = 1;
            }

            stickybit = (xlr[1] & 0x00003FFF) - 1;
            stickybit = (u32)~(stickybit) >> 31; /* (stickybit >= 0) */
            xlrsc[1] = (xlr[1] >> 13)&0x1FFE | stickybit;
            curunder = !!(xlr[1] & 0x08000000);
            curunder = curunder | (u32)(xlrsc[1] - clipxhshift)>>31;
            xlrsc[1] = curunder ? clipxhshift : (xlr[1]>>13)&0x3FFE | stickybit;
            curover  = !!(xlrsc[1] & 0x00002000);
            xlrsc[1] = xlrsc[1] & 0x1FFF;
            curover |= (u32)~(xlrsc[1] - clipxlshift) >> 31;
            xlrsc[1] = curover ? clipxlshift : xlrsc[1];
            span[j].majorx[spix] = xlrsc[1] & 0x1FFF;
            allover &= curover;
            allunder &= curunder;

            stickybit = (xlr[0] & 0x00003FFF) - 1; /* xleft/2 & 0x1FFF */
            stickybit = (u32)~(stickybit) >> 31; /* (stickybit >= 0) */
            xlrsc[0] = (xlr[0] >> 13)&0x1FFE | stickybit;
            curunder = !!(xlr[0] & 0x08000000);
            curunder = curunder | (u32)(xlrsc[0] - clipxhshift)>>31;
            xlrsc[0] = curunder ? clipxhshift : (xlr[0]>>13)&0x3FFE | stickybit;
            curover  = !!(xlrsc[0] & 0x00002000);
            xlrsc[0] &= 0x1FFF;
            curover |= (u32)~(xlrsc[0] - clipxlshift) >> 31;
            xlrsc[0] = curover ? clipxlshift : xlrsc[0];
            span[j].minorx[spix] = xlrsc[0] & 0x1FFF;
            allover &= curover;
            allunder &= curunder;

            curcross = ((xlr[1 - flip]&0x0FFFC000 ^ 0x08000000)
                     <  (xlr[0 + flip]&0x0FFFC000 ^ 0x08000000));
            invaly |= curcross;
            span[j].invalyscan[spix] = invaly;
            allinval &= invaly;
            if (invaly != 0)
                { /* branch */ }
            else
            {
                xlrsc[0] = (xlrsc[0] >> 3) & 0xFFF;
                xlrsc[1] = (xlrsc[1] >> 3) & 0xFFF;
                minmax[0]
                  = (xlrsc[flip - 0] < minmax[0]) ? xlrsc[flip - 0] : minmax[0];
                minmax[1]
                  = (xlrsc[1 - flip] > minmax[1]) ? xlrsc[1 - flip] : minmax[1];
            }

            if (spix == ldflag)
#ifdef USE_SSE_SUPPORT
            {
                __m128i xmm_frac;
                __m128i delta_x_high, delta_diff;
                __m128i prod_hi, prod_lo;
                __m128i result;

                span[j].unscrx = xlr[1] >> 16;
                xfrac = (xlr[1] >> 8) & 0xFF;
                xmm_frac = _mm_set1_epi32(xfrac);

                delta_x_high = _mm_load_si128((__m128i *)d_rgba_dxh);
                prod_lo = _mm_mul_epu32(delta_x_high, xmm_frac);
                delta_x_high = _mm_srli_epi64(delta_x_high, 32);
                prod_hi = _mm_mul_epu32(delta_x_high, xmm_frac);
                prod_lo = _mm_shuffle_epi32(prod_lo, _MM_SHUFFLE(3, 1, 2, 0));
                prod_hi = _mm_shuffle_epi32(prod_hi, _MM_SHUFFLE(3, 1, 2, 0));
                delta_x_high = _mm_unpacklo_epi32(prod_lo, prod_hi);

                delta_diff = _mm_load_si128((__m128i *)d_rgba_diff);
                result = _mm_load_si128((__m128i *)rgba);
                result = _mm_srli_epi32(result, 9);
                result = _mm_slli_epi32(result, 9);
                result = _mm_add_epi32(result, delta_diff);
                result = _mm_sub_epi32(result, delta_x_high);
                result = _mm_srli_epi32(result, 10);
                result = _mm_slli_epi32(result, 10);
                _mm_store_si128((__m128i *)span[j].rgba, result);

                delta_x_high = _mm_load_si128((__m128i *)d_stwz_dxh);
                prod_lo = _mm_mul_epu32(delta_x_high, xmm_frac);
                delta_x_high = _mm_srli_epi64(delta_x_high, 32);
                prod_hi = _mm_mul_epu32(delta_x_high, xmm_frac);
                prod_lo = _mm_shuffle_epi32(prod_lo, _MM_SHUFFLE(3, 1, 2, 0));
                prod_hi = _mm_shuffle_epi32(prod_hi, _MM_SHUFFLE(3, 1, 2, 0));
                delta_x_high = _mm_unpacklo_epi32(prod_lo, prod_hi);

                delta_diff = _mm_load_si128((__m128i *)d_stwz_diff);
                result = _mm_load_si128((__m128i *)stwz);
                result = _mm_srli_epi32(result, 9);
                result = _mm_slli_epi32(result, 9);
                result = _mm_add_epi32(result, delta_diff);
                result = _mm_sub_epi32(result, delta_x_high);
                result = _mm_srli_epi32(result, 10);
                result = _mm_slli_epi32(result, 10);
                _mm_store_si128((__m128i *)span[j].stwz, result);
            }
#else
            {
                span[j].unscrx = xlr[1] >> 16;
                xfrac = (xlr[1] >> 8) & 0xFF;
                span[j].rgba[0]
                  = ((rgba[0] & ~0x1FF) + d_rgba_diff[0] - xfrac*d_rgba_dxh[0])
                  & ~0x000003FF;
                span[j].rgba[1]
                  = ((rgba[1] & ~0x1FF) + d_rgba_diff[1] - xfrac*d_rgba_dxh[1])
                  & ~0x000003FF;
                span[j].rgba[2]
                  = ((rgba[2] & ~0x1FF) + d_rgba_diff[2] - xfrac*d_rgba_dxh[2])
                  & ~0x000003FF;
                span[j].rgba[3]
                  = ((rgba[3] & ~0x1FF) + d_rgba_diff[3] - xfrac*d_rgba_dxh[3])
                  & ~0x000003FF;
                span[j].stwz[0]
                  = ((stwz[0] & ~0x1FF) + d_stwz_diff[0] - xfrac*d_stwz_dxh[0])
                  & ~0x000003FF;
                span[j].stwz[1]
                  = ((stwz[1] & ~0x1FF) + d_stwz_diff[1] - xfrac*d_stwz_dxh[1])
                  & ~0x000003FF;
                span[j].stwz[2]
                  = ((stwz[2] & ~0x1FF) + d_stwz_diff[2] - xfrac*d_stwz_dxh[2])
                  & ~0x000003FF;
                span[j].stwz[3]
                  = ((stwz[3] & ~0x1FF) + d_stwz_diff[3] - xfrac*d_stwz_dxh[3])
                  & ~0x000003FF;
            }
#endif
            if (spix == 3)
            {
                const int invalidline = (sckeepodd ^ j) & scfield
                                      | (allinval | allover | allunder);
                span[j].lx = minmax[flip - 0];
                span[j].rx = minmax[1 - flip];
                span[j].validline = invalidline ^ 1;
            }
        }
        if (spix == 3)
        {
            rgba[0] += d_rgba_de[0];
            rgba[1] += d_rgba_de[1];
            rgba[2] += d_rgba_de[2];
            rgba[3] += d_rgba_de[3];
            stwz[0] += d_stwz_de[0];
            stwz[1] += d_stwz_de[1];
            stwz[2] += d_stwz_de[2];
            stwz[3] += d_stwz_de[3];
        }
        xlr[0] += xlr_inc[0];
        xlr[1] += xlr_inc[1];
    }
    render_spans(yhlimit >> 2, yllimit >> 2, tilenum, flip);
#ifdef USE_MMX_DECODES
    _mm_empty();
#endif
    return;
}

STRICTINLINE static u16 normalize_dzpix(u16 sum)
{
    register int count;

    if (sum & 0xC000)
        return 0x8000;
    if (sum == 0x0000)
        return 0x0001;
    if (sum == 0x0001)
        return 0x0003;
    for (count = 0x2000; count > 0; count >>= 1)
        if (sum & count)
            return (count << 1);
    return 0;
}

NOINLINE static void render_spans(
    int yhlimit, int yllimit, int tilenum, int flip)
{
    const unsigned int cycle_type = other_modes.cycle_type & 03;

    if (other_modes.f.stalederivs == 0)
        { /* branch */ }
    else
    {
        deduce_derivatives();
        other_modes.f.stalederivs = 0;
    }
    fbread1_ptr = fbread_func[fb_size];
    fbread2_ptr = fbread2_func[fb_size];
    fbwrite_ptr = fbwrite_func[fb_size];

#ifdef _DEBUG
    ++render_cycle_mode_counts[cycle_type];
#endif

    if (cycle_type & 02)
        if (cycle_type & 01)
            render_spans_fill(yhlimit, yllimit, flip);
        else
            render_spans_copy(yhlimit, yllimit, tilenum, flip);
    else
        if (cycle_type & 01)
            render_spans_2cycle_ptr(yhlimit, yllimit, tilenum, flip);
        else
            render_spans_1cycle_ptr(yhlimit, yllimit, tilenum, flip);
    return;
}

#ifdef USE_SSE_SUPPORT
INLINE __m128i mm_mullo_epi32_seh(__m128i dest, __m128i src)
{ /* source scalar element, shift half:  src[0] == src[1] && src[2] == src[3] */
    __m128i prod_m, prod_n;
/* "SEH" also means "Half of the source elements are equal to each other." */
    prod_n = _mm_srli_epi64(dest, 32);
    prod_m = _mm_mul_epu32(dest, src);
    prod_n = _mm_mul_epu32(prod_n, src);
#ifdef ANOTHER_WAY_TO_UNPACK_THE_PRODUCTS
    return mm_unpacklo_epi64_hz(prod_m, prod_n);
#else
    prod_m = _mm_shuffle_epi32(prod_m, _MM_SHUFFLE(3, 1, 2, 0));
    prod_n = _mm_shuffle_epi32(prod_n, _MM_SHUFFLE(3, 1, 2, 0));
    prod_m = _mm_unpacklo_epi32(prod_m, prod_n);
    return (prod_m);
#endif
}
#endif
