/* SPDX-License-Identifier: GPL-2.0 */
/*
 * RP2350 (included by fs/ramfs/inode.c, see apply.sh). Without an MMU there
 * is no CONFIG_SHMEM, so "tmpfs" is ramfs, which ignores size=: a big file
 * in /tmp would take kernel memory until the OOM killer ran. Here ramfs keeps
 * size= (bytes, k/m/g, or a percentage of RAM): past it a write fails with
 * ENOSPC. And its page allocations may fail (ENOMEM to the writer) instead
 * of running the OOM killer.
 */
#include <linux/statfs.h>

/* pages in the page cache of the filesystem's files */
static unsigned long ramfs_used_pages(struct super_block *sb)
{
	struct inode *inode;
	unsigned long n = 0;

	spin_lock(&sb->s_inode_list_lock);
	list_for_each_entry(inode, &sb->s_inodes, i_sb_list)
		n += READ_ONCE(inode->i_mapping->nrpages);
	spin_unlock(&sb->s_inode_list_lock);
	return n;
}

/* room for npages more pages under size=? 0 or -ENOSPC */
int ramfs_rp_room(struct inode *inode, unsigned long npages)
{
	struct ramfs_fs_info *fsi = inode->i_sb->s_fs_info;
	unsigned long max = fsi ? fsi->mount_opts.max_pages : 0;

	if (max && ramfs_used_pages(inode->i_sb) + npages > max)
		return -ENOSPC;
	return 0;
}

static int ramfs_rp_write_begin(struct file *file, struct address_space *mapping,
				loff_t pos, unsigned len, struct folio **foliop, void **fsdata)
{
	loff_t start = pos & PAGE_MASK;
	int err;

	/* a page that isn't there yet counts against size= */
	if (!filemap_range_has_page(mapping, start, start + PAGE_SIZE - 1)) {
		err = ramfs_rp_room(mapping->host, 1);
		if (err)
			return err;
	}
	return simple_write_begin(file, mapping, pos, len, foliop, fsdata);
}

/* ram_aops (libfs, whose read_folio and write_end are static) with the
 * write_begin above; filled in at the first inode */
static struct address_space_operations ramfs_rp_aops;

static const struct address_space_operations *ramfs_rp_get_aops(void)
{
	if (!ramfs_rp_aops.write_begin) {
		ramfs_rp_aops = ram_aops;
		ramfs_rp_aops.write_begin = ramfs_rp_write_begin;
	}
	return &ramfs_rp_aops;
}

static int ramfs_rp_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct ramfs_fs_info *fsi = dentry->d_sb->s_fs_info;
	unsigned long used;
	int err = simple_statfs(dentry, buf);

	if (err || !fsi->mount_opts.max_pages)
		return err;
	used = ramfs_used_pages(dentry->d_sb);
	buf->f_blocks = fsi->mount_opts.max_pages;
	buf->f_bfree = buf->f_bavail = buf->f_blocks > used ? buf->f_blocks - used : 0;
	return 0;
}

/* size=: bytes (k, m, g) or N% of RAM, in pages; 0: no limit */
static int ramfs_rp_parse_size(const char *s, unsigned long *pages)
{
	char *rest;
	unsigned long long v = memparse(s, &rest);

	if (*rest == '%') {
		v = div_u64((u64)totalram_pages() * v, 100);
		rest++;
	} else {
		v = DIV_ROUND_UP_ULL(v, PAGE_SIZE);
	}
	if (*rest)
		return -EINVAL;
	*pages = v;
	return 0;
}
