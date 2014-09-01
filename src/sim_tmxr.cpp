/* sim_tmxr.c: Telnet terminal multiplexor library

   Copyright (c) 2001-2011, Robert M Supnik

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

   Based on the original DZ11 simulator by Thord Nilson, as updated by
   Arthur Krewat.

   02-Jun-11    MP      Fixed telnet option negotiation loop with some clients
                        Added Option Negotiation and Debugging Support
   17-Jan-11    MP      Added Buffered line capabilities
   16-Jan-11    MP      Made option negotiation more reliable
   20-Nov-08    RMS     Added three new standardized SHOW routines
   30-Sep-08    JDB     Reverted tmxr_find_ldsc to original implementation
   27-May-08    JDB     Added line connection order to tmxr_poll_conn,
                        added tmxr_set_lnorder and tmxr_show_lnorder
   14-May-08    JDB     Print device and line to which connection was made
   11-Apr-07    JDB     Worked around Telnet negotiation problem with QCTerm
   16-Aug-05    RMS     Fixed C++ declaration and cast problems
   29-Jun-05    RMS     Extended tmxr_dscln to support unit array devices
                        Fixed bug in SET LOG/NOLOG
   04-Jan-04    RMS     Changed TMXR ldsc to be pointer to linedesc array
                        Added tmxr_linemsg, circular output pointers, logging
                        (from Mark Pizzolato)
   29-Dec-03    RMS     Added output stall support
   01-Nov-03    RMS     Cleaned up attach routine
   09-Mar-03    RMS     Fixed bug in SHOW CONN
   22-Dec-02    RMS     Fixed bugs in IAC+IAC receive and transmit sequences
                        Added support for received break (all from by Mark Pizzolato)
                        Fixed bug in attach
   31-Oct-02    RMS     Fixed bug in 8b (binary) support
   22-Aug-02    RMS     Added tmxr_open_master, tmxr_close_master
   30-Dec-01    RMS     Added tmxr_fstats, tmxr_dscln, renamed tmxr_fstatus
   03-Dec-01    RMS     Changed tmxr_fconns for extended SET/SHOW
   20-Oct-01    RMS     Fixed bugs in read logic (found by Thord Nilson).
                        Added tmxr_rqln, tmxr_tqln

   This library includes:

   tmxr_poll_conn -     poll for connection
   tmxr_reset_ln -      reset line
   tmxr_getc_ln -       get character for line
   tmxr_poll_rx -       poll receive
   tmxr_putc_ln -       put character for line
   tmxr_poll_tx -       poll transmit
   tmxr_open_master -   open master connection
   tmxr_close_master -  close master connection
   tmxr_attach  -       attach terminal multiplexor
   tmxr_detach  -       detach terminal multiplexor
   tmxr_ex      -       (null) examine
   tmxr_dep     -       (null) deposit
   tmxr_msg     -       send message to socket
   tmxr_linemsg -       send message to line
   tmxr_fconns  -       output connection status
   tmxr_fstats  -       output connection statistics
   tmxr_dscln   -       disconnect line (SET routine)
   tmxr_rqln    -       number of available characters for line
   tmxr_tqln    -       number of buffered characters for line
   tmxr_set_lnorder -   set line connection order
   tmxr_show_lnorder -  show line connection order

   All routines are OS-independent.
*/

#include "sim_defs.h"
#include "sim_sock.h"
#include "sim_tmxr.h"
#include "scp.h"
#include <ctype.h>

/* Telnet protocol constants - negatives are for init'ing signed char data */

/* Commands */
#define TN_IAC          -1                              /* protocol delim */
#define TN_DONT         -2                              /* dont */
#define TN_DO           -3                              /* do */
#define TN_WONT         -4                              /* wont */
#define TN_WILL         -5                              /* will */
#define TN_SB           -6                              /* sub-option negotiation */
#define TN_GA           -7                              /* go ahead */
#define TN_EL           -8                              /* erase line */
#define TN_EC           -9                              /* erase character */
#define TN_AYT          -10                             /* are you there */
#define TN_AO           -11                             /* abort output */
#define TN_IP           -12                             /* interrupt process */
#define TN_BRK          -13                             /* break */
#define TN_DATAMK       -14                             /* data mark */
#define TN_NOP          -15                             /* no operation */
#define TN_SE           -16                             /* end sub-option negot */

/* Options */

#define TN_BIN          0                               /* bin */
#define TN_ECHO         1                               /* echo */
#define TN_SGA          3                               /* sga */
#define TN_LINE         34                              /* line mode */
#define TN_CR           015                             /* carriage return */
#define TN_LF           012                             /* line feed */
#define TN_NUL          000                             /* null */

/* Telnet line states */

#define TNS_NORM        000                             /* normal */
#define TNS_IAC         001                             /* IAC seen */
#define TNS_WILL        002                             /* WILL seen */
#define TNS_WONT        003                             /* WONT seen */
#define TNS_SKIP        004                             /* skip next cmd */
#define TNS_CRPAD       005                             /* CR padding */
#define TNS_DO          006                             /* DO request pending rejection */
#define TNS_DONT        007                             /* DONT request pending rejection */

void tmxr_rmvrc (TMLN *lp, int32 p);
int32 tmxr_send_buffered_data (TMLN *lp);
TMLN *tmxr_find_ldsc (UNIT *uptr, int32 val, TMXR *mp);

extern int32 sim_switches;
extern char sim_name[];
extern SMP_FILE *sim_log;
extern uint32 sim_os_msec (void);

/* Poll for new connection

   Called from unit service routine to test for new connection

   Inputs:
        *mp     =       pointer to terminal multiplexor descriptor
   Outputs:
        line number activated, -1 if none

   If a connection order is defined for the descriptor, and the first value is
   not -1 (indicating default order), then the order array is used to find an
   open line.  Otherwise, a search is made of all lines in numerical sequence.
*/

int32 tmxr_poll_conn (TMXR *mp)
{
    SOCKET newsock;
    TMLN *lp;
    int32 *op;
    int32 i, j, psave;
    uint32 ipaddr;
    char cmsg[80];
    char dmsg[80] = "";
    char lmsg[80] = "";
    char msgbuf[256];
    static char mantra[] =
    {
#if defined (VM_VAX)
        TN_IAC, TN_DONT, TN_LINE,
        TN_IAC, TN_WILL, TN_SGA,
        TN_IAC, TN_DO,   TN_SGA,
        TN_IAC, TN_WILL, TN_ECHO,
        TN_IAC, TN_WILL, TN_BIN,
        TN_IAC, TN_DO,   TN_BIN
#else
        TN_IAC, TN_WILL, TN_LINE,
        TN_IAC, TN_WILL, TN_SGA,
        TN_IAC, TN_WILL, TN_ECHO,
        TN_IAC, TN_WILL, TN_BIN,
        TN_IAC, TN_DO, TN_BIN
#endif
    };

    newsock = sim_accept_conn (mp->master, &ipaddr);        /* poll connect */
    if (newsock != INVALID_SOCKET) {                        /* got a live one? */
        op = mp->lnorder;                                   /* get line connection order list pointer */
        i = mp->lines;                                      /* play it safe in case lines == 0 */

        for (j = 0; j < mp->lines; j++, i++) {              /* find next avail line */
            if (op && (*op >= 0) && (*op < mp->lines))      /* order list present and valid? */
                i = *op++;                                  /* get next line in list to try */
            else                                            /* no list or not used or range error */
                i = j;                                      /* get next sequential line */

            lp = mp->ldsc + i;                              /* get pointer to line descriptor */
            if (lp->conn == 0)                              /* is the line available? */
                break;                                      /* yes, so stop search */
            }

        if (i >= mp->lines) {                               /* all busy? */
            tmxr_msg (newsock, "All connections busy\r\n");
            sim_close_sock (newsock, 0);
            }
        else {
            lp = mp->ldsc + i;                              /* get line desc */
            lp->conn = newsock;                             /* record connection */
            lp->ipad = ipaddr;                              /* ip address */
            lp->mp = mp;                                    /* save mux */
            sim_write_sock (newsock, mantra, sizeof(mantra));
            tmxr_debug (TMXR_DBG_XMT, lp, "Sending", mantra, sizeof(mantra));
            sprintf (cmsg, "\n\r\nConnected to the %s simulator ", sim_name);

            if (mp->dptr) {                                 /* device defined? */
                sprintf (dmsg, "%s device",                 /* report device name */
                               sim_dname (mp->dptr));

                if (mp->lines > 1)                          /* more than one line? */
                    sprintf (lmsg, ", line %d", i);         /* report the line number */
                }

            sprintf (msgbuf, "%s%s%s\r\n\n", cmsg, dmsg, lmsg);
            lp->cnms = sim_os_msec ();                      /* time of conn */
            if (!mp->buffered) {
                lp->txbpi = 0;                              /* init buf pointers */
                lp->txbpr = (int32)(lp->txbsz - strlen (msgbuf));
                lp->rxcnt = lp->txcnt = lp->txdrp = 0;      /* init counters */
                }
            else
                if (lp->txcnt > lp->txbsz)
                    lp->txbpr = (lp->txbpi + 1) % lp->txbsz;
                else
                    lp->txbpr = (int32)(lp->txbsz - strlen (msgbuf));
            lp->tsta = 0;                                   /* init telnet state */
            lp->xmte = 1;                                   /* enable transmit */
            lp->dstb = 0;                                   /* default bin mode */
            psave = lp->txbpi;                              /* save insertion pointer */
            lp->txbpi = lp->txbpr;                          /* insert connection message */
            tmxr_linemsg (lp, msgbuf);                      /* beginning of buffer */
            lp->txbpi = psave;                              /* restore insertion pointer */
            tmxr_poll_tx (mp);                              /* flush output */
            lp->txcnt -= (int32)strlen (msgbuf);            /* adjust statistics */
            return i;
            }
        }                                                   /* end if newsock */
    return -1;
}

/* Reset line */

void tmxr_reset_ln (TMLN *lp)
{
    if (lp->txlog)                                          /* dump log */
        fflush (lp->txlog);
    tmxr_send_buffered_data (lp);                           /* send buffered data */
    sim_close_sock (lp->conn, 0);                           /* reset conn */
    lp->conn = lp->tsta = 0;                                /* reset state */
    lp->rxbpr = lp->rxbpi = 0;
    if (!lp->txbfd) 
        lp->txbpr = lp->txbpi = 0;
    lp->xmte = 1;
    lp->dstb = 0;
}

/* Get character from specific line

   Inputs:
        *lp     =       pointer to terminal line descriptor
   Output:
        valid + char, 0 if line
*/

int32 tmxr_getc_ln (TMLN *lp)
{
    int32 j, val = 0;
    uint32 tmp;

    if (lp->conn && lp->rcve) {                             /* conn & enb? */
        j = lp->rxbpi - lp->rxbpr;                          /* # input chrs */
        if (j) {                                            /* any? */
            tmp = lp->rxb[lp->rxbpr];                       /* get char */
            val = TMXR_VALID | (tmp & 0377);                /* valid + chr */
            if (lp->rbr[lp->rxbpr])                         /* break? */
                val = val | SCPE_BREAK;
            lp->rxbpr = lp->rxbpr + 1;                      /* adv pointer */
            }
        }                                                   /* end if conn */
    if (lp->rxbpi == lp->rxbpr)                             /* empty? zero ptrs */
        lp->rxbpi = lp->rxbpr = 0;
    return val;
}

/* Poll for input

   Inputs:
        *mp     =       pointer to terminal multiplexor descriptor
   Outputs:     none
*/

void tmxr_poll_rx (TMXR *mp)
{
    int32 i, nbytes, j;
    TMLN *lp;

    for (i = 0; i < mp->lines; i++) {                       /* loop thru lines */
        lp = mp->ldsc + i;                                  /* get line desc */
        if (!lp->conn || !lp->rcve)                         /* skip if !conn */
            continue;

        nbytes = 0;
        if (lp->rxbpi == 0)                                 /* need input? */
            nbytes = sim_read_sock (lp->conn,               /* yes, read */
                &(lp->rxb[lp->rxbpi]),                      /* leave spc for */
                TMXR_MAXBUF - TMXR_GUARD);                  /* Telnet cruft */
        else if (lp->tsta)                                  /* in Telnet seq? */
            nbytes = sim_read_sock (lp->conn,               /* yes, read to end */
                &(lp->rxb[lp->rxbpi]),
                TMXR_MAXBUF - lp->rxbpi);
        if (nbytes < 0)                                     /* closed? reset ln */
            tmxr_reset_ln (lp);
        else if (nbytes > 0) {                              /* if data rcvd */

            tmxr_debug (TMXR_DBG_RCV, lp, "Received", &(lp->rxb[lp->rxbpi]), nbytes);

            j = lp->rxbpi;                                  /* start of data */
            memset (&lp->rbr[j], 0, nbytes);                /* clear status */
            lp->rxbpi = lp->rxbpi + nbytes;                 /* adv pointers */
            lp->rxcnt = lp->rxcnt + nbytes;

    /* Examine new data, remove TELNET cruft before making input available */

            for (; j < lp->rxbpi; ) {                       /* loop thru char */
                signed char tmp = lp->rxb[j];               /* get char */
                switch (lp->tsta) {                         /* case tlnt state */

                case TNS_NORM:                              /* normal */
                    if (tmp == TN_IAC) {                    /* IAC? */
                        lp->tsta = TNS_IAC;                 /* change state */
                        tmxr_rmvrc (lp, j);                 /* remove char */
                        break;
                        }
                    if ((tmp == TN_CR) && lp->dstb)         /* CR, no bin */
                        lp->tsta = TNS_CRPAD;               /* skip pad char */
                    j = j + 1;                              /* advance j */
                    break;

                case TNS_IAC:                               /* IAC prev */
                    if (tmp == TN_IAC) {                    /* IAC + IAC */
                        lp->tsta = TNS_NORM;                /* treat as normal */
                        j = j + 1;                          /* advance j */
                        break;                              /* keep IAC */
                        }
                    if (tmp == TN_BRK) {                    /* IAC + BRK? */
                        lp->tsta = TNS_NORM;                /* treat as normal */
                        lp->rxb[j] = 0;                     /* char is null */
                        lp->rbr[j] = 1;                     /* flag break */
                        j = j + 1;                          /* advance j */
                        break;
                        }
                    switch (tmp) {
                    case TN_WILL:                           /* IAC + WILL? */
                        lp->tsta = TNS_WILL;
                        break;
                    case TN_WONT:                           /* IAC + WONT? */
                        lp->tsta = TNS_WONT;
                        break;
                    case TN_DO:                             /* IAC + DO? */
                        lp->tsta = TNS_DO;
                        break;
                    case TN_DONT:                           /* IAC + DONT? */
                        lp->tsta = TNS_DONT;
                        break;
                    case TN_GA: case TN_EL:                 /* IAC + other 2 byte types */
                    case TN_EC: case TN_AYT:                
                    case TN_AO: case TN_IP:
                    case TN_NOP: 
                        lp->tsta = TNS_NORM;                /* ignore */
                        break;
                    case TN_SB:                             /* IAC + SB sub-opt negotiation */
                    case TN_DATAMK:                         /* IAC + data mark */
                    case TN_SE:                             /* IAC + SE sub-opt end */
                        lp->tsta = TNS_NORM;                /* ignore */
                        break;
                        }
                    tmxr_rmvrc (lp, j);                     /* remove char */
                    break;

                case TNS_WILL: case TNS_WONT:               /* IAC+WILL/WONT prev */
                    if (tmp == TN_BIN) {                    /* BIN? */
                        if (lp->tsta == TNS_WILL)
                            lp->dstb = 0;
                        else lp->dstb = 1;
                        }
                    tmxr_rmvrc (lp, j);                     /* remove it */
                    lp->tsta = TNS_NORM;                    /* next normal */
                    break;

                /* Negotiation with the HP terminal emulator "QCTerm" is not working.
                   QCTerm says "WONT BIN" but sends bare CRs.  RFC 854 says:

                     Note that "CR LF" or "CR NUL" is required in both directions
                     (in the default ASCII mode), to preserve the symmetry of the
                     NVT model.  ...The protocol requires that a NUL be inserted
                     following a CR not followed by a LF in the data stream.

                   Until full negotiation is implemented, we work around the problem
                   by checking the character following the CR in non-BIN mode and
                   strip it only if it is LF or NUL.  This should not affect
                   conforming clients.
                */

                case TNS_CRPAD:                             /* only LF or NUL should follow CR */
                    lp->tsta = TNS_NORM;                    /* next normal */
                    if ((tmp == TN_LF) ||                   /* CR + LF ? */
                        (tmp == TN_NUL))                    /* CR + NUL? */
                        tmxr_rmvrc (lp, j);                 /* remove it */
                    break;

                case TNS_DO:                                /* pending DO request */
                case TNS_DONT:                              /* pending DONT request */
                case TNS_SKIP: default:                     /* skip char */
                    tmxr_rmvrc (lp, j);                     /* remove char */
                    lp->tsta = TNS_NORM;                    /* next normal */
                    break;
                    }                                       /* end case state */
                }                                           /* end for char */
                if (nbytes != (lp->rxbpi-lp->rxbpr))
                    tmxr_debug (TMXR_DBG_RCV, lp, "Remaining", &(lp->rxb[lp->rxbpi]), lp->rxbpi-lp->rxbpr);
            }                                               /* end else nbytes */
        }                                                   /* end for lines */
    for (i = 0; i < mp->lines; i++) {                       /* loop thru lines */
        lp = mp->ldsc + i;                                  /* get line desc */
        if (lp->rxbpi == lp->rxbpr)                         /* if buf empty, */
            lp->rxbpi = lp->rxbpr = 0;                      /* reset pointers */
        }                                                   /* end for */
}

/* Return count of available characters for line */

int32 tmxr_rqln (TMLN *lp)
{
    return (lp->rxbpi - lp->rxbpr + ((lp->rxbpi < lp->rxbpr)? TMXR_MAXBUF: 0));
}

/* Remove character p (and matching status) from line l input buffer */

void tmxr_rmvrc (TMLN *lp, int32 p)
{
    for ( ; p < lp->rxbpi; p++) {
        lp->rxb[p] = lp->rxb[p + 1];
        lp->rbr[p] = lp->rbr[p + 1];
        }
    lp->rxbpi = lp->rxbpi - 1;
}

/* Store character in line buffer

   Inputs:
        *lp     =       pointer to line descriptor
        chr     =       characters
   Outputs:
        status  =       ok, connection lost, or stall
*/

t_stat tmxr_putc_ln (TMLN *lp, int32 chr)
{
    if (lp->txlog)                                          /* log if available */
        fputc (chr, lp->txlog);
    if ((lp->conn == 0) && (!lp->txbfd))                    /* no conn & not buffered? */
        if (lp->txlog)                                      /* if it was logged, we got it */           
            return SCPE_OK;
        else {
            ++lp->txdrp;                                    /* lost */
            return SCPE_LOST;
            }
#define TXBUF_AVAIL(lp) (lp->txbsz - tmxr_tqln (lp))
#define TXBUF_CHAR(lp, c) {                                   \
        lp->txb[lp->txbpi++] = (char)(c);                     \
        lp->txbpi %= lp->txbsz;                               \
        if (lp->txbpi == lp->txbpr)                           \
            lp->txbpr = (1+lp->txbpr)%lp->txbsz, ++lp->txdrp; \
        }
    if ((lp->txbfd) || (TXBUF_AVAIL(lp) > 1)) {             /* room for char (+ IAC)? */
        if (TN_IAC == (char) chr)                           /* char == IAC ? */
            TXBUF_CHAR (lp, TN_IAC);                        /* stuff extra IAC char */
        TXBUF_CHAR (lp, chr);                               /* buffer char & adv pointer */
        if ((!lp->txbfd) && (TXBUF_AVAIL (lp) <= TMXR_GUARD))/* near full? */
            lp->xmte = 0;                                   /* disable line */
        return SCPE_OK;                                     /* char sent */
        }
    ++lp->txdrp; lp->xmte = 0;                              /* no room, dsbl line */
    return SCPE_STALL;                                      /* char not sent */
}

/* Poll for output

   Inputs:
        *mp     =       pointer to terminal multiplexor descriptor
   Outputs:
        none
*/

void tmxr_poll_tx (TMXR *mp)
{
    int32 i, nbytes;
    TMLN *lp;

    for (i = 0; i < mp->lines; i++)                     /* loop thru lines */
    {
        lp = mp->ldsc + i;                              /* get line desc */
        if (lp->conn == 0)                              /* skip if !conn */
            continue;
        nbytes = tmxr_send_buffered_data (lp);          /* buffered bytes */
        if (nbytes == 0)                                /* buf empty? enab line */
            lp->xmte = 1;
    }
}

/* Send buffered data across network

   Inputs:
        *lp     =       pointer to line descriptor
   Outputs:
        returns number of bytes still buffered
*/

int32 tmxr_send_buffered_data (TMLN *lp)
{
    int32 nbytes, sbytes;

    nbytes = tmxr_tqln(lp);                                 /* avail bytes */
    if (nbytes) {                                           /* >0? write */
        if (lp->txbpr < lp->txbpi)                          /* no wrap? */
            sbytes = sim_write_sock (lp->conn,              /* write all data */
                &(lp->txb[lp->txbpr]), nbytes);
        else sbytes = sim_write_sock (lp->conn,             /* write to end buf */
                &(lp->txb[lp->txbpr]), lp->txbsz - lp->txbpr);
        if (sbytes != SOCKET_ERROR) {                       /* ok? */
            tmxr_debug (TMXR_DBG_XMT, lp, "Sent", &(lp->txb[lp->txbpr]), sbytes);
            lp->txbpr = (lp->txbpr + sbytes);               /* update remove ptr */
            if (lp->txbpr >= lp->txbsz)                     /* wrap? */
                lp->txbpr = 0;
            lp->txcnt = lp->txcnt + sbytes;                 /* update counts */
            nbytes = nbytes - sbytes;
            }
        if (nbytes && (lp->txbpr == 0))     {               /* more data and wrap? */
            sbytes = sim_write_sock (lp->conn, lp->txb, nbytes);
            if (sbytes != SOCKET_ERROR) {                   /* ok */
                tmxr_debug (TMXR_DBG_XMT, lp, "Sent", lp->txb, sbytes);
                lp->txbpr = (lp->txbpr + sbytes);           /* update remove ptr */
                if (lp->txbpr >= lp->txbsz)                 /* wrap? */
                    lp->txbpr = 0;
                lp->txcnt = lp->txcnt + sbytes;             /* update counts */
                nbytes = nbytes - sbytes;
                }
            }
        }                                                   /* end if nbytes */
    return nbytes;
}

/* Return count of buffered characters for line */

int32 tmxr_tqln (TMLN *lp)
{
    return (lp->txbpi - lp->txbpr + ((lp->txbpi < lp->txbpr)? lp->txbsz: 0));
}

/* Open master socket */

t_stat tmxr_open_master (TMXR *mp, char *cptr)
{
    int32 i, port;
    SOCKET sock;
    TMLN *lp;
    t_stat r;

    if (!isdigit(*cptr)) {
        char gbuf[CBUFSIZE];
        cptr = get_glyph (cptr, gbuf, '=');
        if (0 == MATCH_CMD (gbuf, "LOG")) {
            if ((NULL == cptr) || ('\0' == *cptr))
                return SCPE_2FARG;
            strncpy(mp->logfiletmpl, cptr, sizeof(mp->logfiletmpl)-1);
            for (i = 0; i < mp->lines; i++) {
                lp = mp->ldsc + i;
                sim_close_logfile (&lp->txlogref);
                lp->txlog = NULL;
                lp->txlogname = (char*) realloc(lp->txlogname, CBUFSIZE);
                if (mp->lines > 1)
                    sprintf(lp->txlogname, "%s_%d", mp->logfiletmpl, i);
                else
                    strcpy(lp->txlogname, mp->logfiletmpl);
                r = sim_open_logfile (lp->txlogname, TRUE, &lp->txlog, &lp->txlogref);
                if (r == SCPE_OK)
                    setvbuf(lp->txlog, NULL, _IOFBF, 65536);
                else {
                    free (lp->txlogname);
                    lp->txlogname = NULL;
                    break;
                    }
                }
            return r;
            }
        if ((0 == MATCH_CMD (gbuf, "NOBUFFERED")) || 
            (0 == MATCH_CMD (gbuf, "UNBUFFERED"))) {
            if (mp->buffered) {
                mp->buffered = 0;
                for (i = 0; i < mp->lines; i++) { /* default line buffers */
                    lp = mp->ldsc + i;
                    lp->txbsz = TMXR_MAXBUF;
                    lp->txb = (char *)realloc(lp->txb, lp->txbsz);
                    lp->txbfd = lp->txbpi = lp->txbpr = 0;
                    }
                }
            return SCPE_OK;
            }
        if (0 == MATCH_CMD (gbuf, "BUFFERED")) {
            if ((NULL == cptr) || ('\0' == *cptr))
                mp->buffered = 32768;
            else {
                i = (int32) get_uint (cptr, 10, 1024*1024, &r);
                if ((r != SCPE_OK) || (i == 0))
                    return SCPE_ARG;
                mp->buffered = i;
                }
            for (i = 0; i < mp->lines; i++) { /* initialize line buffers */
                lp = mp->ldsc + i;
                lp->txbsz = mp->buffered;
                lp->txbfd = 1;
                lp->txb = (char *)realloc(lp->txb, lp->txbsz);
                lp->txbpi = lp->txbpr = 0;
                }
            return SCPE_OK;
            }
        if (0 == MATCH_CMD (gbuf, "NOLOG")) {
            if ((NULL != cptr) && ('\0' != *cptr))
                return SCPE_2MARG;
            mp->logfiletmpl[0] = '\0';
            for (i = 0; i < mp->lines; i++) { /* close line logs */
                lp = mp->ldsc + i;
                free(lp->txlogname);
                lp->txlogname = NULL;
                if (lp->txlog) {
                    sim_close_logfile (&lp->txlogref);
                    lp->txlog = NULL;
                    }
                }
            return SCPE_OK;
            }
        return SCPE_ARG;
        }
    port = (int32) get_uint (cptr, 10, 65535, &r);          /* get port */
    if ((r != SCPE_OK) || (port == 0))
        return SCPE_ARG;
    sock = sim_master_sock (port);                          /* make master socket */
    if (sock == INVALID_SOCKET)                             /* open error */
        return SCPE_OPENERR;
    smp_printf ("Listening on port %d (socket %d)\n", port, sock);
    if (sim_log)
        fprintf (sim_log, "Listening on port %d (socket %d)\n", port, sock);
    mp->port = port;                                        /* save port */
    mp->master = sock;                                      /* save master socket */
    for (i = 0; i < mp->lines; i++) {                       /* initialize lines */
        lp = mp->ldsc + i;
        lp->conn = lp->tsta = 0;
        lp->rxbpi = lp->rxbpr = 0;
        lp->txbpi = lp->txbpr = 0;
        if (!mp->buffered) {
            lp->txbfd = lp->txbpi = lp->txbpr = 0;
            lp->txbsz = TMXR_MAXBUF;
            lp->txb = (char *)realloc(lp->txb, lp->txbsz);
            }
        lp->rxcnt = lp->txcnt = lp->txdrp = 0;
        lp->xmte = 1;
        lp->dstb = 0;
        }
    return SCPE_OK;
}

/* Attach unit to master socket */

t_stat tmxr_attach (TMXR *mp, UNIT *uptr, char *cptr)
{
    char* tptr;
    t_stat r;
    char pmsg[20], bmsg[32] = "", lmsg[64+PATH_MAX] = "";

    tptr = (char *) malloc (strlen (cptr) +                 /* get string buf */
                            sizeof(pmsg) + 
                            sizeof(bmsg) + sizeof(lmsg));
    if (tptr == NULL)                                       /* no more mem? */
        return SCPE_MEM;
    r = tmxr_open_master (mp, cptr);                        /* open master socket */
    if (r != SCPE_OK) {                                     /* error? */
        free (tptr);                                        /* release buf */
        return SCPE_OPENERR;
        }
    sprintf (pmsg, "%d", mp->port);                         /* copy port */
    if (mp->buffered)
        sprintf (bmsg, ", buffered=%d", mp->buffered);      /* buffer info */
    if (mp->logfiletmpl[0])
        sprintf (lmsg, ", log=%s", mp->logfiletmpl);        /* logfile info */
    sprintf (tptr, "%s%s%s", pmsg, bmsg, lmsg);             /* assemble all */
    uptr->filename = tptr;                                  /* save */
    uptr->flags = uptr->flags | UNIT_ATT;                   /* no more errors */

    if (mp->dptr == NULL)                                   /* has device been set? */
        mp->dptr = find_dev_from_unit (uptr);               /* no, so set device now */

    return SCPE_OK;
}

/* Close master socket */

t_stat tmxr_close_master (TMXR *mp)
{
    int32 i;
    TMLN *lp;

    for (i = 0; i < mp->lines; i++) {                       /* loop thru conn */
        lp = mp->ldsc + i;
        if (lp->conn) {
            tmxr_linemsg (lp, "\r\nDisconnected from the ");
            tmxr_linemsg (lp, sim_name);
            tmxr_linemsg (lp, " simulator\r\n\n");
            tmxr_reset_ln (lp);
            }                                               /* end if conn */
        }                                                   /* end for */
    sim_close_sock (mp->master, 1);                         /* close master socket */
    mp->master = 0;
    return SCPE_OK;
}

/* Detach unit from master socket */

t_stat tmxr_detach (TMXR *mp, UNIT *uptr)
{
    if (!(uptr->flags & UNIT_ATT))                          /* attached? */
        return SCPE_OK;
    tmxr_close_master (mp);                                 /* close master socket */
    free (uptr->filename);                                  /* free port string */
    uptr->filename = NULL;
    uptr->flags = uptr->flags & ~UNIT_ATT;                  /* not attached */
    return SCPE_OK;
}

/* Stub examine and deposit */

t_stat tmxr_ex (t_value *vptr, t_addr addr, UNIT *uptr, int32 sw)
{
    return SCPE_NOFNC;
}

t_stat tmxr_dep (t_value val, t_addr addr, UNIT *uptr, int32 sw)
{
    return SCPE_NOFNC;
}

/* Output message to socket or line descriptor */

void tmxr_msg (SOCKET sock, char *msg)
{
    if (sock)
        sim_write_sock (sock, msg, (int32) strlen (msg));
}

void tmxr_linemsg (TMLN *lp, const char *msg)
{
    int32 len;

    for (len = (int32) strlen (msg); len > 0; --len)
        tmxr_putc_ln (lp, *msg++);
}

/* Print connections - used only in named SHOW command */

void tmxr_fconns (SMP_FILE *st, TMLN *lp, int32 ln)
{
    if (ln >= 0)
        fprintf (st, "line %d: ", ln);
    if (lp->conn) {
        int32 o1, o2, o3, o4, hr, mn, sc;
        uint32 ctime;

        o1 = (lp->ipad >> 24) & 0xFF;
        o2 = (lp->ipad >> 16) & 0xFF;
        o3 = (lp->ipad >> 8) & 0xFF;
        o4 = (lp->ipad) & 0xFF;
        ctime = (sim_os_msec () - lp->cnms) / 1000;
        hr = ctime / 3600;
        mn = (ctime / 60) % 60;
        sc = ctime % 60;
        fprintf (st, "IP address %d.%d.%d.%d", o1, o2, o3, o4);
        if (ctime)
            fprintf (st, ", connected %02d:%02d:%02d\n", hr, mn, sc);
        }
    else fprintf (st, "line disconnected\n");
    if (lp->txlog)
        fprintf (st, "Logging to %s\n", lp->txlogname);
}

/* Print statistics - used only in named SHOW command */

void tmxr_fstats (SMP_FILE *st, TMLN *lp, int32 ln)
{
    static const char *enab = "on";
    static const char *dsab = "off";

    if (ln >= 0)
        fprintf (st, "line %d:\b", ln);
    if (!lp->conn)
        fprintf (st, "line disconnected\n");
    if (lp->rxcnt)
        fprintf (st, "  input (%s) queued/total = %d/%d\n",
            (lp->rcve? enab: dsab),
            tmxr_rqln (lp), lp->rxcnt);
    if (lp->txcnt || lp->txbpi)
        fprintf (st, "  output (%s) queued/total = %d/%d\n",
            (lp->xmte? enab: dsab),
            tmxr_tqln (lp), lp->txcnt);
    if (lp->txbfd)
        fprintf (st, "  output buffer size = %d\n", lp->txbsz);
    if (lp->txcnt || lp->txbpi)
        fprintf (st, "  bytes in buffer = %d\n", 
                   ((lp->txcnt > 0) && (lp->txcnt > lp->txbsz)) ? lp->txbsz : lp->txbpi);
    if (lp->txdrp)
        fprintf (st, "  dropped = %d\n", lp->txdrp);
}

/* Disconnect line */

t_stat tmxr_dscln (UNIT *uptr, int32 val, char *cptr, void *desc)
{
    TMXR *mp = (TMXR *) desc;
    TMLN *lp;
    int32 ln;
    t_stat r;

    if (mp == NULL)
        return SCPE_IERR;
    if (val) {                                              /* = n form */
        if (cptr == NULL)
            return SCPE_ARG;
        ln = (int32) get_uint (cptr, 10, mp->lines - 1, &r);
        if (r != SCPE_OK)
            return SCPE_ARG;
        lp = mp->ldsc + ln;
        }
    else {
        lp = tmxr_find_ldsc (uptr, 0, mp);
        if (lp == NULL)
            return SCPE_IERR;
        }
    if (lp->conn) {
        tmxr_linemsg (lp, "\r\nOperator disconnected line\r\n\n");
        tmxr_reset_ln (lp);
        }
    return SCPE_OK;
}

/* Enable logging for line */

t_stat tmxr_set_log (UNIT *uptr, int32 val, char *cptr, void *desc)
{
    TMXR *mp = (TMXR *) desc;
    TMLN *lp;

    if (cptr == NULL)                                       /* no file name? */
        return SCPE_2FARG;
    lp = tmxr_find_ldsc (uptr, val, mp);                    /* find line desc */
    if (lp == NULL)
        return SCPE_IERR;
    if (lp->txlog)                                          /* close existing log */
        tmxr_set_nolog (NULL, val, NULL, desc);
    lp->txlogname = (char *) calloc (CBUFSIZE, sizeof (char)); /* alloc namebuf */
    if (lp->txlogname == NULL)                              /* can't? */
        return SCPE_MEM;
    strncpy (lp->txlogname, cptr, CBUFSIZE);                /* save file name */
    sim_open_logfile (cptr, TRUE, &lp->txlog, &lp->txlogref);/* open log */
    if (lp->txlog == NULL) {                                /* error? */
        free (lp->txlogname);                               /* free buffer */
        return SCPE_OPENERR;
        }
    return SCPE_OK;
}

/* Disable logging for line */

t_stat tmxr_set_nolog (UNIT *uptr, int32 val, char *cptr, void *desc)
{
    TMXR *mp = (TMXR *) desc;
    TMLN *lp;

    if (cptr)                                               /* no arguments */
        return SCPE_2MARG;
    lp = tmxr_find_ldsc (uptr, val, mp);                    /* find line desc */
    if (lp == NULL)
        return SCPE_IERR;
    if (lp->txlog) {                                        /* logging? */
        sim_close_logfile (&lp->txlogref);                  /* close log */
        free (lp->txlogname);                               /* free namebuf */
        lp->txlog = NULL;
        lp->txlogname = NULL;
        }
    return SCPE_OK;
}

/* Show logging status for line */

t_stat tmxr_show_log (SMP_FILE *st, UNIT *uptr, int32 val, void *desc)
{
    TMXR *mp = (TMXR *) desc;
    TMLN *lp;

    lp = tmxr_find_ldsc (uptr, val, mp);                    /* find line desc */
    if (lp == NULL)
        return SCPE_IERR;
    if (lp->txlog)
        fprintf (st, "logging to %s", lp->txlogname);
    else fprintf (st, "no logging");
    return SCPE_OK;
}

/* Find line descriptor.

   Note: This routine may be called with a UNIT that does not belong to the
   device indicated in the TMXR structure.  That is, the multiplexer lines may
   belong to a device other than the one attached to the socket (the HP 2100 MUX
   device is one example).  Therefore, we must look up the device from the unit
   at each call, rather than depending on the DPTR stored in the TMXR.
*/

TMLN *tmxr_find_ldsc (UNIT *uptr, int32 val, TMXR *mp)
{
    if (uptr) {                                             /* called from SET? */
        DEVICE *dptr = find_dev_from_unit (uptr);           /* find device */
        if (dptr == NULL)                                   /* what?? */
            return NULL;
        val = sim_unit_index(uptr);                         /* implicit line # */
        }
    if ((val < 0) || (val >= mp->lines))                    /* invalid line? */
        return NULL;
    return mp->ldsc + val;                                  /* line descriptor */
}

/* Set the line connection order.

   Example command for eight-line multiplexer:

      SET <dev> LINEORDER=1;5;2-4;7

   Resulting connection order: 1,5,2,3,4,7,0,6.

   Parameters:
    - uptr = (not used)
    - val  = (not used)
    - cptr = pointer to first character of range specification
    - desc = pointer to multiplexer's TMXR structure

   On entry, cptr points to the value portion of the command string, which may
   be either a semicolon-separated list of line ranges or the keyword ALL.

   If a line connection order array is not defined in the multiplexer
   descriptor, the command is rejected.  If the specified range encompasses all
   of the lines, the first value of the connection order array is set to -1 to
   indicate sequential connection order.  Otherwise, the line values in the
   array are set to the order specified by the command string.  All values are
   populated, first with those explicitly specified in the command string, and
   then in ascending sequence with those not specified.

   If an error occurs, the original line order is not disturbed.
*/

t_stat tmxr_set_lnorder (UNIT *uptr, int32 val, char *cptr, void *desc)
{
    TMXR *mp = (TMXR *) desc;
    char *tptr;
    t_addr low, high, max = (t_addr) mp->lines - 1;
    int32 *list;
    t_bool *set;
    uint32 line, idx = 0;
    t_stat result = SCPE_OK;

    if (mp->lnorder == NULL)                                /* line connection order undefined? */
        return SCPE_NXPAR;                                  /* "Non-existent parameter" error */

    else if ((cptr == NULL) || (*cptr == '\0'))             /* line range not supplied? */
        return SCPE_MISVAL;                                 /* "Missing value" error */

    list = (int32 *) calloc (mp->lines, sizeof (int32));    /* allocate new line order array */

    if (list == NULL)                                       /* allocation failed? */
        return SCPE_MEM;                                    /* report it */

    set = (t_bool *) calloc (mp->lines, sizeof (t_bool));   /* allocate line set tracking array */

    if (set == NULL) {                                      /* allocation failed? */
        free (list);                                        /* free successful list allocation */
        return SCPE_MEM;                                    /* report it */
        }

    tptr = cptr + strlen (cptr);                            /* append a semicolon */
    *tptr++ = ';';                                          /*   to the command string */
    *tptr = '\0';                                           /*   to make parsing easier for get_range */

    while (*cptr) {                                                 /* parse command string */
        cptr = get_range (NULL, cptr, &low, &high, 10, max, ';');   /* get a line range */

        if (cptr == NULL) {                                 /* parsing error? */
            result = SCPE_ARG;                              /* "Invalid argument" error */
            break;
            }

        else if ((low > max) || (high > max)) {             /* line out of range? */
            result = SCPE_SUB;                              /* "Subscript out of range" error */
            break;
            }

        else if ((low == 0) && (high == max)) {             /* entire line range specified? */
            list [0] = -1;                                  /* set sequential order flag */
            idx = (uint32) max + 1;                         /* indicate no fill-in needed */
            break;
            }

        else
            for (line = (uint32) low; line <= (uint32) high; line++) /* see if previously specified */
                if (set [line] == FALSE) {                  /* not already specified? */
                    set [line] = TRUE;                      /* now it is */
                    list [idx] = line;                      /* add line to connection order */
                    idx = idx + 1;                          /* bump "specified" count */
                    }
        }

    if (result == SCPE_OK) {                                /* assignment successful? */
        if (idx <= max)                                     /* any lines not specified? */
            for (line = 0; line <= max; line++)             /* fill them in sequentially */
                if (set [line] == FALSE) {                  /* specified? */
                    list [idx] = line;                      /* no, so add it */
                    idx = idx + 1;
                    }

        memcpy (mp->lnorder, list, mp->lines * sizeof (int32)); /* copy working array to connection array */
        }

    free (list);                                            /* free list allocation */
    free (set);                                             /* free set allocation */

    return result;
}

/* Show line connection order.

   Parameters:
    - st   = stream on which output is to be written
    - uptr = (not used)
    - val  = (not used)
    - desc = pointer to multiplexer's TMXR structure

   If a connection order array is not defined in the multiplexer descriptor, the
   command is rejected.  If the first value of the connection order array is set
   to -1, then the connection order is sequential.  Otherwise, the line values
   in the array are printed as a semicolon-separated list.  Ranges are printed
   where possible to shorten the output.
*/

t_stat tmxr_show_lnorder (SMP_FILE *st, UNIT *uptr, int32 val, void *desc)
{
    int32 i, j, low, last;
    TMXR *mp = (TMXR *) desc;
    int32 *iptr = mp->lnorder;
    t_bool first = TRUE;

    if (iptr == NULL)                                       /* connection order undefined? */
        return SCPE_NXPAR;                                  /* "Non-existent parameter" error */

    if (*iptr < 0)                                          /* sequential order indicated? */
        fprintf (st, "Order=0-%d\n", mp->lines - 1);        /* print full line range */

    else {
        low = last = *iptr++;                               /* set first line value */

        for (j = 1; j <= mp->lines; j++) {                  /* print remaining lines in order list */
            if (j < mp->lines)                              /* more lines to process? */
                i = *iptr++;                                /* get next line in list */
            else                                            /* final iteration */
                i = -1;                                     /* get "tie-off" value */

            if (i != last + 1) {                            /* end of a range? */
                if (first) {                                /* first line to print? */
                    fputs ("Order=", st);                   /* print header */
                    first = FALSE;
                    }

                else                                        /* not first line printed */
                    fputc (';', st);                        /* print separator */

                if (low == last)                            /* range null? */
                    fprintf (st, "%d", last);               /* print single line value */

                else                                        /* range established */
                    fprintf (st, "%d-%d", low, last);       /* print start and end line */

                low = i;                                    /* start new range */
                }

            last = i;                                       /* note value for range check */
            }
        }

    if (first == FALSE)                                     /* sanity check for lines == 0 */
        fputc ('\n', st);

    return SCPE_OK;
}

/* Show summary processor */

t_stat tmxr_show_summ (SMP_FILE *st, UNIT *uptr, int32 val, void *desc)
{
    TMXR *mp = (TMXR *) desc;
    int32 i, t;

    if (mp == NULL)
        return SCPE_IERR;
    for (i = t = 0; i < mp->lines; i++)
        t = t + (mp->ldsc[i].conn != 0);
    if (t == 1)
        fprintf (st, "1 connection");
    else fprintf (st, "%d connections", t);
    return SCPE_OK;
}

/* Show conn/stat processor */

t_stat tmxr_show_cstat (SMP_FILE *st, UNIT *uptr, int32 val, void *desc)
{
    TMXR *mp = (TMXR *) desc;
    int32 i, any;

    if (mp == NULL)
        return SCPE_IERR;
    for (i = any = 0; i < mp->lines; i++) {
        if (mp->ldsc[i].conn) {
            any++;
            if (val)
                tmxr_fconns (st, &mp->ldsc[i], i);
            else tmxr_fstats (st, &mp->ldsc[i], i);
            }
        }
    if (any == 0)
        fprintf (st, (mp->lines == 1? "disconnected\n": "all disconnected\n"));
    return SCPE_OK;
}

/* Show number of lines */

t_stat tmxr_show_lines (SMP_FILE *st, UNIT *uptr, int32 val, void *desc)
{
    TMXR *mp = (TMXR *) desc;

    if (mp == NULL)
        return SCPE_IERR;
    fprintf (st, "lines=%d", mp->lines);
    return SCPE_OK;
}


static const struct
{
    char value;
    const char *name;
}
tn_chars[] =
{
    {TN_IAC,    "TN_IAC"},                /* protocol delim */
    {TN_DONT,   "TN_DONT"},               /* dont */
    {TN_DO,     "TN_DO"},                 /* do */
    {TN_WONT,   "TN_WONT"},               /* wont */
    {TN_WILL,   "TN_WILL"},               /* will */
    {TN_SB,     "TN_SB"},                 /* sub-option negotiation */
    {TN_GA,     "TN_SG"},                 /* go ahead */
    {TN_EL,     "TN_EL"},                 /* erase line */
    {TN_EC,     "TN_EC"},                 /* erase character */
    {TN_AYT,    "TN_AYT"},                /* are you there */
    {TN_AO,     "TN_AO"},                 /* abort output */
    {TN_IP,     "TN_IP"},                 /* interrupt process */
    {TN_BRK,    "TN_BRK"},                /* break */
    {TN_DATAMK, "TN_DATAMK"},             /* data mark */
    {TN_NOP,    "TN_NOP"},                /* no operation */
    {TN_SE,     "TN_SE"},                 /* end sub-option negot */
    /* Options */
    {TN_BIN,    "TN_BIN"},                /* bin */
    {TN_ECHO,   "TN_ECHO"},               /* echo */
    {TN_SGA,    "TN_SGA"},                /* sga */
    {TN_LINE,   "TN_LINE"},               /* line mode */
    {TN_CR,     "TN_CR"},                 /* carriage return */
    {TN_LF,     "TN_LF"},                 /* line feed */
    {0, NULL}
};

AUTO_INIT_DEVLOCK(tmxr_debug_lock);
static char *tmxr_debug_buf = NULL;
static size_t tmxr_debug_buf_used = 0;
static size_t tmxr_debug_buf_size = 0;

static void tmxr_buf_debug_char (char value)
{
    if (tmxr_debug_buf_used + 2 > tmxr_debug_buf_size)
    {
        tmxr_debug_buf_size += 1024;
        tmxr_debug_buf = (char*) realloc(tmxr_debug_buf, tmxr_debug_buf_size);
    }
    tmxr_debug_buf[tmxr_debug_buf_used++] = value;
    tmxr_debug_buf[tmxr_debug_buf_used] = '\0';
}

static void tmxr_buf_debug_string (const char *string)
{
    while (*string)
        tmxr_buf_debug_char (*string++);
}

void tmxr_debug (uint32 dbits, TMLN *lp, const char *msg, char *buf, int bufsize)
{
    if (lp->mp->dptr && (dbits & lp->mp->dptr->dctrl))
    {
        AUTO_LOCK(tmxr_debug_lock);
        int i, j;

        tmxr_debug_buf_used = 0;
        if (tmxr_debug_buf)
            tmxr_debug_buf[tmxr_debug_buf_used] = '\0';
        for (i = 0;  i < bufsize;  ++i)
        {
            for (j = 0; 1; ++j)
            {
                if (NULL == tn_chars[j].name)
                {
                    tmxr_buf_debug_char (buf[i]);
                    break;
                }
                if (buf[i] == tn_chars[j].value)
                {
                    tmxr_buf_debug_char ('_');
                    tmxr_buf_debug_string (tn_chars[j].name);
                    tmxr_buf_debug_char ('_');
                    break;
                }
            }
        }
        sim_debug (dbits, lp->mp->dptr, "%s %d bytes '%s'\n", msg, bufsize, tmxr_debug_buf);
    }
}
