/*
 * Copyright (c) 2008 The DragonFly Project.  All rights reserved.
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
 * $DragonFly: src/sys/vfs/hammer/hammer_flusher.c,v 1.11 2008/05/03 20:21:20 dillon Exp $
 */
/*
 * HAMMER dependancy flusher thread
 *
 * Meta data updates create buffer dependancies which are arranged as a
 * hierarchy of lists.
 */

#include "hammer.h"

static void hammer_flusher_thread(void *arg);
static void hammer_flusher_clean_loose_ios(hammer_mount_t hmp);
static void hammer_flusher_flush(hammer_mount_t hmp);
static int hammer_must_finalize_undo(hammer_mount_t hmp);
static void hammer_flusher_finalize(hammer_transaction_t trans,
		    hammer_off_t start_offset);

void
hammer_flusher_sync(hammer_mount_t hmp)
{
	int seq;

	if (hmp->flusher_td) {
		seq = hmp->flusher_next;
		if (hmp->flusher_signal == 0) {
			hmp->flusher_signal = 1;
			wakeup(&hmp->flusher_signal);
		}
		while ((int)(seq - hmp->flusher_done) > 0)
			tsleep(&hmp->flusher_done, 0, "hmrfls", 0);
	}
}

void
hammer_flusher_async(hammer_mount_t hmp)
{
	if (hmp->flusher_td) {
		if (hmp->flusher_signal == 0) {
			hmp->flusher_signal = 1;
			wakeup(&hmp->flusher_signal);
		}
	}
}

void
hammer_flusher_create(hammer_mount_t hmp)
{
	hmp->flusher_signal = 0;
	hmp->flusher_act = 0;
	hmp->flusher_done = 0;
	hmp->flusher_next = 1;
	lwkt_create(hammer_flusher_thread, hmp, &hmp->flusher_td, NULL,
		    0, -1, "hammer");
}

void
hammer_flusher_destroy(hammer_mount_t hmp)
{
	if (hmp->flusher_td) {
		hmp->flusher_exiting = 1;
		while (hmp->flusher_td) {
			hmp->flusher_signal = 1;
			wakeup(&hmp->flusher_signal);
			tsleep(&hmp->flusher_exiting, 0, "hmrwex", 0);
		}
	}
}

static void
hammer_flusher_thread(void *arg)
{
	hammer_mount_t hmp = arg;

	for (;;) {
		while (hmp->flusher_lock)
			tsleep(&hmp->flusher_lock, 0, "hmrhld", 0);
		hmp->flusher_act = hmp->flusher_next;
		++hmp->flusher_next;
		kprintf("F");
		hammer_flusher_clean_loose_ios(hmp);
		hammer_flusher_flush(hmp);
		hammer_flusher_clean_loose_ios(hmp);
		hmp->flusher_done = hmp->flusher_act;

		wakeup(&hmp->flusher_done);

		/*
		 * Wait for activity.
		 */
		if (hmp->flusher_exiting && TAILQ_EMPTY(&hmp->flush_list))
			break;
		kprintf("E");

		while (hmp->flusher_signal == 0 &&
		       TAILQ_EMPTY(&hmp->flush_list)) {
			tsleep(&hmp->flusher_signal, 0, "hmrwwa", 0);
		}
		hmp->flusher_signal = 0;
	}
	hmp->flusher_td = NULL;
	wakeup(&hmp->flusher_exiting);
	lwkt_exit();
}

static void
hammer_flusher_clean_loose_ios(hammer_mount_t hmp)
{
	hammer_buffer_t buffer;
	hammer_io_t io;

	/*
	 * loose ends - buffers without bp's aren't tracked by the kernel
	 * and can build up, so clean them out.  This can occur when an
	 * IO completes on a buffer with no references left.
	 */
	while ((io = TAILQ_FIRST(&hmp->lose_list)) != NULL) {
		KKASSERT(io->mod_list == &hmp->lose_list);
		TAILQ_REMOVE(io->mod_list, io, mod_entry);
		io->mod_list = NULL;
		hammer_ref(&io->lock);
		buffer = (void *)io;
		hammer_rel_buffer(buffer, 0);
	}
}

/*
 * Flush stuff
 */
static void
hammer_flusher_flush(hammer_mount_t hmp)
{
	struct hammer_transaction trans;
	hammer_blockmap_t rootmap;
	hammer_inode_t ip;
	hammer_off_t start_offset;

	hammer_start_transaction_fls(&trans, hmp);
	rootmap = &hmp->blockmap[HAMMER_ZONE_UNDO_INDEX];
	start_offset = rootmap->next_offset;

	while ((ip = TAILQ_FIRST(&hmp->flush_list)) != NULL) {
		/*
		 * Stop when we hit a different flush group
		 */
		if (ip->flush_group != hmp->flusher_act)
			break;

		/*
		 * Remove the inode from the flush list and inherit
		 * its reference, sync, and clean-up.
		 */
		TAILQ_REMOVE(&hmp->flush_list, ip, flush_entry);
		ip->error = hammer_sync_inode(ip);
		hammer_flush_inode_done(ip);

		/*
		 * XXX this breaks atomicy
		 */
		if (hammer_must_finalize_undo(hmp)) {
			Debugger("Too many undos!!");
			hammer_flusher_finalize(&trans, start_offset);
			start_offset = rootmap->next_offset;
		}
	}
	hammer_flusher_finalize(&trans, start_offset);
	hammer_done_transaction(&trans);
}

/*
 * If the UNDO area gets over half full we have to flush it.  We can't
 * afford the UNDO area becoming completely full as that would break
 * the crash recovery atomicy.
 */
static
int
hammer_must_finalize_undo(hammer_mount_t hmp)
{
	if (hammer_undo_space(hmp) < hammer_undo_max(hmp) / 2) {
		kprintf("*");
		return(1);
	} else {
		return(0);
	}
}

/*
 * To finalize the flush we finish flushing all undo and data buffers
 * still present, then we update the volume header and flush it,
 * then we flush out the mata-data (that can now be undone).
 *
 * Note that as long as the undo fifo's start and end points do not
 * match, we always must at least update the volume header.
 *
 * The sync_lock is used by other threads to issue modifying operations
 * to HAMMER media without crossing a synchronization boundary or messing
 * up the media synchronization operation.  Specifically, the pruning
 * the reblocking ioctls, and allowing the frontend strategy code to
 * allocate media data space.
 */
static
void
hammer_flusher_finalize(hammer_transaction_t trans, hammer_off_t start_offset)
{
	hammer_mount_t hmp = trans->hmp;
	hammer_volume_t root_volume = trans->rootvol;
	hammer_blockmap_t rootmap;
	hammer_io_t io;

	hammer_lock_ex(&hmp->sync_lock);

	/*
	 * Sync the blockmap to the root volume ondisk buffer and generate
	 * the appropriate undo record.  We have to generate the UNDO even
	 * though we flush the volume header along with the UNDO fifo update
	 * because the meta-data (including the volume header) is flushed
	 * after the fifo update, not before.
	 */
	if (root_volume->io.modified) {
		hammer_modify_volume(trans, root_volume,
				    &root_volume->ondisk->vol0_blockmap,
				    sizeof(root_volume->ondisk->vol0_blockmap));
		bcopy(hmp->blockmap, root_volume->ondisk->vol0_blockmap,
		      sizeof(root_volume->ondisk->vol0_blockmap));
		hammer_modify_volume_done(root_volume);
	}

	/*
	 * Flush undo bufs
	 */

	hammer_clear_undo_history(hmp);

	while ((io = TAILQ_FIRST(&hmp->undo_list)) != NULL) {
		KKASSERT(io->modify_refs == 0);
		hammer_ref(&io->lock);
		KKASSERT(io->type != HAMMER_STRUCTURE_VOLUME);
		hammer_io_flush(io);
		hammer_rel_buffer((hammer_buffer_t)io, 1);
	}

	/*
	 * Flush data bufs
	 */
	while ((io = TAILQ_FIRST(&hmp->data_list)) != NULL) {
		KKASSERT(io->modify_refs == 0);
		hammer_ref(&io->lock);
		KKASSERT(io->type != HAMMER_STRUCTURE_VOLUME);
		hammer_io_flush(io);
		hammer_rel_buffer((hammer_buffer_t)io, 1);
	}

	/*
	 * Wait for I/O to complete
	 */
	crit_enter();
	while (hmp->io_running_count) {
		kprintf("W[%d]", hmp->io_running_count);
		tsleep(&hmp->io_running_count, 0, "hmrfl1", 0);
	}
	crit_exit();

	/*
	 * Move the undo FIFO's markers and flush the root volume header.
	 *
	 * If a crash occurs while the root volume header is being written
	 * we just have to hope that the undo range has been updated.  It
	 * should be done in one I/O but XXX this won't be perfect.
	 */
	rootmap = &hmp->blockmap[HAMMER_ZONE_UNDO_INDEX];
	if (rootmap->first_offset != start_offset) {
		hammer_modify_volume(NULL, root_volume, NULL, 0);
		rootmap->first_offset = start_offset;
		hammer_modify_volume_done(root_volume);
	}
	if (root_volume->ondisk->vol0_next_tid != hmp->next_tid) {
		hammer_modify_volume(NULL, root_volume, NULL, 0);
		root_volume->ondisk->vol0_next_tid = hmp->next_tid;
		hammer_modify_volume_done(root_volume);
	}
	if (root_volume->io.modified)
		hammer_io_flush(&root_volume->io);

	/*
	 * Wait for I/O to complete
	 */
	crit_enter();
	while (hmp->io_running_count) {
		tsleep(&hmp->io_running_count, 0, "hmrfl2", 0);
	}
	crit_exit();

	/*
	 * Flush meta-data.  The meta-data will be undone if we crash
	 * so we can safely flush it asynchronously.
	 */
	while ((io = TAILQ_FIRST(&hmp->meta_list)) != NULL) {
		KKASSERT(io->modify_refs == 0);
		hammer_ref(&io->lock);
		KKASSERT(io->type != HAMMER_STRUCTURE_VOLUME);
		hammer_io_flush(io);
		hammer_rel_buffer((hammer_buffer_t)io, 1);
	}
	hammer_unlock(&hmp->sync_lock);
}

