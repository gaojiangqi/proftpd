/*
 * ProFTPD - FTP server daemon
 * Copyright (c) 1997, 1998 Public Flood Software
 * Copyright (c) 1999, 2000 MacGyver aka Habeeb J. Dihu <macgyver@tos.net>
 * Copyright (c) 2001, 2002, 2003 The ProFTPD Project team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307, USA.
 *
 * As a special exemption, Public Flood Software/MacGyver aka Habeeb J. Dihu
 * and other respective copyright holders give permission to link this program
 * with OpenSSL, and distribute the resulting executable, without including
 * the source code for OpenSSL in the source distribution.
 */

/*
 * Data connection management functions
 * $Id: data.c,v 1.59 2003-03-09 22:28:16 castaglia Exp $
 */

#include "conf.h"

#include <signal.h>

#ifdef HAVE_SYS_SENDFILE_H
#include <sys/sendfile.h>
#endif /* HAVE_SYS_SENDFILE_H */

#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif /* HAVE_SYS_UIO_H */

/* local macro */

#define MODE_STRING	(session.sf_flags & (SF_ASCII|SF_ASCII_OVERRIDE) ? \
			 "ASCII" : "BINARY")

/* Internal usage: pointer to current data connection stream in use (may be
 * in either read or write mode)
 */
static pr_netio_stream_t *nstrm = NULL;

/* Called if the "Stalled" timer goes off
 */
static int stalled_timeout_cb(CALLBACK_FRAME) {
  log_pri(PR_LOG_NOTICE, "Data transfer stall timeout: %d seconds",
    TimeoutStalled);
  end_login(1);

  /* Prevent compiler warning.
   */
  return 0;
}

/* this signal is raised if we get OOB data on the control connection, and
 * a data transfer is in progress
 */
static RETSIGTYPE data_urgent(int sig) {
  if (nstrm) {
    session.sf_flags |= SF_ABORT;
    pr_netio_abort(nstrm);
  }

  signal(SIGURG, data_urgent);
}

static int _xlate_ascii_read(char *buf, int *bufsize, int *adjlen)
{
  char *dest = buf,*src = buf;
  int thislen = *bufsize;

  *adjlen = 0;
  while(thislen--) {
    if (*src != '\r')
      *dest++ = *src++;
    else {
      if (thislen == 0) {
	/* copy, but save it for later */
	*dest++ = *src++;
	(*adjlen)++;
	(*bufsize)--;
      } else {
	if (*(src+1) == '\n') { /* skip */
	  (*bufsize)--;
	  src++;
	} else
	  *dest++ = *src++;
      }
    }
  }

  return *bufsize;
}

/* this function rewrites the contents of the given buffer, making sure that
 * each LF has a preceding CR, as required by RFC959:
 *
 *  buf = pointer to a buffer
 *  buflen = length of data in buffer
 *  bufsize = total size of buffer
 *  expand = will contain the number of expansion bytes (CRs) added,
 *           and should be the difference between buflen's original
 *           value and its value when this function returns
 */
static void _xlate_ascii_write(char **buf, unsigned int *buflen,
    unsigned int bufsize, unsigned int *expand) {
  char *tmpbuf = *buf;
  unsigned int tmplen = *buflen;
  unsigned int lfcount = 0;
  int res = 0;
  register unsigned int i = 0;

  /* Make sure this is zero (could be a holdover from a previous call). */
  *expand = 0;

  /* First, determine how many bare LFs are present. */
  if (tmpbuf[0] == '\n')
    lfcount++;

  for (i = 1; i < tmplen; i++)
    if (tmpbuf[i] == '\n' && tmpbuf[i-1] != '\r')
      lfcount++;

  /* Assume that for each LF (including a leading LF), space for another
   * char (a '\r') is needed.  Determine whether there is enough space in
   * the buffer for the adjusted data.  If not, allocate a new buffer that is
   * large enough.  The new buffer is allocated from session.xfer.p, which is
   * fine; this pool has a lifetime only for this current data transfer, and
   * will be cleared after the transfer is done, either having succeeded or
   * failed.
   *
   * Note: the res variable is needed in order to force signedness of the
   * resulting difference.  Without it, this condition would never evaluate
   * to true, as C's promotion rules would insure that the resulting value
   * would be of the same type as the operands: an unsigned int (which will
   * never be less than zero).
   */
  if ((res = (bufsize - tmplen - lfcount)) < 0) {
    pool *copy_pool = make_sub_pool(session.xfer.p);
    char *copy_buf = pcalloc(copy_pool, tmplen);

    memmove(copy_buf, tmpbuf, tmplen);

    /* Allocate a new session.xfer.buf of the needed size. */
    session.xfer.bufsize = tmplen + lfcount;
    session.xfer.buf = pcalloc(session.xfer.p, session.xfer.bufsize);

    /* Allow space for a CR to be inserted before an LF if an LF is the
     * first character in the buffer.
     */
    session.xfer.buf++;
    session.xfer.bufstart = session.xfer.buf;

    memmove(session.xfer.buf, copy_buf, tmplen);
    destroy_pool(copy_pool);

    tmpbuf = session.xfer.buf;
    bufsize = session.xfer.bufsize;
  }

  if (tmpbuf[0] == '\n') {

    /* Shift everything in the buffer to the right one character, making
     * space for a '\r'
     */
    memmove(&(tmpbuf[1]), &(tmpbuf[0]), bufsize);
    tmpbuf[0] = '\r';

    /* Increment the number of "expanded" characters, and decrement the
     * number of bare LFs.
     */
    (*expand)++;
    lfcount--;
  }

  for (i = 1; i < bufsize && (lfcount > 0); i++) {
    if (tmpbuf[i] == '\n' && tmpbuf[i-1] != '\r') {
      memmove(&(tmpbuf[i+1]), &(tmpbuf[i]), bufsize - i);
      tmpbuf[i] = '\r';
      (*expand)++;
      lfcount--;
    }
  }

  /* Always make sure the buffer is NUL-terminated. */
  tmpbuf[tmplen + (*expand)] = '\0';
  *buf = tmpbuf;
  *buflen = tmplen + (*expand);
}

static void data_new_xfer(char *filename, int direction) {
  if (session.xfer.p) {
    destroy_pool(session.xfer.p);
    memset(&session.xfer, 0, sizeof(session.xfer));
  }

  session.xfer.p = make_sub_pool(session.pool);
  session.xfer.filename = pstrdup(session.xfer.p,filename);
  session.xfer.direction = direction;
  session.xfer.bufsize = PR_TUNABLE_BUFFER_SIZE;
  session.xfer.buf = (char *)pcalloc(session.xfer.p, PR_TUNABLE_BUFFER_SIZE + 1);
  session.xfer.buf++;	/* leave room for ascii translation */
  session.xfer.bufstart = session.xfer.buf;
  session.xfer.buflen = 0;
}

static int data_pasv_open(char *reason, off_t size) {
  conn_t *c;
  int rev;

  if (!reason && session.xfer.filename)
    reason = session.xfer.filename;

  /* Set the "stalled" timer, if any, to prevent the connection
   * open from taking too long
   */
  if (TimeoutStalled)
    add_timer(TimeoutStalled, TIMER_STALLED, NULL, stalled_timeout_cb);

  /* We save the state of our current disposition for doing reverse
   * lookups, and then set it to what the configuration wants it to
   * be.
   */
  rev = inet_reverse_dns(session.xfer.p, ServerUseReverseDNS);

  /* Protocol and socket options should be set before handshaking. */

  if (session.xfer.direction == PR_NETIO_IO_RD) {
    inet_set_socket_opts(session.d->pool, session.d,
      (main_server->tcp_rcvbuf_override ?  main_server->tcp_rcvbuf_len : 0), 0);
    inet_set_proto_opts(session.pool, session.d, main_server->tcp_mss_len, 0,
      0, 1, 1);
    
  } else {
    inet_set_socket_opts(session.d->pool, session.d,
      0, (main_server->tcp_sndbuf_override ?  main_server->tcp_sndbuf_len : 0));
    inet_set_proto_opts(session.pool, session.d, main_server->tcp_mss_len, 0,
      0, 1, 1);
  }

  c = inet_accept(session.xfer.p, session.d, session.c, -1, -1, TRUE);
  inet_reverse_dns(session.xfer.p,rev);

  if (c && c->mode != CM_ERROR) {
    inet_close(session.pool, session.d);
    inet_setnonblock(session.pool, c);
    session.d = c;

    log_debug(DEBUG4, "passive data connection opened - local  : %s:%d",
               inet_ntoa(*session.d->local_ipaddr), session.d->local_port);
    log_debug(DEBUG4, "passive data connection opened - remote : %s:%d",
              inet_ntoa(*session.d->remote_ipaddr),
              session.d->remote_port);

    if (session.xfer.xfer_type != STOR_UNIQUE) {
      if (size)
        pr_response_send(R_150, "Opening %s mode data connection for %s "
          "(%" PR_LU " bytes)", MODE_STRING, reason, size);
      else
        pr_response_send(R_150, "Opening %s mode data connection for %s",
          MODE_STRING, reason);

    } else {

      /* Format of 150 responses for STOU is explicitly dictated by
       * RFC 1123:
       *
       *  4.1.2.9  STOU Command: RFC-959 Section 4.1.3
       *
       *    The STOU command stores into a uniquely named file.  When it
       *    receives an STOU command, a Server-FTP MUST return the
       *    actual file name in the "125 Transfer Starting" or the "150
       *    Opening Data Connection" message that precedes the transfer
       *    (the 250 reply code mentioned in RFC-959 is incorrect).  The
       *    exact format of these messages is hereby defined to be as
       *    follows:
       *
       *        125 FILE: pppp
       *        150 FILE: pppp
       *
       *    where pppp represents the unique pathname of the file that
       *    will be written.
       */
      pr_response_send(R_150, "FILE: %s", reason);
    }

    return 0;
  }

  /* Check for error conditions. */
  if (c && c->mode == CM_ERROR)
    log_pri(PR_LOG_ERR, "Error: unable to accept an incoming data "
      "connection (%s)", strerror(c->xerrno));

  pr_response_add_err(R_425, "Unable to build data connection: %s",
    strerror(session.d->xerrno));
  destroy_pool(session.d->pool);
  session.d = NULL;
  return -1;
}

static int data_active_open(char *reason, off_t size) {
  conn_t *c;
  int rev;

  if (!reason && session.xfer.filename)
    reason = session.xfer.filename;

  session.d = inet_create_connection(session.pool, NULL, -1,
    session.c->local_ipaddr, session.c->local_port-1, TRUE);

  /* Set the "stalled" timer, if any, to prevent the connection
   * open from taking too long
   */
  if (TimeoutStalled)
    add_timer(TimeoutStalled, TIMER_STALLED, NULL, stalled_timeout_cb);

  rev = inet_reverse_dns(session.pool, ServerUseReverseDNS);

  /* Protocol and socket options should be set before handshaking. */

  if (session.xfer.direction == PR_NETIO_IO_RD) {
    inet_set_socket_opts(session.d->pool, session.d,
      (main_server->tcp_rcvbuf_override ?  main_server->tcp_rcvbuf_len : 0), 0);
    inet_set_proto_opts(session.pool, session.d, main_server->tcp_mss_len, 0,
      0, 1, 1);
    
  } else {
    inet_set_socket_opts(session.d->pool, session.d,
      0, (main_server->tcp_sndbuf_override ?  main_server->tcp_sndbuf_len : 0));
    inet_set_proto_opts(session.pool, session.d, main_server->tcp_mss_len, 0,
      0, 1, 1);
  }

  if (inet_connect(session.d->pool, session.d, &session.data_addr,
      session.data_port) == -1) {
    pr_response_add_err(R_425, "Unable to build data connection: %s",
      strerror(session.d->xerrno));
    destroy_pool(session.d->pool);
    session.d = NULL;
    return -1;
  }

  c = inet_openrw(session.pool, session.d, NULL, PR_NETIO_STRM_DATA,
    session.d->listen_fd, -1, -1, TRUE);

  inet_reverse_dns(session.pool,rev);

  if (c) {
    log_debug(DEBUG4, "active data connection opened - local  : %s:%d",
	      inet_ntoa(*session.d->local_ipaddr), session.d->local_port);
    log_debug(DEBUG4, "active data connection opened - remote : %s:%d",
	      inet_ntoa(*session.d->remote_ipaddr),
	      session.d->remote_port);

    if (session.xfer.xfer_type != STOR_UNIQUE) {
      if (size)
        pr_response_send(R_150, "Opening %s mode data connection for %s "
          "(%" PR_LU " bytes)", MODE_STRING, reason, size);
      else
        pr_response_send(R_150, "Opening %s mode data connection for %s",
          MODE_STRING, reason);

    } else {

      /* Format of 150 responses for STOU is explicitly dictated by
       * RFC 1123:
       *
       *  4.1.2.9  STOU Command: RFC-959 Section 4.1.3
       *
       *    The STOU command stores into a uniquely named file.  When it
       *    receives an STOU command, a Server-FTP MUST return the
       *    actual file name in the "125 Transfer Starting" or the "150
       *    Opening Data Connection" message that precedes the transfer
       *    (the 250 reply code mentioned in RFC-959 is incorrect).  The
       *    exact format of these messages is hereby defined to be as
       *    follows:
       *
       *        125 FILE: pppp
       *        150 FILE: pppp
       *
       *    where pppp represents the unique pathname of the file that
       *    will be written.
       */
      pr_response_send(R_150, "FILE: %s", reason);
    }

    inet_close(session.pool,session.d);
    inet_setnonblock(session.pool,session.d);
    session.d = c;
    return 0;
  }


  pr_response_add_err(R_425, "Unable to build data connection: %s",
    strerror(session.d->xerrno));
  destroy_pool(session.d->pool);
  session.d = NULL;
  return -1;
}

void pr_data_reset(void) {
  if (session.d && session.d->pool)
    destroy_pool(session.d->pool);
  session.d = NULL;
  session.sf_flags &= (SF_ALL^(SF_ABORT|SF_XFER|SF_PASSIVE|SF_ASCII_OVERRIDE));
}

void pr_data_init(char *filename, int direction) {
  if (!session.xfer.p) {
    data_new_xfer(filename, direction);

  } else {
    if (!(session.sf_flags & SF_PASSIVE))
      log_debug(DEBUG0,
		"data_init oddity: session.xfer exists in non-PASV mode.");

    session.xfer.direction = direction;
  }
}

int pr_data_open(char *filename, char *reason, int direction, off_t size) {
  int res = 0;

  if (!session.xfer.p)
    data_new_xfer(filename, direction);
  else
    session.xfer.direction = direction;

  if (!reason)
    reason = filename;

  /* Passive data transfers... */
  if (session.sf_flags & SF_PASSIVE) {
    if (!session.d) {
      log_pri(PR_LOG_ERR, "Internal error: PASV mode set, but no data "
        "connection listening.");
      end_login(1);
    }

    res = data_pasv_open(reason,size);

  /* Active data transfers... */
  } else {
    if (session.d) {
      log_pri(PR_LOG_ERR, "Internal error: non-PASV mode, yet data "
        "connection already exists?!?");
      end_login(1);
    }

    res = data_active_open(reason,size);
  }

  if (res >= 0) {
    struct sigaction act;

    if (pr_netio_postopen(session.d->instrm) < 0)
      return -1;

    if (pr_netio_postopen(session.d->outstrm) < 0)
      return -1;

    memset(&session.xfer.start_time, '\0', sizeof(session.xfer.start_time));
    gettimeofday(&session.xfer.start_time, NULL);

    if (session.xfer.direction == PR_NETIO_IO_RD)
      nstrm = session.d->instrm;

    else
      nstrm = session.d->outstrm;

    session.sf_flags |= SF_XFER;

    if (TimeoutNoXfer)
      reset_timer(TIMER_NOXFER, ANY_MODULE);

    /* allow aborts */

    /* Set the current NetIO stream to allow interrupted syscalls, so our
     * SIGURG handler can interrupt it
     */
    pr_netio_set_poll_interval(nstrm, 1);

    /* PORTABILITY: sigaction is used here to allow us
     * to indicate (w/ POSIX at least) that we want
     * SIGURG to interrupt syscalls.  Put in whatever
     * is necessary for your arch here (probably not necessary
     * as the only _important_ interrupted syscall is select()),
     * which on any sensible system is interrupted.
     */

    act.sa_handler = data_urgent;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
#ifdef SA_INTERRUPT
    act.sa_flags |= SA_INTERRUPT;
#endif
    sigaction(SIGURG, &act, NULL);
#ifdef HAVE_SIGINTERRUPT
    /* this is the BSD way of ensuring interruption.
     * Linux uses it too (??)
     */
    siginterrupt(SIGURG, 1);
#endif
  }

  return res;
}

/* close == successful transfer */
void pr_data_close(int quiet) {
  nstrm = NULL;

  if (session.d) {
    inet_lingering_close(session.pool, session.d, PR_TUNABLE_TIMEOUTLINGER);
    session.d = NULL;
  }

  /* Aborts no longer necessary */
  signal(SIGURG, SIG_IGN);

  if (TimeoutNoXfer)
    reset_timer(TIMER_NOXFER, ANY_MODULE);

  if (TimeoutStalled)
    remove_timer(TIMER_STALLED, ANY_MODULE);

  session.sf_flags &= (SF_ALL^SF_PASSIVE);
  session.sf_flags &= (SF_ALL^(SF_ABORT|SF_XFER|SF_PASSIVE|SF_ASCII_OVERRIDE));
  session_set_idle();

  if (!quiet)
    pr_response_add(R_226, "Transfer complete.");
}

/* Note: true_abort may be false in real abort situations, because
 * some ftp clients close the data connection at the same time as they
 * send the OOB byte (which results in a broken pipe on our
 * end).  Thus, it's a race between the OOB data and the tcp close
 * finishing.  Either way, it's ok (client will see either "Broken pipe"
 * error or "Aborted").  cmd_abor in mod_xfer cleans up the session
 * flags in any case.  session flags will end up have SF_POST_ABORT
 * set if the OOB byte won the race.
 */
void pr_data_cleanup(void) {
  /* sanity check */
  if (session.d) {
    inet_lingering_close(session.pool, session.d, PR_TUNABLE_TIMEOUTLINGER);
    session.d = NULL;
  }

  if (session.xfer.p)
    destroy_pool(session.xfer.p);

  memset(&session.xfer,0,sizeof(session.xfer));
}

/* In order to avoid clearing the transfer counters in session.xfer, we don't
 * clear session.xfer here, it should be handled by the appropriate
 * LOG_CMD/LOG_CMD_ERR handler calling pr_data_cleanup().
 */
void pr_data_abort(int err, int quiet) {
  int true_abort = XFER_ABORTED;
  nstrm = NULL;

  if (session.d) {
    inet_lingering_close(session.pool, session.d, PR_TUNABLE_TIMEOUTLINGER);
    session.d = NULL;
  }

  if (TimeoutNoXfer)
    reset_timer(TIMER_NOXFER, ANY_MODULE);

  if (TimeoutStalled)
    remove_timer(TIMER_STALLED, ANY_MODULE);

  session.sf_flags &= (SF_ALL^SF_PASSIVE);
  session.sf_flags &= (SF_ALL^(SF_XFER|SF_PASSIVE|SF_ASCII_OVERRIDE));
  session_set_idle();

  /* Aborts no longer necessary */
  signal(SIGURG, SIG_IGN);

  if (TimeoutNoXfer)
    reset_timer(TIMER_NOXFER, ANY_MODULE);

  if (!quiet) {
    char	*respcode = R_426;
    char	*fmt = NULL;
    char	*msg = NULL;
    char	msgbuf[64];

    switch (err) {

    case 0:
      respcode = R_426;
      msg = "Data connection closed.";
      break;

#ifdef ENXIO
    case ENXIO:
      respcode = R_451;
      msg = "Unexpected streams hangup.";
      break;

#endif

#ifdef EAGAIN
    case EAGAIN:		/* FALLTHROUGH */
#endif
#ifdef ENOMEM
    case ENOMEM:
#endif
#if defined(EAGAIN) || defined(ENOMEM)
      respcode = R_451;
      msg = "Insufficient memory or file locked.";
      break;
#endif

#ifdef ETXTBSY
    case ETXTBSY:		/* FALLTHROUGH */
#endif
#ifdef EBUSY
    case EBUSY:
#endif
#if defined(ETXTBSY) || defined(EBUSY)
      respcode = R_451;
      break;
#endif

#ifdef ENOSPC
    case ENOSPC:
      respcode = R_452;
      break;
#endif

#ifdef EDQUOT
    case EDQUOT:		/* FALLTHROUGH */
#endif
#ifdef EFBIG
    case EFBIG:
#endif
#if defined(EDQUOT) || defined(EFBIG)
      respcode = R_552;
      break;
#endif

#ifdef ECOMM
    case ECOMM:		/* FALLTHROUGH */
#endif
#ifdef EDEADLK
    case EDEADLK:		/* FALLTHROUGH */
#endif
#ifdef EDEADLOCK
# if !defined(EDEADLK) || (EDEADLOCK != EDEADLK)
    case EDEADLOCK:		/* FALLTHROUGH */
# endif
#endif
#ifdef EXFULL
    case EXFULL:		/* FALLTHROUGH */
#endif
#ifdef ENOSR
    case ENOSR:		/* FALLTHROUGH */
#endif
#ifdef EPROTO
    case EPROTO:		/* FALLTHROUGH */
#endif
#ifdef ETIME
    case ETIME:		/* FALLTHROUGH */
#endif
#ifdef EIO
    case EIO:		/* FALLTHROUGH */
#endif
#ifdef EFAULT
    case EFAULT:		/* FALLTHROUGH */
#endif
#ifdef ESPIPE
    case ESPIPE:		/* FALLTHROUGH */
#endif
#ifdef EPIPE
    case EPIPE:
#endif
#if defined(ECOMM) || defined(EDEADLK) ||  defined(EDEADLOCK) \
	|| defined(EXFULL) || defined(ENOSR) || defined(EPROTO) \
	|| defined(ETIME) || defined(EIO) || defined(EFAULT) \
	|| defined(ESPIPE) || defined(EPIPE)
      respcode = R_451;
      break;
#endif

#ifdef EREMCHG
    case EREMCHG:		/* FALLTHROUGH */
#endif
#ifdef ESRMNT
    case ESRMNT:		/* FALLTHROUGH */
#endif
#ifdef ESTALE
    case ESTALE:		/* FALLTHROUGH */
#endif
#ifdef ENOLINK
    case ENOLINK:		/* FALLTHROUGH */
#endif
#ifdef ENOLCK
    case ENOLCK:		/* FALLTHROUGH */
#endif
#ifdef ENETRESET
    case ENETRESET:		/* FALLTHROUGH */
#endif
#ifdef ECONNABORTED
    case ECONNABORTED:	/* FALLTHROUGH */
#endif
#ifdef ECONNRESET
    case ECONNRESET:	/* FALLTHROUGH */
#endif
#ifdef ETIMEDOUT
    case ETIMEDOUT:
#endif
#if defined(EREMCHG) || defined(ESRMNT) ||  defined(ESTALE) \
	|| defined(ENOLINK) || defined(ENOLCK) || defined(ENETRESET) \
	|| defined(ECONNABORTED) || defined(ECONNRESET) || defined(ETIMEDOUT)
      respcode = R_450;
      msg = "Link to file server lost.";
      break;
#endif
    }

    if ( msg == NULL && (msg = strerror(err)) == NULL ) {
      if ( snprintf(msgbuf, sizeof msgbuf,
		    "Unknown or out of range errno [%d].",
		    err) > 0 )
	msg = msgbuf;
    }

    pr_response_add_err(respcode, fmt ? fmt : "Transfer aborted.  %s",
      msg ? msg : "");
  }

  if (true_abort)
    session.sf_flags |= SF_POST_ABORT;
}

/* pr_data_xfer() actually transfers the data on the data connection ..
 * ASCII translation is performed if necessary.  direction set
 * when data connection was opened determine if the client buffer
 * is read from or written to.  return 0 if reading and data connection
 * closes, or -1 if error
 */

int pr_data_xfer(char *cl_buf, int cl_size) {
  char *buf = session.xfer.buf;
  int len = 0;
  int total = 0;

  if (session.xfer.direction == PR_NETIO_IO_RD) {
    if (session.d) {
      if (session.sf_flags & (SF_ASCII|SF_ASCII_OVERRIDE)) {
        int adjlen,buflen;
	do {
	  buflen = session.xfer.buflen;        /* how much remains in buf */
	  adjlen = 0;
	
	  if ((len = pr_netio_read(session.d->instrm, buf + buflen,
		  session.xfer.bufsize - buflen, 1)) > 0) {
	    buflen += len;

	    if (TimeoutStalled)
	      reset_timer(TIMER_STALLED, ANY_MODULE);
	  }

	  /* if buflen > 0, data remains in the buffer to be copied. */
	  if (len >= 0 && buflen > 0) {

	    /* Perform translation:
	     * buflen is returned as the modified buffer length after
	     *        translation
	     * adjlen is returned as the number of characters unprocessed in
	     *        the buffer (to be dealt with later)
	     *
	     * We skip the call to _xlate_ascii_read() in one case:
	     * when we have one character in the buffer and have reached
	     * end of data, this is so that _xlate_ascii_read() won't sit
	     * forever waiting for the next character after a final '\r'.
	     */
	    if (len > 0 || buflen > 1)
	      _xlate_ascii_read(buf, &buflen, &adjlen);
	
	    /* now copy everything we can into cl_buf */
	    if (buflen > cl_size) {
	      /* because we have to cut our buffer short, make sure this
	       * is made up for later by increasing adjlen.
	       */
	      adjlen += (buflen - cl_size);
	      buflen = cl_size;
	    }
  	    memcpy(cl_buf,buf,buflen);
	
	    /* copy whatever remains at the end of session.xfer.buf to the
	     * head of the buffer and adjust buf accordingly
	     *
	     * adjlen is now the total bytes still waiting in buf, if
	     * anything remains, copy it to the start of the buffer
	     */
	
	    if (adjlen > 0)
	      memcpy(buf,buf+buflen,adjlen);

	    /* store everything back in session.xfer */
	    session.xfer.buflen = adjlen;
	    total += buflen;
	  }
	
	  /* Restart if data was returned by pr_netio_read() (len > 0) but
	   * no data was copied to the client buffer (buflen = 0).
	   * This indicates that _xlate_ascii_read() needs more data
	   * in order to translate, so we need to call pr_netio_read() again.
           */
	} while(len > 0 && buflen == 0);

        /* Return how much data we actually copied into the client buffer.
         */
        len = buflen;

      } else if ((len = pr_netio_read(session.d->instrm, cl_buf,
          cl_size, 1)) > 0) {

        /* Non-ascii mode doesn't need to use session.xfer.buf */
        if (TimeoutStalled)
          reset_timer(TIMER_STALLED, ANY_MODULE);

        total += len;
      }
    }

  } else { /* PR_NETIO_IO_WR */

    /* copy client buffer to internal buffer, and
     * xlate ascii as necessary
     */
    while (cl_size) {
      int o_size, size = cl_size;

      if (size > PR_TUNABLE_BUFFER_SIZE)
        size = PR_TUNABLE_BUFFER_SIZE;

      o_size = size;
      memcpy(buf, cl_buf, size);

      while (size) {
        char *wb = buf;
        unsigned int wsize = size, adjlen = 0;

        if (session.sf_flags & (SF_ASCII|SF_ASCII_OVERRIDE))
          _xlate_ascii_write(&wb, &wsize, session.xfer.bufsize, &adjlen);

        if (pr_netio_write(session.d->outstrm, wb, wsize) == -1)
          return -1;

        if (TimeoutStalled)
          reset_timer(TIMER_STALLED, ANY_MODULE);

        /* Do not take any added CRs into account for the session sum. */
        total += (wsize - adjlen);
        size -= (wsize - adjlen);

        if (size) {
          /* Advance the output buffer pointer into unsent buffer space. */
          wb += wsize;
	  memcpy(buf, wb, size);
          buf[size] = '\0';
        }
      }

      cl_size -= o_size;
      cl_buf += o_size;
    }

    len = total;
  }

  if (total && TimeoutIdle)
    reset_timer(TIMER_IDLE,ANY_MODULE);

  session.xfer.total_bytes += total;
  session.total_bytes += total;
  return (len < 0 ? -1 : len);
}

#ifdef HAVE_SENDFILE
/* pr_data_sendfile() actually transfers the data on the data connection.
 * ASCII translation is not performed.
 * return 0 if reading and data connection closes, or -1 if error
 */
pr_sendfile_t pr_data_sendfile(int retr_fd, off_t *offset, size_t count) {
  int flags, error;
  pr_sendfile_t len = 0, total = 0;
#if defined(HAVE_AIX_SENDFILE)
  struct sf_parms parms;
  int rc;
#endif /* HAVE_AIX_SENDFILE */

  if (session.xfer.direction == PR_NETIO_IO_RD)
    return -1;

  if ((flags = fcntl(PR_NETIO_FD(session.d->outstrm), F_GETFL)) == -1)
    return -1;

  /* set fd to blocking-mode for sendfile() */
  if (flags & O_NONBLOCK)
    if (fcntl(PR_NETIO_FD(session.d->outstrm), F_SETFL, flags^O_NONBLOCK) == -1)
      return -1;

  for (;;) {
#if defined(HAVE_LINUX_SENDFILE)
    off_t orig_offset = *offset;

    /* Linux semantics are fairly straightforward in a glibc 2.x world:
     *
     * #include <sys/sendfile.h>
     *
     * ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count)
     */
    len = sendfile(PR_NETIO_FD(session.d->outstrm), retr_fd, offset, count);

    if (len != -1 && len < count) {
      /* under linux semantics, this occurs when a signal has interrupted
       * sendfile().
       */

      count -= len;
	
      if (TimeoutStalled)
        reset_timer(TIMER_STALLED, ANY_MODULE);
	
      if (TimeoutIdle)
	reset_timer(TIMER_IDLE, ANY_MODULE);
	
      session.xfer.total_bytes += len;
      session.total_bytes += len;
      total += len;

      continue;
    } else if (len == -1) {
      /* Linux updates offset on error, not len like BSD, fix up so
       * BSD-based code works.
       */
      len = *offset - orig_offset;
      *offset = orig_offset;

#elif defined(HAVE_BSD_SENDFILE)
    /* BSD semantics for sendfile are flexible...it'd be nice if we could
     * standardize on something like it.  The semantics are:
     *
     * #include <sys/types.h>
     * #include <sys/socket.h>
     * #include <sys/uio.h>
     *
     * int sendfile(int in_fd, int out_fd, off_t offset, size_t count,
     *              struct sf_hdtr *hdtr, off_t *len, int flags)
     */
    if (sendfile(retr_fd, PR_NETIO_FD(session.d->outstrm), *offset, count,
        NULL, &len, 0) == -1) {

#elif defined(HAVE_AIX_SENDFILE)

    memset(&parms, 0, sizeof(parms));

    parms.file_descriptor = retr_fd;
    parms.file_offset = (uint64_t) *offset;
    parms.file_bytes = (int64_t) count;

    rc  = send_file(&(PR_NETIO_FD(session.d->outstrm)), &(parms), (uint_t)0);
    len = (int) parms.bytes_sent;

    if (rc == -1 || rc == 1) {

#endif /* HAVE_AIX_SENDFILE */

      /* IMO, BSD's semantics are warped.  Apparently, since we have our
       * alarms tagged SA_INTERRUPT (allowing system calls to be
       * interrupted - primarily for select), BSD will interrupt a
       * sendfile operation as well, so we have to catch and handle this
       * case specially.  It should also be noted that the sendfile(2) man
       * page doesn't state any of this.
       *
       * HP/UX has the same semantics, however, EINTR is well documented
       * as a side effect in the sendfile(2) man page.  HP/UX, however,
       * is implemented horribly wrong.  If a signal would result in
       * -1 being returned and EINTR being set, what ACTUALLY happens is
       * that errno is cleared and the number of bytes written is returned.
       *
       * For obvious reasons, HP/UX sendfile is not supported yet - jss
       */
      if (errno == EINTR) {
        pr_signals_handle();

	/* If we got everything in this transaction, we're done.
	 */
	if ((count -= len) <= 0)
	  break;
	
	*offset += len;
	
	if (TimeoutStalled)
	  reset_timer(TIMER_STALLED, ANY_MODULE);
	
	if (TimeoutIdle)
	  reset_timer(TIMER_IDLE, ANY_MODULE);
	
	session.xfer.total_bytes += len;
	session.total_bytes += len;
	total += len;
	
	continue;
      }

      error = errno;
      fcntl(PR_NETIO_FD(session.d->outstrm), F_SETFL, flags);
      errno = error;

      return -1;
    }

    break;
  }

  if (flags & O_NONBLOCK)
    fcntl(PR_NETIO_FD(session.d->outstrm), F_SETFL, flags);

  if (TimeoutStalled)
    reset_timer(TIMER_STALLED, ANY_MODULE);

  if (TimeoutIdle)
    reset_timer(TIMER_IDLE, ANY_MODULE);

  session.xfer.total_bytes += len;
  session.total_bytes += len;
  total += len;

  return total;
}
#endif /* HAVE_SENDFILE */
