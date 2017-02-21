/*
 * Copyright (c) International Business Machines Corp., 2006
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See
 * the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * Author: Artem Bityutskiy (Битюцкий Артём)
 */

/*
 * UBI attaching sub-system.
 *
 * This sub-system is responsible for attaching MTD devices and it also
 * implements flash media scanning.
 *
 * The attaching information is represented by a &struct ubi_attach_info'
 * object. Information about volumes is represented by &struct ubi_ainf_volume
 * objects which are kept in volume RB-tree with root at the @volumes field.
 * The RB-tree is indexed by the volume ID.
 *
 * Logical eraseblocks are represented by &struct ubi_ainf_peb objects. These
 * objects are kept in per-volume RB-trees with the root at the corresponding
 * &struct ubi_ainf_volume object. To put it differently, we keep an RB-tree of
 * per-volume objects and each of these objects is the root of RB-tree of
 * per-LEB objects.
 *
 * Corrupted physical eraseblocks are put to the @corr list, free physical
 * eraseblocks are put to the @free list and the physical eraseblock to be
 * erased are put to the @erase list.
 *
 * About corruptions
 * ~~~~~~~~~~~~~~~~~
 *
 * UBI protects EC and VID headers with CRC-32 checksums, so it can detect
 * whether the headers are corrupted or not. Sometimes UBI also protects the
 * data with CRC-32, e.g., when it executes the atomic LEB change operation, or
 * when it moves the contents of a PEB for wear-leveling purposes.
 *
 * UBI tries to distinguish between 2 types of corruptions.
 *
 * 1. Corruptions caused by power cuts. These are expected corruptions and UBI
 * tries to handle them gracefully, without printing too many warnings and
 * error messages. The idea is that we do not lose important data in these
 * cases - we may lose only the data which were being written to the media just
 * before the power cut happened, and the upper layers (e.g., UBIFS) are
 * supposed to handle such data losses (e.g., by using the FS journal).
 *
 * When UBI detects a corruption (CRC-32 mismatch) in a PEB, and it looks like
 * the reason is a power cut, UBI puts this PEB to the @erase list, and all
 * PEBs in the @erase list are scheduled for erasure later.
 *
 * 2. Unexpected corruptions which are not caused by power cuts. During
 * attaching, such PEBs are put to the @corr list and UBI preserves them.
 * Obviously, this lessens the amount of available PEBs, and if at some  point
 * UBI runs out of free PEBs, it switches to R/O mode. UBI also loudly informs
 * about such PEBs every time the MTD device is attached.
 *
 * However, it is difficult to reliably distinguish between these types of
 * corruptions and UBI's strategy is as follows (in case of attaching by
 * scanning). UBI assumes corruption type 2 if the VID header is corrupted and
 * the data area does not contain all 0xFFs, and there were no bit-flips or
 * integrity errors (e.g., ECC errors in case of NAND) while reading the data
 * area.  Otherwise UBI assumes corruption type 1. So the decision criteria
 * are as follows.
 *   o If the data area contains only 0xFFs, there are no data, and it is safe
 *     to just erase this PEB - this is corruption type 1.
 *   o If the data area has bit-flips or data integrity errors (ECC errors on
 *     NAND), it is probably a PEB which was being erased when power cut
 *     happened, so this is corruption type 1. However, this is just a guess,
 *     which might be wrong.
 *   o Otherwise this is corruption type 2.
 */

#include <linux/err.h>
#include <linux/slab.h>
#include <linux/crc32.h>
#include <linux/math64.h>
#include <linux/random.h>
#include "ubi.h"

static int self_check_ai(struct ubi_device *ubi, struct ubi_attach_info *ai);

/* Temporary variables used during scanning */
static struct ubi_ec_hdr *ech;
static struct ubi_vid_hdr *vidh;

#ifdef CONFIG_MTK_SLC_BUFFER_SUPPORT
/* static int last_tlc_pnum = -1; */
/*static int slc_number = -1;*/
int ubi_peb_istlc(struct ubi_device *ubi, int pnum)
{
	int ret = 0; /* not tlc peb */
	u64 addr = mtd_partition_start_address(ubi->mtd);

	addr += (u64)pnum * ubi->peb_size;
	ret = mtk_block_istlc(addr); /* Todo, nand driver api */
	return ret;
}
#endif

/**
 * add_to_list - add physical eraseblock to a list.
 * @ai: attaching information
 * @pnum: physical eraseblock number to add
 * @vol_id: the last used volume id for the PEB
 * @lnum: the last used LEB number for the PEB
 * @ec: erase counter of the physical eraseblock
 * @to_head: if not zero, add to the head of the list
 * @list: the list to add to
 *
 * This function allocates a 'struct ubi_ainf_peb' object for physical
 * eraseblock @pnum and adds it to the "free", "erase", or "alien" lists.
 * It stores the @lnum and @vol_id alongside, which can both be
 * %UBI_UNKNOWN if they are not available, not readable, or not assigned.
 * If @to_head is not zero, PEB will be added to the head of the list, which
 * basically means it will be processed first later. E.g., we add corrupted
 * PEBs (corrupted due to power cuts) to the head of the erase list to make
 * sure we erase them first and get rid of corruptions ASAP. This function
 * returns zero in case of success and a negative error code in case of
 * failure.
 */
static int add_to_list(struct ubi_device *ubi, struct ubi_attach_info *ai, int pnum, int vol_id,
		       int lnum, int ec, int to_head, struct list_head *list)
{
	struct ubi_ainf_peb *aeb;

	if (list == &ai->free) {
		dbg_bld("add to free: PEB %d, EC %d", pnum, ec);
	} else if (list == &ai->erase) {
		dbg_bld("add to erase: PEB %d, EC %d", pnum, ec);
	} else if (list == &ai->alien) {
		dbg_bld("add to alien: PEB %d, EC %d", pnum, ec);
		ai->alien_peb_count += 1;
#ifdef CONFIG_MTD_UBI_LOWPAGE_BACKUP
	} else if (list == &ai->waiting) {
		dbg_bld("add to waiting: PEB %d, EC %d", pnum, ec);
#endif
	} else
		BUG();

	aeb = kmem_cache_alloc(ai->aeb_slab_cache, GFP_KERNEL);
	if (!aeb)
		return -ENOMEM;

	aeb->pnum = pnum;
	aeb->vol_id = vol_id;
	aeb->lnum = lnum;
	aeb->ec = ec;
#ifdef CONFIG_MTK_SLC_BUFFER_SUPPORT
	aeb->tlc = ubi_peb_istlc(ubi, pnum);
#endif
	if (to_head)
		list_add(&aeb->u.list, list);
	else
		list_add_tail(&aeb->u.list, list);
	return 0;
}

/**
 * add_corrupted - add a corrupted physical eraseblock.
 * @ai: attaching information
 * @pnum: physical eraseblock number to add
 * @ec: erase counter of the physical eraseblock
 *
 * This function allocates a 'struct ubi_ainf_peb' object for a corrupted
 * physical eraseblock @pnum and adds it to the 'corr' list.  The corruption
 * was presumably not caused by a power cut. Returns zero in case of success
 * and a negative error code in case of failure.
 */
static int add_corrupted(struct ubi_attach_info *ai, int pnum, int ec)
{
	struct ubi_ainf_peb *aeb;

	dbg_bld("add to corrupted: PEB %d, EC %d", pnum, ec);

	aeb = kmem_cache_alloc(ai->aeb_slab_cache, GFP_KERNEL);
	if (!aeb)
		return -ENOMEM;

	ai->corr_peb_count += 1;
	aeb->pnum = pnum;
	aeb->ec = ec;
	list_add(&aeb->u.list, &ai->corr);
	return 0;
}

/**
 * validate_vid_hdr - check volume identifier header.
 * @vid_hdr: the volume identifier header to check
 * @av: information about the volume this logical eraseblock belongs to
 * @pnum: physical eraseblock number the VID header came from
 *
 * This function checks that data stored in @vid_hdr is consistent. Returns
 * non-zero if an inconsistency was found and zero if not.
 *
 * Note, UBI does sanity check of everything it reads from the flash media.
 * Most of the checks are done in the I/O sub-system. Here we check that the
 * information in the VID header is consistent to the information in other VID
 * headers of the same volume.
 */
static int validate_vid_hdr(const struct ubi_vid_hdr *vid_hdr,
			    const struct ubi_ainf_volume *av, int pnum)
{
	int vol_type = vid_hdr->vol_type;
	int vol_id = be32_to_cpu(vid_hdr->vol_id);
	int used_ebs = be32_to_cpu(vid_hdr->used_ebs);
	int data_pad = be32_to_cpu(vid_hdr->data_pad);

	if (av->leb_count != 0) {
		int av_vol_type;

		/*
		 * This is not the first logical eraseblock belonging to this
		 * volume. Ensure that the data in its VID header is consistent
		 * to the data in previous logical eraseblock headers.
		 */

		if (vol_id != av->vol_id) {
			ubi_err("inconsistent vol_id");
			goto bad;
		}

		if (av->vol_type == UBI_STATIC_VOLUME)
			av_vol_type = UBI_VID_STATIC;
		else
			av_vol_type = UBI_VID_DYNAMIC;

		if (vol_type != av_vol_type) {
			ubi_err("inconsistent vol_type");
			goto bad;
		}

		if (used_ebs != av->used_ebs) {
			ubi_err("inconsistent used_ebs");
			goto bad;
		}

		if (data_pad != av->data_pad) {
			ubi_err("inconsistent data_pad");
			goto bad;
		}
	}

	return 0;

bad:
	ubi_err("inconsistent VID header at PEB %d", pnum);
	ubi_dump_vid_hdr(vid_hdr);
	ubi_dump_av(av);
	return -EINVAL;
}

/**
 * add_volume - add volume to the attaching information.
 * @ai: attaching information
 * @vol_id: ID of the volume to add
 * @pnum: physical eraseblock number
 * @vid_hdr: volume identifier header
 *
 * If the volume corresponding to the @vid_hdr logical eraseblock is already
 * present in the attaching information, this function does nothing. Otherwise
 * it adds corresponding volume to the attaching information. Returns a pointer
 * to the allocated "av" object in case of success and a negative error code in
 * case of failure.
 */
static struct ubi_ainf_volume *add_volume(struct ubi_attach_info *ai,
					  int vol_id, int pnum,
					  const struct ubi_vid_hdr *vid_hdr)
{
	struct ubi_ainf_volume *av;
	struct rb_node **p = &ai->volumes.rb_node, *parent = NULL;

	ubi_assert(vol_id == be32_to_cpu(vid_hdr->vol_id));

	/* Walk the volume RB-tree to look if this volume is already present */
	while (*p) {
		parent = *p;
		av = rb_entry(parent, struct ubi_ainf_volume, rb);

		if (vol_id == av->vol_id)
			return av;

		if (vol_id > av->vol_id)
			p = &(*p)->rb_left;
		else
			p = &(*p)->rb_right;
	}

	/* The volume is absent - add it */
	av = kmalloc(sizeof(struct ubi_ainf_volume), GFP_KERNEL);
	if (!av)
		return ERR_PTR(-ENOMEM);

	av->highest_lnum = av->leb_count = 0;
	av->vol_id = vol_id;
	av->root = RB_ROOT;
	av->used_ebs = be32_to_cpu(vid_hdr->used_ebs);
	av->data_pad = be32_to_cpu(vid_hdr->data_pad);
	av->compat = vid_hdr->compat;
	av->vol_type = vid_hdr->vol_type == UBI_VID_DYNAMIC ? UBI_DYNAMIC_VOLUME
							    : UBI_STATIC_VOLUME;
	if (vol_id > ai->highest_vol_id)
		ai->highest_vol_id = vol_id;

	rb_link_node(&av->rb, parent, p);
	rb_insert_color(&av->rb, &ai->volumes);
	ai->vols_found += 1;
	dbg_bld("added volume %d", vol_id);
	return av;
}

/**
 * ubi_compare_lebs - find out which logical eraseblock is newer.
 * @ubi: UBI device description object
 * @aeb: first logical eraseblock to compare
 * @pnum: physical eraseblock number of the second logical eraseblock to
 * compare
 * @vid_hdr: volume identifier header of the second logical eraseblock
 *
 * This function compares 2 copies of a LEB and informs which one is newer. In
 * case of success this function returns a positive value, in case of failure, a
 * negative error code is returned. The success return codes use the following
 * bits:
 *     o bit 0 is cleared: the first PEB (described by @aeb) is newer than the
 *       second PEB (described by @pnum and @vid_hdr);
 *     o bit 0 is set: the second PEB is newer;
 *     o bit 1 is cleared: no bit-flips were detected in the newer LEB;
 *     o bit 1 is set: bit-flips were detected in the newer LEB;
 *     o bit 2 is cleared: the older LEB is not corrupted;
 *     o bit 2 is set: the older LEB is corrupted.
 */
int ubi_compare_lebs(struct ubi_device *ubi, const struct ubi_ainf_peb *aeb,
			int pnum, const struct ubi_vid_hdr *vid_hdr)
{
	int len, err, second_is_newer, bitflips = 0, corrupted = 0;
	uint32_t data_crc, crc;
	struct ubi_vid_hdr *vh = NULL;
	unsigned long long sqnum2 = be64_to_cpu(vid_hdr->sqnum);

	if (sqnum2 == aeb->sqnum) {
		/*
		 * This must be a really ancient UBI image which has been
		 * created before sequence numbers support has been added. At
		 * that times we used 32-bit LEB versions stored in logical
		 * eraseblocks. That was before UBI got into mainline. We do not
		 * support these images anymore. Well, those images still work,
		 * but only if no unclean reboots happened.
		 */
		ubi_err("unsupported on-flash UBI format");
		return -EINVAL;
	}

	/* Obviously the LEB with lower sequence counter is older */
	second_is_newer = (sqnum2 > aeb->sqnum);

	/*
	 * Now we know which copy is newer. If the copy flag of the PEB with
	 * newer version is not set, then we just return, otherwise we have to
	 * check data CRC. For the second PEB we already have the VID header,
	 * for the first one - we'll need to re-read it from flash.
	 *
	 * Note: this may be optimized so that we wouldn't read twice.
	 */

	if (second_is_newer) {
		if (!vid_hdr->copy_flag) {
			/* It is not a copy, so it is newer */
			dbg_bld("second PEB %d is newer, copy_flag is unset",
				pnum);
			return 1;
		}
	} else {
		if (!aeb->copy_flag) {
			/* It is not a copy, so it is newer */
			dbg_bld("first PEB %d is newer, copy_flag is unset",
				pnum);
			return bitflips << 1;
		}

		vh = ubi_zalloc_vid_hdr(ubi, GFP_KERNEL);
		if (!vh)
			return -ENOMEM;

		pnum = aeb->pnum;
		err = ubi_io_read_vid_hdr(ubi, pnum, vh, 0);
		if (err) {
			if (err == UBI_IO_BITFLIPS)
				bitflips = 1;
			else {
				ubi_err("VID of PEB %d header is bad, but it was OK earlier, err %d",
					pnum, err);
				if (err > 0)
					err = -EIO;

				goto out_free_vidh;
			}
		}

		vid_hdr = vh;
	}

	/* Read the data of the copy and check the CRC */

	len = be32_to_cpu(vid_hdr->data_size);

#ifdef CONFIG_UBI_SHARE_BUFFER
	mutex_lock(&ubi_buf_mutex);
#else
	mutex_lock(&ubi->buf_mutex);
#endif
	err = ubi_io_read_data(ubi, ubi->peb_buf, pnum, 0, len);
	if (err && err != UBI_IO_BITFLIPS && !mtd_is_eccerr(err))
		goto out_unlock;

	data_crc = be32_to_cpu(vid_hdr->data_crc);
	crc = crc32(UBI_CRC32_INIT, ubi->peb_buf, len);
	if (crc != data_crc) {
		dbg_bld("PEB %d CRC error: calculated %#08x, must be %#08x",
			pnum, crc, data_crc);
		corrupted = 1;
		bitflips = 0;
		second_is_newer = !second_is_newer;
	} else {
		dbg_bld("PEB %d CRC is OK", pnum);
		bitflips |= !!err;
	}
#ifdef CONFIG_UBI_SHARE_BUFFER
	mutex_unlock(&ubi_buf_mutex);
#else
	mutex_unlock(&ubi->buf_mutex);
#endif

	ubi_free_vid_hdr(ubi, vh);

	if (second_is_newer)
		dbg_bld("second PEB %d is newer, copy_flag is set", pnum);
	else
		dbg_bld("first PEB %d is newer, copy_flag is set", pnum);

	return second_is_newer | (bitflips << 1) | (corrupted << 2);

out_unlock:
#ifdef CONFIG_UBI_SHARE_BUFFER
	mutex_unlock(&ubi_buf_mutex);
#else
	mutex_unlock(&ubi->buf_mutex);
#endif
out_free_vidh:
	ubi_free_vid_hdr(ubi, vh);
	return err;
}

/**
 * ubi_add_to_av - add used physical eraseblock to the attaching information.
 * @ubi: UBI device description object
 * @ai: attaching information
 * @pnum: the physical eraseblock number
 * @ec: erase counter
 * @vid_hdr: the volume identifier header
 * @bitflips: if bit-flips were detected when this physical eraseblock was read
 *
 * This function adds information about a used physical eraseblock to the
 * 'used' tree of the corresponding volume. The function is rather complex
 * because it has to handle cases when this is not the first physical
 * eraseblock belonging to the same logical eraseblock, and the newer one has
 * to be picked, while the older one has to be dropped. This function returns
 * zero in case of success and a negative error code in case of failure.
 */
int ubi_add_to_av(struct ubi_device *ubi, struct ubi_attach_info *ai, int pnum,
		  int ec, const struct ubi_vid_hdr *vid_hdr, int bitflips)
{
	int err, vol_id, lnum;
	unsigned long long sqnum;
	struct ubi_ainf_volume *av;
	struct ubi_ainf_peb *aeb;
	struct rb_node **p, *parent = NULL;

	vol_id = be32_to_cpu(vid_hdr->vol_id);
	lnum = be32_to_cpu(vid_hdr->lnum);
	sqnum = be64_to_cpu(vid_hdr->sqnum);

	dbg_bld("PEB %d, LEB %d:%d, EC %d, sqnum %llu, bitflips %d",
		pnum, vol_id, lnum, ec, sqnum, bitflips);

	av = add_volume(ai, vol_id, pnum, vid_hdr);
	if (IS_ERR(av))
		return PTR_ERR(av);

	if (ai->max_sqnum < sqnum)
		ai->max_sqnum = sqnum;

	/*
	 * Walk the RB-tree of logical eraseblocks of volume @vol_id to look
	 * if this is the first instance of this logical eraseblock or not.
	 */
	p = &av->root.rb_node;
	while (*p) {
		int cmp_res;

		parent = *p;
		aeb = rb_entry(parent, struct ubi_ainf_peb, u.rb);
		if (lnum != aeb->lnum) {
			if (lnum < aeb->lnum)
				p = &(*p)->rb_left;
			else
				p = &(*p)->rb_right;
			continue;
		}

		/*
		 * There is already a physical eraseblock describing the same
		 * logical eraseblock present.
		 */

		dbg_bld("this LEB already exists: PEB %d, sqnum %llu, EC %d",
			aeb->pnum, aeb->sqnum, aeb->ec);

		/*
		 * Make sure that the logical eraseblocks have different
		 * sequence numbers. Otherwise the image is bad.
		 *
		 * However, if the sequence number is zero, we assume it must
		 * be an ancient UBI image from the era when UBI did not have
		 * sequence numbers. We still can attach these images, unless
		 * there is a need to distinguish between old and new
		 * eraseblocks, in which case we'll refuse the image in
		 * 'ubi_compare_lebs()'. In other words, we attach old clean
		 * images, but refuse attaching old images with duplicated
		 * logical eraseblocks because there was an unclean reboot.
		 */
		if (aeb->sqnum == sqnum && sqnum != 0) {
			ubi_err("two LEBs with same sequence number %llu",
				sqnum);
			ubi_dump_aeb(aeb, 0);
			ubi_dump_vid_hdr(vid_hdr);
			return -EINVAL;
		}

		/*
		 * Now we have to drop the older one and preserve the newer
		 * one.
		 */
		cmp_res = ubi_compare_lebs(ubi, aeb, pnum, vid_hdr);
		if (cmp_res < 0)
			return cmp_res;

		if (cmp_res & 1) {
			/*
			 * This logical eraseblock is newer than the one
			 * found earlier.
			 */
			err = validate_vid_hdr(vid_hdr, av, pnum);
			if (err)
				return err;

			err = add_to_list(ubi, ai, aeb->pnum, aeb->vol_id,
					  aeb->lnum, aeb->ec, cmp_res & 4,
					  &ai->erase);
			if (err)
				return err;

			aeb->ec = ec;
			aeb->pnum = pnum;
			aeb->vol_id = vol_id;
			aeb->lnum = lnum;
			aeb->scrub = ((cmp_res & 2) || bitflips);
			aeb->copy_flag = vid_hdr->copy_flag;
			aeb->sqnum = sqnum;
#ifdef CONFIG_MTK_SLC_BUFFER_SUPPORT
			aeb->tlc = ubi_peb_istlc(ubi, pnum);
#endif

			if (av->highest_lnum == lnum)
				av->last_data_size =
					be32_to_cpu(vid_hdr->data_size);
			return 0;
		}
		/*
		 * This logical eraseblock is older than the one found
		 * previously.
		 */
		return add_to_list(ubi, ai, pnum, vol_id, lnum, ec,
				   cmp_res & 4, &ai->erase);
	}

	/*
	 * We've met this logical eraseblock for the first time, add it to the
	 * attaching information.
	 */

	err = validate_vid_hdr(vid_hdr, av, pnum);
	if (err)
		return err;

	aeb = kmem_cache_alloc(ai->aeb_slab_cache, GFP_KERNEL);
	if (!aeb)
		return -ENOMEM;

	aeb->ec = ec;
	aeb->pnum = pnum;
	aeb->vol_id = vol_id;
	aeb->lnum = lnum;
	aeb->scrub = bitflips;
	aeb->copy_flag = vid_hdr->copy_flag;
	aeb->sqnum = sqnum;
#ifdef CONFIG_MTK_SLC_BUFFER_SUPPORT
	aeb->tlc = ubi_peb_istlc(ubi, pnum);
#endif

	if (av->highest_lnum <= lnum) {
		av->highest_lnum = lnum;
		av->last_data_size = be32_to_cpu(vid_hdr->data_size);
	}

	av->leb_count += 1;
	rb_link_node(&aeb->u.rb, parent, p);
	rb_insert_color(&aeb->u.rb, &av->root);
	return 0;
}

/**
 * ubi_find_av - find volume in the attaching information.
 * @ai: attaching information
 * @vol_id: the requested volume ID
 *
 * This function returns a pointer to the volume description or %NULL if there
 * are no data about this volume in the attaching information.
 */
struct ubi_ainf_volume *ubi_find_av(const struct ubi_attach_info *ai,
				    int vol_id)
{
	struct ubi_ainf_volume *av;
	struct rb_node *p = ai->volumes.rb_node;

	while (p) {
		av = rb_entry(p, struct ubi_ainf_volume, rb);

		if (vol_id == av->vol_id)
			return av;

		if (vol_id > av->vol_id)
			p = p->rb_left;
		else
			p = p->rb_right;
	}

	return NULL;
}

/**
 * ubi_remove_av - delete attaching information about a volume.
 * @ai: attaching information
 * @av: the volume attaching information to delete
 */
void ubi_remove_av(struct ubi_attach_info *ai, struct ubi_ainf_volume *av)
{
	struct rb_node *rb;
	struct ubi_ainf_peb *aeb;

	dbg_bld("remove attaching information about volume %d", av->vol_id);

	while ((rb = rb_first(&av->root))) {
		aeb = rb_entry(rb, struct ubi_ainf_peb, u.rb);
		rb_erase(&aeb->u.rb, &av->root);
		list_add_tail(&aeb->u.list, &ai->erase);
	}

	rb_erase(&av->rb, &ai->volumes);
	kfree(av);
	ai->vols_found -= 1;
}

/**
 * early_erase_peb - erase a physical eraseblock.
 * @ubi: UBI device description object
 * @ai: attaching information
 * @pnum: physical eraseblock number to erase;
 * @ec: erase counter value to write (%UBI_UNKNOWN if it is unknown)
 *
 * This function erases physical eraseblock 'pnum', and writes the erase
 * counter header to it. This function should only be used on UBI device
 * initialization stages, when the EBA sub-system had not been yet initialized.
 * This function returns zero in case of success and a negative error code in
 * case of failure.
 */
static int early_erase_peb(struct ubi_device *ubi,
			   const struct ubi_attach_info *ai, int pnum, int ec)
{
	int err;
	struct ubi_ec_hdr *ec_hdr;

	if ((long long)ec >= UBI_MAX_ERASECOUNTER) {
		/*
		 * Erase counter overflow. Upgrade UBI and use 64-bit
		 * erase counters internally.
		 */
		ubi_err("erase counter overflow at PEB %d, EC %d", pnum, ec);
		return -EINVAL;
	}

	ec_hdr = kzalloc(ubi->ec_hdr_alsize, GFP_KERNEL);
	if (!ec_hdr)
		return -ENOMEM;

	ec_hdr->ec = cpu_to_be64(ec);

	err = ubi_io_sync_erase(ubi, pnum, 0);
	if (err < 0)
		goto out_free;

	err = ubi_io_write_ec_hdr(ubi, pnum, ec_hdr);

out_free:
	kfree(ec_hdr);
	return err;
}

/**
 * ubi_early_get_peb - get a free physical eraseblock.
 * @ubi: UBI device description object
 * @ai: attaching information
 *
 * This function returns a free physical eraseblock. It is supposed to be
 * called on the UBI initialization stages when the wear-leveling sub-system is
 * not initialized yet. This function picks a physical eraseblocks from one of
 * the lists, writes the EC header if it is needed, and removes it from the
 * list.
 *
 * This function returns a pointer to the "aeb" of the found free PEB in case
 * of success and an error code in case of failure.
 */
struct ubi_ainf_peb *ubi_early_get_peb(struct ubi_device *ubi,
				       struct ubi_attach_info *ai)
{
	int err = 0;
	struct ubi_ainf_peb *aeb, *tmp_aeb;

	if (!list_empty(&ai->free)) {
#ifdef CONFIG_MTK_SLC_BUFFER_SUPPORT
		list_for_each_entry_safe(aeb, tmp_aeb, &ai->free, u.list) {
			if (aeb->tlc)
				continue;
			list_del(&aeb->u.list);
			dbg_bld("return free PEB %d, EC %d", aeb->pnum, aeb->ec);
			return aeb;
		}
#else
		aeb = list_entry(ai->free.next, struct ubi_ainf_peb, u.list);
		list_del(&aeb->u.list);
		dbg_bld("return free PEB %d, EC %d", aeb->pnum, aeb->ec);
		return aeb;
#endif
	}

	/*
	 * We try to erase the first physical eraseblock from the erase list
	 * and pick it if we succeed, or try to erase the next one if not. And
	 * so forth. We don't want to take care about bad eraseblocks here -
	 * they'll be handled later.
	 */
	list_for_each_entry_safe(aeb, tmp_aeb, &ai->erase, u.list) {
#ifdef CONFIG_MTK_SLC_BUFFER_SUPPORT
		if (aeb->tlc)
			continue;
#endif
		if (aeb->ec == UBI_UNKNOWN)
			aeb->ec = ai->mean_ec;

		err = early_erase_peb(ubi, ai, aeb->pnum, aeb->ec+1);
		if (err)
			continue;

		aeb->ec += 1;
		list_del(&aeb->u.list);
		dbg_bld("return PEB %d, EC %d", aeb->pnum, aeb->ec);
		return aeb;
	}

	ubi_err("no free eraseblocks");
	return ERR_PTR(-ENOSPC);
}

/**
 * check_corruption - check the data area of PEB.
 * @ubi: UBI device description object
 * @vid_hdr: the (corrupted) VID header of this PEB
 * @pnum: the physical eraseblock number to check
 *
 * This is a helper function which is used to distinguish between VID header
 * corruptions caused by power cuts and other reasons. If the PEB contains only
 * 0xFF bytes in the data area, the VID header is most probably corrupted
 * because of a power cut (%0 is returned in this case). Otherwise, it was
 * probably corrupted for some other reasons (%1 is returned in this case). A
 * negative error code is returned if a read error occurred.
 *
 * If the corruption reason was a power cut, UBI can safely erase this PEB.
 * Otherwise, it should preserve it to avoid possibly destroying important
 * information.
 */
static int check_corruption(struct ubi_device *ubi, struct ubi_vid_hdr *vid_hdr,
			    int pnum)
{
	int err;

#ifdef CONFIG_UBI_SHARE_BUFFER
	mutex_lock(&ubi_buf_mutex);
#else
	mutex_lock(&ubi->buf_mutex);
#endif
	memset(ubi->peb_buf, 0x00, ubi->leb_size);

	err = ubi_io_read(ubi, ubi->peb_buf, pnum, ubi->leb_start, ubi->leb_size);
	if (err == UBI_IO_BITFLIPS || mtd_is_eccerr(err)) {
		/*
		 * Bit-flips or integrity errors while reading the data area.
		 * It is difficult to say for sure what type of corruption is
		 * this, but presumably a power cut happened while this PEB was
		 * erased, so it became unstable and corrupted, and should be
		 * erased.
		 */
		err = 0;
		goto out_unlock;
	}

	if (err)
		goto out_unlock;

	if (ubi_check_pattern(ubi->peb_buf, 0xFF, ubi->leb_size))
		goto out_unlock;

	ubi_err("PEB %d contains corrupted VID header, and the data does not contain all 0xFF",
		pnum);
	ubi_err("this may be a non-UBI PEB or a severe VID header corruption which requires manual inspection");
	ubi_dump_vid_hdr(vid_hdr);
	pr_err("hexdump of PEB %d offset %d, length %d",
	       pnum, ubi->leb_start, ubi->leb_size);
	ubi_dbg_print_hex_dump(KERN_DEBUG, "", DUMP_PREFIX_OFFSET, 32, 1,
			       ubi->peb_buf, ubi->leb_size, 1);
	err = 1;

out_unlock:
#ifdef CONFIG_UBI_SHARE_BUFFER
	mutex_unlock(&ubi_buf_mutex);
#else
	mutex_unlock(&ubi->buf_mutex);
#endif
	return err;
}

/**
 * scan_peb - scan and process UBI headers of a PEB.
 * @ubi: UBI device description object
 * @ai: attaching information
 * @pnum: the physical eraseblock number
 * @vid: The volume ID of the found volume will be stored in this pointer
 * @sqnum: The sqnum of the found volume will be stored in this pointer
 *
 * This function reads UBI headers of PEB @pnum, checks them, and adds
 * information about this PEB to the corresponding list or RB-tree in the
 * "attaching info" structure. Returns zero if the physical eraseblock was
 * successfully handled and a negative error code in case of failure.
 */
static int scan_peb(struct ubi_device *ubi, struct ubi_attach_info *ai,
		    int pnum, int *vid, unsigned long long *sqnum)
{
	long long uninitialized_var(ec);
	int err, bitflips = 0, vol_id = -1, ec_err = 0;
#ifdef CONFIG_MTK_SLC_BUFFER_SUPPORT
	uint32_t crc, map = 1, mtbl_vol_id;
	int istlc = ubi_peb_istlc(ubi, pnum);
#endif

	dbg_bld("scan PEB %d", pnum);

	/* Skip bad physical eraseblocks */
	err = ubi_io_is_bad(ubi, pnum);
	if (err < 0)
		return err;
	else if (err) {
		ai->bad_peb_count += 1;
		return 0;
	}
#ifdef CONFIG_MTK_SLC_BUFFER_SUPPORT
	if (istlc && ubi->mtbl != NULL) {
		memset(ech, 0, sizeof(struct ubi_ec_hdr));
		ech->magic = cpu_to_be32(UBI_EC_HDR_MAGIC);
		ech->version = UBI_VERSION;
		ech->vid_hdr_offset = cpu_to_be32(ubi->vid_hdr_offset);
		ech->data_offset = cpu_to_be32(ubi->leb_start);
		ech->image_seq = cpu_to_be32(ubi->image_seq);
		ech->ec = cpu_to_be64((uint64_t)be32_to_cpu(ubi->mtbl->info[pnum].ec));
		crc = crc32(UBI_CRC32_INIT, ech, UBI_EC_HDR_SIZE_CRC);
		ech->hdr_crc = cpu_to_be32(crc);
		map = be32_to_cpu(ubi->mtbl->info[pnum].map);
		mtbl_vol_id = be32_to_cpu(ubi->mtbl->info[pnum].vol_id);
	} else
#endif
	err = ubi_io_read_ec_hdr(ubi, pnum, ech, 0);
	if (err < 0)
		return err;
	switch (err) {
	case 0:
		break;
	case UBI_IO_BITFLIPS:
		bitflips = 1;
		break;
	case UBI_IO_FF:
		ai->empty_peb_count += 1;
		return add_to_list(ubi, ai, pnum, UBI_UNKNOWN, UBI_UNKNOWN,
				   UBI_UNKNOWN, 0, &ai->erase);
	case UBI_IO_FF_BITFLIPS:
		ai->empty_peb_count += 1;
		return add_to_list(ubi, ai, pnum, UBI_UNKNOWN, UBI_UNKNOWN,
				   UBI_UNKNOWN, 1, &ai->erase);
	case UBI_IO_BAD_HDR_EBADMSG:
	case UBI_IO_BAD_HDR:
#ifdef CONFIG_MTK_SLC_BUFFER_SUPPORT
		if (istlc) {
			ai->empty_peb_count += 1;
			return add_to_list(ubi, ai, pnum, UBI_UNKNOWN, UBI_UNKNOWN,
					UBI_UNKNOWN, 1, &ai->erase);
		}
#endif
		/*
		 * We have to also look at the VID header, possibly it is not
		 * corrupted. Set %bitflips flag in order to make this PEB be
		 * moved and EC be re-created.
		 */
		ec_err = err;
		ec = UBI_UNKNOWN;
		bitflips = 1;
		break;
	default:
		ubi_err("'ubi_io_read_ec_hdr()' returned unknown code %d", err);
		return -EINVAL;
	}

	if (!ec_err) {
		int image_seq;

		/* Make sure UBI version is OK */
		if (ech->version != UBI_VERSION) {
			ubi_err("this UBI version is %d, image version is %d",
				UBI_VERSION, (int)ech->version);
			return -EINVAL;
		}

		ec = be64_to_cpu(ech->ec);
		if (ec > UBI_MAX_ERASECOUNTER) {
			/*
			 * Erase counter overflow. The EC headers have 64 bits
			 * reserved, but we anyway make use of only 31 bit
			 * values, as this seems to be enough for any existing
			 * flash. Upgrade UBI and use 64-bit erase counters
			 * internally.
			 */
			ubi_err("erase counter overflow, max is %d",
				UBI_MAX_ERASECOUNTER);
			ubi_dump_ec_hdr(ech);
			return -EINVAL;
		}

		/*
		 * Make sure that all PEBs have the same image sequence number.
		 * This allows us to detect situations when users flash UBI
		 * images incorrectly, so that the flash has the new UBI image
		 * and leftovers from the old one. This feature was added
		 * relatively recently, and the sequence number was always
		 * zero, because old UBI implementations always set it to zero.
		 * For this reasons, we do not panic if some PEBs have zero
		 * sequence number, while other PEBs have non-zero sequence
		 * number.
		 */
		image_seq = be32_to_cpu(ech->image_seq);
		if (!ubi->image_seq)
			ubi->image_seq = image_seq;
		if (image_seq && ubi->image_seq != image_seq) {
			ubi_err("bad image sequence number %d in PEB %d, expected %d",
				image_seq, pnum, ubi->image_seq);
			ubi_dump_ec_hdr(ech);
			return -EINVAL;
		}
	}
#ifdef CONFIG_MTK_SLC_BUFFER_SUPPORT
	if (istlc && ubi->mtbl == NULL) {
		if (ec_err)
			ubi_err("pnum %d ec hdr corrupt(%d) && mtbl is empty\n", pnum, ec_err);
		else
			ubi_change_empty_ec(ubi, pnum, (int)ec, 0, 0);
	}
#endif

	/* OK, we've done with the EC header, let's look at the VID header */

#ifdef CONFIG_MTK_SLC_BUFFER_SUPPORT
	if (istlc && ubi->mtbl != NULL && map == 0)
		err = UBI_IO_FF;
	else
#endif
	err = ubi_io_read_vid_hdr(ubi, pnum, vidh, 0);
	if (err < 0)
		return err;
	switch (err) {
	case 0:
		break;
	case UBI_IO_BITFLIPS:
		bitflips = 1;
		break;
	case UBI_IO_BAD_HDR_EBADMSG:
		if (ec_err == UBI_IO_BAD_HDR_EBADMSG)
			/*
			 * Both EC and VID headers are corrupted and were read
			 * with data integrity error, probably this is a bad
			 * PEB, bit it is not marked as bad yet. This may also
			 * be a result of power cut during erasure.
			 */
			ai->maybe_bad_peb_count += 1;
	case UBI_IO_BAD_HDR:
		if (ec_err)
			/*
			 * Both headers are corrupted. There is a possibility
			 * that this a valid UBI PEB which has corresponding
			 * LEB, but the headers are corrupted. However, it is
			 * impossible to distinguish it from a PEB which just
			 * contains garbage because of a power cut during erase
			 * operation. So we just schedule this PEB for erasure.
			 *
			 * Besides, in case of NOR flash, we deliberately
			 * corrupt both headers because NOR flash erasure is
			 * slow and can start from the end.
			 */
			err = 0;
		else
			/*
			 * The EC was OK, but the VID header is corrupted. We
			 * have to check what is in the data area.
			 */
			err = check_corruption(ubi, vidh, pnum);

		if (err < 0)
			return err;
		else if (!err)
			/* This corruption is caused by a power cut */
#ifdef CONFIG_MTD_UBI_LOWPAGE_BACKUP
			err = add_to_list(ubi, ai, pnum, UBI_UNKNOWN, UBI_UNKNOWN, ec, 1, &ai->waiting);
#else
			err = add_to_list(ubi, ai, pnum, UBI_UNKNOWN, UBI_UNKNOWN, ec, 1, &ai->erase);
#endif
		else
			/* This is an unexpected corruption */
			err = add_corrupted(ai, pnum, ec);
		if (err)
			return err;
		goto adjust_mean_ec;
	case UBI_IO_FF_BITFLIPS:
		err = add_to_list(ubi, ai, pnum, UBI_UNKNOWN, UBI_UNKNOWN,
				  ec, 1, &ai->erase);
		if (err)
			return err;
		goto adjust_mean_ec;
	case UBI_IO_FF:
		if (ec_err || bitflips)
			err = add_to_list(ubi, ai, pnum, UBI_UNKNOWN,
					  UBI_UNKNOWN, ec, 1, &ai->erase);
		else
			err = add_to_list(ubi, ai, pnum, UBI_UNKNOWN,
					  UBI_UNKNOWN, ec, 0, &ai->free);
		if (err)
			return err;
		goto adjust_mean_ec;
	default:
		ubi_err("'ubi_io_read_vid_hdr()' returned unknown code %d",
			err);
		return -EINVAL;
	}

	vol_id = be32_to_cpu(vidh->vol_id);
	if (vid)
		*vid = vol_id;
	if (sqnum)
		*sqnum = be64_to_cpu(vidh->sqnum);
	if (vol_id > UBI_MAX_VOLUMES && vol_id != UBI_LAYOUT_VOLUME_ID) {
		int lnum = be32_to_cpu(vidh->lnum);

		/* Unsupported internal volume */
		switch (vidh->compat) {
		case UBI_COMPAT_DELETE:
			if (vol_id != UBI_FM_SB_VOLUME_ID
			    && vol_id != UBI_FM_DATA_VOLUME_ID) {
				ubi_msg("\"delete\" compatible internal volume %d:%d found, will remove it",
					vol_id, lnum);
			}
			err = add_to_list(ubi, ai, pnum, vol_id, lnum,
					  ec, 1, &ai->erase);
			if (err)
				return err;
			return 0;

		case UBI_COMPAT_RO:
			ubi_msg("read-only compatible internal volume %d:%d found, switch to read-only mode",
				vol_id, lnum);
			ubi->ro_mode = 1;
			break;

		case UBI_COMPAT_PRESERVE:
			ubi_msg("\"preserve\" compatible internal volume %d:%d found",
				vol_id, lnum);
			err = add_to_list(ubi, ai, pnum, vol_id, lnum,
					  ec, 0, &ai->alien);
			if (err)
				return err;
			return 0;

		case UBI_COMPAT_REJECT:
			ubi_err("incompatible internal volume %d:%d found",
				vol_id, lnum);
			return -EINVAL;
		}
	}

	if (ec_err)
		ubi_warn("valid VID header but corrupted EC header at PEB %d",
			 pnum);
#ifdef CONFIG_MTK_SLC_BUFFER_SUPPORT
	if (istlc && ubi->mtbl == NULL)
		ubi_change_empty_ec(ubi, pnum, (int)ec, vol_id, 1);
#endif
	err = ubi_add_to_av(ubi, ai, pnum, ec, vidh, bitflips);
	if (err)
		return err;

adjust_mean_ec:
	if (!ec_err) {
#ifdef CONFIG_MTK_SLC_BUFFER_SUPPORT
		/*int istlc = ubi_peb_istlc(ubi, pnum);*/
		if (istlc) { /* update tlc ec */
			ai->tlc_ec_sum += ec;
			ai->tlc_ec_count += 1;
			if (ec > ai->tlc_max_ec)
				ai->tlc_max_ec = ec;
			if (ec < ai->tlc_min_ec)
				ai->tlc_min_ec = ec;
			return 0;
		}
#endif
		ai->ec_sum += ec;
		ai->ec_count += 1;
		if (ec > ai->max_ec)
			ai->max_ec = ec;
		if (ec < ai->min_ec)
			ai->min_ec = ec;
	}

	return 0;
}

/**
 * late_analysis - analyze the overall situation with PEB.
 * @ubi: UBI device description object
 * @ai: attaching information
 *
 * This is a helper function which takes a look what PEBs we have after we
 * gather information about all of them ("ai" is compete). It decides whether
 * the flash is empty and should be formatted of whether there are too many
 * corrupted PEBs and we should not attach this MTD device. Returns zero if we
 * should proceed with attaching the MTD device, and %-EINVAL if we should not.
 */
static int late_analysis(struct ubi_device *ubi, struct ubi_attach_info *ai)
{
	struct ubi_ainf_peb *aeb;
	int max_corr, peb_count;

	peb_count = ubi->peb_count - ai->bad_peb_count - ai->alien_peb_count;
	max_corr = peb_count / 20 ?: 8;

	/*
	 * Few corrupted PEBs is not a problem and may be just a result of
	 * unclean reboots. However, many of them may indicate some problems
	 * with the flash HW or driver.
	 */
	if (ai->corr_peb_count) {
		ubi_err("%d PEBs are corrupted and preserved",
			ai->corr_peb_count);
		pr_err("Corrupted PEBs are:");
		list_for_each_entry(aeb, &ai->corr, u.list)
			pr_cont(" %d", aeb->pnum);
		pr_cont("\n");

		/*
		 * If too many PEBs are corrupted, we refuse attaching,
		 * otherwise, only print a warning.
		 */
		if (ai->corr_peb_count >= max_corr) {
			ubi_err("too many corrupted PEBs, refusing");
			return -EINVAL;
		}
	}

	if (ai->empty_peb_count + ai->maybe_bad_peb_count == peb_count) {
		/*
		 * All PEBs are empty, or almost all - a couple PEBs look like
		 * they may be bad PEBs which were not marked as bad yet.
		 *
		 * This piece of code basically tries to distinguish between
		 * the following situations:
		 *
		 * 1. Flash is empty, but there are few bad PEBs, which are not
		 *    marked as bad so far, and which were read with error. We
		 *    want to go ahead and format this flash. While formatting,
		 *    the faulty PEBs will probably be marked as bad.
		 *
		 * 2. Flash contains non-UBI data and we do not want to format
		 *    it and destroy possibly important information.
		 */
		if (ai->maybe_bad_peb_count <= 2) {
			ai->is_empty = 1;
			ubi_msg("empty MTD device detected");
			get_random_bytes(&ubi->image_seq,
					 sizeof(ubi->image_seq));
		} else {
			ubi_err("MTD device is not UBI-formatted and possibly contains non-UBI data - refusing it");
			return -EINVAL;
		}

	}

	return 0;
}

/**
 * destroy_av - free volume attaching information.
 * @av: volume attaching information
 * @ai: attaching information
 *
 * This function destroys the volume attaching information.
 */
static void destroy_av(struct ubi_attach_info *ai, struct ubi_ainf_volume *av)
{
	struct ubi_ainf_peb *aeb;
	struct rb_node *this = av->root.rb_node;

	while (this) {
		if (this->rb_left)
			this = this->rb_left;
		else if (this->rb_right)
			this = this->rb_right;
		else {
			aeb = rb_entry(this, struct ubi_ainf_peb, u.rb);
			this = rb_parent(this);
			if (this) {
				if (this->rb_left == &aeb->u.rb)
					this->rb_left = NULL;
				else
					this->rb_right = NULL;
			}

			kmem_cache_free(ai->aeb_slab_cache, aeb);
		}
	}
	kfree(av);
}

/**
 * destroy_ai - destroy attaching information.
 * @ai: attaching information
 */
static void destroy_ai(struct ubi_attach_info *ai)
{
	struct ubi_ainf_peb *aeb, *aeb_tmp;
	struct ubi_ainf_volume *av;
	struct rb_node *rb;

#ifdef CONFIG_MTD_UBI_LOWPAGE_BACKUP
	list_for_each_entry_safe(aeb, aeb_tmp, &ai->waiting, u.list) {
		list_del(&aeb->u.list);
		kmem_cache_free(ai->aeb_slab_cache, aeb);
	}
#endif
	list_for_each_entry_safe(aeb, aeb_tmp, &ai->alien, u.list) {
		list_del(&aeb->u.list);
		kmem_cache_free(ai->aeb_slab_cache, aeb);
	}
	list_for_each_entry_safe(aeb, aeb_tmp, &ai->erase, u.list) {
		list_del(&aeb->u.list);
		kmem_cache_free(ai->aeb_slab_cache, aeb);
	}
	list_for_each_entry_safe(aeb, aeb_tmp, &ai->corr, u.list) {
		list_del(&aeb->u.list);
		kmem_cache_free(ai->aeb_slab_cache, aeb);
	}
	list_for_each_entry_safe(aeb, aeb_tmp, &ai->free, u.list) {
		list_del(&aeb->u.list);
		kmem_cache_free(ai->aeb_slab_cache, aeb);
	}

	/* Destroy the volume RB-tree */
	rb = ai->volumes.rb_node;
	while (rb) {
		if (rb->rb_left)
			rb = rb->rb_left;
		else if (rb->rb_right)
			rb = rb->rb_right;
		else {
			av = rb_entry(rb, struct ubi_ainf_volume, rb);

			rb = rb_parent(rb);
			if (rb) {
				if (rb->rb_left == &av->rb)
					rb->rb_left = NULL;
				else
					rb->rb_right = NULL;
			}

			destroy_av(ai, av);
		}
	}

	if (ai->aeb_slab_cache)
		kmem_cache_destroy(ai->aeb_slab_cache);

	kfree(ai);
}

/**
 * scan_all - scan entire MTD device.
 * @ubi: UBI device description object
 * @ai: attach info object
 * @start: start scanning at this PEB
 *
 * This function does full scanning of an MTD device and returns complete
 * information about it in form of a "struct ubi_attach_info" object. In case
 * of failure, an error code is returned.
 */
static int scan_all(struct ubi_device *ubi, struct ubi_attach_info *ai,
		    int start)
{
	int err, pnum;
	struct rb_node *rb1, *rb2;
	struct ubi_ainf_volume *av;
	struct ubi_ainf_peb *aeb;
#ifdef CONFIG_MTK_SLC_BUFFER_SUPPORT
	int last_tlc_pnum = -1;
#endif

	err = -ENOMEM;

	ech = kzalloc(ubi->ec_hdr_alsize, GFP_KERNEL);
	if (!ech)
		return err;

	vidh = ubi_zalloc_vid_hdr(ubi, GFP_KERNEL);
	if (!vidh)
		goto out_ech;
#ifdef CONFIG_MTK_SLC_BUFFER_SUPPORT
	for (pnum = ubi->peb_count - 1; pnum >= start; pnum--) {
#else
	for (pnum = start; pnum < ubi->peb_count; pnum++) {
#endif
		cond_resched();

		dbg_gen("process PEB %d", pnum);

#ifdef CONFIG_MTK_SLC_BUFFER_SUPPORT
		if (last_tlc_pnum == -1 && ubi_peb_istlc(ubi, pnum) && ubi->mtbl == NULL) {

			last_tlc_pnum = pnum;
			/*slc_number = ubi->peb_count - (pnum + 1);*/
			err = ubi_read_mtbl_record(ubi, ai, pnum + 1);
		}
#endif
		err = scan_peb(ubi, ai, pnum, NULL, NULL);
		if (err < 0)
			goto out_vidh;
	}

	ubi_msg("scanning is finished");

	/* Calculate mean erase counter */
	if (ai->ec_count)
		ai->mean_ec = div_u64(ai->ec_sum, ai->ec_count);
#ifdef CONFIG_MTK_SLC_BUFFER_SUPPORT
	if (ai->tlc_ec_count)
		ai->tlc_mean_ec = div_u64(ai->tlc_ec_sum, ai->tlc_ec_count);
#endif
	err = late_analysis(ubi, ai);
	if (err)
		goto out_vidh;

	/*
	 * In case of unknown erase counter we use the mean erase counter
	 * value.
	 */
	ubi_rb_for_each_entry(rb1, av, &ai->volumes, rb) {
		ubi_rb_for_each_entry(rb2, aeb, &av->root, u.rb)
			if (aeb->ec == UBI_UNKNOWN) {
#ifdef CONFIG_MTK_SLC_BUFFER_SUPPORT
				if (ubi_peb_istlc(ubi, aeb->pnum))
					aeb->ec = ai->tlc_mean_ec;
				else
#endif
					aeb->ec = ai->mean_ec;
			}
	}

	list_for_each_entry(aeb, &ai->free, u.list) {
		if (aeb->ec == UBI_UNKNOWN) {
#ifdef CONFIG_MTK_SLC_BUFFER_SUPPORT
			if (ubi_peb_istlc(ubi, aeb->pnum))
				aeb->ec = ai->tlc_mean_ec;
			else
#endif
				aeb->ec = ai->mean_ec;
		}
	}

	list_for_each_entry(aeb, &ai->corr, u.list)
		if (aeb->ec == UBI_UNKNOWN) {
#ifdef CONFIG_MTK_SLC_BUFFER_SUPPORT
			if (ubi_peb_istlc(ubi, aeb->pnum))
				aeb->ec = ai->tlc_mean_ec;
			else
#endif
				aeb->ec = ai->mean_ec;
		}

	list_for_each_entry(aeb, &ai->erase, u.list)
		if (aeb->ec == UBI_UNKNOWN) {
#ifdef CONFIG_MTK_SLC_BUFFER_SUPPORT
			if (ubi_peb_istlc(ubi, aeb->pnum))
				aeb->ec = ai->tlc_mean_ec;
			else
#endif
				aeb->ec = ai->mean_ec;
		}

	err = self_check_ai(ubi, ai);
	if (err)
		goto out_vidh;

	ubi_free_vid_hdr(ubi, vidh);
	kfree(ech);

	return 0;

out_vidh:
	ubi_free_vid_hdr(ubi, vidh);
out_ech:
	kfree(ech);
	return err;
}

#ifdef CONFIG_MTD_UBI_FASTMAP

/**
 * scan_fastmap - try to find a fastmap and attach from it.
 * @ubi: UBI device description object
 * @ai: attach info object
 *
 * Returns 0 on success, negative return values indicate an internal
 * error.
 * UBI_NO_FASTMAP denotes that no fastmap was found.
 * UBI_BAD_FASTMAP denotes that the found fastmap was invalid.
 */
static int scan_fast(struct ubi_device *ubi, struct ubi_attach_info *ai)
{
	int err, pnum, fm_anchor = -1;
	unsigned long long max_sqnum = 0;

	err = -ENOMEM;

	ech = kzalloc(ubi->ec_hdr_alsize, GFP_KERNEL);
	if (!ech)
		goto out;

	vidh = ubi_zalloc_vid_hdr(ubi, GFP_KERNEL);
	if (!vidh)
		goto out_ech;

	for (pnum = 0; pnum < UBI_FM_MAX_START; pnum++) {
		int vol_id = -1;
		unsigned long long sqnum = -1;

		cond_resched();

		dbg_gen("process PEB %d", pnum);
		err = scan_peb(ubi, ai, pnum, &vol_id, &sqnum);
		if (err < 0)
			goto out_vidh;

		if (vol_id == UBI_FM_SB_VOLUME_ID && sqnum > max_sqnum) {
			max_sqnum = sqnum;
			fm_anchor = pnum;
		}
	}

	ubi_free_vid_hdr(ubi, vidh);
	kfree(ech);

	if (fm_anchor < 0)
		return UBI_NO_FASTMAP;

	return ubi_scan_fastmap(ubi, ai, fm_anchor);

out_vidh:
	ubi_free_vid_hdr(ubi, vidh);
out_ech:
	kfree(ech);
out:
	return err;
}

#endif

static struct ubi_attach_info *alloc_ai(const char *slab_name)
{
	struct ubi_attach_info *ai;

	ai = kzalloc(sizeof(struct ubi_attach_info), GFP_KERNEL);
	if (!ai)
		return ai;

	INIT_LIST_HEAD(&ai->corr);
	INIT_LIST_HEAD(&ai->free);
	INIT_LIST_HEAD(&ai->erase);
	INIT_LIST_HEAD(&ai->alien);
#ifdef CONFIG_MTD_UBI_LOWPAGE_BACKUP
	INIT_LIST_HEAD(&ai->waiting);
#endif
	ai->volumes = RB_ROOT;
	ai->aeb_slab_cache = kmem_cache_create(slab_name,
					       sizeof(struct ubi_ainf_peb),
					       0, 0, NULL);
	if (!ai->aeb_slab_cache) {
		kfree(ai);
		ai = NULL;
	}

	return ai;
}

/**
 * ubi_attach - attach an MTD device.
 * @ubi: UBI device descriptor
 * @force_scan: if set to non-zero attach by scanning
 *
 * This function returns zero in case of success and a negative error code in
 * case of failure.
 */
int ubi_attach(struct ubi_device *ubi, int force_scan)
{
	int err;
	struct ubi_attach_info *ai;
	unsigned long long time = sched_clock();

	ai = alloc_ai("ubi_aeb_slab_cache");
	if (!ai)
		return -ENOMEM;

#ifdef CONFIG_MTD_UBI_FASTMAP
	/* On small flash devices we disable fastmap in any case. */
	if ((int)mtd_div_by_eb(ubi->mtd->size, ubi->mtd) <= UBI_FM_MAX_START) {
		ubi->fm_disabled = 1;
		force_scan = 1;
	}

	if (force_scan)
		err = scan_all(ubi, ai, 0);
	else {
		err = scan_fast(ubi, ai);
		if (err > 0) {
			if (err != UBI_NO_FASTMAP) {
				destroy_ai(ai);
				ai = alloc_ai("ubi_aeb_slab_cache2");
				if (!ai)
					return -ENOMEM;

				err = scan_all(ubi, ai, 0);
			} else {
				err = scan_all(ubi, ai, UBI_FM_MAX_START);
			}
		}
	}
#else
	err = scan_all(ubi, ai, 0);
#endif
	time = sched_clock() - time;
	do_div(time, 1000000);
	ubi_msg("scan done in %lld(ms)\n", time);

	if (err)
		goto out_ai;

	ubi->bad_peb_count = ai->bad_peb_count;
	ubi->good_peb_count = ubi->peb_count - ubi->bad_peb_count;
	ubi->corr_peb_count = ai->corr_peb_count;
	ubi->max_ec = ai->max_ec;
	ubi->mean_ec = ai->mean_ec;
#ifdef CONFIG_MTK_SLC_BUFFER_SUPPORT
	/*ubi->tlc_ec_count = ai->tlc_ec_count;*/
	ubi->tlc_max_ec = ai->tlc_max_ec;
	ubi->tlc_mean_ec = ai->tlc_mean_ec;
	ubi->tlc_ec_sum = ai->tlc_ec_sum + ubi->tlc_mean_ec * (ubi->mtbl_slots - ai->tlc_ec_count); /*tlc for wl*/
	 /*MTK: calc ec_sum*/
	ubi->ec_sum = ai->ec_sum + ubi->mean_ec * (ubi->good_peb_count - ubi->mtbl_slots - ai->ec_count);
#else
	ubi->ec_sum = ai->ec_sum + ubi->mean_ec * (ubi->good_peb_count - ai->ec_count);	/*MTK: calc ec_sum */
#endif
	dbg_gen("max. sequence number:       %llu", ai->max_sqnum);

#ifdef CONFIG_MTD_UBI_LOWPAGE_BACKUP
	ubi->scanning = 1;
	err = ubi_backup_init_scan(ubi, ai);
	if (err)
		goto out_ai;
	ubi->scanning = 0;
#endif
	err = ubi_read_volume_table(ubi, ai);
	if (err)
		goto out_ai;

	time = sched_clock();
	err = ubi_wl_init(ubi, ai);
	if (err)
		goto out_vtbl;
	time = sched_clock() - time;
	do_div(time, 1000000);
	ubi_msg("ubi_wl_init_scan done in %lld(ms)\n", time);

	err = ubi_eba_init(ubi, ai);
	if (err)
		goto out_wl;

#ifdef CONFIG_MTD_UBI_FASTMAP
	if (ubi->fm && ubi_dbg_chk_gen(ubi)) {
		struct ubi_attach_info *scan_ai;

		scan_ai = alloc_ai("ubi_ckh_aeb_slab_cache");
		if (!scan_ai) {
			err = -ENOMEM;
			goto out_wl;
		}

		err = scan_all(ubi, scan_ai, 0);
		if (err) {
			destroy_ai(scan_ai);
			goto out_wl;
		}

		err = self_check_eba(ubi, ai, scan_ai);
		destroy_ai(scan_ai);

		if (err)
			goto out_wl;
	}
#endif

	destroy_ai(ai);
	return 0;

out_wl:
	ubi_wl_close(ubi);
out_vtbl:
	ubi_free_internal_volumes(ubi);
	vfree(ubi->vtbl);
out_ai:
	destroy_ai(ai);
	return err;
}

/**
 * self_check_ai - check the attaching information.
 * @ubi: UBI device description object
 * @ai: attaching information
 *
 * This function returns zero if the attaching information is all right, and a
 * negative error code if not or if an error occurred.
 */
static int self_check_ai(struct ubi_device *ubi, struct ubi_attach_info *ai)
{
	int pnum, err, vols_found = 0;
	struct rb_node *rb1, *rb2;
	struct ubi_ainf_volume *av;
	struct ubi_ainf_peb *aeb, *last_aeb;
	uint8_t *buf;
	int min_ec, max_ec;

	if (!ubi_dbg_chk_gen(ubi))
		return 0;

	/*
	 * At first, check that attaching information is OK.
	 */
	ubi_rb_for_each_entry(rb1, av, &ai->volumes, rb) {
		int leb_count = 0;

		cond_resched();

		vols_found += 1;

		if (ai->is_empty) {
			ubi_err("bad is_empty flag");
			goto bad_av;
		}

		if (av->vol_id < 0 || av->highest_lnum < 0 ||
		    av->leb_count < 0 || av->vol_type < 0 || av->used_ebs < 0 ||
		    av->data_pad < 0 || av->last_data_size < 0) {
			ubi_err("negative values");
			goto bad_av;
		}

		if (av->vol_id >= UBI_MAX_VOLUMES &&
		    av->vol_id < UBI_INTERNAL_VOL_START) {
			ubi_err("bad vol_id");
			goto bad_av;
		}

		if (av->vol_id > ai->highest_vol_id) {
			ubi_err("highest_vol_id is %d, but vol_id %d is there",
				ai->highest_vol_id, av->vol_id);
			goto out;
		}

		if (av->vol_type != UBI_DYNAMIC_VOLUME &&
		    av->vol_type != UBI_STATIC_VOLUME) {
			ubi_err("bad vol_type");
			goto bad_av;
		}

		if (av->data_pad > ubi->leb_size / 2) {
			ubi_err("bad data_pad");
			goto bad_av;
		}

		last_aeb = NULL;
		ubi_rb_for_each_entry(rb2, aeb, &av->root, u.rb) {
			cond_resched();

			last_aeb = aeb;
			leb_count += 1;
#ifdef CONFIG_MTK_SLC_BUFFER_SUPPORT
			if (ubi_peb_istlc(ubi, aeb->pnum)) {
				min_ec = ai->tlc_min_ec;
				max_ec = ai->tlc_max_ec;
			} else
#endif
			{
				min_ec = ai->min_ec;
				max_ec = ai->max_ec;
			}

			if (aeb->pnum < 0 || aeb->ec < 0) {
				ubi_err("negative values");
				goto bad_aeb;
			}
			if (aeb->ec < min_ec) {
				ubi_err("bad ai->min_ec (%d), %d found",
					ai->min_ec, aeb->ec);
				goto bad_aeb;
			}
			if (aeb->ec > max_ec) {
				ubi_err("bad ai->max_ec (%d), %d found",
					ai->max_ec, aeb->ec);
				goto bad_aeb;
			}

			if (aeb->pnum >= ubi->peb_count) {
				ubi_err("too high PEB number %d, total PEBs %d",
					aeb->pnum, ubi->peb_count);
				goto bad_aeb;
			}

			if (av->vol_type == UBI_STATIC_VOLUME) {
				if (aeb->lnum >= av->used_ebs) {
					ubi_err("bad lnum or used_ebs");
					goto bad_aeb;
				}
			} else {
				if (av->used_ebs != 0) {
					ubi_err("non-zero used_ebs");
					goto bad_aeb;
				}
			}

			if (aeb->lnum > av->highest_lnum) {
				ubi_err("incorrect highest_lnum or lnum");
				goto bad_aeb;
			}
		}

		if (av->leb_count != leb_count) {
			ubi_err("bad leb_count, %d objects in the tree",
				leb_count);
			goto bad_av;
		}

		if (!last_aeb)
			continue;

		aeb = last_aeb;

		if (aeb->lnum != av->highest_lnum) {
			ubi_err("bad highest_lnum");
			goto bad_aeb;
		}
	}

	if (vols_found != ai->vols_found) {
		ubi_err("bad ai->vols_found %d, should be %d",
			ai->vols_found, vols_found);
		goto out;
	}

	/* Check that attaching information is correct */
	ubi_rb_for_each_entry(rb1, av, &ai->volumes, rb) {
		last_aeb = NULL;
		ubi_rb_for_each_entry(rb2, aeb, &av->root, u.rb) {
			int vol_type;

			cond_resched();

			last_aeb = aeb;

			err = ubi_io_read_vid_hdr(ubi, aeb->pnum, vidh, 1);
			if (err && err != UBI_IO_BITFLIPS) {
				ubi_err("VID header is not OK (%d)", err);
				if (err > 0)
					err = -EIO;
				return err;
			}

			vol_type = vidh->vol_type == UBI_VID_DYNAMIC ?
				   UBI_DYNAMIC_VOLUME : UBI_STATIC_VOLUME;
			if (av->vol_type != vol_type) {
				ubi_err("bad vol_type");
				goto bad_vid_hdr;
			}

			if (aeb->sqnum != be64_to_cpu(vidh->sqnum)) {
				ubi_err("bad sqnum %llu", aeb->sqnum);
				goto bad_vid_hdr;
			}

			if (av->vol_id != be32_to_cpu(vidh->vol_id)) {
				ubi_err("bad vol_id %d", av->vol_id);
				goto bad_vid_hdr;
			}

			if (av->compat != vidh->compat) {
				ubi_err("bad compat %d", vidh->compat);
				goto bad_vid_hdr;
			}

			if (aeb->lnum != be32_to_cpu(vidh->lnum)) {
				ubi_err("bad lnum %d", aeb->lnum);
				goto bad_vid_hdr;
			}

			if (av->used_ebs != be32_to_cpu(vidh->used_ebs)) {
				ubi_err("bad used_ebs %d", av->used_ebs);
				goto bad_vid_hdr;
			}

			if (av->data_pad != be32_to_cpu(vidh->data_pad)) {
				ubi_err("bad data_pad %d", av->data_pad);
				goto bad_vid_hdr;
			}
		}

		if (!last_aeb)
			continue;

		if (av->highest_lnum != be32_to_cpu(vidh->lnum)) {
			ubi_err("bad highest_lnum %d", av->highest_lnum);
			goto bad_vid_hdr;
		}

		if (av->last_data_size != be32_to_cpu(vidh->data_size)) {
			ubi_err("bad last_data_size %d", av->last_data_size);
			goto bad_vid_hdr;
		}
	}

	/*
	 * Make sure that all the physical eraseblocks are in one of the lists
	 * or trees.
	 */
	buf = kzalloc(ubi->peb_count, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	for (pnum = 0; pnum < ubi->peb_count; pnum++) {
		err = ubi_io_is_bad(ubi, pnum);
		if (err < 0) {
			kfree(buf);
			return err;
		} else if (err)
			buf[pnum] = 1;
	}

	ubi_rb_for_each_entry(rb1, av, &ai->volumes, rb)
		ubi_rb_for_each_entry(rb2, aeb, &av->root, u.rb)
			buf[aeb->pnum] = 1;

	list_for_each_entry(aeb, &ai->free, u.list)
		buf[aeb->pnum] = 1;

	list_for_each_entry(aeb, &ai->corr, u.list)
		buf[aeb->pnum] = 1;

	list_for_each_entry(aeb, &ai->erase, u.list)
		buf[aeb->pnum] = 1;

	list_for_each_entry(aeb, &ai->alien, u.list)
		buf[aeb->pnum] = 1;

	err = 0;
	for (pnum = 0; pnum < ubi->peb_count; pnum++)
		if (!buf[pnum]) {
			ubi_err("PEB %d is not referred", pnum);
			err = 1;
		}

	kfree(buf);
	if (err)
		goto out;
	return 0;

bad_aeb:
	ubi_err("bad attaching information about LEB %d", aeb->lnum);
	ubi_dump_aeb(aeb, 0);
	ubi_dump_av(av);
	goto out;

bad_av:
	ubi_err("bad attaching information about volume %d", av->vol_id);
	ubi_dump_av(av);
	goto out;

bad_vid_hdr:
	ubi_err("bad attaching information about volume %d", av->vol_id);
	ubi_dump_av(av);
	ubi_dump_vid_hdr(vidh);

out:
	dump_stack();
	return -EINVAL;
}

#ifdef CONFIG_MTD_UBI_LOWPAGE_BACKUP
/**
 * check_pattern - check if buffer contains only a certain byte pattern.
 * @buf: buffer to check
 * @patt: the pattern to check
 * @size: buffer size in bytes
 *
 * This function returns %1 in there are only @patt bytes in @buf, and %0 if
 * something else was also found.
 */
enum {
	RECOVERY_NONE = 0,
	RECOVERY_FROM_VOLUME,
	RECOVERY_FROM_CORR
};

static int check_pattern(const void *buf, uint8_t patt, int size)
{
	int i;

	for (i = 0; i < size; i++)
		if (((const uint8_t *)buf)[i] != patt)
			return 0;
	return 1;
}

/**
 * ubi_backup_search_empty - search first empty page in the block.
 * @ubi: ubi structure
 * @pnum: the pnum to search
 *
 * This function returns offset of first empty page in the block.
 */
static int ubi_backup_search_empty(const struct ubi_device *ubi, int pnum)
{
	int low, high, mid;
	int first = ubi->peb_size;
	int offset, err = 0;

	low = blb_get_startpage();
	high = ubi->peb_size / ubi->mtd->writesize - 1;
	while (low <= high) {
		mid = (low + high) / 2;
		offset = mid * ubi->mtd->writesize;
		err = ubi_io_read_oob(ubi, ubi->databuf, ubi->oobbuf, pnum, offset);
		if (err == 0 && check_pattern(ubi->oobbuf, 0xFF, ubi->mtd->oobavail)
		    && check_pattern(ubi->databuf, 0xFF, ubi->mtd->writesize)) {
			first = offset;
			high = mid - 1;
		} else {
			low = mid + 1;
		}
	}
	return first;
}

int blb_recovery_peb(struct ubi_device *ubi, struct ubi_attach_info *ai,
		     struct ubi_blb_spare *p_blb_spare, int pnum, int num,
		     int backup_pnum, struct ubi_ainf_peb *cad_peb)
{
	struct ubi_ainf_volume *av;
	int i, err, data_size, offset, tries = 0;
	struct ubi_ainf_peb *old_seb, *new_seb = NULL;
	struct rb_node *rb;
	int recovery = RECOVERY_NONE;
	int source_vol_id, source_lnum, source_pnum, source_page;
	uint32_t crc;
	struct ubi_vid_hdr *vid_hdr = NULL;

	source_page = be16_to_cpu(p_blb_spare->page);
	source_vol_id = be32_to_cpu(p_blb_spare->vol_id);
	source_pnum = be16_to_cpu(p_blb_spare->pnum);
	source_lnum = be16_to_cpu(p_blb_spare->lnum);

	av = ubi_find_av(ai, source_vol_id);
	if (!av) {
		ubi_msg("volume id %d was not found", source_vol_id);
		err = -EINVAL;
		goto out_free;
	}

	/* check from volume */
	ubi_rb_for_each_entry(rb, old_seb, &av->root, u.rb)
		if (old_seb->pnum == source_pnum && old_seb->lnum == source_lnum) {
			recovery = RECOVERY_FROM_VOLUME;
			goto recovery;
		}

	list_for_each_entry(old_seb, &ai->corr, u.list)
		if (old_seb->pnum == source_pnum) {
			recovery = RECOVERY_FROM_CORR;
			list_del(&old_seb->u.list);
			goto recovery;
		}
	list_for_each_entry(old_seb, &ai->waiting, u.list)
		if (old_seb->pnum == source_pnum) {
			recovery = RECOVERY_FROM_CORR;
			list_del(&old_seb->u.list);
			goto recovery;
		}

	list_for_each_entry(old_seb, &ai->free, u.list)
		if (old_seb->pnum == source_pnum) {
			list_del(&old_seb->u.list);
			ubi_msg("add corrept peb %d, ec %d from free to erase list", old_seb->pnum,
				old_seb->ec);
			err =
			add_to_list(ubi, ai, old_seb->pnum, old_seb->vol_id, old_seb->lnum, old_seb->ec, 1,
					&ai->erase);
			if (err)
				return err;
			kmem_cache_free(ai->aeb_slab_cache, old_seb);
			break;
		}

	list_for_each_entry(old_seb, &ai->alien, u.list)
		if (old_seb->pnum == source_pnum) {
			list_del(&old_seb->u.list);
			ubi_msg("add corrept peb %d, ec %d from alien to erase list", old_seb->pnum,
				old_seb->ec);
			err =
				add_to_list(ubi, ai, old_seb->pnum, old_seb->vol_id, old_seb->lnum, old_seb->ec, 1,
					&ai->erase);
			if (err)
				return err;
			kmem_cache_free(ai->aeb_slab_cache, old_seb);
			break;
		}
	if (cad_peb != NULL)
		kmem_cache_free(ai->aeb_slab_cache, cad_peb);
	return 0;

recovery:
	ubi_msg("recovery from %d", recovery);
	data_size = ubi->leb_size - be32_to_cpu(av->data_pad);
#ifdef CONFIG_UBI_SHARE_BUFFER
	mutex_lock(&ubi_buf_mutex);
#else
	mutex_lock(&ubi->buf_mutex);
#endif
	for (offset = 0; offset < data_size; offset += ubi->mtd->writesize) {
		/* ubi_msg("read source(%d) from %d, %d bytes", old_seb->pnum, offset, ubi->mtd->writesize); */
		err = ubi_io_read_data(ubi, (void *)(((char *)ubi->peb_buf) + offset),
				       old_seb->pnum, offset, ubi->mtd->writesize);
		if (err < 0)
			ubi_warn("error %d while reading data from PEB %d:0x%x", err, old_seb->pnum,
				 offset);
	}

	for (i = 0; i < num; i++) {
		ubi_msg("read backup(%d) from %d", pnum,
			ubi->next_offset[0] - (i + 1) * ubi->mtd->writesize);
		err =
		    ubi_io_read_oob(ubi, ubi->databuf, ubi->oobbuf, pnum,
				    ubi->next_offset[0] - (i + 1) * ubi->mtd->writesize);
		source_page = be16_to_cpu(p_blb_spare->page);
		if (source_page >= ubi->leb_start / ubi->mtd->writesize) {
			ubi_msg("copy backup page %d to offset 0x%x", source_page,
				(source_page * ubi->mtd->writesize) - ubi->leb_start);
			memcpy((void *)(((char *)ubi->peb_buf) +
					(source_page * ubi->mtd->writesize) - ubi->leb_start),
			       (const void *)ubi->databuf, ubi->mtd->writesize);
		}
	}

	data_size = ubi_calc_data_len(ubi, (char *)ubi->peb_buf, data_size);
	ubi_msg("calc CRC data size %d", data_size);
	crc = crc32(UBI_CRC32_INIT, (char *)ubi->peb_buf, data_size);

	vid_hdr = ubi_zalloc_vid_hdr(ubi, GFP_KERNEL);
	if (!vid_hdr) {
		err = -ENOMEM;
		goto out_free;
	}

	vid_hdr->sqnum = cpu_to_be64(++ai->max_sqnum);
	vid_hdr->vol_id = cpu_to_be32(source_vol_id);
	vid_hdr->lnum = cpu_to_be32(source_lnum);
	vid_hdr->compat = ubi_get_compat(ubi, source_vol_id);
	vid_hdr->data_pad = cpu_to_be32(av->data_pad);
	vid_hdr->used_ebs = 0;
	if (av->used_ebs != 0)
		ubi_msg("bad used_ebs 0x%x", av->used_ebs);

	vid_hdr->vol_type = UBI_VID_DYNAMIC;
	if (data_size > 0) {
		vid_hdr->copy_flag = 1;
		vid_hdr->data_size = cpu_to_be32(data_size);
		vid_hdr->data_crc = cpu_to_be32(crc);
	}

retry:
	if (tries == 0 && cad_peb != NULL) {
		new_seb = cad_peb;
	} else {
		new_seb = ubi_early_get_peb(ubi, ai);
		if (IS_ERR(new_seb)) {
			err = -EINVAL;
			goto out_free;
		}

		if (backup_pnum == UBI_LEB_UNMAPPED) {
			ubi_warn("no leb 1 for backup page 1 of recovery PEB");
		} else if ((ubi->peb_size - ubi->next_offset[1]) < ubi->mtd->writesize) {
			ubi_warn("no space to backup page 1 of recovery PEB");
		} else {
			struct ubi_blb_spare *blb_spare = (struct ubi_blb_spare *)ubi->oobbuf;

			blb_spare->num = cpu_to_be16(1);
			blb_spare->pnum = cpu_to_be16(new_seb->pnum);
			blb_spare->lnum = cpu_to_be16(source_lnum);
			blb_spare->vol_id = cpu_to_be32(source_vol_id);
			blb_spare->page = cpu_to_be16(1);
			blb_spare->sqnum = cpu_to_be64(++ai->max_sqnum);
			crc = crc32(UBI_CRC32_INIT, blb_spare, sizeof(struct ubi_blb_spare) - 4);
			blb_spare->crc = cpu_to_be32(crc);

			sprintf(ubi->databuf, "VIDVIDVID");
			err =
			    ubi_io_write_oob(ubi, ubi->databuf, ubi->oobbuf, backup_pnum,
					     ubi->next_offset[1]);
			if (err)
				ubi_err("ERROR: write backup page 1 of recovery PEB fail");
			else
				ubi_msg("backup[1] %d:%d to %d:%d, num %d", new_seb->pnum, 1,
					backup_pnum, ubi->next_offset[1] / ubi->mtd->writesize, 1);

			ubi->next_offset[1] += ubi->mtd->writesize;
		}
	}
	ubi_msg("using peb %d to recovery", new_seb->pnum);
	err = ubi_io_write_vid_hdr(ubi, new_seb->pnum, vid_hdr);
	if (err)
		goto write_error;

	if (data_size > 0) {
		err = ubi_io_write_data(ubi, ubi->peb_buf, new_seb->pnum, 0, data_size);
		if (err)
			goto write_error;
	}

	err =
	    add_to_list(ubi, ai, old_seb->pnum, old_seb->vol_id, old_seb->lnum, old_seb->ec, 1,
			&ai->erase);
	if (err)
		goto out_free;

	if (recovery == RECOVERY_FROM_VOLUME) {
		old_seb->pnum = new_seb->pnum;
		old_seb->ec = new_seb->ec;
		old_seb->sqnum = vid_hdr->sqnum;
	} else {
		err = ubi_add_to_av(ubi, ai, new_seb->pnum, new_seb->ec, vid_hdr, 0);
		if (err)
			goto out_free;
	}
	kmem_cache_free(ai->aeb_slab_cache, new_seb);
	ubi_free_vid_hdr(ubi, vid_hdr);

#ifdef CONFIG_UBI_SHARE_BUFFER
	mutex_unlock(&ubi_buf_mutex);
#else
	mutex_unlock(&ubi->buf_mutex);
#endif
	return 0;

write_error:
	if (err != -EIO || !ubi->bad_allowed) {
		ubi_ro_mode(ubi);
		kmem_cache_free(ai->aeb_slab_cache, new_seb);
		goto out_free;
	}

	err = add_to_list(ubi, ai, new_seb->pnum, new_seb->vol_id, new_seb->lnum, new_seb->pnum, 1,
			  &ai->corr);
	kmem_cache_free(ai->aeb_slab_cache, new_seb);
	if (err || ++tries > UBI_IO_RETRIES) {
		ubi_ro_mode(ubi);
		goto out_free;
	}

	vid_hdr->sqnum = cpu_to_be64(++ai->max_sqnum);
	ubi_msg("try another PEB");
	goto retry;

out_free:
	if (vid_hdr)
		ubi_free_vid_hdr(ubi, vid_hdr);
#ifdef CONFIG_UBI_SHARE_BUFFER
	mutex_unlock(&ubi_buf_mutex);
#else
	mutex_unlock(&ubi->buf_mutex);
#endif
	return err;
}

int ubi_backup_init_scan(struct ubi_device *ubi, struct ubi_attach_info *ai)
{
	int i, j, err = 0;
	struct ubi_vid_hdr *vid_hdr = NULL;
	struct ubi_ainf_volume *av;
	struct ubi_ainf_peb *seb, *backup_seb[2], *old_seb = NULL;/* , *new_seb; */
	struct rb_node *rb;
	struct ubi_blb_spare *p_blb_spare;
	int pnum = 0;
	int page_cnt;
	int source_pnum = 0, source_lnum = 0, source_vol_id = 0, source_page = 0, num = 0;
	int corrupt;		/* , recovery, tries = 0; */
	/* int data_size; */
	uint32_t crc;
	struct ubi_ainf_peb *seb_tmp;
	struct ubi_ainf_peb *candidate_peb = NULL;
	int high_page;

	page_cnt = (1 << (ubi->mtd->erasesize_shift - ubi->mtd->writesize_shift));

	ubi->databuf = vmalloc(ubi->mtd->writesize);
	ubi->oobbuf = vmalloc(ubi->mtd->oobavail);
	if (!ubi->databuf || !ubi->oobbuf) {
		err = -ENOMEM;
		goto out_free;
	}
	ubi->leb_scrub[0] = 0;
	ubi->leb_scrub[1] = 0;
	ubi->next_offset[0] = 0;
	ubi->next_offset[1] = 0;
	backup_seb[0] = NULL;
	backup_seb[1] = NULL;
	mutex_init(&ubi->blb_mutex);

	av = ubi_find_av(ai, UBI_BACKUP_VOLUME_ID);
	if (!av) {
		ubi_msg("blb the backup volume was not found");
		return 0;
	}

	ubi_msg("blb check backup volume(0x%x):%d", UBI_BACKUP_VOLUME_ID, av->vol_id);

	p_blb_spare = (struct ubi_blb_spare *)ubi->oobbuf;
	/* Get two PEBs of backup volume */
	ubi_rb_for_each_entry(rb, seb, &av->root, u.rb) {
		int lnum = seb->lnum;

		ubi_assert(lnum < 2);
		backup_seb[lnum] = seb;
		ubi->next_offset[lnum] = ubi_backup_search_empty(ubi, seb->pnum);
	}
	/* check sqnum */
	if (backup_seb[0] != NULL && backup_seb[1] != NULL) {
		int peb0 = -1, peb1 = -1;
		unsigned long long sqnum0 = 0, sqnum1 = 0;

		pnum = backup_seb[0]->pnum;
		ubi_msg("blb block %d, pnum %d next offset 0x%x(page %d)", 0, pnum,
			ubi->next_offset[0], ubi->next_offset[0] / ubi->mtd->writesize);
		err =
		    ubi_io_read_oob(ubi, NULL, ubi->oobbuf, pnum,
				    ubi->next_offset[0] - ubi->mtd->writesize);
		if (err < 0) {
			ubi_msg("blb this page of LEB0 was scrubbed or WL");
			backup_seb[0] = NULL;
		} else {
			crc = crc32(UBI_CRC32_INIT, p_blb_spare, sizeof(struct ubi_blb_spare) - 4);
			if (crc != be32_to_cpu(p_blb_spare->crc)) {
				ubi_msg("blb this page of LEB0 crc error");
				backup_seb[0] = NULL;
			} else {
				peb0 = be16_to_cpu(p_blb_spare->pnum);
				sqnum0 = be64_to_cpu(p_blb_spare->sqnum);
				if (ai->max_sqnum < sqnum0)
					ai->max_sqnum = sqnum0;
			}
		}

		pnum = backup_seb[1]->pnum;
		ubi_msg("blb block %d, pnum %d next offset 0x%x(page %d)", 1, pnum,
			ubi->next_offset[1], ubi->next_offset[1] / ubi->mtd->writesize);
		err =
		    ubi_io_read_oob(ubi, NULL, ubi->oobbuf, pnum,
				    ubi->next_offset[1] - ubi->mtd->writesize);
		if (err < 0) {
			ubi_msg("blb this page of LEB1 was scrubbed or WL");
			backup_seb[1] = NULL;
		} else {
			crc = crc32(UBI_CRC32_INIT, p_blb_spare, sizeof(struct ubi_blb_spare) - 4);
			if (crc != be32_to_cpu(p_blb_spare->crc)) {
				ubi_msg("blb this page of LEB0 crc error");
				backup_seb[1] = NULL;
			} else {
				peb1 = be16_to_cpu(p_blb_spare->pnum);
				sqnum1 = be64_to_cpu(p_blb_spare->sqnum);
				if (ai->max_sqnum < sqnum1)
					ai->max_sqnum = sqnum1;
			}
		}

		ubi_msg("sqnum0  %llu , sqnum1 %llu", sqnum0, sqnum1);
		if (peb0 == peb1 && peb0 != -1) {
			ubi_msg("blb two record have the same peb %d", peb0);
			if (sqnum1 > sqnum0) {
				ubi_msg("blb LEB1 is new %d", peb0);
				backup_seb[0] = NULL;
			} else {
				ubi_msg("blb LEB0 is new %d", peb0);
				backup_seb[1] = NULL;
			}
		}
	}

	for (j = 1; j >= 0; j--) {
		if (backup_seb[j] == NULL)
			continue;

		pnum = backup_seb[j]->pnum;
		ubi_msg("blb block %d, pnum %d next offset 0x%x(page %d)", j, pnum,
			ubi->next_offset[j], ubi->next_offset[j] / ubi->mtd->writesize);
		err =
		    ubi_io_read_oob(ubi, ubi->databuf, ubi->oobbuf, pnum,
				    ubi->next_offset[j] - ubi->mtd->writesize);
		if (err >= 0) {
			source_page = be16_to_cpu(p_blb_spare->page);
			num = be16_to_cpu(p_blb_spare->num);
			source_vol_id = be32_to_cpu(p_blb_spare->vol_id);
			source_pnum = be16_to_cpu(p_blb_spare->pnum);
			source_lnum = be16_to_cpu(p_blb_spare->lnum);
			crc = crc32(UBI_CRC32_INIT, p_blb_spare, sizeof(struct ubi_blb_spare) - 4);
			if (crc != be32_to_cpu(p_blb_spare->crc)) {
				ubi_msg("blb this page crc error");
				continue;
			} else {
				ubi_msg("blb this page crc match");
			}
		} else {
			ubi_msg("blb this page was scrubbed or WL");
			ubi->leb_scrub[j] = 1;
			continue;
		}

		ubi_msg("blb Spare Strut page: %X, num: %X, vol_id: %X, pnum: %X, lnum: %X",
			p_blb_spare->page, p_blb_spare->num, p_blb_spare->vol_id,
			p_blb_spare->pnum, p_blb_spare->lnum);

		ubi_msg("blb backup @pnum %d, offset %d", pnum, ubi->next_offset[j]);
		ubi_msg("blb backup source @pnum %d, lnum %d, vol_id %d, page %d, sq %d",
			source_pnum, source_lnum, source_vol_id, source_page, num);

		if (p_blb_spare->page == 0xFFFF && p_blb_spare->num == 0xFFFF &&
		    p_blb_spare->vol_id == 0xFFFFFFFF && p_blb_spare->pnum == 0xFFFF &&
		    p_blb_spare->lnum == 0xFFFF) {

			ubi_msg("blb the backup volume was scrubbed or WL, no need to restore");
			continue;
		}
		/* Check if source page corrupts, and recover */
		corrupt = 0;
		for (i = 0; i < num; i++) {
			/* read backup page */
			ubi_msg("blb check backup @pnum %d, offset 0x%x", pnum,
				ubi->next_offset[j] - (i + 1) * ubi->mtd->writesize);
			if (i > 0) {
				err = ubi_io_read_oob(ubi, ubi->databuf, ubi->oobbuf, pnum,
						    ubi->next_offset[j] - (i +
									   1) *
						    ubi->mtd->writesize);
				if (err < 0) {
					corrupt = 0;
					ubi_msg("blb this page was scrubbed or WL");
					ubi->leb_scrub[j] = 1;
					break;
				}
				source_page = be16_to_cpu(p_blb_spare->page);
				source_vol_id = be32_to_cpu(p_blb_spare->vol_id);
				source_pnum = be16_to_cpu(p_blb_spare->pnum);
				source_lnum = be16_to_cpu(p_blb_spare->lnum);
			}

			if (source_page == 1) {
				char *buf = ubi->databuf;

				ubi_msg("databuf %c%c%c%c%c%c%c%c%c", buf[0], buf[1], buf[2],
					buf[3], buf[4], buf[5], buf[6], buf[7], buf[8]);
				if (strncmp("VIDVIDVID", ubi->databuf, 9) == 0) {
					int check_page = 2;

					if (source_vol_id == UBI_BACKUP_VOLUME_ID)
						check_page = blb_get_startpage();
					ubi_msg("vid special case, checking page %d", check_page);
					err = ubi_io_read_oob(ubi, ubi->databuf, NULL, source_pnum,
							    check_page * ubi->mtd->writesize);
					if (err)
						continue;
					err = ubi_check_pattern(ubi->databuf, 0xFF,
							      ubi->mtd->writesize);
					if (err == 1) {
						ubi_msg("Page 2(%d) are all 0xFF", source_pnum);
						corrupt = 2;
						break;
					}
					continue;
				}
			}
			/* read source page */
			ubi_msg("check source @pnum %d, offset 0x%x", source_pnum,
				source_page * ubi->mtd->writesize);
			err = ubi_io_read_oob(ubi, ubi->databuf, NULL, source_pnum,
					    source_page * ubi->mtd->writesize);
			ubi_msg("checked source @pnum %d, offset 0x%x, ret %d", source_pnum,
				source_page * ubi->mtd->writesize, err);
			if (err < 0 || err == UBI_IO_BITFLIPS) {
				ubi_msg("source @pnum %d, offset 0x%x correct/bitflips =%d",
					source_pnum, source_page * ubi->mtd->writesize, err);
				corrupt = 1;
				break;
			}
			/* read high page */
			high_page = mtk_nand_paired_page_transfer(source_page, false);
			ubi_msg("check high @pnum %d, offset 0x%x", source_pnum,
				high_page * ubi->mtd->writesize);
			err = ubi_io_read_oob(ubi, ubi->databuf, NULL, source_pnum,
					    high_page * ubi->mtd->writesize);
			ubi_msg("checked high @pnum %d, offset 0x%x, ret %d", source_pnum,
				high_page * ubi->mtd->writesize, err);
			if (err < 0 || err == UBI_IO_BITFLIPS) {
				ubi_msg("high @pnum %d, offset 0x%x correct/bitflips =%d",
					source_pnum, high_page * ubi->mtd->writesize, err);
				corrupt = 1;
				break;
			}
			if (check_pattern(ubi->databuf, 0xFF, ubi->mtd->writesize) == 1) {
				ubi_msg("high pare are empty");
				av = ubi_find_av(ai, source_vol_id);
				if (!av) {
					ubi_msg("volume id %d was not found", source_vol_id);
					ubi_msg("old_seb NULL");
					corrupt = 1;
					break;
				}
				ubi_rb_for_each_entry(rb, old_seb, &av->root, u.rb) {
					if (old_seb->pnum == source_pnum) {
						ubi_msg("old_seb peb %d", old_seb->pnum);
						break;
					}
				}
				if (old_seb != NULL && old_seb->pnum == source_pnum) {
					ubi_msg("old seq %llu , blb seq %llu", old_seb->sqnum,
						be64_to_cpu(p_blb_spare->sqnum));
					if (old_seb->sqnum < be64_to_cpu(p_blb_spare->sqnum)) {
						corrupt = 1;
						break;
					}
				} else if (source_page == 1) {
					ubi_msg("old_seb NULL");
					corrupt = 1;
					break;
				}
			}
			ubi_msg("high pare has content");
		}
		if (corrupt == 1) {
			int backup_pnum = UBI_LEB_UNMAPPED;

			ubi_msg("corrupt %d", corrupt);
			if (backup_seb[1] != NULL)
				backup_pnum = backup_seb[1]->pnum;
			blb_recovery_peb(ubi, ai, p_blb_spare, pnum, num, backup_pnum,
					 candidate_peb);
			candidate_peb = NULL;
		} else if (corrupt == 2) {
			av = ubi_find_av(ai, source_vol_id);
			if (!av) {
				ubi_msg("volume id %d was not found", source_vol_id);
			} else {
				ubi_rb_for_each_entry(rb, old_seb, &av->root, u.rb) {
					if (old_seb->pnum == source_pnum)
						break;
				}
				if (old_seb != NULL && old_seb->pnum == source_pnum) {
					rb_erase(&old_seb->u.rb, &av->root);
					if (candidate_peb != NULL) {
						ubi_msg("candidate peb %d doesn't be used, add to free list",
								candidate_peb->pnum);
						add_to_list(ubi, ai, candidate_peb->pnum, candidate_peb->vol_id,
								candidate_peb->lnum, candidate_peb->ec, 1, &ai->free);
						kmem_cache_free(ai->aeb_slab_cache, candidate_peb);
					}
					ubi_msg("candidate peb %d", old_seb->pnum);
					candidate_peb = old_seb;
				}
			}
			list_for_each_entry(old_seb, &ai->free, u.list)
				if (old_seb->pnum == source_pnum) {
					list_del(&old_seb->u.list);
					ubi_msg("candidate peb %d", old_seb->pnum);
					candidate_peb = old_seb;
					break;
				}
			list_for_each_entry(old_seb, &ai->corr, u.list)
				if (old_seb->pnum == source_pnum) {
					list_del(&old_seb->u.list);
					ubi_msg("candidate peb %d", old_seb->pnum);
					candidate_peb = old_seb;
					break;
				}
			if (candidate_peb != NULL) {
				ubi_msg("erasing candidate peb %d", candidate_peb->pnum);
				err =
				    early_erase_peb(ubi, ai, candidate_peb->pnum,
						    candidate_peb->ec + 1);
				if (err) {
					ubi_msg("erasing candidate peb %d fail %d",
						candidate_peb->pnum, err);
					add_to_list(ubi, ai, old_seb->pnum, old_seb->vol_id,
						    old_seb->lnum, old_seb->ec, 1, &ai->erase);
					kmem_cache_free(ai->aeb_slab_cache, candidate_peb);
					candidate_peb = NULL;
				}
				candidate_peb->ec++;
			}

		}
	}
	if (candidate_peb != NULL) {
		ubi_msg("candidate peb %d doesn't be used, add to free list", candidate_peb->pnum);
		add_to_list(ubi, ai, candidate_peb->pnum, candidate_peb->vol_id, candidate_peb->lnum,
			    candidate_peb->ec, 1, &ai->free);
		kmem_cache_free(ai->aeb_slab_cache, candidate_peb);
	}
	list_for_each_entry_safe(old_seb, seb_tmp, &ai->waiting, u.list) {
		list_del(&old_seb->u.list);
		ubi_msg("move to erase from waiting: PEB %d, EC %d", old_seb->pnum, old_seb->ec);
		err =
		    add_to_list(ubi, ai, old_seb->pnum, old_seb->vol_id, old_seb->lnum, old_seb->ec, 1,
				&ai->erase);
		kmem_cache_free(ai->aeb_slab_cache, old_seb);
	}
	return 0;

out_free:
	if (ubi->databuf)
		vfree(ubi->databuf);
	if (ubi->oobbuf)
		vfree(ubi->oobbuf);
	if (vid_hdr)
		ubi_free_vid_hdr(ubi, vid_hdr);

	return err;
}
#endif
