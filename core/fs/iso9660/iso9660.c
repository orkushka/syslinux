#include <dprintf.h>
#include <stdio.h>
#include <string.h>
#include <sys/dirent.h>
#include <core.h>
#include <cache.h>
#include <disk.h>
#include <fs.h>
#include "iso9660_fs.h"

/* Convert to lower case string */
static inline char iso_tolower(char c)
{
    if (c >= 'A' && c <= 'Z')
	c += 0x20;

    return c;
}

static struct inode *new_iso_inode(struct fs_info *fs)
{
    return alloc_inode(fs, 0, sizeof(struct iso9660_pvt_inode));
}

static inline struct iso_sb_info *ISO_SB(struct fs_info *fs)
{
    return fs->fs_info;
}

/*
 * Mangle a filename pointed to by src into a buffer pointed
 * to by dst; ends on encountering any whitespace.
 * dst is preserved.
 *
 * This verifies that a filename is < FilENAME_MAX characters,
 * doesn't contain whitespace, zero-pads the output buffer,
 * and removes trailing dots and redumndant slashes, so "repe
 * cmpsb" can do a compare, and the path-searching routine gets
 * a bit of an easier job.
 *
 */
static void iso_mangle_name(char *dst, const char *src)
{
    char *p = dst;
    int i = FILENAME_MAX - 1;

    while (not_whitespace(*src)) {
        if ( *src == '/' ) {
            if ( *(src+1) == '/' ) {
                i--;
                src++;
                continue;
            }
        }

        *dst++ = *src ++;
        i--;
    }

    while ( 1 ) {
        if ( dst == p )
            break;

        if ( (*(dst-1) != '.') && (*(dst-1) != '/') )
            break;

        dst --;
        i ++;
    }

    i ++;
    for (; i > 0; i -- )
        *dst++ = '\0';
}

static size_t iso_convert_name(char *dst, const char *src, int len)
{
    char *p = dst;
    char c;
    
    if (len == 1) {
	switch (*src) {
	case 1:
	    *p++ = '.';
	    /* fall through */
	case 0:
	    *p++ = '.';
	    goto done;
	default:
	    /* nothing special */
	    break;
	}
    }

    while (len-- && (c = *src++)) {
	if (c == ';')	/* Remove any filename version suffix */
	    break;
	*p++ = iso_tolower(c);
    }
    
    /* Then remove any terminal dots */
    while (p > dst+1 && p[-1] == '.')
	p--;

done:
    *p = '\0';
    return p - dst;
}

/* 
 * Unlike strcmp, it does return 1 on match, or reutrn 0 if not match.
 */
static bool iso_compare_name(const char *de_name, size_t len,
			     const char *file_name)
{
    char iso_file_name[256];
    char *p = iso_file_name;
    char c1, c2;
    size_t i;
    
    i = iso_convert_name(iso_file_name, de_name, len);
    dprintf("Compare: \"%s\" to \"%s\" (len %zu)\n",
	    file_name, iso_file_name, i);

    do {
	c1 = *p++;
	c2 = iso_tolower(*file_name++);

	/* compare equal except for case? */
	if (c1 != c2)
	    return false;
    } while (c1);

    return true;
}

static inline int cdrom_read_blocks(struct disk *disk, void *buf, 
				    int block, int blocks)
{
    return disk->rdwr_sectors(disk, buf, block, blocks, 0);
}

/*
 * Get multiple clusters from a file, given the file pointer.
 */
static uint32_t iso_getfssec(struct file *file, char *buf,
			     int blocks, bool *have_more)
{
    struct fs_info *fs = file->fs;
    struct disk *disk = fs->fs_dev->disk;
    uint32_t bytes_read = blocks << fs->block_shift;
    uint32_t bytes_left = file->inode->size - file->offset;
    uint32_t blocks_left = (bytes_left + BLOCK_SIZE(file->fs) - 1) 
	>> file->fs->block_shift;
    block_t block = PVT(file->inode)->lba + (file->offset >> fs->block_shift);

    if (blocks > blocks_left)
        blocks = blocks_left;
    cdrom_read_blocks(disk, buf, block, blocks);

    if (bytes_read >= bytes_left) {
        bytes_read = bytes_left;
        *have_more = 0;
    } else {
        *have_more = 1;
    }
    
    file->offset += bytes_read;
    return bytes_read;
}

/*
 * Find a entry in the specified dir with name _dname_.
 */
static const struct iso_dir_entry *
iso_find_entry(const char *dname, struct inode *inode)
{
    struct fs_info *fs = inode->fs;
    block_t dir_block = PVT(inode)->lba;
    int i = 0, offset = 0;
    const char *de_name;
    int de_name_len, de_len;
    const struct iso_dir_entry *de;
    const char *data = NULL;

    dprintf("iso_find_entry: \"%s\"\n", dname);
    
    while (1) {
	if (!data) {
	    dprintf("Getting block %d from block %llu\n", i, dir_block);
	    if (++i > inode->blocks)
		return NULL;	/* End of directory */
	    data = get_cache(fs->fs_dev, dir_block++);
	    offset = 0;
	}

	de = (const struct iso_dir_entry *)(data + offset);
	de_len = de->length;
	offset += de_len;
	
	/* Make sure we have a full directory entry */
	if (de_len < 33 || offset > BLOCK_SIZE(fs)) {
	    /*
	     * Zero = end of sector, or corrupt directory entry
	     *
	     * ECMA-119:1987 6.8.1.1: "Each Directory Record shall end
	     * in the Logical Sector in which it begins.
	     */
	    data = NULL;
	    continue;
	}
	
	de_name_len = de->name_len;
	de_name = de->name;
	if (iso_compare_name(de_name, de_name_len, dname)) {
	    dprintf("Found.\n");
	    return de;
	}
    }
}

static inline int get_inode_mode(uint8_t flags)
{
    if (flags & 0x02)
	return I_DIR;
    else
	return I_FILE;
}

static struct inode *iso_get_inode(struct fs_info *fs,
				   const struct iso_dir_entry *de)
{
    struct inode *inode = new_iso_inode(fs);
    if (!inode)
	return NULL;

    dprintf("Getting inode for: %.*s\n", de->name_len, de->name);

    inode->mode   = get_inode_mode(de->flags);
    inode->size   = de->size_le;
    PVT(inode)->lba = de->extent_le;
    inode->blocks = (inode->size + BLOCK_SIZE(fs) - 1) >> BLOCK_SHIFT(fs);

    return inode;
}

static struct inode *iso_iget_root(struct fs_info *fs)
{
    const struct iso_dir_entry *root = &ISO_SB(fs)->root;

    return iso_get_inode(fs, root);
}

static struct inode *iso_iget(const char *dname, struct inode *parent)
{
    const struct iso_dir_entry *de;
    
    dprintf("iso_iget %p %s\n", parent, dname);

    de = iso_find_entry(dname, parent);
    if (!de)
	return NULL;
    
    return iso_get_inode(parent->fs, de);
}

static struct dirent *iso_readdir(struct file *file)
{
    struct fs_info *fs = file->fs;
    struct inode *inode = file->inode;
    const struct iso_dir_entry *de;
    struct dirent *dirent;
    const char *data = NULL;
    
    while (1) {
	size_t offset = file->offset & (BLOCK_SIZE(fs) - 1);

	if (!data) {
	    uint32_t i = file->offset >> BLOCK_SHIFT(fs);
	    if (i >= inode->blocks)
		return NULL;
	    data = get_cache(fs->fs_dev, PVT(inode)->lba + i);
	}
	de = (const struct iso_dir_entry *)(data + offset);
	
	if (de->length < 33 || offset + de->length > BLOCK_SIZE(fs)) {
	    file->offset = (file->offset + BLOCK_SIZE(fs))
		& ~(BLOCK_SIZE(fs) - 1); /* Start of the next block */
	    data = NULL;
	    continue;
	}
	break;
    }
    
    if (!(dirent = malloc(sizeof(*dirent)))) {
	malloc_error("dirent structure in iso_readdir");
	return NULL;
    }
    
    dirent->d_ino = 0;           /* Inode number is invalid to ISO fs */
    dirent->d_off = file->offset;
    dirent->d_type = get_inode_mode(de->flags);
    dirent->d_reclen = iso_convert_name(dirent->d_name,
					de->name, de->name_len);

    file->offset += de->length;  /* Update for next reading */
    
    return dirent;
}

/* Load the config file, return 1 if failed, or 0 */
static int iso_load_config(void)
{
    const char *search_directories[] = {
	"/boot/isolinux", 
	"/isolinux",
	"/",
	NULL
    };
    com32sys_t regs;
    int i;
    
    for (i = 0; search_directories[i]; i++) {
	    memset(&regs, 0, sizeof regs);
	    snprintf(ConfigName, FILENAME_MAX, "%s/isolinux.cfg",
		     search_directories[i]);
	    regs.edi.w[0] = OFFS_WRT(ConfigName, 0);
	    call16(core_open, &regs, &regs);
	    if (!(regs.eflags.l & EFLAGS_ZF))
		break;
    }
    if (!search_directories[i])
	return -1;
    
    /* Set the current working directory */
    chdir(search_directories[i]);
    return 0;
}


static int iso_fs_init(struct fs_info *fs)
{
    struct iso_sb_info *sbi;
    
    sbi = malloc(sizeof(*sbi));
    if (!sbi) {
	malloc_error("iso_sb_info structure");
	return 1;
    }
    fs->fs_info = sbi;
    
    cdrom_read_blocks(fs->fs_dev->disk, trackbuf, 16, 1);
    memcpy(&sbi->root, trackbuf + ROOT_DIR_OFFSET, sizeof(sbi->root));

    fs->sector_shift = fs->fs_dev->disk->sector_shift;
    fs->block_shift  = 11;
    fs->sector_size  = 1 << fs->sector_shift;
    fs->block_size   = 1 << fs->block_shift;

    /* Initialize the cache */
    cache_init(fs->fs_dev, fs->block_shift);

    return fs->block_shift;
}


const struct fs_ops iso_fs_ops = {
    .fs_name       = "iso",
    .fs_flags      = FS_USEMEM | FS_THISIND,
    .fs_init       = iso_fs_init,
    .searchdir     = NULL, 
    .getfssec      = iso_getfssec,
    .close_file    = generic_close_file,
    .mangle_name   = iso_mangle_name,
    .unmangle_name = generic_unmangle_name,
    .load_config   = iso_load_config,
    .iget_root     = iso_iget_root,
    .iget          = iso_iget,
    .readdir       = iso_readdir
};
