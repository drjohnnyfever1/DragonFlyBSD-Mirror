/*
 * Copyright (c) 1988, 1989, 1993, 1994
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)termios.h	8.3 (Berkeley) 3/28/94
 * $FreeBSD: src/sys/sys/termios.h,v 1.13.2.1 2001/03/06 06:31:44 jhb Exp $
 */

#ifndef _SYS_TERMIOS_H_
#define	_SYS_TERMIOS_H_

#include <sys/cdefs.h>
#include <sys/_termios.h>
/* Needed by tcgetsid(3). */
#include <sys/stdint.h>
#ifndef _PID_T_DECLARED
typedef	__pid_t pid_t;
#define	_PID_T_DECLARED
#endif

#if __BSD_VISIBLE
#define	CCEQ(val, c)	((c) == (val) ? (val) != _POSIX_VDISABLE : 0)
#endif

/*
 * Commands passed to tcsetattr() for setting the termios structure.
 */
#define	TCSANOW		0		/* make change immediate */
#define	TCSADRAIN	1		/* drain output, then change */
#define	TCSAFLUSH	2		/* drain output, flush input */
#if __BSD_VISIBLE
#define	TCSASOFT	0x10		/* flag - don't alter h.w. state */
#endif

#define	TCIFLUSH	1
#define	TCOFLUSH	2
#define	TCIOFLUSH	3
#define	TCOOFF		1
#define	TCOON		2
#define	TCIOFF		3
#define	TCION		4

__BEGIN_DECLS
speed_t	cfgetispeed(const struct termios *);
speed_t	cfgetospeed(const struct termios *);
int	cfsetispeed(struct termios *, speed_t);
int	cfsetospeed(struct termios *, speed_t);
int	tcgetattr(int, struct termios *);
int	tcsetattr(int, int, const struct termios *);
int	tcdrain(int);
int	tcflow(int, int);
int	tcflush(int, int);
int	tcsendbreak(int, int);

#if __POSIX_VISIBLE >= 200809 || __XSI_VISIBLE
pid_t	tcgetsid(int);
#endif	/* __POSIX_VISIBLE >= 200809 || __XSI_VISIBLE */

#if __BSD_VISIBLE
void	cfmakeraw(struct termios *);
int	cfsetspeed(struct termios *, speed_t);
#endif /* __BSD_VISIBLE */
__END_DECLS

#if __BSD_VISIBLE

/*
 * Include tty ioctl's that aren't just for backwards compatibility
 * with the old tty driver.  These ioctl definitions were previously
 * in <sys/ioctl.h>.
 */
#include <sys/ttycom.h>
#endif

/*
 * END OF PROTECTED INCLUDE.
 */
#endif /* !_SYS_TERMIOS_H_ */

#if __BSD_VISIBLE
#include <sys/ttydefaults.h>
#endif
