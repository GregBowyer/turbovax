/* sim_console.c: simulator console I/O library

   Copyright (c) 1993-2011, Robert M Supnik

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

   07-Dec-11    MP      Added sim_ttisatty to support reasonable behaviour (i.e. 
                        avoid in infinite loop) in the main command input
                        loop when EOF is detected and input is coming from 
                        a file (or a null device: /dev/null or NUL:) This may
                        happen when a simulator is running in a background 
                        process.
   17-Apr-11    MP      Cleaned up to support running in a background/detached
                        process
   20-Jan-11    MP      Fixed support for BREAK key on Windows to account 
                        for/ignore other keyboard Meta characters.
   18-Jan-11    MP      Added log file reference count support
   17-Jan-11    MP      Added support for a "Buffered" behaviors which include:
                        - If Buffering is enabled and Telnet is enabled, a
                          telnet connection is not required for simulator 
                          operation (instruction execution).
                        - If Buffering is enabled, all console output is 
                          written to the buffer at all times (deleting the
                          oldest buffer contents on overflow).
                        - when a connection is established on the console 
                          telnet port, the whole contents of the Buffer is
                          presented on the telnet session and connection 
                          will then proceed as if the connection had always
                          been there.
                        This concept allows a simulator to run in the background
                        and when needed a console session to be established.  
                        The "when needed" case usually will be interested in 
                        what already happened before looking to address what 
                        to do, hence the buffer contents being presented.
   28-Dec-10    MP      Added support for BREAK key on Windows
   30-Sep-06    RMS     Fixed non-printable characters in KSR mode
   22-Jun-06    RMS     Implemented SET/SHOW PCHAR
   31-May-06    JDB     Fixed bug if SET CONSOLE DEBUG with no argument
   22-Nov-05    RMS     Added central input/output conversion support
   05-Nov-04    RMS     Moved SET/SHOW DEBUG under CONSOLE hierarchy
   28-Oct-04    JDB     Fixed SET CONSOLE to allow comma-separated parameters
   20-Aug-04    RMS     Added OS/2 EMX fixes (from Holger Veit)
   14-Jul-04    RMS     Revised Windows console code (from Dave Bryan)
   28-May-04    RMS     Added SET/SHOW CONSOLE
                RMS     Added break, delete character maps
   02-Jan-04    RMS     Removed timer routines, added Telnet console routines
                RMS     Moved console logging to OS-independent code
   25-Apr-03    RMS     Added long seek support from Mark Pizzolato
                        Added Unix priority control from Mark Pizzolato
   24-Sep-02    RMS     Removed VT support, added Telnet console support
                        Added CGI support (from Brian Knittel)
                        Added MacOS sleep (from Peter Schorn)
   14-Jul-02    RMS     Added Windows priority control from Mark Pizzolato
   20-May-02    RMS     Added Windows VT support from Fischer Franz
   01-Feb-02    RMS     Added VAX fix from Robert Alan Byer
   19-Sep-01    RMS     More Mac changes
   31-Aug-01    RMS     Changed int64 to t_int64 for Windoze
   20-Jul-01    RMS     Added Macintosh support (from Louis Chretien, Peter Schorn,
                                and Ben Supnik)
   15-May-01    RMS     Added logging support
   05-Mar-01    RMS     Added clock calibration support
   08-Dec-00    BKR     Added OS/2 support (from Bruce Ray)
   18-Aug-98    RMS     Added BeOS support
   13-Oct-97    RMS     Added NetBSD terminal support
   25-Jan-97    RMS     Added POSIX terminal I/O support
   02-Jan-97    RMS     Fixed bug in sim_poll_kbd

   This module implements the following routines to support terminal I/O:

   sim_poll_kbd -       poll for keyboard input
   sim_putchar  -       output character to console
   sim_putchar_s -      output character to console, stall if congested
   sim_set_console -    set console parameters
   sim_show_console -   show console parameters
   sim_set_cons_buff -  set console buffered
   sim_set_cons_unbuff -set console unbuffered
   sim_set_cons_log -   set console log
   sim_set_cons_nolog - set console nolog
   sim_show_cons_buff - show console buffered
   sim_show_cons_log -  show console log
   sim_tt_inpcvt -      convert input character per mode
   sim_tt_outcvt -      convert output character per mode

   sim_ttinit   -       called once to get initial terminal state
   sim_ttrun    -       called to put terminal into run state
   sim_ttcmd    -       called to return terminal to command state
   sim_ttclose  -       called once before the simulator exits
   sim_ttisatty -       called to determine if running interactively
   sim_os_poll_kbd -    poll for keyboard input
   sim_os_putchar -     output character to console

   The first group is OS-independent; the second group is OS-dependent.

   The following routines are exposed but deprecated:

   sim_set_telnet -     set console to Telnet port
   sim_set_notelnet -   close console Telnet port
   sim_show_telnet -    show console status
*/

#include "sim_defs.h"
#include "sim_sock.h"
#include "sim_tmxr.h"
#include <ctype.h>

#define KMAP_WRU        0
#define KMAP_BRK        1
#define KMAP_DEL        2
#define KMAP_MASK       0377
#define KMAP_NZ         0400

int32 sim_int_char = 005;                               /* interrupt character */
int32 sim_brk_char = 000;                               /* break character */
int32 sim_tt_pchar = 0x00002780;
#if defined (_WIN32) || defined (__OS2__) || (defined (__MWERKS__) && defined (macintosh))
int32 sim_del_char = '\b';                              /* delete character */
#else
int32 sim_del_char = 0177;
#endif
TMLN sim_con_ldsc = { 0 };                              /* console line descr */
TMXR sim_con_tmxr = { 1, 0, 0, &sim_con_ldsc };         /* console line mux */
AUTO_INIT_LOCK(sim_con_lock,                            /* lock for sim_con_ldsc and sim_con_tmxr */
        SIM_LOCK_CRITICALITY_OS_HI,
        DEVLOCK_SPINWAIT_CYCLES);                        

extern int32 sim_quiet;
extern SMP_FILE *sim_log, *sim_deb;
extern SMP_FILEREF *sim_log_ref, *sim_deb_ref;

/* Set/show data structures */

static CTAB set_con_tab[] = {
    { "WRU", &sim_set_kmap, KMAP_WRU | KMAP_NZ },
    { "BRK", &sim_set_kmap, KMAP_BRK },
    { "DEL", &sim_set_kmap, KMAP_DEL |KMAP_NZ },
    { "PCHAR", &sim_set_pchar, 0 },
    { "TELNET", &sim_set_telnet, 0 },
    { "NOTELNET", &sim_set_notelnet, 0 },
    { "LOG", &sim_set_logon, 0 },
    { "NOLOG", &sim_set_logoff, 0 },
    { "DEBUG", &sim_set_debon, 0 },
    { "NODEBUG", &sim_set_deboff, 0 },
    { NULL, NULL, 0 }
    };

static SHTAB show_con_tab[] = {
    { "WRU", &sim_show_kmap, KMAP_WRU },
    { "BRK", &sim_show_kmap, KMAP_BRK },
    { "DEL", &sim_show_kmap, KMAP_DEL },
    { "PCHAR", &sim_show_pchar, 0 },
    { "LOG", &sim_show_cons_log, 0 },
    { "TELNET", &sim_show_telnet, 0 },
    { "DEBUG", &sim_show_debug, 0 },
    { "BUFFERED", &sim_show_cons_buff, 0 },
    { NULL, NULL, 0 }
    };

static CTAB set_con_telnet_tab[] = {
    { "LOG", &sim_set_cons_log, 0 },
    { "NOLOG", &sim_set_cons_nolog, 0 },
    { "BUFFERED", &sim_set_cons_buff, 0 },
    { "NOBUFFERED", &sim_set_cons_unbuff, 0 },
    { "UNBUFFERED", &sim_set_cons_unbuff, 0 },
    { NULL, NULL, 0 }
    };

static int32 *cons_kmap[] = {
    &sim_int_char,
    &sim_brk_char,
    &sim_del_char
    };

/* Console I/O package.

   The console terminal can be attached to the controlling window
   or to a Telnet connection.  If attached to a Telnet connection,
   the console is described by internal terminal multiplexor
   sim_con_tmxr and internal terminal line description sim_con_ldsc.
*/

/* SET CONSOLE command */

t_stat sim_set_console (int32 flag, char *cptr)
{
char *cvptr, gbuf[CBUFSIZE];
CTAB *ctptr;
t_stat r;

if ((cptr == NULL) || (*cptr == 0))
    return SCPE_2FARG;
while (*cptr != 0) {                                    /* do all mods */
    cptr = get_glyph_nc (cptr, gbuf, ',');              /* get modifier */
    if (cvptr = strchr (gbuf, '='))                     /* = value? */
        *cvptr++ = 0;
    get_glyph (gbuf, gbuf, 0);                          /* modifier to UC */
    if (ctptr = find_ctab (set_con_tab, gbuf)) {        /* match? */
        r = ctptr->action (ctptr->arg, cvptr);          /* do the rest */
        if (r != SCPE_OK)
            return r;
        }
    else return SCPE_NOPARAM;
    }
return SCPE_OK;
}

/* SHOW CONSOLE command */

t_stat sim_show_console (SMP_FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, char *cptr)
{
    char gbuf[CBUFSIZE];
    SHTAB *shptr;
    int32 i;

    if (*cptr == 0)                                         /* show all */
    {
        for (i = 0; show_con_tab[i].name; i++)
            show_con_tab[i].action (st, dptr, uptr, show_con_tab[i].arg, cptr);
        return SCPE_OK;
    }
    while (*cptr != 0)
    {
        cptr = get_glyph (cptr, gbuf, ',');                 /* get modifier */
        if (shptr = find_shtab (show_con_tab, gbuf))
            shptr->action (st, dptr, uptr, shptr->arg, cptr);
        else return SCPE_NOPARAM;
    }
    return SCPE_OK;
}

/* Set keyboard map */

t_stat sim_set_kmap (int32 flag, char *cptr)
{
DEVICE *dptr = sim_devices[0];
int32 val, rdx;
t_stat r;

if ((cptr == NULL) || (*cptr == 0))
    return SCPE_2FARG;
if (dptr->dradix == 16) rdx = 16;
else rdx = 8;
val = (int32) get_uint (cptr, rdx, 0177, &r);
if ((r != SCPE_OK) ||
    ((val == 0) && (flag & KMAP_NZ)))
    return SCPE_ARG;
*(cons_kmap[flag & KMAP_MASK]) = val;
return SCPE_OK;
}

/* Show keyboard map */

t_stat sim_show_kmap (SMP_FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, char *cptr)
{
if (sim_devices[0]->dradix == 16)
    fprintf (st, "%s = %X\n", show_con_tab[flag].name, *(cons_kmap[flag & KMAP_MASK]));
else fprintf (st, "%s = %o\n", show_con_tab[flag].name, *(cons_kmap[flag & KMAP_MASK]));
return SCPE_OK;
}

/* Set printable characters */

t_stat sim_set_pchar (int32 flag, char *cptr)
{
DEVICE *dptr = sim_devices[0];
uint32 val, rdx;
t_stat r;

if ((cptr == NULL) || (*cptr == 0))
    return SCPE_2FARG;
if (dptr->dradix == 16) rdx = 16;
else rdx = 8;
val = (uint32) get_uint (cptr, rdx, 0xFFFFFFFF, &r);
if ((r != SCPE_OK) ||
    ((val & 0x00002400) == 0))
    return SCPE_ARG;
sim_tt_pchar = val;
return SCPE_OK;
}

/* Show printable characters */

t_stat sim_show_pchar (SMP_FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, char *cptr)
{
if (sim_devices[0]->dradix == 16)
    fprintf (st, "pchar mask = %X\n", sim_tt_pchar);
else fprintf (st, "pchar mask = %o\n", sim_tt_pchar);
return SCPE_OK;
}

/* Set log routine */

t_stat sim_set_logon (int32 flag, char *cptr)
{
char gbuf[CBUFSIZE];
t_stat r;

if ((cptr == NULL) || (*cptr == 0))                     /* need arg */
    return SCPE_2FARG;
cptr = get_glyph_nc (cptr, gbuf, 0);                    /* get file name */
if (*cptr != 0)                                         /* now eol? */
    return SCPE_2MARG;
sim_set_logoff (0, NULL);                               /* close cur log */
r = sim_open_logfile (gbuf, FALSE, &sim_log, &sim_log_ref); /* open log */
if (r != SCPE_OK)                                       /* error? */
    return r;
if (!sim_quiet)
    smp_printf ("Logging to file \"%s\"\n", 
             sim_logfile_name (sim_log, sim_log_ref));
fprintf (sim_log, "Logging to file \"%s\"\n", 
             sim_logfile_name (sim_log, sim_log_ref));  /* start of log */
return SCPE_OK;
}

/* Set nolog routine */

t_stat sim_set_logoff (int32 flag, char *cptr)
{
if (cptr && (*cptr != 0))                               /* now eol? */
    return SCPE_2MARG;
if (sim_log == NULL)                                    /* no log? */
    return SCPE_OK;
if (!sim_quiet)
    smp_printf ("Log file closed\n");
fprintf (sim_log, "Log file closed\n");
sim_close_logfile (&sim_log_ref);                       /* close log */
sim_log = NULL;
return SCPE_OK;
}

/* Show log status */

t_stat sim_show_log (SMP_FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, char *cptr)
{
if (cptr && (*cptr != 0))
    return SCPE_2MARG;
if (sim_log)
    fprintf (st, "Logging enabled to \"%s\"\n", 
                 sim_logfile_name (sim_log, sim_log_ref));
else fprintf (st, "Logging disabled\n");
return SCPE_OK;
}

/* Set debug routine */

t_stat sim_set_debon (int32 flag, char *cptr)
{
char gbuf[CBUFSIZE];
t_stat r;

if ((cptr == NULL) || (*cptr == 0))                     /* need arg */
    return SCPE_2FARG;
cptr = get_glyph_nc (cptr, gbuf, 0);                    /* get file name */
if (*cptr != 0)                                         /* now eol? */
    return SCPE_2MARG;
r = sim_open_logfile (gbuf, FALSE, &sim_deb, &sim_deb_ref);

if (r != SCPE_OK)
    return r;
if (!sim_quiet)
    smp_printf ("Debug output to \"%s\"\n", 
            sim_logfile_name (sim_deb, sim_deb_ref));
if (sim_log)
    fprintf (sim_log, "Debug output to \"%s\"\n", 
                      sim_logfile_name (sim_deb, sim_deb_ref));
return SCPE_OK;
}

/* Set nodebug routine */

t_stat sim_set_deboff (int32 flag, char *cptr)
{
t_stat r;

if (cptr && (*cptr != 0))                               /* now eol? */
    return SCPE_2MARG;
if (sim_deb == NULL)                                    /* no log? */
    return SCPE_OK;
r = sim_close_logfile (&sim_deb_ref);
sim_deb = NULL;
if (!sim_quiet)
    smp_printf ("Debug output disabled\n");
if (sim_log)
    fprintf (sim_log, "Debug output disabled\n");
return SCPE_OK;
}

/* Show debug routine */

t_stat sim_show_debug (SMP_FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, char *cptr)
{
if (cptr && (*cptr != 0))
    return SCPE_2MARG;
if (sim_deb)
    fprintf (st, "Debug output enabled to \"%s\"\n", 
                 sim_logfile_name (sim_deb, sim_deb_ref));
else fprintf (st, "Debug output disabled\n");
return SCPE_OK;
}

/* SET CONSOLE command */

/* Set console to Telnet port (and parameters) */

t_stat sim_set_telnet (int32 flg, char *cptr)
{
char *cvptr, gbuf[CBUFSIZE];
CTAB *ctptr;
t_stat r;

if ((cptr == NULL) || (*cptr == 0))
    return SCPE_2FARG;
while (*cptr != 0) {                                    /* do all mods */
    cptr = get_glyph_nc (cptr, gbuf, ',');              /* get modifier */
    if (cvptr = strchr (gbuf, '='))                     /* = value? */
        *cvptr++ = 0;
    get_glyph (gbuf, gbuf, 0);                          /* modifier to UC */
    if (isdigit (*gbuf)) {
        if (sim_con_tmxr.master)                        /* already open? */
            sim_set_notelnet (0, NULL);                 /* close first */
        return tmxr_open_master (&sim_con_tmxr, gbuf);  /* open master socket */
        }
    else
        if (ctptr = find_ctab (set_con_telnet_tab, gbuf)) { /* match? */
            r = ctptr->action (ctptr->arg, cvptr);          /* do the rest */
            if (r != SCPE_OK)
                return r;
            }
        else return SCPE_NOPARAM;
    }
return SCPE_OK;
}

/* Close console Telnet port */

t_stat sim_set_notelnet (int32 flag, char *cptr)
{
if (cptr && (*cptr != 0))                               /* too many arguments? */
    return SCPE_2MARG;
if (sim_con_tmxr.master == 0)                           /* ignore if already closed */
    return SCPE_OK;
return tmxr_close_master (&sim_con_tmxr);               /* close master socket */
}

/* Show console Telnet status */

t_stat sim_show_telnet (SMP_FILE *st, DEVICE *dunused, UNIT *uunused, int32 flag, char *cptr)
{
if (cptr && (*cptr != 0))
    return SCPE_2MARG;
if (sim_con_tmxr.master == 0)
    fprintf (st, "Connected to console window\n");
else {
    if (sim_con_ldsc.conn == 0)
        fprintf (st, "Listening on port %d\n", sim_con_tmxr.port);
    else {
        fprintf (st, "Listening on port %d, connected to socket %d\n",
            sim_con_tmxr.port, sim_con_ldsc.conn);
        tmxr_fconns (st, &sim_con_ldsc, -1);
        }
    tmxr_fstats (st, &sim_con_ldsc, -1);
    }
return SCPE_OK;
}

/* Set console to Buffering  */

t_stat sim_set_cons_buff (int32 flg, char *cptr)
{
char cmdbuf[CBUFSIZE];

sprintf(cmdbuf, "BUFFERED%c%s", cptr ? '=' : '\0', cptr ? cptr : "");
return tmxr_open_master (&sim_con_tmxr, cmdbuf);      /* open master socket */
}

/* Set console to NoBuffering */

t_stat sim_set_cons_unbuff (int32 flg, char *cptr)
{
char cmdbuf[CBUFSIZE];

sprintf(cmdbuf, "UNBUFFERED%c%s", cptr ? '=' : '\0', cptr ? cptr : "");
return tmxr_open_master (&sim_con_tmxr, cmdbuf);      /* open master socket */
}

/* Set console to Logging */

t_stat sim_set_cons_log (int32 flg, char *cptr)
{
char cmdbuf[CBUFSIZE];

sprintf(cmdbuf, "LOG%c%s", cptr ? '=' : '\0', cptr ? cptr : "");
return tmxr_open_master (&sim_con_tmxr, cmdbuf);      /* open master socket */
}

/* Set console to NoLogging */

t_stat sim_set_cons_nolog (int32 flg, char *cptr)
{
char cmdbuf[CBUFSIZE];

sprintf(cmdbuf, "NOLOG%c%s", cptr ? '=' : '\0', cptr ? cptr : "");
return tmxr_open_master (&sim_con_tmxr, cmdbuf);      /* open master socket */
}

t_stat sim_show_cons_log (SMP_FILE *st, DEVICE *dunused, UNIT *uunused, int32 flag, char *cptr)
{
if (cptr && (*cptr != 0))
    return SCPE_2MARG;
if (sim_con_tmxr.ldsc->txlog)
    fprintf (st, "Log File being written to %s\n", sim_con_tmxr.ldsc->txlogname);
else
    fprintf (st, "No Logging\n");
return SCPE_OK;
}

t_stat sim_show_cons_buff (SMP_FILE *st, DEVICE *dunused, UNIT *uunused, int32 flag, char *cptr)
{
if (cptr && (*cptr != 0))
    return SCPE_2MARG;
if (!sim_con_tmxr.buffered)
    fprintf (st, "Unbuffered\n");
else
    fprintf (st, "Buffer Size = %d\n", sim_con_tmxr.buffered);
return SCPE_OK;
}

/* Log File Open/Close/Show Support */

/* Open log file */

t_stat sim_open_logfile (char *filename, t_bool binary, SMP_FILE **pf, SMP_FILEREF **pref)
{
char *tptr, gbuf[CBUFSIZE];

if ((filename == NULL) || (*filename == 0))             /* too few arguments? */
    return SCPE_2FARG;
tptr = get_glyph (filename, gbuf, 0);
if (*tptr != 0)                                         /* now eol? */
    return SCPE_2MARG;
sim_close_logfile (pref);
*pf = NULL;
if (strcmp (gbuf, "LOG") == 0) {                        /* output to log? */
    if (sim_log == NULL)                                /* any log? */
        return SCPE_ARG;
    *pf = sim_log;
    *pref = sim_log_ref;
    if (*pref)
        ++(*pref)->refcount;
    }
else if (strcmp (gbuf, "DEBUG") == 0) {                 /* output to debug? */
    if (sim_deb == NULL)                              /* any debug? */
        return SCPE_ARG;
    *pf = sim_deb;
    *pref = sim_deb_ref;
    if (*pref)
        ++(*pref)->refcount;
    }
else if (strcmp (gbuf, "STDOUT") == 0) {                /* output to stdout? */
    *pf = smp_stdout;
    *pref = NULL;
    }
else if (strcmp (gbuf, "STDERR") == 0) {                /* output to stderr? */
    *pf = smp_stderr;
    *pref = NULL;
    }
else {
    *pref = (SMP_FILEREF*) calloc (1, sizeof(**pref));
    if (!*pref)
        return SCPE_MEM;
    get_glyph_nc (filename, gbuf, 0);                   /* reparse */
    strncpy ((*pref)->name, gbuf, sizeof((*pref)->name)-1);
    *pf = smp_fopen (gbuf, (binary ? "ab" : "a"));      /* open file */
    if (*pf == NULL) {                                  /* error? */
        free (*pref);
        *pref = NULL;
        return SCPE_OPENERR;
        }
    (*pref)->file = *pf;
    (*pref)->refcount = 1;                               /* need close */
    }
return SCPE_OK;
}

/* Close log file */

t_stat sim_close_logfile (SMP_FILEREF **pref)
{
if (NULL == *pref)
    return SCPE_OK;
(*pref)->refcount = (*pref)->refcount  - 1;
if ((*pref)->refcount > 0)
    return SCPE_OK;
fclose ((*pref)->file);
free (*pref);
*pref = NULL;
return SCPE_OK;
}

/* Show logfile support routine */

const char *sim_logfile_name (SMP_FILE *st, SMP_FILEREF *ref)
{
if (!st)
    return "";
if (st == smp_stdout)
    return "STDOUT";
if (st == smp_stderr)
    return "STDERR";
if (!ref)
    return "";
return ref->name;
}

/* Check connection before executing */

t_stat sim_check_console (int32 sec)
{
    int32 c, i;

    if (sim_con_tmxr.master == 0)                           /* not Telnet? done */
        return SCPE_OK;

    if (sim_con_ldsc.conn || sim_con_ldsc.txbfd)            /* connected or buffered? */
    {
        tmxr_poll_rx (&sim_con_tmxr);                       /* poll (check disconn) */
        if (sim_con_ldsc.conn || sim_con_ldsc.txbfd)        /* still connected? */
        {
            if (! sim_con_ldsc.conn)
            {
                smp_printf ("Running with Buffered Console\r\n"); /* print transition */
                fflush (smp_stdout);
                if (sim_log)                                /* log file? */
                {
                    fprintf (sim_log, "Running with Buffered Console\n");
                    fflush (sim_log);
                }
            }
            return SCPE_OK;
        }
    }

    for (i = 0; i < sec; i++)                               /* loop */
    {
        if (tmxr_poll_conn (&sim_con_tmxr) >= 0)            /* poll connect */
        {
            sim_con_ldsc.rcve = 1;                          /* rcv enabled */
            if (i)                                          /* if delayed */
            {
                smp_printf ("Running\r\n");                 /* print transition */
                fflush (smp_stdout);
                if (sim_log)                                /* log file? */
                {
                    fprintf (sim_log, "Running\n");
                    fflush (sim_log);
                }
            }
            return SCPE_OK;                                 /* ready to proceed */
        }
        c = sim_os_poll_kbd ();                             /* check for stop char */
        if (c == SCPE_STOP || weak_read(stop_cpus))
            return SCPE_STOP;
        if ((i % 10) == 0)                                  /* Status every 10 sec */
        {
            smp_printf ("Waiting for console Telnet connection\r\n");
            fflush (smp_stdout);
            if (sim_log)                                   /* log file? */
            {
                fprintf (sim_log, "Waiting for console Telnet connection\n");
                fflush (sim_log);
            }
        }
        sim_os_sleep (1);                                   /* wait 1 second */
    }
    return SCPE_TTMO;                                       /* timed out */
}

/* Poll for character */

t_stat sim_poll_kbd (t_bool use_console)
{
    int32 c;

    if (use_console)
    {
        c = sim_os_poll_kbd ();                             /* get character */
        if (c == SCPE_STOP || sim_con_tmxr.master == 0)     /* ^E or not Telnet? */
            return c;                                       /* in-window */
    }
    else
    {
        if (sim_con_tmxr.master == 0)                       /* not Telnet? */
            return SCPE_OK;
    }

    AUTO_LOCK(sim_con_lock);

    if (sim_con_ldsc.conn == 0)                             /* no Telnet conn? */
    {
        if (! sim_con_ldsc.txbfd)                           /* unbuffered? */
            return SCPE_LOST;                               /* connection lost */
        if (tmxr_poll_conn (&sim_con_tmxr) >= 0)            /* poll connect */
            sim_con_ldsc.rcve = 1;                          /* rcv enabled */
        else                                                /* fall through to poll reception */
            return SCPE_OK;                                 /* unconnected and buffered - nothing to receive */
    }
    tmxr_poll_rx (&sim_con_tmxr);                           /* poll for input */
    if (c = tmxr_getc_ln (&sim_con_ldsc))                   /* any char? */ 
        return (c & (SCPE_BREAK | 0377)) | SCPE_KFLAG;
    return SCPE_OK;
}

/* Output character */

t_stat sim_putchar (int32 c)
{
    AUTO_LOCK(sim_con_lock);
    if (sim_con_tmxr.master == 0)                           /* not Telnet? */
    {
        if (sim_log)                                        /* log file? */
            fputc (c, sim_log);
        return sim_os_putchar (c);                          /* in-window version */
    }
    if (sim_log && !sim_con_ldsc.txlog)                     /* log file, but no line log? */
        fputc (c, sim_log);
    if (sim_con_ldsc.conn == 0)                             /* no Telnet conn? */
    {
        if (! sim_con_ldsc.txbfd)                           /* unbuffered? */
            return SCPE_LOST;                               /* connection lost */
        if (tmxr_poll_conn (&sim_con_tmxr) >= 0)            /* poll connect */
            sim_con_ldsc.rcve = 1;                          /* rcv enabled */
    }
    tmxr_putc_ln (&sim_con_ldsc, c);                        /* output char */
    tmxr_poll_tx (&sim_con_tmxr);                           /* poll xmt */
    return SCPE_OK;
}

t_stat sim_putchar_s (int32 c)
{
    AUTO_LOCK(sim_con_lock);
    t_stat r;

    if (sim_con_tmxr.master == 0)                           /* not Telnet? */
    {
        if (sim_log)                                        /* log file? */
            fputc (c, sim_log);
        return sim_os_putchar (c);                          /* in-window version */
    }
    if (sim_log && !sim_con_ldsc.txlog)                     /* log file, but no line log? */
        fputc (c, sim_log);
    if (sim_con_ldsc.conn == 0)                             /* no Telnet conn? */
    {
        if (!sim_con_ldsc.txbfd)                            /* non-buffered Telnet conn? */
            return SCPE_LOST;                               /* lost */
        if (tmxr_poll_conn (&sim_con_tmxr) >= 0)            /* poll connect */
            sim_con_ldsc.rcve = 1;                          /* rcv enabled */
    }
    if (sim_con_ldsc.xmte == 0)                             /* xmt disabled? */
        r = SCPE_STALL;
    else
        r = tmxr_putc_ln (&sim_con_ldsc, c);                /* no, Telnet output */
    tmxr_poll_tx (&sim_con_tmxr);                           /* poll xmt */
    return r;                                               /* return status */
}

/* Input character processing */

int32 sim_tt_inpcvt (int32 c, uint32 mode)
{
uint32 md = mode & TTUF_M_MODE;

if (md != TTUF_MODE_8B) {
    c = c & 0177;
    if (md == TTUF_MODE_UC) {
        if (islower (c))
            c = toupper (c);
        if (mode & TTUF_KSR)
            c = c | 0200;
        }
    }
else c = c & 0377;
return c;
}

/* Output character processing */

int32 sim_tt_outcvt (int32 c, uint32 mode)
{
uint32 md = mode & TTUF_M_MODE;

if (md != TTUF_MODE_8B) {
    c = c & 0177;
    if (md == TTUF_MODE_UC) {
        if (islower (c))
            c = toupper (c);
        if ((mode & TTUF_KSR) && (c >= 0140))
            return -1;
        }
    if (((md == TTUF_MODE_UC) || (md == TTUF_MODE_7P)) &&
        ((c == 0177) ||
         ((c < 040) && !((sim_tt_pchar >> c) & 1))))
        return -1;
    }
else c = c & 0377;
return c;
}

/* process character received from console keyboard,
   can be (c | SCPE_KFLAG) or SCPE_BREAK */
void sim_con_rcv_char (int32 c)
{
    if (sim_con_tmxr.master == 0 || (c & SCPE_BREAK))
    {
        /* no Telnet or BRK (Ctrl/P), forward input to CPU typeahead buffer */
        if (! tti_rcv_char(c))
        {
            /* TTI typeahead was full or BRK HALT was rejected: bell */
            sim_os_putchar(7);
        }
    }
}

/* VMS routines, from Ben Thomas, with fixes from Robert Alan Byer */

#if defined (VMS)

#if defined(__VAX)
#define sys$assign SYS$ASSIGN
#define sys$qiow SYS$QIOW
#endif

#include <descrip.h>
#include <ttdef.h>
#include <tt2def.h>
#include <iodef.h>
#include <ssdef.h>
#include <starlet.h>
#include <unistd.h>

#define EFN 0
uint32 tty_chan = 0;

typedef struct {
    unsigned short sense_count;
    unsigned char sense_first_char;
    unsigned char sense_reserved;
    unsigned int stat;
    unsigned int stat2; } SENSE_BUF;

typedef struct {
    unsigned short status;
    unsigned short count;
    unsigned int dev_status; } IOSB;

SENSE_BUF cmd_mode = { 0 };
SENSE_BUF run_mode = { 0 };

t_stat sim_ttinit (void)
{
unsigned int status;
IOSB iosb;
$DESCRIPTOR (terminal_device, "tt");

status = sys$assign (&terminal_device, &tty_chan, 0, 0);
if (status != SS$_NORMAL)
    return SCPE_TTIERR;
status = sys$qiow (EFN, tty_chan, IO$_SENSEMODE, &iosb, 0, 0,
    &cmd_mode, sizeof (cmd_mode), 0, 0, 0, 0);
if ((status != SS$_NORMAL) || (iosb.status != SS$_NORMAL))
    return SCPE_TTIERR;
run_mode = cmd_mode;
run_mode.stat = cmd_mode.stat | TT$M_NOECHO & ~(TT$M_HOSTSYNC | TT$M_TTSYNC);
run_mode.stat2 = cmd_mode.stat2 | TT2$M_PASTHRU;
return SCPE_OK;
}

t_stat sim_ttrun (void)
{
unsigned int status;
IOSB iosb;

status = sys$qiow (EFN, tty_chan, IO$_SETMODE, &iosb, 0, 0,
    &run_mode, sizeof (run_mode), 0, 0, 0, 0);
if ((status != SS$_NORMAL) || (iosb.status != SS$_NORMAL))
    return SCPE_TTIERR;
return SCPE_OK;
}

t_stat sim_ttcmd (void)
{
unsigned int status;
IOSB iosb;

status = sys$qiow (EFN, tty_chan, IO$_SETMODE, &iosb, 0, 0,
    &cmd_mode, sizeof (cmd_mode), 0, 0, 0, 0);
if ((status != SS$_NORMAL) || (iosb.status != SS$_NORMAL))
    return SCPE_TTIERR;
return SCPE_OK;
}

t_bool sim_ttisatty (void)
{
return isatty (fileno (stdin));
}

t_stat sim_ttclose (void)
{
return sim_ttcmd ();
}

t_stat sim_os_poll_kbd (void)
{
unsigned int status, term[2];
unsigned char buf[4];
IOSB iosb;
SENSE_BUF sense;

term[0] = 0; term[1] = 0;
status = sys$qiow (EFN, tty_chan, IO$_SENSEMODE | IO$M_TYPEAHDCNT, &iosb,
    0, 0, &sense, 8, 0, term, 0, 0);
if ((status != SS$_NORMAL) || (iosb.status != SS$_NORMAL))
    return SCPE_TTIERR;
if (sense.sense_count == 0) return SCPE_OK;
term[0] = 0; term[1] = 0;
status = sys$qiow (EFN, tty_chan,
    IO$_READLBLK | IO$M_NOECHO | IO$M_NOFILTR | IO$M_TIMED | IO$M_TRMNOECHO,
    &iosb, 0, 0, buf, 1, 0, term, 0, 0);
if ((status != SS$_NORMAL) || (iosb.status != SS$_NORMAL))
    return SCPE_OK;
if (buf[0] == sim_int_char) return SCPE_STOP;
if (sim_brk_char && (buf[0] == sim_brk_char))
    return SCPE_BREAK;
return (buf[0] | SCPE_KFLAG);
}

t_stat sim_os_putchar (int32 out)
{
unsigned int status;
char c;
IOSB iosb;

c = out;
status = sys$qiow (EFN, tty_chan, IO$_WRITELBLK | IO$M_NOFORMAT,
    &iosb, 0, 0, &c, 1, 0, 0, 0, 0);
if ((status != SS$_NORMAL) || (iosb.status != SS$_NORMAL))
    return SCPE_TTOERR;
return SCPE_OK;
}

/* Win32 routines */

#elif defined (_WIN32)

#include <conio.h>
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#define RAW_MODE 0
static HANDLE std_input = NULL;
static HANDLE std_output = NULL;
static DWORD saved_mode;
 
static BOOL WINAPI
ControlHandler(DWORD dwCtrlType)
{
    extern void int_handler(int sig);
    DWORD Mode;

    switch (dwCtrlType)
    {
    case CTRL_BREAK_EVENT:      // Use CTRL-Break or CTRL-C to simulate 
    case CTRL_C_EVENT:          // SERVICE_CONTROL_STOP in debug mode
        int_handler(0);
        return TRUE;

    case CTRL_CLOSE_EVENT:      // Window is Closing
    case CTRL_LOGOFF_EVENT:     // User is logging off
        if (!GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &Mode))
            return TRUE;        // Not our User, so ignore
        /* fall through */

    case CTRL_SHUTDOWN_EVENT:   // System is shutting down
        int_handler(0);
        return TRUE;
    }
    return FALSE;
}

t_stat sim_ttinit (void)
{
    SetConsoleCtrlHandler(ControlHandler, TRUE);
    std_input = GetStdHandle (STD_INPUT_HANDLE);
    std_output = GetStdHandle (STD_OUTPUT_HANDLE);

    if (std_input && std_input != INVALID_HANDLE_VALUE)     /* Not Background process? */
        GetConsoleMode (std_input, &saved_mode);            /* Save Mode */

    return SCPE_OK;
}
 
t_stat sim_ttrun (void)
{
    if (std_input && std_input != INVALID_HANDLE_VALUE)     /* Not Background process? */
    {
        if (!GetConsoleMode(std_input, &saved_mode) ||
            !SetConsoleMode(std_input, RAW_MODE))
        {
            return SCPE_TTYERR;
        }
    }

    if (sim_log)
    {
        fflush (sim_log);
        _setmode (_fileno (sim_log), _O_BINARY);
    }

    return SCPE_OK;
}

t_stat sim_ttcmd (void)
{
    if (sim_log)
    {
        fflush (sim_log);
        _setmode (_fileno (sim_log), _O_TEXT);
    }

    if (std_input && std_input != INVALID_HANDLE_VALUE &&     /* Not Background process? */
        !SetConsoleMode(std_input, saved_mode))               /* Restore Normal mode */
    {
        return SCPE_TTYERR;
    }

    return SCPE_OK;
}

t_stat sim_ttclose (void)
{
    return SCPE_OK;
}

t_bool sim_ttisatty (void)
{
    DWORD Mode;
    return std_input && std_input != INVALID_HANDLE_VALUE && GetConsoleMode (std_input, &Mode);
}

t_stat sim_os_poll_kbd (void)
{
    int c;

    if (std_input == NULL || std_input == INVALID_HANDLE_VALUE)      /* No keyboard for background processes */
        return SCPE_OK;
    
    DWORD ev_count = 0;
    GetNumberOfConsoleInputEvents(std_input, &ev_count);

    /* ToDo: megre changes from 3.8.2 */

    if (!_kbhit ())
    {
        /* 
         * Drain non-keyboard events and also keyboard events not consumed by kbhit/getch,
         * so console handle is unmarked as signalled. Remove only those events that were
         * present in the input queue before kbhit.
         */
        while (ev_count)
        {
            INPUT_RECORD ir[10];
            DWORD nread;
            DWORD nrd = 10;
            if (ev_count < nrd)  nrd = ev_count;
            ReadConsoleInput(std_input, ir, nrd, & nread);
            ev_count -= nrd;
        }

        return SCPE_OK;
    }

    c = _getch ();
    if ((c & 0177) == sim_del_char)
        c = 0177;
    if ((c & 0177) == sim_int_char)
        return SCPE_STOP;
    if (sim_brk_char && (c & 0177) == sim_brk_char)
        return SCPE_BREAK;
    return c | SCPE_KFLAG;
}

t_stat sim_os_putchar (int32 c)
{
    DWORD unused;

    if (c != 0177)
        WriteConsoleA(std_output, &c, 1, &unused, NULL);
    return SCPE_OK;
}

class smp_pollable_console_keyboard_impl : public smp_pollable_console_keyboard
{
private:
    HANDLE hConsoleInput;
public:
    smp_pollable_console_keyboard_impl()
    {
        hConsoleInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    virtual ~smp_pollable_console_keyboard_impl()
    {
        if (hConsoleInput != INVALID_HANDLE_VALUE)
            CloseHandle(hConsoleInput);
    }
    smp_pollable_handle_t pollable_handle()
    {
        return hConsoleInput;
    }
    virtual const char* pollable_handle_op()
    {
        return "h";
    }

    void clear() {}
    void wait() {}
    t_bool trywait() { return FALSE; }
    void release(int count = 1) {}
};

smp_pollable_console_keyboard* smp_pollable_console_keyboard::instance = NULL;
smp_pollable_console_keyboard* smp_pollable_console_keyboard::get()
{
    if (instance == NULL)
        instance = new smp_pollable_console_keyboard_impl();
    return instance;
}

/* OS/2 routines, from Bruce Ray and Holger Veit */

#elif defined (__OS2__)

#include <conio.h>

t_stat sim_ttinit (void)
{
return SCPE_OK;
}

t_stat sim_ttrun (void)
{
return SCPE_OK;
}

t_stat sim_ttcmd (void)
{
return SCPE_OK;
}

t_bool sim_ttisatty (void)
{
return 1;
}

t_stat sim_ttclose (void)
{
return SCPE_OK;
}

t_stat sim_os_poll_kbd (void)
{
int c;

#if defined (__EMX__)
switch (c = _read_kbd(0,0,0)) {                         /* EMX has _read_kbd */

    case -1:                                            /* no char*/
        return SCPE_OK;

    case 0:                                             /* char pending */
        c = _read_kbd(0,1,0);
        break;

    default:                                            /* got char */
        break;
        }
#else
if (!kbhit ())
    return SCPE_OK;
c = getch();
#endif
if ((c & 0177) == sim_del_char)
    c = 0177;
if ((c & 0177) == sim_int_char)
    return SCPE_STOP;
if (sim_brk_char && ((c & 0177) == sim_brk_char))
    return SCPE_BREAK;
return c | SCPE_KFLAG;
}

t_stat sim_os_putchar (int32 c)
{
if (c != 0177) {
#if defined (__EMX__)
    smp_putchar (c);
#else
    putch (c);
#endif
    fflush (smp_stdout);
    }
return SCPE_OK;
}

/* Metrowerks CodeWarrior Macintosh routines, from Louis Chretien and
   Peter Schorn */

#elif defined (__MWERKS__) && defined (macintosh)

#include <console.h>
#include <Mactypes.h>
#include <string.h>
#include <sioux.h>
#include <unistd.h>
#include <siouxglobals.h>
#include <Traps.h>
#include <LowMem.h>

/* function prototypes */

Boolean SIOUXIsAppWindow(WindowPtr window);
void SIOUXDoMenuChoice(long menuValue);
void SIOUXUpdateMenuItems(void);
void SIOUXUpdateScrollbar(void);
int ps_kbhit(void);
int ps_getch(void);

extern char sim_name[];
extern pSIOUXWin SIOUXTextWindow;
static CursHandle iBeamCursorH = NULL;                  /* contains the iBeamCursor */

static void updateCursor(void) {
    WindowPtr window;
    window = FrontWindow();
    if (SIOUXIsAppWindow(window)) {
        GrafPtr savePort;
        Point localMouse;
        GetPort(&savePort);
        SetPort(window);
#if TARGET_API_MAC_CARBON
        GetGlobalMouse(&localMouse);
#else
        localMouse = LMGetMouseLocation();
#endif
        GlobalToLocal(&localMouse);
        if (PtInRect(localMouse, &(*SIOUXTextWindow->edit)->viewRect) && iBeamCursorH) {
            SetCursor(*iBeamCursorH);
        }
        else {
            SetCursor(&qd.arrow);
        }
        TEIdle(SIOUXTextWindow->edit);
        SetPort(savePort);
    }
    else {
        SetCursor(&qd.arrow);
        TEIdle(SIOUXTextWindow->edit);
    }
    return;
}

int ps_kbhit(void) {
    EventRecord event;
    int c;
    updateCursor();
    SIOUXUpdateScrollbar();
    while (GetNextEvent(updateMask | osMask | mDownMask | mUpMask | activMask |
             highLevelEventMask | diskEvt, &event)) {
        SIOUXHandleOneEvent(&event);
    }
    if (SIOUXQuitting) {
        exit(1);
    }
    if (EventAvail(keyDownMask,&event)) {
        c = event.message&charCodeMask;
        if ((event.modifiers & cmdKey) && (c > 0x20)) {
            GetNextEvent(keyDownMask, &event);
            SIOUXHandleOneEvent(&event);
            if (SIOUXQuitting) {
                exit(1);
            }
            return false;
        }
        return true;
    }
    else {
        return false;
    }
}

int ps_getch(void) {
    int c;
    EventRecord event;
    fflush(smp_stdout);
    updateCursor();
    while(!GetNextEvent(keyDownMask,&event)) {
        if (GetNextEvent(updateMask | osMask | mDownMask | mUpMask | activMask |
             highLevelEventMask | diskEvt, &event)) {
            SIOUXUpdateScrollbar();
            SIOUXHandleOneEvent(&event);
        }
    }
    if (SIOUXQuitting) {
        exit(1);
    }
    c = event.message&charCodeMask;
    if ((event.modifiers & cmdKey) && (c > 0x20)) {
        SIOUXUpdateMenuItems();
        SIOUXDoMenuChoice(MenuKey(c));
    }
    if (SIOUXQuitting) {
        exit(1);
    }
   return c;
}

/* Note that this only works if the call to sim_ttinit comes before any output to the console */

t_stat sim_ttinit (void) {
    int i;
    /* this blank will later be replaced by the number of characters */
    char title[50] = " ";
    unsigned char ptitle[50];
    SIOUXSettings.autocloseonquit       = TRUE;
    SIOUXSettings.asktosaveonclose = FALSE;
    SIOUXSettings.showstatusline = FALSE;
    SIOUXSettings.columns = 80;
    SIOUXSettings.rows = 40;
    SIOUXSettings.toppixel = 42;
    SIOUXSettings.leftpixel     = 6;
    iBeamCursorH = GetCursor(iBeamCursor);
    strcat(title, sim_name);
    strcat(title, " Simulator");
    title[0] = strlen(title) - 1;                       /* Pascal string done */
    for (i = 0; i <= title[0]; i++) {                   /* copy to unsigned char */
        ptitle[i] = title[i];
    }
    SIOUXSetTitle(ptitle);
    return SCPE_OK;
}

t_stat sim_ttrun (void)
{
return SCPE_OK;
}

t_stat sim_ttcmd (void)
{
return SCPE_OK;
}

t_bool sim_ttisatty (void)
{
return 1;
}

t_stat sim_ttclose (void)
{
return SCPE_OK;
}

t_stat sim_os_poll_kbd (void)
{
int c;

if (!ps_kbhit ())
    return SCPE_OK;
c = ps_getch();
if ((c & 0177) == sim_del_char)
    c = 0177;
if ((c & 0177) == sim_int_char) return SCPE_STOP;
if (sim_brk_char && ((c & 0177) == sim_brk_char))
    return SCPE_BREAK;
return c | SCPE_KFLAG;
}

t_stat sim_os_putchar (int32 c)
{
if (c != 0177) {
    smp_putchar (c);
    fflush (smp_stdout);
    }
return SCPE_OK;
}

/* BSD UNIX routines */

#elif defined (BSDTTY)

#include <sgtty.h>
#include <fcntl.h>
#include <unistd.h>

struct sgttyb cmdtty,runtty;                            /* V6/V7 stty data */
struct tchars cmdtchars,runtchars;                      /* V7 editing */
struct ltchars cmdltchars,runltchars;                   /* 4.2 BSD editing */
int cmdfl,runfl;                                        /* TTY flags */

t_stat sim_ttinit (void)
{
    cmdfl = fcntl (0, F_GETFL, 0);                          /* get old flags  and status */
    runfl = cmdfl | FNDELAY;
    if (ioctl (0, TIOCGETP, &cmdtty) < 0)
        return SCPE_TTIERR;
    if (ioctl (0, TIOCGETC, &cmdtchars) < 0)
        return SCPE_TTIERR;
    if (ioctl (0, TIOCGLTC, &cmdltchars) < 0)
        return SCPE_TTIERR;
    runtty = cmdtty;                                        /* initial run state */
    runtty.sg_flags = cmdtty.sg_flags & ~(ECHO|CRMOD) | CBREAK;
    runtchars.t_intrc = sim_int_char;                       /* interrupt */
    runtchars.t_quitc = 0xFF;                               /* no quit */
    runtchars.t_startc = 0xFF;                              /* no host sync */
    runtchars.t_stopc = 0xFF;
    runtchars.t_eofc = 0xFF;
    runtchars.t_brkc = 0xFF;
    runltchars.t_suspc = 0xFF;                              /* no specials of any kind */
    runltchars.t_dsuspc = 0xFF;
    runltchars.t_rprntc = 0xFF;
    runltchars.t_flushc = 0xFF;
    runltchars.t_werasc = 0xFF;
    runltchars.t_lnextc = 0xFF;
    return SCPE_OK;                                         /* return success */
}

t_stat sim_ttrun (void)
{
    runtchars.t_intrc = sim_int_char;                       /* in case changed */
    fcntl (0, F_SETFL, runfl);                              /* non-block mode */
    if (ioctl (0, TIOCSETP, &runtty) < 0)
        return SCPE_TTIERR;
    if (ioctl (0, TIOCSETC, &runtchars) < 0)
        return SCPE_TTIERR;
    if (ioctl (0, TIOCSLTC, &runltchars) < 0)
        return SCPE_TTIERR;
    return SCPE_OK;
}

t_stat sim_ttcmd (void)
{
    fcntl (0, F_SETFL, cmdfl);                              /* block mode */
    if (ioctl (0, TIOCSETP, &cmdtty) < 0)
        return SCPE_TTIERR;
    if (ioctl (0, TIOCSETC, &cmdtchars) < 0)
        return SCPE_TTIERR;
    if (ioctl (0, TIOCSLTC, &cmdltchars) < 0)
        return SCPE_TTIERR;
    return SCPE_OK;
}

t_bool sim_ttisatty (void)
{
    return isatty (0);
}

t_stat sim_ttclose (void)
{
    return sim_ttcmd ();
}

t_stat sim_os_poll_kbd (void)
{
    int status;
    unsigned char buf[1];

    status = read (0, buf, 1);
    if (status != 1) return SCPE_OK;
    if (sim_brk_char && (buf[0] == sim_brk_char))
        return SCPE_BREAK;
    else return (buf[0] | SCPE_KFLAG);
}

t_stat sim_os_putchar (int32 out)
{
    char c = out;
    write (1, &c, 1);
    return SCPE_OK;
}

/* POSIX UNIX routines, from Leendert Van Doorn */

#else

#include <termios.h>
#include <unistd.h>

static struct termios cmdtty, runtty;

t_stat sim_ttinit (void)
{
    if (!isatty (fileno (stdin)))                           /* skip if !tty */
        return SCPE_OK;
    if (tcgetattr (0, &cmdtty) < 0)                         /* get old flags */
        return SCPE_TTIERR;
    runtty = cmdtty;
    runtty.c_lflag = runtty.c_lflag & ~(ECHO | ICANON);     /* no echo or edit */
    runtty.c_oflag = runtty.c_oflag & ~OPOST;               /* no output edit */
    runtty.c_iflag = runtty.c_iflag & ~ICRNL;               /* no cr conversion */
    runtty.c_cc[VINTR] = sim_int_char;                      /* interrupt */
    runtty.c_cc[VQUIT] = 0;                                 /* no quit */
    runtty.c_cc[VERASE] = 0;
    runtty.c_cc[VKILL] = 0;
    runtty.c_cc[VEOF] = 0;
    runtty.c_cc[VEOL] = 0;
    runtty.c_cc[VSTART] = 0;                                /* no host sync */
    runtty.c_cc[VSUSP] = 0;
    runtty.c_cc[VSTOP] = 0;
#if defined (VREPRINT)
    runtty.c_cc[VREPRINT] = 0;                              /* no specials */
#endif
#if defined (VDISCARD)
    runtty.c_cc[VDISCARD] = 0;
#endif
#if defined (VWERASE)
    runtty.c_cc[VWERASE] = 0;
#endif
#if defined (VLNEXT)
    runtty.c_cc[VLNEXT] = 0;
#endif
    runtty.c_cc[VMIN] = 0;                                  /* no waiting */
    runtty.c_cc[VTIME] = 0;
#if defined (VDSUSP)
    runtty.c_cc[VDSUSP] = 0;
#endif
#if defined (VSTATUS)
    runtty.c_cc[VSTATUS] = 0;
#endif
    return SCPE_OK;
}

t_stat sim_ttrun (void)
{
    if (!isatty (fileno (stdin)))                           /* skip if !tty */
        return SCPE_OK;
    runtty.c_cc[VINTR] = sim_int_char;                      /* in case changed */
    if (tcsetattr (0, TCSAFLUSH, &runtty) < 0)
        return SCPE_TTIERR;
    return SCPE_OK;
}

t_stat sim_ttcmd (void)
{
    if (!isatty (fileno (stdin)))                           /* skip if !tty */
        return SCPE_OK;
    if (tcsetattr (0, TCSAFLUSH, &cmdtty) < 0)
        return SCPE_TTIERR;
    return SCPE_OK;
}

t_bool sim_ttisatty(void)
{
    return isatty (fileno (stdin));
}

t_stat sim_ttclose (void)
{
    return sim_ttcmd ();
}

t_stat sim_os_poll_kbd (void)
{
    DECL_RESTARTABLE(rc);
    unsigned char buf[1];

    switch (smp_wait(smp_pollable_console_keyboard::get(), 0))
    {
    case 0:    return SCPE_OK;
    case -1:   return SCPE_STOP;
    default:   break;
    }

    DO_RESTARTABLE(rc, read (0, buf, 1));
    if (rc == -1 && errno == EWOULDBLOCK)  return SCPE_OK;
    if (rc == 0)  return SCPE_OK;
    if (rc != 1) return SCPE_STOP;
    if (sim_brk_char && buf[0] == sim_brk_char)
        return SCPE_BREAK;
    else
        return (buf[0] | SCPE_KFLAG);
}

t_stat sim_os_putchar (int32 out)
{
    DECL_RESTARTABLE(rc);
    char c = out;
    DO_RESTARTABLE(rc, write (1, &c, 1));
    return SCPE_OK;
}

class smp_pollable_console_keyboard_impl : public smp_pollable_console_keyboard
{
public:
    smp_pollable_console_keyboard_impl() {}
    virtual ~smp_pollable_console_keyboard_impl() {}
    smp_pollable_handle_t pollable_handle()
        { return 0; }
    virtual const char* pollable_handle_op()
        { return "r"; }
    void clear() {}
    void wait() {}
    t_bool trywait() { return FALSE; }
    void release(int count = 1) {}
};

smp_pollable_console_keyboard* smp_pollable_console_keyboard::instance = NULL;
smp_pollable_console_keyboard* smp_pollable_console_keyboard::get()
{
    if (instance == NULL)
        instance = new smp_pollable_console_keyboard_impl();
    return instance;
}

#endif
