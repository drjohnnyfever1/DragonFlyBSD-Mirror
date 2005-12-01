/*
 * Copyright (c) 2005 The DragonFly Project.  All rights reserved.
 * 
 * This code is derived from software contributed to The DragonFly Project
 * by Matthew Dillon <dillon@backplane.com>
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * 
 * $DragonFly: src/sys/kern/lwkt_serialize.c,v 1.9 2005/12/01 18:34:09 dillon Exp $
 */
/*
 * This API provides a fast locked-bus-cycle-based serializer.  It's
 * basically a low level NON-RECURSIVE exclusive lock that can be held across
 * a blocking condition.  It is NOT a mutex.
 *
 * This serializer is primarily designed for low level situations and
 * interrupt/device interaction.  There are two primary facilities.  First,
 * the serializer facility itself.  Second, an integrated interrupt handler 
 * disablement facility.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/rtprio.h>
#include <sys/queue.h>
#include <sys/thread2.h>
#include <sys/serialize.h>
#include <sys/sysctl.h>
#include <sys/kthread.h>
#include <machine/cpu.h>
#include <sys/lock.h>
#include <sys/caps.h>

static void lwkt_serialize_sleep(void *info);
static void lwkt_serialize_wakeup(void *info);

void
lwkt_serialize_init(lwkt_serialize_t s)
{
    atomic_intr_init(&s->interlock);
#ifdef INVARIANTS
    s->last_td = (void *)-4;
#endif
}

void
lwkt_serialize_enter(lwkt_serialize_t s)
{
#ifdef INVARIANTS
    KKASSERT(s->last_td != curthread);
#endif
    atomic_intr_cond_enter(&s->interlock, lwkt_serialize_sleep, s);
#ifdef INVARIANTS
    s->last_td = curthread;
#endif
}

/*
 * Returns non-zero on success
 */
int
lwkt_serialize_try(lwkt_serialize_t s)
{
    int error;

#ifdef INVARIANTS
    KKASSERT(s->last_td != curthread);
#endif
    if ((error = atomic_intr_cond_try(&s->interlock)) == 0) {
#ifdef INVARIANTS
	s->last_td = curthread;
#endif
	return(1);
    }
    return (0);
}

void
lwkt_serialize_exit(lwkt_serialize_t s)
{
#ifdef INVARIANTS
    KKASSERT(s->last_td == curthread);
    s->last_td = (void *)-2;
#endif
    atomic_intr_cond_exit(&s->interlock, lwkt_serialize_wakeup, s);
}

/*
 * Interrupt handler disablement support, used by drivers.  Non-stackable
 * (uses bit 30).
 */
void
lwkt_serialize_handler_disable(lwkt_serialize_t s)
{
    atomic_intr_handler_disable(&s->interlock);
}

void
lwkt_serialize_handler_enable(lwkt_serialize_t s)
{
    atomic_intr_handler_enable(&s->interlock);
}

void
lwkt_serialize_handler_call(lwkt_serialize_t s, void (*func)(void *, void *), 
			    void *arg, void *frame)
{
    /*
     * note: a return value of 0 indicates that the interrupt handler is 
     * enabled.
     */
    if (atomic_intr_handler_is_enabled(&s->interlock) == 0) {
	atomic_intr_cond_enter(&s->interlock, lwkt_serialize_sleep, s);
#ifdef INVARIANTS
	s->last_td = curthread;
#endif
	if (atomic_intr_handler_is_enabled(&s->interlock) == 0)
	    func(arg, frame);
#ifdef INVARIANTS
	KKASSERT(s->last_td == curthread);
	s->last_td = (void *)-2;
#endif
	atomic_intr_cond_exit(&s->interlock, lwkt_serialize_wakeup, s);
    }
}

/*
 * Similar to handler_call but does not block.  Returns 0 on success, 
 * and 1 on failure.
 */
int
lwkt_serialize_handler_try(lwkt_serialize_t s, void (*func)(void *, void *),
			   void *arg, void *frame)
{
    /*
     * note: a return value of 0 indicates that the interrupt handler is 
     * enabled.
     */
    if (atomic_intr_handler_is_enabled(&s->interlock) == 0) {
	if (atomic_intr_cond_try(&s->interlock) == 0) {
#ifdef INVARIANTS
	    s->last_td = curthread;
#endif
	    func(arg, frame);
#ifdef INVARIANTS
	    KKASSERT(s->last_td == curthread);
	    s->last_td = (void *)-2;
#endif
	    atomic_intr_cond_exit(&s->interlock, lwkt_serialize_wakeup, s);
	    return(0);
	}
    }
    return(1);
}


/*
 * Helper functions
 *
 * It is possible to race an interrupt which acquires and releases the
 * bit, then calls wakeup before we actually go to sleep, so we
 * need to check that the interlock is still acquired from within
 * a critical section prior to sleeping.
 */
static void
lwkt_serialize_sleep(void *info)
{
    lwkt_serialize_t s = info;
    crit_enter();
    tsleep_interlock(s);
    if (atomic_intr_cond_test(&s->interlock) != 0)
	    tsleep(s, 0, "slize", 0);
    crit_exit();
}

static void
lwkt_serialize_wakeup(void *info)
{
    wakeup(info);
}

