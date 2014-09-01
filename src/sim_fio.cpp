/* sim_fio.c: simulator file I/O library

   Copyright (c) 1993-2008, Robert M Supnik

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
   ROBERT M SUPNIK BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
   IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   Except as contained in this notice, the name of Robert M Supnik shall not be
   used in advertising or otherwise to promote the sale, use or other dealings
   in this Software without prior written authorization from Robert M Supnik.

   03-Jun-11    MP      Simplified VMS 64b support and made more portable
   02-Feb-11    MP      Added sim_fsize_ex and sim_fsize_name_ex returning t_addr
                        Added export of sim_buf_copy_swapped and sim_buf_swap_data
   28-Jun-07    RMS     Added VMS IA64 support (from Norm Lastovica)
   10-Jul-06    RMS     Fixed linux conditionalization (from Chaskiel Grundman)
   15-May-06    RMS     Added sim_fsize_name
   21-Apr-06    RMS     Added FreeBSD large file support (from Mark Martinec)
   19-Nov-05    RMS     Added OS/X large file support (from Peter Schorn)
   16-Aug-05    RMS     Fixed C++ declaration and cast problems
   17-Jul-04    RMS     Fixed bug in optimized sim_fread (reported by Scott Bailey)
   26-May-04    RMS     Optimized sim_fread (suggested by John Dundas)
   02-Jan-04    RMS     Split out from SCP

   This library includes:

   sim_finit         -       initialize package
   sim_fopen         -       open file
   sim_fread         -       endian independent read (formerly fxread)
   sim_write         -       endian independent write (formerly fxwrite)
   sim_fseek         -       extended (>32b) seek (formerly fseek_ext)
   sim_fsize         -       get file size
   sim_fsize_name    -       get file size of named file
   sim_fsize_ex      -       get file size as a t_addr
   sim_fsize_name_ex -       get file size as a t_addr of named file
   sim_buf_copy_swapped -    copy data swapping elements along the way
   sim_buf_swap_data -       swap data elements inplace in buffer

   sim_fopen and sim_fseek are OS-dependent.  The other routines are not.
   sim_fsize is always a 32b routine (it is used only with small capacity random
   access devices like fixed head disks and DECtapes).
*/

#include "sim_defs.h"

// static unsigned char sim_flip[FLIP_SIZE];
int32 sim_end = 1;                                      /* 1 = little */
AUTO_TLS(flip_tls_key);

/* OS-independent, endian independent binary I/O package

   For consistency, all binary data read and written by the simulator
   is stored in little endian data order.  That is, in a multi-byte
   data item, the bytes are written out right to left, low order byte
   to high order byte.  On a big endian host, data is read and written
   from high byte to low byte.  Consequently, data written on a little
   endian system must be byte reversed to be usable on a big endian
   system, and vice versa.

   These routines are analogs of the standard C runtime routines
   fread and fwrite.  If the host is little endian, or the data items
   are size char, then the calls are passed directly to fread or
   fwrite.  Otherwise, these routines perform the necessary byte swaps.
   Sim_fread swaps in place, sim_fwrite uses an intermediate buffer.
*/

int32 sim_finit (void)
{
    union {int32 i; char c[sizeof (int32)]; } end_test;

    end_test.i = 1;                                         /* test endian-ness */
    sim_end = end_test.c[0];
    return sim_end;
}

void sim_buf_swap_data (void *bptr, size_t size, size_t count)
{
    uint32 j;
    int32 k;
    unsigned char by, *sptr, *dptr;

    if (sim_end || (count == 0) || (size == sizeof (char)))
        return;
    for (j = 0, dptr = sptr = (unsigned char *) bptr;       /* loop on items */
         j < count; j++) { 
        for (k = (int32)(size - 1); k >= (((int32) size + 1) / 2); k--) {
            by = *sptr;                                     /* swap end-for-end */
            *sptr++ = *(dptr + k);
            *(dptr + k) = by;
            }
        sptr = dptr = dptr + size;                          /* next item */
        }
}

size_t sim_fread (void *bptr, size_t size, size_t count, SMP_FILE *fptr)
{
    size_t c;

    if ((size == 0) || (count == 0))                        /* check arguments */
        return 0;
    c = fread (bptr, size, count, fptr);                    /* read buffer */
    if (sim_end || (size == sizeof (char)) || (c == 0))     /* le, byte, or err? */
        return c;                                           /* done */
    sim_buf_swap_data (bptr, size, count);
    return c;
}

void sim_buf_copy_swapped (void *dbuf, void *sbuf, size_t size, size_t count)
{
    size_t j;
    int32 k;
    unsigned char *sptr = (unsigned char *)sbuf;
    unsigned char *dptr = (unsigned char *)dbuf;

    if (sim_end || (size == sizeof (char)))
    {
        memcpy (dptr, sptr, size * count);
        return;
    }
    for (j = 0; j < count; j++) {                           /* loop on items */
        for (k = (int32)(size - 1); k >= 0; k--)
            *(dptr + k) = *sptr++;
        dptr = dptr + size;
    }
}

size_t sim_fwrite (void *bptr, size_t size, size_t count, SMP_FILE *fptr)
{
    size_t c, nelem, nbuf, lcnt, total;
    int32 i;
    unsigned char *sptr;

    if (size == 0 || count == 0)                            /* check arguments */
        return 0;
    if (sim_end || size == sizeof (char))                   /* le or byte? */
        return fwrite (bptr, size, count, fptr);            /* done */

    t_byte* sim_flip = (t_byte*) tls_get_value(flip_tls_key);
    if (sim_flip == NULL)
    {
        sim_flip = (t_byte*) malloc(FLIP_SIZE);
        if (sim_flip == NULL)
        {
            smp_printf ("\nUnable to allocate I/O buffer: out of memory\n");
            if (sim_log)
                fprintf (sim_log, "Unable to allocate I/O buffer: out of memory\n");
            return 0;
        }
        tls_set_value(flip_tls_key, sim_flip);
    }

    nelem = FLIP_SIZE / size;                               /* elements in buffer */
    nbuf = count / nelem;                                   /* number buffers */
    lcnt = count % nelem;                                   /* count in last buf */
    if (lcnt)
        nbuf = nbuf + 1;
    else
        lcnt = nelem;
    total = 0;
    sptr = (unsigned char *) bptr;                          /* init input ptr */
    for (i = (int32) nbuf;  i > 0;  i--)                    /* loop on buffers */
    {
        c = (i == 1) ? lcnt: nelem;
        sim_buf_copy_swapped (sim_flip, sptr, size, c);
        sptr = sptr + size * count;
        c = fwrite (sim_flip, size, c, fptr);
        if (c == 0)
            return total;
        total = total + c;
    }
    return total;
}

/* Forward Declaration */

static t_addr _sim_ftell (SMP_FILE *st);

/* Get file size */

t_addr sim_fsize_ex (SMP_FILE *fp)
{
    t_addr pos, sz;

    if (fp == NULL)
        return 0;
    pos = _sim_ftell (fp);
    sim_fseek (fp, 0, SEEK_END);
    sz = _sim_ftell (fp);
    sim_fseek (fp, pos, SEEK_SET);
    return sz;
}

t_addr sim_fsize_name_ex (char *fname)
{
    SMP_FILE *fp;
    t_addr sz;

    if ((fp = sim_fopen (fname, "rb")) == NULL)
        return 0;
    sz = sim_fsize_ex (fp);
    fclose (fp);
    return sz;
}

uint32 sim_fsize_name (char *fname)
{
    return (uint32)(sim_fsize_name_ex (fname));
}

uint32 sim_fsize (SMP_FILE *fp)
{
    return (uint32)(sim_fsize_ex (fp));
}

/* OS-dependent routines */

/* Optimized file open */

SMP_FILE *sim_fopen (const char *file, const char *mode)
{
#if defined (VMS)
    return smp_fopen (file, mode, "ALQ=32", "DEQ=4096",
            "MBF=6", "MBC=127", "FOP=cbt,tef", "ROP=rah,wbh", "CTX=stm");
#elif defined (USE_INT64) && defined (USE_ADDR64) && defined (__linux)
    return smp_fopen64 (file, mode);
#else
    return smp_fopen (file, mode);
#endif
}

/* Long seek */

#if defined (USE_INT64) && defined (USE_ADDR64)

/* 64b VMS */

#if (defined (__ALPHA) || defined (__ia64)) && defined (VMS) && (__DECC_VER >= 60590001)
#define _SIM_IO_FSEEK_EXT_      1

int sim_fseek (SMP_FILE *st, t_addr offset, int whence)
{
    return fseeko (st, (off_t)offset, whence);
}

static t_addr _sim_ftell (SMP_FILE *st)
{
    return (t_addr)(ftello (st));
}

#endif

/* Alpha UNIX - natively 64b */

#if defined (__ALPHA) && defined (__unix__)             /* Alpha UNIX */
#define _SIM_IO_FSEEK_EXT_      1

int sim_fseek (SMP_FILE *st, t_addr offset, int whence)
{
    return fseek (st, offset, whence);
}

static t_addr _sim_ftell (SMP_FILE *st)
{
    return (t_addr)(ftell (st));
}

#endif

/* Windows */

#if defined (_WIN32)
#define _SIM_IO_FSEEK_EXT_      1
#include <sys/stat.h>

int sim_fseek (SMP_FILE *st, t_addr offset, int whence)
{
    fpos_t fileaddr;
    struct _stati64 statb;

    switch (whence)
    {
    case SEEK_SET:
        fileaddr = offset;
        break;

    case SEEK_END:
        if (_fstati64 (_fileno (st), &statb))
            return (-1);
        fileaddr = statb.st_size + offset;
        break;
    case SEEK_CUR:
        if (fgetpos (st, &fileaddr))
            return (-1);
        fileaddr = fileaddr + offset;
        break;

    default:
        errno = EINVAL;
        return (-1);
    }

    return fsetpos (st, &fileaddr);
}

static t_addr _sim_ftell (SMP_FILE *st)
{
    fpos_t fileaddr;
    if (fgetpos (st, &fileaddr))
        return (-1);
    return (t_addr) fileaddr;
}

#endif                                                  /* end Windows */

/* Linux */

#if defined (__linux)
#define _SIM_IO_FSEEK_EXT_      1

int sim_fseek (SMP_FILE *st, t_addr xpos, int origin)
{
    return fseeko64(st, xpos, origin);
}

static t_addr _sim_ftell (SMP_FILE *st)
{
    return (t_addr) ftello64(st);
}

#endif                                                  /* end Linux with LFS */

/* Apple OS/X */

#if defined (__APPLE__) || defined (__FreeBSD__)
#define _SIM_IO_FSEEK_EXT_      1

int sim_fseek (SMP_FILE *st, t_addr xpos, int origin) 
{
    return fseeko(st, xpos, origin);
}

static t_addr _sim_ftell (SMP_FILE *st)
{
    return (t_addr) ftello(st);
}

#endif  /* end Apple OS/X */

#endif                                                  /* end 64b seek defs */

/* Default: no OS-specific routine has been defined */

#if !defined (_SIM_IO_FSEEK_EXT_)
#define _SIM_IO_FSEEK_EXT_      0

int sim_fseek (SMP_FILE *st, t_addr xpos, int origin)
{
    return fseek (st, (int32) xpos, origin);
}

static t_addr _sim_ftell (SMP_FILE *st)
{
    return (t_addr) ftell(st);
}

#endif

const uint32 sim_taddr_64 = _SIM_IO_FSEEK_EXT_;