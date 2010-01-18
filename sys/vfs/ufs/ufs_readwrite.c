/*-
 * Copyright (c) 1993
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
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
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
 *	@(#)ufs_readwrite.c	8.11 (Berkeley) 5/8/95
 * $FreeBSD: src/sys/ufs/ufs/ufs_readwrite.c,v 1.65.2.14 2003/04/04 22:21:29 tegge Exp $
 * $DragonFly: src/sys/vfs/ufs/ufs_readwrite.c,v 1.26 2008/06/19 23:27:39 dillon Exp $
 */

#define	BLKSIZE(a, b, c)	blksize(a, b, c)
#define	FS			struct fs
#define	I_FS			i_fs

#include <vm/vm.h>
#include <vm/vm_object.h>
#include <vm/vm_pager.h>
#include <vm/vm_map.h>
#include <vm/vnode_pager.h>
#include <sys/event.h>
#include <sys/vmmeter.h>
#include <sys/sysctl.h>
#include <vm/vm_page2.h>

#include "opt_directio.h"

#define VN_KNOTE(vp, b) \
	KNOTE((struct klist *)&vp->v_pollinfo.vpi_selinfo.si_note, (b))

#ifdef DIRECTIO
extern int ffs_rawread(struct vnode *vp, struct uio *uio, int *workdone);
#endif

SYSCTL_DECL(_vfs_ffs);
static int getpages_uses_bufcache = 1;
SYSCTL_INT(_vfs_ffs, OID_AUTO, getpages_uses_bufcache, CTLFLAG_RW, &getpages_uses_bufcache, 0, "");

/*
 * Vnode op for reading.
 *
 * ffs_read(struct vnode *a_vp, struct uio *a_uio, int a_ioflag,
 *	    struct ucred *a_cred)
 */
/* ARGSUSED */
int
ffs_read(struct vop_read_args *ap)
{
	struct vnode *vp;
	struct inode *ip;
	struct uio *uio;
	FS *fs;
	struct buf *bp;
	off_t bytesinfile;
	int xfersize, blkoffset;
	int error, orig_resid;
	u_short mode;
	int seqcount;
	int ioflag;

	vp = ap->a_vp;
	seqcount = ap->a_ioflag >> 16;
	ip = VTOI(vp);
	mode = ip->i_mode;
	uio = ap->a_uio;
	ioflag = ap->a_ioflag;
#ifdef DIRECTIO
	if ((ioflag & IO_DIRECT) != 0) {
		int workdone;

		error = ffs_rawread(vp, uio, &workdone);
		if (error || workdone)
			return error;
	}
#endif

#ifdef DIAGNOSTIC
	if (uio->uio_rw != UIO_READ)
		panic("ffs_read: mode");

	if (vp->v_type == VLNK) {
		if ((int)ip->i_size < vp->v_mount->mnt_maxsymlinklen)
			panic("ffs_read: short symlink");
	} else if (vp->v_type != VREG && vp->v_type != VDIR)
		panic("ffs_read: type %d", vp->v_type);
#endif
	fs = ip->I_FS;
	if ((uint64_t)uio->uio_offset > fs->fs_maxfilesize)
		return (EFBIG);

	orig_resid = uio->uio_resid;
	if (orig_resid <= 0)
		return (0);

	bytesinfile = ip->i_size - uio->uio_offset;
	if (bytesinfile <= 0) {
		if ((vp->v_mount->mnt_flag & MNT_NOATIME) == 0)
			ip->i_flag |= IN_ACCESS;
		return 0;
	}

	/*
	 * Ok so we couldn't do it all in one vm trick...
	 * so cycle around trying smaller bites..
	 */
	for (error = 0, bp = NULL; uio->uio_resid > 0; bp = NULL) {
		if ((bytesinfile = ip->i_size - uio->uio_offset) <= 0)
			break;

		error = ffs_blkatoff_ra(vp, uio->uio_offset, NULL,
					&bp, seqcount);
		if (error)
			break;

		/*
		 * If IO_DIRECT then set B_DIRECT for the buffer.  This
		 * will cause us to attempt to release the buffer later on
		 * and will cause the buffer cache to attempt to free the
		 * underlying pages.
		 */
		if (ioflag & IO_DIRECT)
			bp->b_flags |= B_DIRECT;

		/*
		 * We should only get non-zero b_resid when an I/O error
		 * has occurred, which should cause us to break above.
		 * However, if the short read did not cause an error,
		 * then we want to ensure that we do not uiomove bad
		 * or uninitialized data.
		 *
		 * XXX b_resid is only valid when an actual I/O has occured
		 * and may be incorrect if the buffer is B_CACHE or if the
		 * last op on the buffer was a failed write.  This KASSERT
		 * is a precursor to removing it from the UFS code.
		 */
		KASSERT(bp->b_resid == 0, ("bp->b_resid != 0"));

		/*
		 * Calculate how much data we can copy
		 */
		blkoffset = blkoff(fs, uio->uio_offset);
		xfersize = bp->b_bufsize - blkoffset;
		if (xfersize > uio->uio_resid)
			xfersize = uio->uio_resid;
		if (xfersize > bytesinfile)
			xfersize = bytesinfile;
		if (xfersize <= 0) {
			panic("ufs_readwrite: impossible xfersize: %d",
			      xfersize);
		}

		/*
		 * otherwise use the general form
		 */
		error = uiomove(bp->b_data + blkoffset, (int)xfersize, uio);

		if (error)
			break;

		if ((ioflag & (IO_VMIO|IO_DIRECT)) && 
		    (LIST_FIRST(&bp->b_dep) == NULL)) {
			/*
			 * If there are no dependencies, and it's VMIO,
			 * then we don't need the buf, mark it available
			 * for freeing. The VM has the data.
			 */
			bp->b_flags |= B_RELBUF;
			brelse(bp);
		} else {
			/*
			 * Otherwise let whoever
			 * made the request take care of
			 * freeing it. We just queue
			 * it onto another list.
			 */
			bqrelse(bp);
		}
	}

	/* 
	 * This can only happen in the case of an error
	 * because the loop above resets bp to NULL on each iteration
	 * and on normal completion has not set a new value into it.
	 * so it must have come from a 'break' statement
	 */
	if (bp != NULL) {
		if ((ioflag & (IO_VMIO|IO_DIRECT)) && 
		    (LIST_FIRST(&bp->b_dep) == NULL)) {
			bp->b_flags |= B_RELBUF;
			brelse(bp);
		} else {
			bqrelse(bp);
		}
	}

	if ((error == 0 || uio->uio_resid != orig_resid) &&
	    (vp->v_mount->mnt_flag & MNT_NOATIME) == 0)
		ip->i_flag |= IN_ACCESS;
	return (error);
}

/*
 * Vnode op for writing.
 *
 * ffs_write(struct vnode *a_vp, struct uio *a_uio, int a_ioflag,
 *	     struct ucred *a_cred)
 */
int
ffs_write(struct vop_write_args *ap)
{
	struct vnode *vp;
	struct uio *uio;
	struct inode *ip;
	FS *fs;
	struct buf *bp;
	ufs_daddr_t lbn;
	off_t osize;
	int seqcount;
	int blkoffset, error, extended, flags, ioflag, resid, size, xfersize;
	struct thread *td;

	extended = 0;
	seqcount = ap->a_ioflag >> 16;
	ioflag = ap->a_ioflag;
	uio = ap->a_uio;
	vp = ap->a_vp;
	ip = VTOI(vp);

#ifdef DIAGNOSTIC
	if (uio->uio_rw != UIO_WRITE)
		panic("ffs_write: mode");
#endif

	switch (vp->v_type) {
	case VREG:
		if (ioflag & IO_APPEND)
			uio->uio_offset = ip->i_size;
		if ((ip->i_flags & APPEND) && uio->uio_offset != ip->i_size)
			return (EPERM);
		/* FALLTHROUGH */
	case VLNK:
		break;
	case VDIR:
		panic("ffs_write: dir write");
		break;
	default:
		panic("ffs_write: type %p %d (%d,%d)", vp, (int)vp->v_type,
			(int)uio->uio_offset,
			(int)uio->uio_resid
		);
	}

	fs = ip->I_FS;
	if (uio->uio_offset < 0 ||
	    (uint64_t)uio->uio_offset + uio->uio_resid > fs->fs_maxfilesize) {
		return (EFBIG);
	}
	/*
	 * Maybe this should be above the vnode op call, but so long as
	 * file servers have no limits, I don't think it matters.
	 */
	td = uio->uio_td;
	if (vp->v_type == VREG && td && td->td_proc &&
	    uio->uio_offset + uio->uio_resid >
	    td->td_proc->p_rlimit[RLIMIT_FSIZE].rlim_cur) {
		lwpsignal(td->td_proc, td->td_lwp, SIGXFSZ);
		return (EFBIG);
	}

	resid = uio->uio_resid;
	osize = ip->i_size;

	/*
	 * NOTE! These B_ flags are actually balloc-only flags, not buffer
	 * flags.  They are similar to the BA_ flags in fbsd.
	 */
	if (seqcount > B_SEQMAX)
		flags = B_SEQMAX << B_SEQSHIFT;
	else
		flags = seqcount << B_SEQSHIFT;
	if ((ioflag & IO_SYNC) && !DOINGASYNC(vp))
		flags |= B_SYNC;

	for (error = 0; uio->uio_resid > 0;) {
		lbn = lblkno(fs, uio->uio_offset);
		blkoffset = blkoff(fs, uio->uio_offset);
		xfersize = fs->fs_bsize - blkoffset;
		if (uio->uio_resid < xfersize)
			xfersize = uio->uio_resid;

		if (uio->uio_offset + xfersize > ip->i_size)
			vnode_pager_setsize(vp, uio->uio_offset + xfersize);

#if 0
		/*      
		 * If doing a dummy write to flush the buffer for a
		 * putpages we must perform a read-before-write to
		 * fill in any missing spots and clear any invalid
		 * areas.  Otherwise a multi-page buffer may not properly
		 * flush.
		 *
		 * We must clear any invalid areas
		 */
		if (uio->uio_segflg == UIO_NOCOPY) {
			error = ffs_blkatoff(vp, uio->uio_offset, NULL, &bp);
			if (error)
				break;
			bqrelse(bp);
		}
#endif

		/*
		 * We must clear invalid areas.
		 */
		if (xfersize < fs->fs_bsize || uio->uio_segflg == UIO_NOCOPY)
			flags |= B_CLRBUF;
		else
			flags &= ~B_CLRBUF;
/* XXX is uio->uio_offset the right thing here? */
		error = VOP_BALLOC(vp, uio->uio_offset, xfersize,
				   ap->a_cred, flags, &bp);
		if (error != 0)
			break;
		/*
		 * If the buffer is not valid and we did not clear garbage
		 * out above, we have to do so here even though the write
		 * covers the entire buffer in order to avoid a mmap()/write
		 * race where another process may see the garbage prior to
		 * the uiomove() for a write replacing it.
		 */
		if ((bp->b_flags & B_CACHE) == 0 && (flags & B_CLRBUF) == 0)
			vfs_bio_clrbuf(bp);
		if (ioflag & IO_DIRECT)
			bp->b_flags |= B_DIRECT;
		if ((ioflag & (IO_SYNC|IO_INVAL)) == (IO_SYNC|IO_INVAL))
			bp->b_flags |= B_NOCACHE;

		if (uio->uio_offset + xfersize > ip->i_size) {
			ip->i_size = uio->uio_offset + xfersize;
			extended = 1;
		}

		size = BLKSIZE(fs, ip, lbn) - bp->b_resid;
		if (size < xfersize)
			xfersize = size;

		error = uiomove(bp->b_data + blkoffset, (int)xfersize, uio);
		if ((ioflag & (IO_VMIO|IO_DIRECT)) && 
		    (LIST_FIRST(&bp->b_dep) == NULL)) {
			bp->b_flags |= B_RELBUF;
		}

		/*
		 * If IO_SYNC each buffer is written synchronously.  Otherwise
		 * if we have a severe page deficiency write the buffer 
		 * asynchronously.  Otherwise try to cluster, and if that
		 * doesn't do it then either do an async write (if O_DIRECT),
		 * or a delayed write (if not).
		 */

		if (ioflag & IO_SYNC) {
			(void)bwrite(bp);
		} else if (vm_page_count_severe() || 
			    buf_dirty_count_severe() ||
			    (ioflag & IO_ASYNC)) {
			bp->b_flags |= B_CLUSTEROK;
			bawrite(bp);
		} else if (xfersize + blkoffset == fs->fs_bsize) {
			if ((vp->v_mount->mnt_flag & MNT_NOCLUSTERW) == 0) {
				bp->b_flags |= B_CLUSTEROK;
				cluster_write(bp, (off_t)ip->i_size, fs->fs_bsize, seqcount);
			} else {
				bawrite(bp);
			}
		} else if (ioflag & IO_DIRECT) {
			bp->b_flags |= B_CLUSTEROK;
			bawrite(bp);
		} else {
			bp->b_flags |= B_CLUSTEROK;
			bdwrite(bp);
		}
		if (error || xfersize == 0)
			break;
		ip->i_flag |= IN_CHANGE | IN_UPDATE;
	}
	/*
	 * If we successfully wrote any data, and we are not the superuser
	 * we clear the setuid and setgid bits as a precaution against
	 * tampering.
	 */
	if (resid > uio->uio_resid && ap->a_cred && ap->a_cred->cr_uid != 0)
		ip->i_mode &= ~(ISUID | ISGID);
	if (resid > uio->uio_resid)
		VN_KNOTE(vp, NOTE_WRITE | (extended ? NOTE_EXTEND : 0));
	if (error) {
		if (ioflag & IO_UNIT) {
			(void)ffs_truncate(vp, osize, ioflag & IO_SYNC,
					   ap->a_cred);
			uio->uio_offset -= resid - uio->uio_resid;
			uio->uio_resid = resid;
		}
	} else if (resid > uio->uio_resid && (ioflag & IO_SYNC)) {
		error = ffs_update(vp, 1);
	}

	return (error);
}


/*
 * get page routine
 */
int
ffs_getpages(struct vop_getpages_args *ap)
{
	off_t foff, physoffset;
	int i, size, bsize;
	struct vnode *dp, *vp;
	vm_object_t obj;
	vm_pindex_t pindex, firstindex;
	vm_page_t mreq;
	int bbackwards, bforwards;
	int pbackwards, pforwards;
	int firstpage;
	off_t reqoffset;
	off_t doffset;
	int poff;
	int pcount;
	int rtval;
	int pagesperblock;

	/*
	 * If set just use the system standard getpages which issues a
	 * UIO_NOCOPY VOP_READ.
	 */
	if (getpages_uses_bufcache) {
		return vop_stdgetpages(ap);
	}

	pcount = round_page(ap->a_count) / PAGE_SIZE;
	mreq = ap->a_m[ap->a_reqpage];
	firstindex = ap->a_m[0]->pindex;

	/*
	 * if ANY DEV_BSIZE blocks are valid on a large filesystem block,
	 * then the entire page is valid.  Since the page may be mapped,
	 * user programs might reference data beyond the actual end of file
	 * occuring within the page.  We have to zero that data.
	 */
	if (mreq->valid) {
		if (mreq->valid != VM_PAGE_BITS_ALL)
			vm_page_zero_invalid(mreq, TRUE);
		for (i = 0; i < pcount; i++) {
			if (i != ap->a_reqpage) {
				vm_page_free(ap->a_m[i]);
			}
		}
		return VM_PAGER_OK;
	}

	vp = ap->a_vp;
	obj = vp->v_object;
	bsize = vp->v_mount->mnt_stat.f_iosize;
	pindex = mreq->pindex;
	foff = IDX_TO_OFF(pindex) /* + ap->a_offset should be zero */;

	if (bsize < PAGE_SIZE)
		return vnode_pager_generic_getpages(ap->a_vp, ap->a_m,
						    ap->a_count, ap->a_reqpage,
						    ap->a_seqaccess);

	/*
	 * foff is the file offset of the required page
	 * reqlblkno is the logical block that contains the page
	 * poff is the bytes offset of the page in the logical block
	 */
	poff = (int)(foff % bsize);
	reqoffset = foff - poff;

	if (VOP_BMAP(vp, reqoffset, &doffset, &bforwards, &bbackwards, BUF_CMD_READ) ||
	    doffset == NOOFFSET
	) {
		for (i = 0; i < pcount; i++) {
			if (i != ap->a_reqpage)
				vm_page_free(ap->a_m[i]);
		}
		if (doffset == NOOFFSET) {
			if ((mreq->flags & PG_ZERO) == 0)
				vm_page_zero_fill(mreq);
			vm_page_undirty(mreq);
			mreq->valid = VM_PAGE_BITS_ALL;
			return VM_PAGER_OK;
		} else {
			return VM_PAGER_ERROR;
		}
	}

	physoffset = doffset + poff;
	pagesperblock = bsize / PAGE_SIZE;

	/*
	 * find the first page that is contiguous.
	 *
	 * bforwards and bbackwards are the number of contiguous bytes
	 * available before and after the block offset.  poff is the page
	 * offset, in bytes, relative to the block offset.
	 *
	 * pforwards and pbackwards are the number of contiguous pages
	 * relative to the requested page, non-inclusive of the requested
	 * page (so a pbackwards and  pforwards of 0 indicates just the
	 * requested page).
	 */
	firstpage = 0;
	if (ap->a_count) {
		/*
		 * Calculate pbackwards and clean up any requested
		 * pages that are too far back.
		 */
		pbackwards = (poff + bbackwards) >> PAGE_SHIFT;
		if (ap->a_reqpage > pbackwards) {
			firstpage = ap->a_reqpage - pbackwards;
			for (i = 0; i < firstpage; i++)
				vm_page_free(ap->a_m[i]);
		}

		/*
		 * Calculate pforwards
		 */
		pforwards = (bforwards - poff - PAGE_SIZE) >> PAGE_SHIFT;
		if (pforwards < 0)
			pforwards = 0;
		if (pforwards < (pcount - (ap->a_reqpage + 1))) {
			for(i = ap->a_reqpage + pforwards + 1; i < pcount; i++)
				vm_page_free(ap->a_m[i]);
			pcount = ap->a_reqpage + pforwards + 1;
		}

		/*
		 * Adjust pcount to be relative to firstpage.  All pages prior
		 * to firstpage in the array have been cleaned up.
		 */
		pcount -= firstpage;
	}

	/*
	 * calculate the size of the transfer
	 */
	size = pcount * PAGE_SIZE;

	if ((IDX_TO_OFF(ap->a_m[firstpage]->pindex) + size) > vp->v_filesize) {
		size = vp->v_filesize - IDX_TO_OFF(ap->a_m[firstpage]->pindex);
	}

	physoffset -= foff;
	dp = VTOI(ap->a_vp)->i_devvp;
	rtval = VOP_GETPAGES(dp, &ap->a_m[firstpage], size,
			     (ap->a_reqpage - firstpage), physoffset,
			     ap->a_seqaccess);

	return (rtval);
}

