/*
 *
 * tdfxfb.c
 *
 * Author: Hannu Mallat <hmallat@cc.hut.fi>
 *
 * Copyright � 1999 Hannu Mallat
 * All rights reserved
 *
 * Created      : Thu Sep 23 18:17:43 1999, hmallat
 * Last modified: Tue Nov  2 21:19:47 1999, hmallat
 *
 * Lots of the information here comes from the Daryll Strauss' Banshee 
 * patches to the XF86 server, and the rest comes from the 3dfx
 * Banshee specification. I'm very much indebted to Daryll for his
 * work on the X server.
 *
 * Voodoo3 support was contributed Harold Oga. Lots of additions
 * (proper acceleration, 24 bpp, hardware cursor) and bug fixes by Attila
 * Kesmarki. Thanks guys!
 * 
 * While I _am_ grateful to 3Dfx for releasing the specs for Banshee,
 * I do wish the next version is a bit more complete. Without the XF86
 * patches I couldn't have gotten even this far... for instance, the
 * extensions to the VGA register set go completely unmentioned in the
 * spec! Also, lots of references are made to the 'SST core', but no
 * spec is publicly available, AFAIK.
 *
 * The structure of this driver comes pretty much from the Permedia
 * driver by Ilario Nardinocchi, which in turn is based on skeletonfb.
 * 
 * TODO:
 * - support for 16/32 bpp needs fixing (funky bootup penguin)
 * - multihead support (basically need to support an array of fb_infos)
 * - banshee and voodoo3 now supported -- any others? afaik, the original
 *   voodoo was a 3d-only card, so we won't consider that. what about
 *   voodoo2?
 * - support other architectures (PPC, Alpha); does the fact that the VGA
 *   core can be accessed only thru I/O (not memory mapped) complicate
 *   things?
 *
 * Version history:
 *
 * 0.1.3 (released 1999-11-02) added Attila's panning support, code
 *			       reorg, hwcursor address page size alignment
 *                             (for mmaping both frame buffer and regs),
 *                             and my changes to get rid of hardcoded
 *                             VGA i/o register locations (uses PCI
 *                             configuration info now)
 * 0.1.2 (released 1999-10-19) added Attila Kesmarki's bug fixes and
 *                             improvements
 * 0.1.1 (released 1999-10-07) added Voodoo3 support by Harold Oga.
 * 0.1.0 (released 1999-10-06) initial version
 *
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/nvram.h>
#include <asm/io.h>
#include <linux/timer.h>

#ifdef CONFIG_MTRR
#include <asm/mtrr.h>
#endif

#include <video/tdfx.h>
#include <linux/spinlock.h>

//#define TDFXFB_DEBUG 
#ifdef TDFXFB_DEBUG
#define DPRINTK(a,b...) printk(KERN_DEBUG "fb: %s: " a, __FUNCTION__ , ## b)
#else
#define DPRINTK(a,b...)
#endif 

#define BANSHEE_MAX_PIXCLOCK 270000.0
#define VOODOO3_MAX_PIXCLOCK 300000.0
#define VOODOO5_MAX_PIXCLOCK 350000.0

static struct fb_fix_screeninfo tdfx_fix __initdata = {
    "3Dfx", (unsigned long) NULL, 0, FB_TYPE_PACKED_PIXELS, 0,
    FB_VISUAL_PSEUDOCOLOR, 0, 1, 1, 0, (unsigned long) NULL, 0,
    FB_ACCEL_3DFX_BANSHEE
};

static struct fb_var_screeninfo tdfx_var __initdata = {
    /* "640x480, 8 bpp @ 60 Hz */
    640, 480, 640, 1024, 0, 0, 8, 0,
    {0, 8, 0}, {0, 8, 0}, {0, 8, 0}, {0, 0, 0},
    0, FB_ACTIVATE_NOW, -1, -1, FB_ACCELF_TEXT,
    39722, 40, 24, 32, 11, 96, 2,
    0, FB_VMODE_NONINTERLACED
};

/*
 *  Frame buffer device API
 */
int tdfxfb_init(void);
void tdfxfb_setup(char *options, int *ints); 

static int tdfxfb_check_var(struct fb_var_screeninfo *var, struct fb_info *fb); 
static int tdfxfb_set_par(struct fb_info *info); 
static int tdfxfb_cursor(struct fb_info *info, struct fbcursor *cursor);
static int  tdfxfb_setcolreg(u_int regno, u_int red, u_int green, u_int blue, 
			     u_int transp, struct fb_info *info); 
static int tdfxfb_blank(int blank, struct fb_info *info); 
static int tdfxfb_pan_display(struct fb_var_screeninfo *var,struct fb_info *fb);
static void tdfxfb_fillrect(struct fb_info *info, int x, int y,
			    unsigned int width, unsigned int height, 
	  	            unsigned long color, int rop);
static void tdfxfb_copyarea(struct fb_info *info, int sx, int sy,  
			    unsigned int width, unsigned int height,
		            int dx, int dy); 
static void tdfxfb_imageblit(struct fb_info *info, struct fb_image *image); 
static int tdfxfb_ioctl(struct inode* inode, struct file *file, u_int cmd, 
			u_long arg, struct fb_info *info); 

static struct fb_ops tdfxfb_ops = {
	owner:		THIS_MODULE,
	fb_check_var:	tdfxfb_check_var,
	fb_set_par:	tdfxfb_set_par,
	//fb_cursor:	tdfxfb_cursor,
	fb_setcolreg:	tdfxfb_setcolreg,
	fb_blank:	tdfxfb_blank,
	fb_pan_display:	tdfxfb_pan_display,
	fb_fillrect:	tdfxfb_fillrect,
	fb_copyarea:	tdfxfb_copyarea,
	fb_imageblit:	tdfxfb_imageblit,
	fb_ioctl:	tdfxfb_ioctl,
};

/*
 * do_xxx: Hardware-specific functions
 */
static void do_flashcursor(unsigned long ptr);
static u32 do_calc_pll(int freq, int *freq_out);
static void  do_write_regs(struct banshee_reg *reg);
static unsigned long do_lfb_size(void);

/*
 * Driver data 
 */

static struct fb_info fb_info;
static struct tdfx_par default_par;
static u32 psuedo_palette[17];

static int  nopan   = 0;
static int  nowrap  = 1;      // not implemented (yet)
static int  inverse = 0;
#ifdef CONFIG_MTRR
static int  nomtrr = 0;
#endif
static int  nohwcursor = 0;
static const char *mode_option __initdata = NULL;

/* ------------------------------------------------------------------------- 
 *                      Hardware-specific funcions
 * ------------------------------------------------------------------------- */

#ifdef VGA_REG_IO 
static inline  u8 vga_inb(u32 reg) { return inb(reg); }
static inline u16 vga_inw(u32 reg) { return inw(reg); }
static inline u16 vga_inl(u32 reg) { return inl(reg); }

static inline void vga_outb(u32 reg,  u8 val) { outb(val, reg); }
static inline void vga_outw(u32 reg, u16 val) { outw(val, reg); }
static inline void vga_outl(u32 reg, u32 val) { outl(val, reg); }
#else
static inline  u8 vga_inb(u32 reg) { 
  return inb(default_par.iobase + reg - 0x300); 
}
static inline u16 vga_inw(u32 reg) { 
  return inw(default_par.iobase + reg - 0x300); 
}
static inline u16 vga_inl(u32 reg) { 
  return inl(default_par.iobase + reg - 0x300); 
}

static inline void vga_outb(u32 reg,  u8 val) { 
  outb(val, default_par.iobase + reg - 0x300); 
}
static inline void vga_outw(u32 reg, u16 val) { 
  outw(val, default_par.iobase + reg - 0x300); 
}
static inline void vga_outl(u32 reg, u32 val) { 
  outl(val, default_par.iobase + reg - 0x300); 
}
#endif

static inline void gra_outb(u32 idx, u8 val) {
  vga_outb(GRA_I, idx); vga_outb(GRA_D, val);
}

static inline u8 gra_inb(u32 idx) {
  vga_outb(GRA_I, idx); return vga_inb(GRA_D);
}

static inline void seq_outb(u32 idx, u8 val) {
  vga_outb(SEQ_I, idx); vga_outb(SEQ_D, val);
}

static inline u8 seq_inb(u32 idx) {
  vga_outb(SEQ_I, idx); return vga_inb(SEQ_D);
}

static inline void crt_outb(u32 idx, u8 val) {
  vga_outb(CRT_I, idx); vga_outb(CRT_D, val);
}

static inline u8 crt_inb(u32 idx) {
  vga_outb(CRT_I, idx); return vga_inb(CRT_D);
}

static inline void att_outb(u32 idx, u8 val) {
  unsigned char tmp;
  tmp = vga_inb(IS1_R);
  vga_outb(ATT_IW, idx);
  vga_outb(ATT_IW, val);
}

static inline u8 att_inb(u32 idx) {
  unsigned char tmp;
  tmp = vga_inb(IS1_R);
  vga_outb(ATT_IW, idx);
  return vga_inb(ATT_IW);
}

static inline void vga_disable_video(void) {
  unsigned char s;
  s = seq_inb(0x01) | 0x20;
  seq_outb(0x00, 0x01);
  seq_outb(0x01, s);
  seq_outb(0x00, 0x03);
}

static inline void vga_enable_video(void) {
  unsigned char s;
  s = seq_inb(0x01) & 0xdf;
  seq_outb(0x00, 0x01);
  seq_outb(0x01, s);
  seq_outb(0x00, 0x03);
}

static inline void vga_disable_palette(void) {
  vga_inb(IS1_R);
  vga_outb(ATT_IW, 0x00);
}

static inline void vga_enable_palette(void) {
  vga_inb(IS1_R);
  vga_outb(ATT_IW, 0x20);
}

static inline u32 tdfx_inl(unsigned int reg) {
  return readl(default_par.regbase_virt + reg);
}

static inline void tdfx_outl(unsigned int reg, u32 val) {
  writel(val, default_par.regbase_virt + reg);
}

static inline void banshee_make_room(int size) {
  while((tdfx_inl(STATUS) & 0x1f) < size);
}
 
static inline void banshee_wait_idle(void) {
  int i = 0;

  banshee_make_room(1);
  tdfx_outl(COMMAND_3D, COMMAND_3D_NOP);

  while(1) {
    i = (tdfx_inl(STATUS) & STATUS_BUSY) ? 0 : i + 1;
    if(i == 3) break;
  }
}

/*
 * Set the color of a palette entry in 8bpp mode 
 */
static inline void do_setpalentry(unsigned regno, u32 c) {  
   banshee_make_room(2); tdfx_outl(DACADDR,  regno); tdfx_outl(DACDATA,  c); }

/*
 * Invert the hardware cursor image (timerfunc)  
 */
static void do_flashcursor(unsigned long ptr)
{
   struct tdfx_par *par = (struct tdfx_par *) ptr;
   unsigned long flags;	

   spin_lock_irqsave(par->DAClock, flags);
   banshee_make_room(1);
   tdfx_outl(VIDPROCCFG, tdfx_inl(VIDPROCCFG) ^ VIDCFG_HWCURSOR_ENABLE );
   par->hwcursor.timer.expires=jiffies+HZ/2;
   add_timer(&par->hwcursor.timer);
   spin_unlock_irqrestore(par->DAClock, flags);
}

static u32 do_calc_pll(int freq, int* freq_out) 
{
  int m, n, k, best_m, best_n, best_k, f_cur, best_error;
  int fref = 14318;
  
  /* this really could be done with more intelligence --
     255*63*4 = 64260 iterations is silly */
  best_error = freq;
  best_n = best_m = best_k = 0;
  for(n = 1; n < 256; n++) {
    for(m = 1; m < 64; m++) {
      for(k = 0; k < 4; k++) {
	f_cur = fref*(n + 2)/(m + 2)/(1 << k);
	if(abs(f_cur - freq) < best_error) {
	  best_error = abs(f_cur-freq);
	  best_n = n;
	  best_m = m;
	  best_k = k;
	}
      }
    }
  }
  n = best_n;
  m = best_m;
  k = best_k;
  *freq_out = fref*(n + 2)/(m + 2)/(1 << k);

  return (n << 8) | (m << 2) | k;
}

static void do_write_regs(struct banshee_reg* reg) 
{
  int i;

  banshee_wait_idle();

  tdfx_outl(MISCINIT1, tdfx_inl(MISCINIT1) | 0x01);

  crt_outb(0x11, crt_inb(0x11) & 0x7f); /* CRT unprotect */

  banshee_make_room(3);
  tdfx_outl(VGAINIT1,      reg->vgainit1 &  0x001FFFFF);
  tdfx_outl(VIDPROCCFG,    reg->vidcfg   & ~0x00000001);
#if 0
  tdfx_outl(PLLCTRL1,      reg->mempll);
  tdfx_outl(PLLCTRL2,      reg->gfxpll);
#endif
  tdfx_outl(PLLCTRL0,      reg->vidpll);

  vga_outb(MISC_W, reg->misc[0x00] | 0x01);

  for(i = 0; i < 5; i++)
    seq_outb(i, reg->seq[i]);

  for(i = 0; i < 25; i++)
    crt_outb(i, reg->crt[i]);

  for(i = 0; i < 9; i++)
    gra_outb(i, reg->gra[i]);

  for(i = 0; i < 21; i++)
    att_outb(i, reg->att[i]);

  crt_outb(0x1a, reg->ext[0]);
  crt_outb(0x1b, reg->ext[1]);

  vga_enable_palette();
  vga_enable_video();

  banshee_make_room(11);
  tdfx_outl(VGAINIT0,      reg->vgainit0);
  tdfx_outl(DACMODE,       reg->dacmode);
  tdfx_outl(VIDDESKSTRIDE, reg->stride);
  if (nohwcursor) {
     tdfx_outl(HWCURPATADDR,  0);
  } else {
     tdfx_outl(HWCURPATADDR,  reg->curspataddr);
     tdfx_outl(HWCURC0,       reg->cursc0);
     tdfx_outl(HWCURC1,       reg->cursc1);
     tdfx_outl(HWCURLOC,      reg->cursloc);
  }
   
  tdfx_outl(VIDSCREENSIZE, reg->screensize);
  tdfx_outl(VIDDESKSTART,  reg->startaddr);
  tdfx_outl(VIDPROCCFG,    reg->vidcfg);
  tdfx_outl(VGAINIT1,      reg->vgainit1);  
  tdfx_outl(MISCINIT0,    reg->miscinit0);	

  banshee_make_room(8);
  tdfx_outl(SRCBASE,         reg->srcbase);
  tdfx_outl(DSTBASE,         reg->dstbase);
  tdfx_outl(COMMANDEXTRA_2D, 0);
  tdfx_outl(CLIP0MIN,        0);
  tdfx_outl(CLIP0MAX,        0x0fff0fff);
  tdfx_outl(CLIP1MIN,        0);
  tdfx_outl(CLIP1MAX,        0x0fff0fff);
  tdfx_outl(SRCXY, 0);

  banshee_wait_idle();
}

static unsigned long do_lfb_size(void) 
{
  u32 draminit0 = 0;
  u32 draminit1 = 0;
  u32 miscinit1 = 0;
  u32 lfbsize   = 0;
  int sgram_p     = 0;

  draminit0 = tdfx_inl(DRAMINIT0);  
  draminit1 = tdfx_inl(DRAMINIT1);
  
  if ((fb_info.dev == PCI_DEVICE_ID_3DFX_BANSHEE) ||
      (fb_info.dev == PCI_DEVICE_ID_3DFX_VOODOO3)) {             	 
	sgram_p = (draminit1 & DRAMINIT1_MEM_SDRAM) ? 0 : 1;
  
	lfbsize = sgram_p ?
    		(((draminit0 & DRAMINIT0_SGRAM_NUM)  ? 2 : 1) * 
     		((draminit0 & DRAMINIT0_SGRAM_TYPE) ? 8 : 4) * 1024 * 1024) :
    		16 * 1024 * 1024;
  } else {
     /* Voodoo4/5 */
     u32 chips, psize, banks;

     chips = ((draminit0 & (1 << 26)) == 0) ? 4 : 8;
     psize = 1 << ((draminit0 & 0x38000000) >> 28);
     banks = ((draminit0 & (1 << 30)) == 0) ? 2 : 4;
     lfbsize = chips * psize * banks;
     lfbsize <<= 20;
  }                 
  /* disable block writes for SDRAM (why?) */
  miscinit1 = tdfx_inl(MISCINIT1);
  miscinit1 |= sgram_p ? 0 : MISCINIT1_2DBLOCK_DIS;
  miscinit1 |= MISCINIT1_CLUT_INV;

  banshee_make_room(1); 
  tdfx_outl(MISCINIT1, miscinit1);

  return lfbsize;
}

/* ------------------------------------------------------------------------- */

static int tdfxfb_check_var(struct fb_var_screeninfo *var,struct fb_info *info) 
{
  struct tdfx_par *par = (struct tdfx_par *) info->par; 
  u32 lpitch;

  if(var->bits_per_pixel != 8  && var->bits_per_pixel != 16 &&
     var->bits_per_pixel != 24 && var->bits_per_pixel != 32) {
    DPRINTK("depth not supported: %u\n", var->bits_per_pixel);
    return -EINVAL;
  }

  if(var->xres != var->xres_virtual) {
    DPRINTK("virtual x resolution != physical x resolution not supported\n");
    return -EINVAL;
  }

  if(var->yres > var->yres_virtual) {
    DPRINTK("virtual y resolution < physical y resolution not possible\n");
    return -EINVAL;
  }

  if(var->xoffset) {
    DPRINTK("xoffset not supported\n");
    return -EINVAL;
  }

  /* fixme: does Voodoo3 support interlace? Banshee doesn't */
  if((var->vmode & FB_VMODE_MASK) == FB_VMODE_INTERLACED) {
    DPRINTK("interlace not supported\n");
    return -EINVAL;
  }

  var->xres        = (var->xres + 15) & ~15; /* could sometimes be 8 */
  lpitch           = var->xres * ((var->bits_per_pixel + 7)>>3);
  
  if (var->xres < 320 || var->xres > 2048) {
    DPRINTK("width not supported: %u\n", var->xres);
    return -EINVAL;
  }
  
  if(var->yres < 200 || var->yres > 2048) {
    DPRINTK("height not supported: %u\n", var->yres);
    return -EINVAL;
  }
  
  if (lpitch * var->yres_virtual > info->fix.smem_len) {
     DPRINTK("no memory for screen (%ux%ux%u)\n",
     var->xres, var->yres_virtual, var->bits_per_pixel);
     return -EINVAL;
  }
  
  if (PICOS2KHZ(var->pixclock) > par->max_pixclock) {
    DPRINTK("pixclock too high (%uKHz)\n", PICOS2KHZ(var->pixclock));
    return -EINVAL;
  }

  switch(var->bits_per_pixel) {
  case 8:
    var->red.length = var->green.length = var->blue.length = 8;
    break;
  case 16:
    var->red.offset   = 11;
    var->red.length   = 5;
    var->green.offset = 5;
    var->green.length = 6;
    var->blue.offset  = 0;
    var->blue.length  = 5;
    break;
  case 24:
    var->red.offset=16;
    var->green.offset=8;
    var->blue.offset=0;
    var->red.length = var->green.length = var->blue.length = 8;
  case 32:
    var->red.offset   = 16;
    var->green.offset = 8;
    var->blue.offset  = 0;
    var->red.length = var->green.length = var->blue.length = 8;
    break;
  }
  var->height = var->width = -1;
  return 0;
}

static int tdfxfb_set_par(struct fb_info *info)
{
  struct tdfx_par *par = (struct tdfx_par *) info->par;	
  struct banshee_reg reg;
  u32 cpp;
  u32 hd, hs, he, ht, hbs, hbe;
  u32 vd, vs, ve, vt, vbs, vbe;
  u32 wd;
  int fout;
  int freq;
  
  info->cmap.len = (info->var.bits_per_pixel == 8) ? 256 : 16;
  par->baseline  = 0;
 
  memset(&reg, 0, sizeof(reg));
  cpp = (info->var.bits_per_pixel + 7)/8;
  
  wd = (info->var.xres >> 3) - 1;

  hd  = (info->var.xres >> 3) - 1;
  hs  = ((info->var.xres + info->var.right_margin) >> 3) - 1;
  he  = ((info->var.xres + info->var.right_margin + info->var.hsync_len) >> 3) - 1;
  ht  = ((info->var.xres + info->var.right_margin + info->var.hsync_len + info->var.left_margin) >> 3) - 1;
  hbs = hd;
  hbe = ht;

  vd  = info->var.yres - 1;
  vs  = info->var.yres + info->var.lower_margin - 1;
  ve  = info->var.yres + info->var.lower_margin + info->var.vsync_len - 1;
  vt  = info->var.yres + info->var.lower_margin + info->var.vsync_len + info->var.upper_margin - 2;
  vbs = vd;
  vbe = vt;
  
  /* this is all pretty standard VGA register stuffing */
  reg.misc[0x00] = 
    0x0f |
    (info->var.xres < 400 ? 0xa0 :
     info->var.xres < 480 ? 0x60 :
     info->var.xres < 768 ? 0xe0 : 0x20);
     
  reg.gra[0x00] = 0x00;
  reg.gra[0x01] = 0x00;
  reg.gra[0x02] = 0x00;
  reg.gra[0x03] = 0x00;
  reg.gra[0x04] = 0x00;
  reg.gra[0x05] = 0x40;
  reg.gra[0x06] = 0x05;
  reg.gra[0x07] = 0x0f;
  reg.gra[0x08] = 0xff;

  reg.att[0x00] = 0x00;
  reg.att[0x01] = 0x01;
  reg.att[0x02] = 0x02;
  reg.att[0x03] = 0x03;
  reg.att[0x04] = 0x04;
  reg.att[0x05] = 0x05;
  reg.att[0x06] = 0x06;
  reg.att[0x07] = 0x07;
  reg.att[0x08] = 0x08;
  reg.att[0x09] = 0x09;
  reg.att[0x0a] = 0x0a;
  reg.att[0x0b] = 0x0b;
  reg.att[0x0c] = 0x0c;
  reg.att[0x0d] = 0x0d;
  reg.att[0x0e] = 0x0e;
  reg.att[0x0f] = 0x0f;
  reg.att[0x10] = 0x41;
  reg.att[0x11] = 0x00;
  reg.att[0x12] = 0x0f;
  reg.att[0x13] = 0x00;
  reg.att[0x14] = 0x00;

  reg.seq[0x00] = 0x03;
  reg.seq[0x01] = 0x01; /* fixme: clkdiv2? */
  reg.seq[0x02] = 0x0f;
  reg.seq[0x03] = 0x00;
  reg.seq[0x04] = 0x0e;

  reg.crt[0x00] = ht - 4;
  reg.crt[0x01] = hd;
  reg.crt[0x02] = hbs;
  reg.crt[0x03] = 0x80 | (hbe & 0x1f);
  reg.crt[0x04] = hs;
  reg.crt[0x05] = 
    ((hbe & 0x20) << 2) | 
    (he & 0x1f);
  reg.crt[0x06] = vt;
  reg.crt[0x07] = 
    ((vs & 0x200) >> 2) |
    ((vd & 0x200) >> 3) |
    ((vt & 0x200) >> 4) |
    0x10 |
    ((vbs & 0x100) >> 5) |
    ((vs  & 0x100) >> 6) |
    ((vd  & 0x100) >> 7) |
    ((vt  & 0x100) >> 8);
  reg.crt[0x08] = 0x00;
  reg.crt[0x09] = 
    0x40 |
    ((vbs & 0x200) >> 4);
  reg.crt[0x0a] = 0x00;
  reg.crt[0x0b] = 0x00;
  reg.crt[0x0c] = 0x00;
  reg.crt[0x0d] = 0x00;
  reg.crt[0x0e] = 0x00;
  reg.crt[0x0f] = 0x00;
  reg.crt[0x10] = vs;
  reg.crt[0x11] = 
    (ve & 0x0f) |
    0x20;
  reg.crt[0x12] = vd;
  reg.crt[0x13] = wd;
  reg.crt[0x14] = 0x00;
  reg.crt[0x15] = vbs;
  reg.crt[0x16] = vbe + 1; 
  reg.crt[0x17] = 0xc3;
  reg.crt[0x18] = 0xff;
  
  /* Banshee's nonvga stuff */
  reg.ext[0x00] = (((ht  & 0x100) >> 8) | 
		   ((hd  & 0x100) >> 6) |
		   ((hbs & 0x100) >> 4) |
		   ((hbe &  0x40) >> 1) |
		   ((hs  & 0x100) >> 2) |
		   ((he  &  0x20) << 2)); 
  reg.ext[0x01] = (((vt  & 0x400) >> 10) |
		   ((vd  & 0x400) >>  8) | 
		   ((vbs & 0x400) >>  6) |
		   ((vbe & 0x400) >>  4));
  
  reg.vgainit0 = 
    VGAINIT0_8BIT_DAC     |
    VGAINIT0_EXT_ENABLE   |
    VGAINIT0_WAKEUP_3C3   |
    VGAINIT0_ALT_READBACK |
    VGAINIT0_EXTSHIFTOUT;
  reg.vgainit1 = tdfx_inl(VGAINIT1) & 0x1fffff;

  reg.vidcfg = 
    VIDCFG_VIDPROC_ENABLE |
    VIDCFG_DESK_ENABLE    |
    VIDCFG_CURS_X11 |
    ((cpp - 1) << VIDCFG_PIXFMT_SHIFT) |
    (cpp != 1 ? VIDCFG_CLUT_BYPASS : 0);
 
  /* Setup the cursor */	 
  par->hwcursor.enable=reg.vidcfg | VIDCFG_HWCURSOR_ENABLE;
  par->hwcursor.disable=reg.vidcfg;
   
  reg.cursloc   = 0;
   
  reg.cursc0    = 0; 
  reg.cursc1    = 0xffffff;
   
  reg.curspataddr = (unsigned long) info->cursor.image;   

  reg.stride    = info->var.xres * cpp;
  reg.startaddr = par->baseline*reg.stride;
  reg.srcbase   = reg.startaddr;
  reg.dstbase   = reg.startaddr;

  /* PLL settings */
  freq = PICOS2KHZ(info->var.pixclock);

  reg.dacmode &= ~DACMODE_2X;
  reg.vidcfg  &= ~VIDCFG_2X;
  if(freq > par->max_pixclock/2) {
    freq = freq > par->max_pixclock ? par->max_pixclock : freq;
    reg.dacmode |= DACMODE_2X;
    reg.vidcfg  |= VIDCFG_2X;
  }
  reg.vidpll = do_calc_pll(freq, &fout);
#if 0
  reg.mempll = do_calc_pll(..., &fout);
  reg.gfxpll = do_calc_pll(..., &fout);
#endif

  reg.screensize = info->var.xres | (info->var.yres << 12);
  reg.vidcfg &= ~VIDCFG_HALF_MODE;
  reg.miscinit0 = tdfx_inl(MISCINIT0);

#if defined(__BIG_ENDIAN)
  switch (info->var.bits_per_pixel) {
    case 8:
      reg.miscinit0 &= ~(1 << 30);
      reg.miscinit0 &= ~(1 << 31);
      break;
    case 16:
      reg.miscinit0 |= (1 << 30);
      reg.miscinit0 |= (1 << 31);
      break;
    case 24:
    case 32:
      reg.miscinit0 |= (1 << 30);
      reg.miscinit0 &= ~(1 << 31);
      break;
  }
#endif             
  do_write_regs(&reg);
  memcpy(info->par, par, sizeof(struct tdfx_par));

  /* Now change fb_fix_screeninfo according to changes in par */
  info->fix.line_length = info->var.xres * ((info->var.bits_per_pixel + 7)>>3);
  info->fix.visual = (info->var.bits_per_pixel == 8) 
  	  	                 ? FB_VISUAL_PSEUDOCOLOR
                     		 : FB_VISUAL_TRUECOLOR;
  return 0;	
}

static int tdfxfb_cursor(struct fb_info *info, struct fbcursor *cursor)
{
   struct tdfx_par *par = (struct tdfx_par *) info->par;
   unsigned long flags;
    
   /* If the cursor is not be changed this means either we want the current
    * cursor state (if enable is set) or we want to query what we can do with
    * the cursor (if enable is not set) */
   if (nohwcursor || !cursor->set) return 0; 

   /* Too large of a cursor :-( */	
   if (cursor->size.x > 64 || cursor->size.y > 64) 
	return -ENXIO;	

   /* If we are going to be changing things we should disable 
      the cursor first */
   if (info->cursor.enable) {
	spin_lock_irqsave(par->DAClock, flags);
   	info->cursor.enable = 0;
    	del_timer(&(par->hwcursor.timer));
    	tdfx_outl(VIDPROCCFG, par->hwcursor.disable); 
	spin_unlock_irqrestore(par->DAClock, flags);
   }
 
   /* Disable the Cursor */	
   if ((cursor->set && FB_CUR_SETCUR) && !cursor->enable)
	return 0;

   /* fix cursor color - XFree86 forgets to restore it properly */
   if (cursor->set && FB_CUR_SETCMAP) {
	unsigned long bg_color, fg_color;

	cursor->cmap.len = 2; 	/* Voodoo 3+ only support 2 color cursors */
	fg_color = ((cursor->cmap.red[cursor->cmap.start] << 16) | 
		    (cursor->cmap.green[cursor->cmap.start] << 8)  |
		    (cursor->cmap.blue[cursor->cmap.start]));
   	bg_color = ((cursor->cmap.red[cursor->cmap.start+1] << 16) |
                    (cursor->cmap.green[cursor->cmap.start+1] << 8) | 
		    (cursor->cmap.blue[cursor->cmap.start+1]));
	fb_copy_cmap(&cursor->cmap, &info->cursor.cmap, 0);   
	spin_lock_irqsave(par->DAClock, flags);
	banshee_make_room(2);
	tdfx_outl(HWCURC0, bg_color);
	tdfx_outl(HWCURC1, fg_color);
   	spin_unlock_irqrestore(par->DAClock, flags);
   }

   if (cursor->set && FB_CUR_SETPOS) {
	int x, y;

   	x = cursor->pos.x;
   	y = cursor->pos.y;
   	y -= info->var.yoffset;
	info->cursor.pos.x = x;
	info->cursor.pos.y = y;
	x += 63;
	y += 63;	
   	spin_lock_irqsave(par->DAClock, flags);
	banshee_make_room(1);
	tdfx_outl(HWCURLOC, (y << 16) + x);
   	spin_unlock_irqrestore(par->DAClock, flags);
   }	
  
   /* Not supported so we fake it */	 
   if (cursor->set && FB_CUR_SETHOT) {
	info->cursor.hot.x = cursor->hot.x;
	info->cursor.hot.y = cursor->hot.y;
   }	  
	
   if (cursor->set && FB_CUR_SETSHAPE) {
	/* Voodoo 3 and above cards use 2 monochrome cursor patterns.
         *   The reason is so the card can fetch 8 words at a time 
         * and are stored on chip for use for the next 8 scanlines. 
         * This reduces the number of times for access to draw the  
         * cursor for each screen refresh. 
	 *    Each pattern is a bitmap of 64 bit wide and 64 bit high 
	 * (total of 8192 bits or 1024 Kbytes). The two patterns are
         * stored in such a way that pattern 0 always resides in the
         * lower half (least significant 64 bits) of a 128 bit word 
	 * and pattern 1 the upper half. If you examine the data of 
         * the cursor image the graphics card uses then from the 
         * begining you see line one of pattern 0, line one of 
	 * pattern 1, line two of pattern 0, line two of pattern 1, 
         * etc etc. The linear stride for the cursor is always 16 bytes
         * (128 bits) which is the maximum cursor width times two for
         * the two monochrome patterns. 
	 */
	u8 *cursorbase = (u8 *) info->cursor.image;
	char *bitmap = cursor->image;
	char *mask = cursor->mask;
	int i, j, k, h = 0;

	for (i = 0; i < 64; i++) {
		if (i < cursor->size.y) {
			j = (cursor->size.x + 7) >> 3;
			k = 8 - j;			

			for (;j > 0; j--) {
				/* Pattern 0. Copy the cursor bitmap to it */
				fb_writeb(*bitmap, cursorbase + h);	
				bitmap++;
				/* Pattern 1. Copy the cursor mask to it */
				fb_writeb(*mask, cursorbase + h + 8);
				mask++;
				h++;
			}
			for (;k > 0; k--) {
				fb_writeb(0, cursorbase + h);
				fb_writeb(~0, cursorbase + h + 8);
				h++;
			}
		} else {
			fb_writel(0, cursorbase + h);
			fb_writel(0, cursorbase + h + 4);
			fb_writel(~0, cursorbase + h + 8);
			fb_writel(~0, cursorbase + h + 12);
			h += 16;
		}	
	}
   }
   /* Turn the cursor on */
   cursor->enable = 1;
   info->cursor = *cursor;
   mod_timer(&par->hwcursor.timer, jiffies+HZ/2);
   spin_lock_irqsave(par->DAClock, flags);
   banshee_make_room(1);
   tdfx_outl(VIDPROCCFG, par->hwcursor.enable);
   spin_unlock_irqrestore(par->DAClock, flags);
   return 0;     
}

static int tdfxfb_setcolreg(unsigned regno, unsigned red, unsigned green,  
			    unsigned blue,unsigned transp,struct fb_info *info) 
{
   u32 rgbcol;
   
   if (regno >= info->cmap.len) return 1;
   
   switch (info->fix.visual) {
   	case FB_VISUAL_PSEUDOCOLOR:
      		rgbcol=(((u32)red   & 0xff00) << 8) |
		(((u32)green & 0xff00) << 0) |
		(((u32)blue  & 0xff00) >> 8);
      		do_setpalentry(regno, rgbcol);
      		break;
	/* Truecolor has no hardware color palettes. */
    	case FB_VISUAL_TRUECOLOR:
		rgbcol = (red << info->var.red.offset) |
                         (green << info->var.green.offset) |
                         (blue << info->var.blue.offset) |
                         (transp << info->var.transp.offset);
                if (info->var.bits_per_pixel <= 16)
                        ((u16*)(info->pseudo_palette))[regno] = rgbcol;
                else
                        ((u32*)(info->pseudo_palette))[regno] = rgbcol;
      		break;
    	default:
      		DPRINTK("bad depth %u\n", info->var.bits_per_pixel);
      		break;
   }
   return 0;
}

/* 0 unblank, 1 blank, 2 no vsync, 3 no hsync, 4 off */
static int tdfxfb_blank(int blank, struct fb_info *info)
{ 
  u32 dacmode, state = 0, vgablank = 0;

  dacmode = tdfx_inl(DACMODE);

  switch (blank) {
  case 0: /* Screen: On; HSync: On, VSync: On */    
    state    = 0;
    vgablank = 0;
    break;
  case 1: /* Screen: Off; HSync: On, VSync: On */
    state    = 0;
    vgablank = 1;
    break;
  case 2: /* Screen: Off; HSync: On, VSync: Off */
    state    = BIT(3);
    vgablank = 1;
    break;
  case 3: /* Screen: Off; HSync: Off, VSync: On */
    state    = BIT(1);
    vgablank = 1;
    break;
  case 4: /* Screen: Off; HSync: Off, VSync: Off */
    state    = BIT(1) | BIT(3);
    vgablank = 1;
    break;
  }

  dacmode &= ~(BIT(1) | BIT(3));
  dacmode |= state;
  banshee_make_room(1); 
  tdfx_outl(DACMODE, dacmode);
  if(vgablank) 
    vga_disable_video();
  else
    vga_enable_video();
  return 0;
}

/*   
 * Set the starting position of the visible screen to var->yoffset
 */   
static int tdfxfb_pan_display(struct fb_var_screeninfo *var,
			      struct fb_info *info) 
{
  u32 addr;  	

  if(nopan)                return -EINVAL;
  if(var->xoffset)         return -EINVAL;
  if(var->yoffset > var->yres_virtual)   return -EINVAL;
  if(nowrap && 
     (var->yoffset + var->yres > var->yres_virtual)) return -EINVAL;

  addr = var->yoffset * info->fix.line_length;
  banshee_make_room(1);
  tdfx_outl(VIDDESKSTART, addr);
   
  info->var.xoffset = var->xoffset;
  info->var.yoffset = var->yoffset; 
  return 0;
}
/*
 * FillRect 2D command (solidfill or invert (via ROP_XOR))   
 */
static void tdfxfb_fillrect(struct fb_info *info, int x, int y,
			    unsigned int width, unsigned int height, 
	  	            unsigned long color, int rop)
{
   u32 bpp = info->var.bits_per_pixel;
   u32 stride = info->fix.line_length;
   u32 fmt= stride | ((bpp+((bpp==8) ? 0 : 8)) << 13); 
   int tdfx_rop;
   	
   if (rop == ROP_COPY) 
	tdfx_rop = TDFX_ROP_COPY;
   else 			 
	tdfx_rop = TDFX_ROP_XOR;

   banshee_make_room(5);
   tdfx_outl(DSTFORMAT, fmt);
   tdfx_outl(COLORFORE, color);
   tdfx_outl(COMMAND_2D, COMMAND_2D_FILLRECT | (tdfx_rop << 24));
   tdfx_outl(DSTSIZE,    width | (height << 16));
   tdfx_outl(LAUNCH_2D,  x | (y << 16));
   banshee_wait_idle();
}

/*
 * Screen-to-Screen BitBlt 2D command (for the bmove fb op.) 
 */
static void tdfxfb_copyarea(struct fb_info *info, int sx, int sy,  
	 	            unsigned int width, unsigned int height,
		            int dx, int dy) 
{
   u32 bpp = info->var.bits_per_pixel;
   u32 stride = info->fix.line_length;
   u32 blitcmd = COMMAND_2D_S2S_BITBLT | (TDFX_ROP_COPY << 24);
   u32 fmt= stride | ((bpp+((bpp==8) ? 0 : 8)) << 13); 
   
   if (sx <= dx) {
     //-X 
     blitcmd |= BIT(14);
     sx += width-1;
     dx += width-1;
   }
   if (sy <= dy) {
     //-Y  
     blitcmd |= BIT(15);
     sy += height-1;
     dy += height-1;
   }
   
   banshee_make_room(6);

   tdfx_outl(SRCFORMAT, fmt);
   tdfx_outl(DSTFORMAT, fmt);
   tdfx_outl(COMMAND_2D, blitcmd); 
   tdfx_outl(DSTSIZE,   width | (height << 16));
   tdfx_outl(DSTXY,     dx | (dy << 16));
   tdfx_outl(LAUNCH_2D, sx | (sy << 16)); 
   banshee_wait_idle();
}

static void tdfxfb_imageblit(struct fb_info *info, struct fb_image *pixmap) 
{
   int size = pixmap->height*((pixmap->width*pixmap->depth + 7)>>3);
   int i, stride = info->fix.line_length;
   u32 bpp = info->var.bits_per_pixel;
   u32 dstfmt = stride | ((bpp+((bpp==8) ? 0 : 8)) << 13); 
   u8 *chardata = (u8 *) pixmap->data;
   u32 srcfmt;

   if (pixmap->depth == 1) {
   	banshee_make_room(8 + ((size + 3) >> 2));
   	tdfx_outl(COLORFORE, pixmap->fg_color);
   	tdfx_outl(COLORBACK, pixmap->bg_color);
  	srcfmt = 0x400000;
   } else {
   	banshee_make_room(6 + ((size + 3) >> 2));
	srcfmt = 0xBEEFDEAD;
   }	

   tdfx_outl(SRCXY,     0);
   tdfx_outl(DSTXY,     pixmap->x | (pixmap->y << 16));
   tdfx_outl(COMMAND_2D, COMMAND_2D_H2S_BITBLT | (TDFX_ROP_COPY << 24));
   tdfx_outl(SRCFORMAT, srcfmt);
   tdfx_outl(DSTFORMAT, dstfmt);
   tdfx_outl(DSTSIZE,   pixmap->width | (pixmap->height << 16));

   /* Send four bytes at a time of data */	
   for (i = (size >> 2) ; i > 0; i--) { 
	tdfx_outl(LAUNCH_2D,*(u32*)chardata);
	chardata += 4; 
   }	

   /* Send the leftovers now */	
   i = size%4;	
   switch (i) {
	case 0: break;
      	case 1:  tdfx_outl(LAUNCH_2D,*chardata); break;
      	case 2:  tdfx_outl(LAUNCH_2D,*(u16*)chardata); break;
      	case 3:  tdfx_outl(LAUNCH_2D,*(u16*)chardata | ((chardata[3]) << 24)); break;
   }
   banshee_wait_idle();
}

static int tdfxfb_ioctl(struct inode *inode, struct file *file, u_int cmd, 
			u_long arg, struct fb_info *info)
{ 
/* These IOCTLs ar just for testing only... 
   switch (cmd) {
    case 0x4680: 
      nowrap=nopan=0;
      return 0;
    case 0x4681:
      nowrap=nopan=1;
      return 0;
   }*/
   return -EINVAL;
}

static void tdfxfb_hwcursor_init(struct fb_info *info) 
{
   unsigned int start;

   start = (info->fix.smem_len-1024) & PAGE_MASK;
   info->fix.smem_len = start; 
   info->cursor.image = info->screen_base + info->fix.smem_len;
   printk("tdfxfb: reserving 1024 bytes for the hwcursor at %p\n",
	   info->cursor.image);
}

int __init tdfxfb_init(void) 
{
  struct pci_dev *pdev = NULL;
 	 
  while ((pdev = pci_find_device(PCI_VENDOR_ID_3DFX, PCI_ANY_ID, pdev))) {
    if(((pdev->class >> 16) == PCI_BASE_CLASS_DISPLAY) &&
       ((pdev->device == PCI_DEVICE_ID_3DFX_BANSHEE) ||
	(pdev->device == PCI_DEVICE_ID_3DFX_VOODOO3) || 
	(pdev->device == PCI_DEVICE_ID_3DFX_VOODOO5))) {
      /* Configure the default fb_fix_screeninfo first */
      memset(&fb_info, 0, sizeof(fb_info));
      fb_info.par = &default_par;
      switch (pdev->device) {
	case PCI_DEVICE_ID_3DFX_BANSHEE:	
         	strcat(tdfx_fix.id, " Banshee");
         	default_par.max_pixclock = BANSHEE_MAX_PIXCLOCK;
		break;
      	case PCI_DEVICE_ID_3DFX_VOODOO3:
         	strcat(tdfx_fix.id, " Voodoo3");
         	default_par.max_pixclock = VOODOO3_MAX_PIXCLOCK;
     		break;
	case PCI_DEVICE_ID_3DFX_VOODOO5:
         	strcat(tdfx_fix.id, " Voodoo5");
         	default_par.max_pixclock = VOODOO5_MAX_PIXCLOCK;
     		break;
      }

      tdfx_fix.mmio_start = pci_resource_start(pdev, 0);
      tdfx_fix.mmio_len = 1 << 24;
      default_par.regbase_virt = ioremap_nocache(tdfx_fix.mmio_start, 1 << 24);
      if(!default_par.regbase_virt) {
	printk("fb: Can't remap %s register area.\n", tdfx_fix.id);
	return -ENXIO;
      }
      
      tdfx_fix.smem_start = pci_resource_start(pdev, 1);
      if(!(tdfx_fix.smem_len = do_lfb_size())) {
	iounmap(default_par.regbase_virt);
	printk("fb: Can't count %s memory.\n", tdfx_fix.id);
	return -ENXIO;
      }
      fb_info.screen_base = ioremap_nocache(tdfx_fix.smem_start, 
						    tdfx_fix.smem_len);
      if(!fb_info.screen_base) {
	printk("fb: Can't remap %s framebuffer.\n", tdfx_fix.id);
	iounmap(default_par.regbase_virt);
	return -ENXIO;
      }

      default_par.iobase = pci_resource_start(pdev, 2);
      
      printk("fb: %s memory = %dK\n", tdfx_fix.id, tdfx_fix.smem_len >> 10);

#ifdef CONFIG_MTRR
      if (!nomtrr) {
          default_par.mtrr_idx = mtrr_add(tdfx_fix.smem_start, 
					  tdfx_fix.smem_len,
	  			      	  MTRR_TYPE_WRCOMB, 1);
	    printk("fb: MTRR's  turned on\n");
      }
#endif

      /* clear framebuffer memory */
      memset_io(fb_info.screen_base, 0, tdfx_fix.smem_len);
	
      tdfx_fix.ypanstep    = nopan ? 0 : 1;
      tdfx_fix.ywrapstep   = nowrap ? 0 : 1;
   
      fb_info.node       = -1;
      fb_info.fbops      = &tdfxfb_ops;
      fb_info.fix 	 = tdfx_fix; 	
      fb_info.pseudo_palette = psuedo_palette;
      fb_info.flags      = FBINFO_FLAG_DEFAULT;
 
      if(!mode_option || 
	 !fb_find_mode(&fb_info.var, &fb_info, mode_option, NULL, 0, NULL, 8)) 
	fb_info.var = tdfx_var;
     
      if (!nohwcursor) {
	tdfxfb_hwcursor_init(&fb_info);
      	init_timer(&default_par.hwcursor.timer);
      	default_par.hwcursor.timer.function = do_flashcursor; 
      	default_par.hwcursor.timer.data = (unsigned long)(&default_par);
      	spin_lock_init(default_par->DAClock);
      }	
      fb_info.cursor.set = 0;
      fb_info.cursor.enable = 0;	 

      if (register_framebuffer(&fb_info) < 0) {
	printk("tdfxfb: can't register framebuffer\n");
	return -ENXIO;
      }

      printk("fb%d: %s frame buffer device\n", GET_FB_IDX(fb_info.node), 
	     fb_info.fix.id);
    }
  }
  return 0; 
}

/**
 *	tdfxfb_exit - Driver cleanup
 *
 *	Releases all resources allocated during the
 *	course of the driver's lifetime.
 *
 *	FIXME - do results of fb_alloc_cmap need disposal?
 */
static void __exit tdfxfb_exit (void)
{
	struct tdfx_par *par = (struct tdfx_par *) fb_info.par;

	unregister_framebuffer(&fb_info);
	del_timer_sync(&par->hwcursor.timer);

#ifdef CONFIG_MTRR
	if (!nomtrr) {
       	  mtrr_del(par->mtrr_idx, fb_info.fix.smem_start, fb_info.fix.smem_len);
	  printk("fb: MTRR's  turned off\n");
        }
#endif
	iounmap(par->regbase_virt);
	iounmap(fb_info.screen_base);
}

MODULE_AUTHOR("Hannu Mallat <hmallat@cc.hut.fi>");
MODULE_DESCRIPTION("3Dfx framebuffer device driver");

#ifdef MODULE
module_init(tdfxfb_init);
#endif
module_exit(tdfxfb_exit);


#ifndef MODULE
void tdfxfb_setup(char *options, int *ints)
{ 
  char* this_opt;

  if(!options || !*options)
    return;

  while(this_opt = strsep(&options, ",")) {
    if(!*this_opt)
      continue;
    if(!strcmp(this_opt, "inverse")) {
      inverse = 1;
      fb_invert_cmaps();
    } else if(!strcmp(this_opt, "nopan")) {
      nopan = 1;
    } else if(!strcmp(this_opt, "nowrap")) {
      nowrap = 1;
    } else if (!strcmp(this_opt, "nohwcursor")) {
      nohwcursor = 1;
#ifdef CONFIG_MTRR
    } else if (!strcmp(this_opt, "nomtrr")) {
      nomtrr = 1;
#endif
    } else {
      mode_option = this_opt;
    }
  } 
}
#endif
