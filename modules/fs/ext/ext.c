#include <ext.h>
#include <ext_disk.h>

#include <boot/boot.h>

#include <dev/device.h>
#include <fs/fs_syscall.h>
#include <mm/mm_syscall.h>
#include <mm/cache.h>

#include <mm/mm.h>
#include <arch/arch.h>
#include <task/task.h>

#define EXT_MAP_CACHE_TARGET_BYTES (128u * 1024u)
#define EXT_MAP_CACHE_MIN_ENTRIES 16u
#define EXT_MAP_CACHE_MAX_ENTRIES 128u
#define EXT_MAX_BLOCK_SIZE 4096u

#define EXT_QUOTA_USER_MAGIC 0xd9c01f11u
#define EXT_QUOTA_BLOCK_SIZE 1024u
#define EXT_QUOTA_TREEOFF 1u
#define EXT_QUOTA_BLOCK_HEADER_SIZE 16u
#define EXT_QUOTA_ENTRY_SIZE 72u
#define EXT_QUOTA_PTRS_PER_BLOCK                                               \
    ((EXT_QUOTA_BLOCK_SIZE - EXT_QUOTA_BLOCK_HEADER_SIZE) / sizeof(uint32_t))
#define EXT_QUOTA_ENTRIES_PER_BLOCK                                            \
    ((EXT_QUOTA_BLOCK_SIZE - EXT_QUOTA_BLOCK_HEADER_SIZE) /                    \
     EXT_QUOTA_ENTRY_SIZE)

typedef struct ext_map_cache_entry {
    uint64_t block;
    uint8_t *data;
    bool valid;
} ext_map_cache_entry_t;

typedef struct ext_mount_ctx {
    uint64_t dev;
    spinlock_t lock;
    ext_super_block_t sb;
    ext_group_desc_t *groups;
    uint32_t group_count;
    uint32_t block_size;
    uint32_t inode_size;
    uint32_t desc_size;
    uint32_t ptrs_per_block;
    uint64_t blocks_count;
    uint64_t inodes_count;
    bool has_extents;
    bool has_64bit;
    bool has_metadata_csum;
    bool has_dir_index;
    uint32_t checksum_seed;
    ext_map_cache_entry_t *map_cache_entries;
    uint32_t map_cache_entry_count;
} ext_mount_ctx_t;

typedef struct ext_inode_info {
    struct vfs_inode vfs_inode;
    ext_inode_disk_t inode_cache;
    bool inode_valid;
    char *symlink;
} ext_inode_info_t;

typedef struct ext_xattr_record {
    uint8_t index;
    char *name;
    uint8_t *value;
    uint32_t size;
} ext_xattr_record_t;

typedef struct ext_quota_header {
    uint32_t dqh_magic;
    uint32_t dqh_version;
} __attribute__((packed)) ext_quota_header_t;

typedef struct ext_quota_block_header {
    uint32_t dqdh_next_free;
    uint32_t dqdh_prev_free;
    uint16_t dqdh_entries;
    uint16_t dqdh_pad1;
    uint32_t dqdh_pad2;
} __attribute__((packed)) ext_quota_block_header_t;

typedef struct ext_quota_disk_entry {
    uint32_t dqb_id;
    uint32_t dqb_pad;
    uint64_t dqb_ihardlimit;
    uint64_t dqb_isoftlimit;
    uint64_t dqb_curinodes;
    uint64_t dqb_bhardlimit;
    uint64_t dqb_bsoftlimit;
    uint64_t dqb_curspace;
    uint64_t dqb_btime;
    uint64_t dqb_itime;
} __attribute__((packed)) ext_quota_disk_entry_t;

typedef struct ext_dir_lookup {
    bool found;
    uint32_t inode;
    uint8_t file_type;
    uint32_t lblock;
    uint16_t offset;
    uint16_t rec_len;
    bool has_prev;
    uint16_t prev_offset;
    uint16_t prev_rec_len;
} ext_dir_lookup_t;

typedef struct ext_dir_block_ctx {
    uint64_t pblock;
    uint32_t lblock;
    uint32_t data_size;
    bool indexed_root;
    bool has_tail;
    ext_dir_entry_tail_t *tail;
} ext_dir_block_ctx_t;

#define EXT_EXTENT_MAX_DEPTH 5u

typedef struct ext_extent_path {
    ext_extent_header_t *hdr;
    uint8_t *buf;
    uint64_t block;
    uint16_t slot;
} ext_extent_path_t;

static int ext_dev_read(ext_mount_ctx_t *fs, uint64_t offset, void *buf,
                        size_t size);
static int ext_dev_write(ext_mount_ctx_t *fs, uint64_t offset, const void *buf,
                         size_t size);
static int ext_read_block(ext_mount_ctx_t *fs, uint64_t block, void *buf);
static int ext_write_block(ext_mount_ctx_t *fs, uint64_t block,
                           const void *buf);
static int ext_read_inode(ext_mount_ctx_t *fs, uint32_t ino,
                          ext_inode_disk_t *inode);
static int ext_write_inode(ext_mount_ctx_t *fs, uint32_t ino,
                           const ext_inode_disk_t *inode);
static int ext_alloc_block_locked(ext_mount_ctx_t *fs, uint32_t prefer_group,
                                  uint64_t *out_block);
static int ext_free_block_locked(ext_mount_ctx_t *fs, uint64_t block);
static int ext_write_group_desc(ext_mount_ctx_t *fs, uint32_t group);
static int ext_inode_get_block_locked(ext_mount_ctx_t *fs, uint32_t ino,
                                      ext_inode_disk_t *inode,
                                      uint32_t logical_block, bool create,
                                      uint64_t *out_block);
static int ext_zero_inode_tail_locked(ext_mount_ctx_t *fs, uint32_t ino,
                                      ext_inode_disk_t *inode,
                                      uint64_t new_size);
static int ext_inode_resize_locked(ext_mount_ctx_t *fs, uint32_t ino,
                                   ext_inode_disk_t *inode, uint64_t new_size,
                                   bool write_back);
static void ext_inode_copy_size_blocks(ext_inode_disk_t *dst,
                                       const ext_inode_disk_t *src);
static void ext_sync_vfs_inode(struct vfs_inode *inode, ext_mount_ctx_t *fs,
                               const ext_inode_disk_t *disk_inode);
static struct vfs_inode *ext_iget_locked(struct vfs_super_block *sb,
                                         uint32_t ino);

static struct vfs_file_system_type ext_fs_type;
static const struct vfs_super_operations ext_super_ops;
static const struct vfs_inode_operations ext_inode_ops;
static const struct vfs_file_operations ext_dir_ops;
static const struct vfs_file_operations ext_file_ops;

static uint64_t ext_now(void) {
    return boot_get_boottime() + nano_time() / 1000000000;
}

static uint16_t ext_dir_rec_len(size_t name_len) {
    return (uint16_t)PADDING_UP(8 + name_len, 4);
}

static bool ext_dir_entry_valid(const ext_dir_entry_t *entry, uint32_t off,
                                uint32_t data_size) {
    uint32_t name_capacity;

    if (!entry || off > data_size || data_size - off < sizeof(*entry))
        return false;
    if (entry->rec_len < sizeof(*entry) || (entry->rec_len & 3))
        return false;
    if (entry->rec_len > data_size - off)
        return false;

    name_capacity = entry->rec_len - sizeof(*entry);
    if (entry->name_len > name_capacity)
        return false;
    if (entry->inode && ext_dir_rec_len(entry->name_len) > entry->rec_len)
        return false;

    return true;
}

static bool ext_dir_entry_name_eq(const ext_dir_entry_t *entry,
                                  const char *name, size_t name_len) {
    return entry && entry->inode && entry->name_len == name_len &&
           !memcmp(entry->name, name, name_len);
}

static uint64_t ext_inode_size_get(const ext_inode_disk_t *inode) {
    return (uint64_t)inode->i_size_lo | ((uint64_t)inode->i_size_high << 32);
}

static uint64_t ext_group_inode_bitmap(const ext_group_desc_t *gd);
static uint64_t ext_group_block_bitmap(const ext_group_desc_t *gd);
static uint64_t ext_group_inode_table(const ext_group_desc_t *gd);
static int ext_init_inode_bitmap_locked(ext_mount_ctx_t *fs, uint32_t group);
static int ext_init_block_bitmap_locked(ext_mount_ctx_t *fs, uint32_t group);

static void ext_inode_size_set(ext_inode_disk_t *inode, uint64_t size) {
    inode->i_size_lo = (uint32_t)size;
    inode->i_size_high = (uint32_t)(size >> 32);
}

static uint64_t ext_inode_blocks_get(const ext_inode_disk_t *inode) {
    return (uint64_t)inode->i_blocks_lo |
           ((uint64_t)inode->i_blocks_high << 32);
}

static void ext_inode_blocks_set(ext_inode_disk_t *inode, uint64_t blocks) {
    inode->i_blocks_lo = (uint32_t)blocks;
    inode->i_blocks_high = (uint16_t)(blocks >> 32);
}

static void ext_inode_add_fs_blocks(ext_mount_ctx_t *fs,
                                    ext_inode_disk_t *inode,
                                    uint32_t fs_blocks) {
    uint64_t sectors_per_block = fs->block_size / 512;
    ext_inode_blocks_set(inode, ext_inode_blocks_get(inode) +
                                    (uint64_t)fs_blocks * sectors_per_block);
}

static void ext_inode_sub_fs_blocks(ext_mount_ctx_t *fs,
                                    ext_inode_disk_t *inode,
                                    uint32_t fs_blocks) {
    uint64_t sectors_per_block = fs->block_size / 512;
    uint64_t cur = ext_inode_blocks_get(inode);
    uint64_t delta = (uint64_t)fs_blocks * sectors_per_block;
    ext_inode_blocks_set(inode, cur > delta ? cur - delta : 0);
}

static uint32_t ext_inode_uid_get(const ext_inode_disk_t *inode) {
    return (uint32_t)inode->i_uid | ((uint32_t)inode->i_uid_high << 16);
}

static uint32_t ext_inode_gid_get(const ext_inode_disk_t *inode) {
    return (uint32_t)inode->i_gid | ((uint32_t)inode->i_gid_high << 16);
}

static void ext_inode_uid_set(ext_inode_disk_t *inode, uint32_t uid) {
    inode->i_uid = (uint16_t)uid;
    inode->i_uid_high = (uint16_t)(uid >> 16);
}

static void ext_inode_gid_set(ext_inode_disk_t *inode, uint32_t gid) {
    inode->i_gid = (uint16_t)gid;
    inode->i_gid_high = (uint16_t)(gid >> 16);
}

static uint32_t ext_inode_rdev_get(const ext_inode_disk_t *inode) {
    return inode->i_block[0] & 0xFFFFu;
}

static void ext_inode_rdev_set(ext_inode_disk_t *inode, uint32_t dev) {
    memset(inode->i_block, 0, sizeof(inode->i_block));
    inode->i_block[0] = dev;
}

static bool ext_inode_uses_extents(const ext_inode_disk_t *inode) {
    return inode && (inode->i_flags & EXT4_EXTENTS_FL);
}

static bool ext_inode_is_indexed_dir(const ext_inode_disk_t *inode) {
    return inode && (inode->i_flags & EXT2_INDEX_FL) &&
           ((inode->i_mode & S_IFMT) == EXT2_S_IFDIR);
}

static int ext_require_mutable_linear_dir(const ext_inode_disk_t *inode) {
    /* htree directories need indexed updates; linear edits would corrupt them.
     */
    if (ext_inode_is_indexed_dir(inode))
        return -EOPNOTSUPP;
    return 0;
}

static size_t ext_inode_extra_capacity(ext_mount_ctx_t *fs) {
    if (!fs || fs->inode_size <= EXT2_GOOD_OLD_INODE_SIZE)
        return 0;

    return MIN((size_t)fs->inode_size - EXT2_GOOD_OLD_INODE_SIZE,
               sizeof(ext_inode_disk_t) - EXT2_GOOD_OLD_INODE_SIZE);
}

static uint32_t ext_crc32c_update(uint32_t crc, const void *buf, size_t len) {
    static uint32_t table[256];
    static bool table_ready;
    const uint8_t *p = buf;

    if (!table_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (uint32_t bit = 0; bit < 8; bit++)
                v = (v & 1) ? (v >> 1) ^ 0x82F63B78u : (v >> 1);
            table[i] = v;
        }
        table_ready = true;
    }

    /* ext4 metadata checksums use raw crc32c continuation, without final xor.
     */
    while (len--)
        crc = table[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    return crc;
}

static uint32_t ext_metadata_checksum_seed(const ext_super_block_t *sb) {
    uint32_t crc = 0xFFFFFFFFu;

    crc = ext_crc32c_update(crc, sb->s_uuid, sizeof(sb->s_uuid));
    return crc;
}

static bool ext_dir_has_tail(ext_mount_ctx_t *fs, const ext_inode_disk_t *inode,
                             uint32_t data_size) {
    return fs && inode && fs->has_metadata_csum &&
           ((inode->i_mode & S_IFMT) == EXT2_S_IFDIR) &&
           data_size >= sizeof(ext_dir_entry_tail_t);
}

static ext_dir_entry_tail_t *ext_dir_tail_ptr(const ext_dir_block_ctx_t *ctx,
                                              uint8_t *buf) {
    if (!ctx || !ctx->has_tail)
        return NULL;
    return (ext_dir_entry_tail_t *)(buf + ctx->data_size);
}

static uint32_t ext_group_checksum_offset(ext_mount_ctx_t *fs, uint32_t group) {
    uint64_t gdt_block = fs->block_size == 1024 ? 2 : 1;
    return (uint32_t)(gdt_block * fs->block_size +
                      (uint64_t)group * fs->desc_size);
}

static void ext_group_descriptor_checksum_set(ext_mount_ctx_t *fs,
                                              uint32_t group) {
    ext_group_desc_t tmp;
    size_t desc_len;
    uint32_t crc = 0;
    uint32_t group_le = group;

    if (!fs || !fs->has_metadata_csum || group >= fs->group_count)
        return;

    tmp = fs->groups[group];
    tmp.bg_checksum = 0;
    desc_len = MIN((size_t)fs->desc_size, sizeof(tmp));
    crc = ext_crc32c_update(fs->checksum_seed, &group_le, sizeof(group_le));
    crc = ext_crc32c_update(crc, &tmp, desc_len);
    fs->groups[group].bg_checksum = (uint16_t)crc;
}

static void ext_super_checksum_set(ext_mount_ctx_t *fs) {
    uint32_t crc;
    size_t checksum_off;

    if (!fs || !fs->has_metadata_csum)
        return;

    fs->sb.s_checksum = 0;
    checksum_off = offsetof(ext_super_block_t, s_checksum);
    /* Superblock checksum is seeded directly, not with s_csum_seed. */
    crc = ext_crc32c_update(0xFFFFFFFFu, &fs->sb, checksum_off);
    fs->sb.s_checksum = crc;
}

static bool ext_super_checksum_verify(ext_mount_ctx_t *fs) {
    uint32_t stored;
    uint32_t crc;
    size_t checksum_off;

    if (!fs || !fs->has_metadata_csum)
        return true;

    stored = fs->sb.s_checksum;
    fs->sb.s_checksum = 0;
    checksum_off = offsetof(ext_super_block_t, s_checksum);
    /* Refuse to mount a stale primary superblock before any metadata write. */
    crc = ext_crc32c_update(0xFFFFFFFFu, &fs->sb, checksum_off);
    fs->sb.s_checksum = stored;
    return crc == stored;
}

static void ext_inode_checksum_set(ext_mount_ctx_t *fs, uint32_t ino,
                                   uint8_t *raw) {
    uint32_t crc;
    uint32_t ino_le = ino;
    uint32_t gen = 0;
    ext_inode_disk_t *inode;

    if (!fs || !raw || !fs->has_metadata_csum)
        return;

    inode = (ext_inode_disk_t *)raw;
    gen = inode->i_generation;
    inode->i_checksum_lo = 0;
    inode->i_checksum_hi = 0;
    crc = ext_crc32c_update(fs->checksum_seed, &ino_le, sizeof(ino_le));
    crc = ext_crc32c_update(crc, &gen, sizeof(gen));
    crc = ext_crc32c_update(crc, raw, fs->inode_size);
    inode->i_checksum_lo = (uint16_t)crc;
    if (fs->inode_size > EXT2_GOOD_OLD_INODE_SIZE)
        inode->i_checksum_hi = (uint16_t)(crc >> 16);
}

static uint32_t ext_bitmap_checksum(ext_mount_ctx_t *fs, bool inode_bitmap,
                                    const void *buf) {
    size_t size;

    size = inode_bitmap ? (size_t)fs->sb.s_inodes_per_group / 8
                        : (size_t)fs->block_size;
    /* Bitmap checksums are seeded by the filesystem UUID only. */
    return ext_crc32c_update(fs->checksum_seed, buf, size);
}

static void ext_bitmap_checksum_set(ext_mount_ctx_t *fs, uint32_t group,
                                    bool inode_bitmap, const void *buf) {
    uint32_t crc;

    if (!fs || !buf || !fs->has_metadata_csum || group >= fs->group_count)
        return;

    crc = ext_bitmap_checksum(fs, inode_bitmap, buf);
    if (inode_bitmap) {
        fs->groups[group].bg_inode_bitmap_csum_lo = (uint16_t)crc;
        fs->groups[group].bg_inode_bitmap_csum_hi = (uint16_t)(crc >> 16);
    } else {
        fs->groups[group].bg_block_bitmap_csum_lo = (uint16_t)crc;
        fs->groups[group].bg_block_bitmap_csum_hi = (uint16_t)(crc >> 16);
    }
}

static void ext_bitmap_set_bit(uint8_t *bitmap, uint64_t bit) {
    bitmap[bit >> 3] |= (uint8_t)(1u << (bit & 7));
}

static void ext_bitmap_clear_bit(uint8_t *bitmap, uint64_t bit) {
    bitmap[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
}

static bool ext_bitmap_test_bit(const uint8_t *bitmap, uint64_t bit) {
    return (bitmap[bit >> 3] & (uint8_t)(1u << (bit & 7))) != 0;
}

static uint64_t ext_bitmap_count_free(const uint8_t *bitmap,
                                      uint64_t valid_bits) {
    uint64_t free = 0;

    if (!bitmap)
        return 0;

    for (uint64_t bit = 0; bit < valid_bits; bit++) {
        if (!ext_bitmap_test_bit(bitmap, bit))
            free++;
    }

    return free;
}

static void ext_bitmap_set_padding(ext_mount_ctx_t *fs, uint8_t *bitmap,
                                   uint64_t valid_bits) {
    uint64_t total_bits;

    if (!fs || !bitmap)
        return;

    total_bits = (uint64_t)fs->block_size * 8;
    if (valid_bits >= total_bits)
        return;

    /* ext4 requires all bits past the group's valid range to be set. */
    for (uint64_t bit = valid_bits; bit < total_bits; bit++)
        ext_bitmap_set_bit(bitmap, bit);
}

static uint32_t ext_dir_block_checksum(ext_mount_ctx_t *fs, uint32_t ino,
                                       const ext_inode_disk_t *inode,
                                       uint32_t logical_block,
                                       const uint8_t *buf, uint32_t data_size,
                                       bool has_tail) {
    uint32_t crc;
    uint32_t ino_le = ino;
    uint32_t gen = inode ? inode->i_generation : 0;

    (void)logical_block;
    crc = ext_crc32c_update(fs->checksum_seed, &ino_le, sizeof(ino_le));
    crc = ext_crc32c_update(crc, &gen, sizeof(gen));
    /* Directory block checksums cover dirents before the checksum tail. */
    crc = ext_crc32c_update(crc, buf, data_size);
    (void)has_tail;
    return crc;
}

static void ext_dir_block_checksum_set(ext_mount_ctx_t *fs, uint32_t ino,
                                       const ext_inode_disk_t *inode,
                                       const ext_dir_block_ctx_t *ctx,
                                       uint8_t *buf) {
    ext_dir_entry_tail_t *tail;

    if (!fs || !ctx || !buf || !ctx->has_tail)
        return;

    tail = ext_dir_tail_ptr(ctx, buf);
    tail->det_reserved_zero1 = 0;
    tail->det_rec_len = sizeof(*tail);
    tail->det_reserved_zero2 = 0;
    tail->det_reserved_ft = EXT4_FT_DIR_CSUM;
    tail->det_checksum = ext_dir_block_checksum(
        fs, ino, inode, ctx->lblock, buf, ctx->data_size, ctx->has_tail);
}

static int ext_dir_block_prepare(ext_mount_ctx_t *fs, uint32_t dir_ino,
                                 ext_inode_disk_t *dir_inode, uint32_t lblock,
                                 uint8_t *buf, ext_dir_block_ctx_t *ctx) {
    uint64_t pblock = 0;
    uint32_t data_off = 0;
    uint32_t data_size = fs->block_size;
    int ret;

    if (!fs || !dir_inode || !buf || !ctx)
        return -EINVAL;

    ret = ext_inode_get_block_locked(fs, dir_ino, dir_inode, lblock, false,
                                     &pblock);
    if (ret)
        return ret;

    memset(ctx, 0, sizeof(*ctx));
    ctx->pblock = pblock;
    ctx->lblock = lblock;
    if (!pblock)
        return 0;

    ret = ext_read_block(fs, pblock, buf);
    if (ret)
        return ret;

    if (ext_inode_is_indexed_dir(dir_inode) && lblock == 0) {
        ext_dir_entry_t *dot = (ext_dir_entry_t *)buf;
        ext_dir_entry_t *dotdot;

        if (!ext_dir_entry_valid(dot, 0, fs->block_size))
            return -EIO;
        dotdot = (ext_dir_entry_t *)(buf + dot->rec_len);
        if (!ext_dir_entry_valid(dotdot, dot->rec_len, fs->block_size))
            return -EIO;
        data_off = dot->rec_len + dotdot->rec_len;
        ctx->indexed_root = true;
    }

    if (ext_dir_has_tail(fs, dir_inode, fs->block_size - data_off)) {
        ext_dir_entry_tail_t *tail =
            (ext_dir_entry_tail_t *)(buf + fs->block_size - sizeof(*tail));
        if (tail->det_reserved_zero1 == 0 &&
            tail->det_rec_len == sizeof(*tail) &&
            tail->det_reserved_zero2 == 0 &&
            tail->det_reserved_ft == EXT4_FT_DIR_CSUM) {
            ctx->has_tail = true;
            ctx->tail = tail;
            data_size = fs->block_size - sizeof(*tail);
        }
    }

    ctx->data_size = data_size;
    return 0;
}

static int ext_dir_block_write_locked(ext_mount_ctx_t *fs, uint32_t dir_ino,
                                      const ext_inode_disk_t *dir_inode,
                                      const ext_dir_block_ctx_t *ctx,
                                      uint8_t *buf) {
    if (!fs || !ctx || !buf || !ctx->pblock)
        return -EINVAL;

    if (ctx->has_tail)
        ext_dir_block_checksum_set(fs, dir_ino, dir_inode, ctx, buf);
    return ext_write_block(fs, ctx->pblock, buf);
}

static void ext_inode_init_large_fields(ext_mount_ctx_t *fs,
                                        ext_inode_disk_t *inode) {
    size_t extra;

    if (!fs || !inode)
        return;

    extra = ext_inode_extra_capacity(fs);
    if (extra >= sizeof(inode->i_extra_isize))
        inode->i_extra_isize = (uint16_t)extra;
}

static void ext_inode_touch(ext_inode_disk_t *inode, bool atime, bool mtime,
                            bool ctime) {
    uint32_t now = (uint32_t)ext_now();
    if (atime)
        inode->i_atime = now;
    if (mtime)
        inode->i_mtime = now;
    if (ctime)
        inode->i_ctime = now;
    if (!inode->i_crtime)
        inode->i_crtime = now;
}

static uint32_t ext_mode_to_vfs_type(uint16_t mode) {
    switch (mode & S_IFMT) {
    case EXT2_S_IFDIR:
        return file_dir;
    case EXT2_S_IFLNK:
        return file_symlink;
    case EXT2_S_IFBLK:
        return file_block;
    case EXT2_S_IFCHR:
        return file_stream;
    case EXT2_S_IFIFO:
        return file_fifo;
    case EXT2_S_IFSOCK:
        return file_socket;
    case EXT2_S_IFREG:
    default:
        return file_none;
    }
}

static uint8_t ext_mode_to_dir_file_type(uint16_t mode) {
    switch (mode & S_IFMT) {
    case EXT2_S_IFREG:
        return EXT2_FT_REG_FILE;
    case EXT2_S_IFDIR:
        return EXT2_FT_DIR;
    case EXT2_S_IFCHR:
        return EXT2_FT_CHRDEV;
    case EXT2_S_IFBLK:
        return EXT2_FT_BLKDEV;
    case EXT2_S_IFIFO:
        return EXT2_FT_FIFO;
    case EXT2_S_IFSOCK:
        return EXT2_FT_SOCK;
    case EXT2_S_IFLNK:
        return EXT2_FT_SYMLINK;
    default:
        return EXT2_FT_UNKNOWN;
    }
}

static uint32_t ext_dir_file_type_to_vfs(uint8_t type) {
    switch (type) {
    case EXT2_FT_DIR:
        return file_dir;
    case EXT2_FT_SYMLINK:
        return file_symlink;
    case EXT2_FT_CHRDEV:
        return file_stream;
    case EXT2_FT_BLKDEV:
        return file_block;
    case EXT2_FT_FIFO:
        return file_fifo;
    case EXT2_FT_SOCK:
        return file_socket;
    case EXT2_FT_REG_FILE:
    default:
        return file_none;
    }
}

static unsigned char ext_dir_file_type_to_dtype(uint8_t type) {
    switch (type) {
    case EXT2_FT_DIR:
        return DT_DIR;
    case EXT2_FT_SYMLINK:
        return DT_LNK;
    case EXT2_FT_CHRDEV:
        return DT_CHR;
    case EXT2_FT_BLKDEV:
        return DT_BLK;
    case EXT2_FT_FIFO:
        return DT_FIFO;
    case EXT2_FT_SOCK:
        return DT_SOCK;
    case EXT2_FT_REG_FILE:
    default:
        return DT_REG;
    }
}

static uint64_t ext_group_first_block(ext_mount_ctx_t *fs, uint32_t group) {
    return (uint64_t)fs->sb.s_first_data_block +
           (uint64_t)group * fs->sb.s_blocks_per_group;
}

static uint64_t ext_group_blocks_count(ext_mount_ctx_t *fs, uint32_t group) {
    uint64_t start = ext_group_first_block(fs, group);
    if (start >= fs->blocks_count)
        return 0;
    return MIN((uint64_t)fs->sb.s_blocks_per_group, fs->blocks_count - start);
}

static uint64_t ext_group_inodes_count(ext_mount_ctx_t *fs, uint32_t group) {
    uint64_t start = (uint64_t)group * fs->sb.s_inodes_per_group;
    if (start >= fs->inodes_count)
        return 0;
    return MIN((uint64_t)fs->sb.s_inodes_per_group, fs->inodes_count - start);
}

static uint64_t ext_group_inode_start(uint32_t group,
                                      const ext_mount_ctx_t *fs) {
    if (!fs)
        return 0;
    return (uint64_t)group * fs->sb.s_inodes_per_group + 1;
}

static uint32_t ext_first_nonreserved_inode(const ext_mount_ctx_t *fs) {
    if (!fs)
        return EXT2_GOOD_OLD_FIRST_INO;

    return fs->sb.s_first_ino ? fs->sb.s_first_ino : EXT2_GOOD_OLD_FIRST_INO;
}

static uint64_t ext_group_reserved_inode_count(ext_mount_ctx_t *fs,
                                               uint32_t group) {
    uint64_t group_start;
    uint64_t group_end;
    uint32_t first_nonreserved;

    if (!fs)
        return 0;

    first_nonreserved = ext_first_nonreserved_inode(fs);
    group_start = ext_group_inode_start(group, fs);
    group_end = group_start + ext_group_inodes_count(fs, group);
    if (first_nonreserved <= group_start)
        return 0;
    if (group_start >= group_end)
        return 0;
    if (first_nonreserved >= group_end)
        return group_end - group_start;
    return (uint64_t)first_nonreserved - group_start;
}

static bool ext_group_contains_block(ext_mount_ctx_t *fs, uint32_t group,
                                     uint64_t block, uint64_t *bit_out) {
    uint64_t first;
    uint64_t count;

    if (!fs || group >= fs->group_count)
        return false;

    first = ext_group_first_block(fs, group);
    count = ext_group_blocks_count(fs, group);
    if (block < first || block >= first + count)
        return false;

    if (bit_out)
        *bit_out = block - first;
    return true;
}

static bool ext_sparse_super_group(uint32_t group) {
    uint32_t n;

    if (group == 0 || group == 1)
        return true;

    n = group;
    while (n && n % 3 == 0)
        n /= 3;
    if (n == 1)
        return true;

    n = group;
    while (n && n % 5 == 0)
        n /= 5;
    if (n == 1)
        return true;

    n = group;
    while (n && n % 7 == 0)
        n /= 7;
    return n == 1;
}

static bool ext_group_has_super(ext_mount_ctx_t *fs, uint32_t group) {
    if (!fs || group >= fs->group_count)
        return false;

    if (!(fs->sb.s_feature_ro_compat & EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER))
        return true;

    return ext_sparse_super_group(group);
}

static uint64_t ext_gdt_blocks(ext_mount_ctx_t *fs) {
    uint64_t bytes;

    if (!fs || !fs->desc_size)
        return 0;

    bytes = (uint64_t)fs->group_count * fs->desc_size;
    return (bytes + fs->block_size - 1) / fs->block_size;
}

static void ext_mark_block_bitmap_range(ext_mount_ctx_t *fs, uint32_t group,
                                        uint8_t *bitmap, uint64_t start,
                                        uint64_t count) {
    uint64_t first;
    uint64_t group_blocks;

    if (!fs || !bitmap || !count || group >= fs->group_count)
        return;

    first = ext_group_first_block(fs, group);
    group_blocks = ext_group_blocks_count(fs, group);
    if (start >= first + group_blocks || start + count <= first)
        return;

    uint64_t from = start > first ? start : first;
    uint64_t to = MIN(start + count, first + group_blocks);
    for (uint64_t block = from; block < to; block++)
        ext_bitmap_set_bit(bitmap, block - first);
}

static void ext_mark_block_group_metadata(ext_mount_ctx_t *fs, uint32_t group,
                                          uint8_t *bitmap) {
    uint64_t bit;
    uint64_t first;
    uint64_t gdt_block;
    uint64_t gdt_blocks;
    uint64_t itable_blocks;

    if (!fs || !bitmap || group >= fs->group_count)
        return;

    /* flex_bg may place another group's metadata in this physical group. */
    first = ext_group_first_block(fs, group);
    gdt_block = first + 1;
    if (ext_group_has_super(fs, group)) {
        ext_mark_block_bitmap_range(fs, group, bitmap, first, 1);
        ext_mark_block_bitmap_range(fs, group, bitmap, gdt_block,
                                    ext_gdt_blocks(fs));
        ext_mark_block_bitmap_range(fs, group, bitmap,
                                    gdt_block + ext_gdt_blocks(fs),
                                    fs->sb.s_reserved_gdt_blocks);
    }

    itable_blocks = ((uint64_t)fs->sb.s_inodes_per_group * fs->inode_size +
                     fs->block_size - 1) /
                    fs->block_size;

    for (uint32_t owner = 0; owner < fs->group_count; owner++) {
        ext_group_desc_t *gd = &fs->groups[owner];

        if (ext_group_contains_block(fs, group, ext_group_block_bitmap(gd),
                                     &bit))
            ext_bitmap_set_bit(bitmap, bit);
        if (ext_group_contains_block(fs, group, ext_group_inode_bitmap(gd),
                                     &bit))
            ext_bitmap_set_bit(bitmap, bit);
        ext_mark_block_bitmap_range(fs, group, bitmap,
                                    ext_group_inode_table(gd), itable_blocks);
    }
}

static uint64_t ext_group_last_used_inode(ext_mount_ctx_t *fs, uint32_t group) {
    uint64_t inode_count;
    uint64_t group_start;
    uint8_t *bitmap;
    uint64_t last = 0;
    int ret;

    if (!fs || group >= fs->group_count)
        return 0;

    inode_count = ext_group_inodes_count(fs, group);
    if (!inode_count)
        return 0;

    bitmap = calloc(1, fs->block_size);
    if (!bitmap)
        return inode_count;

    ret =
        ext_read_block(fs, ext_group_inode_bitmap(&fs->groups[group]), bitmap);
    if (ret) {
        free(bitmap);
        return inode_count;
    }

    for (uint64_t bit = inode_count; bit > 0; bit--) {
        uint64_t idx = bit - 1;
        if (ext_bitmap_test_bit(bitmap, idx)) {
            last = idx + 1;
            break;
        }
    }

    free(bitmap);

    group_start = ext_group_inode_start(group, fs);
    if (!last) {
        uint64_t reserved = ext_group_reserved_inode_count(fs, group);
        return reserved;
    }

    return group_start + last - 1;
}

static void ext_group_itable_unused_refresh(ext_mount_ctx_t *fs,
                                            uint32_t group) {
    ext_group_desc_t *gd;
    uint64_t inode_count;
    uint64_t last_used_ino;
    uint64_t group_start;
    uint64_t used;
    uint64_t unused;

    if (!fs || group >= fs->group_count || !fs->has_metadata_csum)
        return;

    gd = &fs->groups[group];
    inode_count = ext_group_inodes_count(fs, group);
    group_start = ext_group_inode_start(group, fs);
    last_used_ino = ext_group_last_used_inode(fs, group);

    if (last_used_ino < group_start)
        used = 0;
    else
        used = last_used_ino - group_start + 1;

    if (used > inode_count)
        used = inode_count;
    unused = inode_count - used;

    gd->bg_itable_unused_lo = (uint16_t)unused;
    gd->bg_itable_unused_hi = (uint16_t)(unused >> 16);
}

static int ext_init_inode_bitmap_locked(ext_mount_ctx_t *fs, uint32_t group) {
    ext_group_desc_t *gd;
    uint64_t inode_count;
    uint8_t *bitmap;
    int ret;

    if (!fs || group >= fs->group_count)
        return -EINVAL;

    gd = &fs->groups[group];
    if (!(gd->bg_flags & EXT4_BG_INODE_UNINIT))
        return 0;

    inode_count = ext_group_inodes_count(fs, group);
    bitmap = calloc(1, fs->block_size);
    if (!bitmap)
        return -ENOMEM;

    if (group == 0) {
        uint64_t reserved = ext_group_reserved_inode_count(fs, group);
        for (uint64_t bit = 0; bit < reserved && bit < inode_count; bit++)
            ext_bitmap_set_bit(bitmap, bit);
    }

    ext_bitmap_set_padding(fs, bitmap, inode_count);
    ext_bitmap_checksum_set(fs, group, true, bitmap);
    ret = ext_write_block(fs, ext_group_inode_bitmap(gd), bitmap);
    free(bitmap);
    if (ret)
        return ret;

    gd->bg_flags &= (uint16_t)~EXT4_BG_INODE_UNINIT;
    gd->bg_flags |= EXT4_BG_INODE_ZEROED;
    ext_group_itable_unused_refresh(fs, group);
    return ext_write_group_desc(fs, group);
}

static int ext_init_block_bitmap_locked(ext_mount_ctx_t *fs, uint32_t group) {
    ext_group_desc_t *gd;
    uint64_t block_count;
    uint8_t *bitmap;
    int ret;

    if (!fs || group >= fs->group_count)
        return -EINVAL;

    gd = &fs->groups[group];
    if (!(gd->bg_flags & EXT4_BG_BLOCK_UNINIT))
        return 0;

    block_count = ext_group_blocks_count(fs, group);
    bitmap = calloc(1, fs->block_size);
    if (!bitmap)
        return -ENOMEM;

    ext_mark_block_group_metadata(fs, group, bitmap);
    ext_bitmap_set_padding(fs, bitmap, block_count);

    ext_bitmap_checksum_set(fs, group, false, bitmap);
    ret = ext_write_block(fs, ext_group_block_bitmap(gd), bitmap);
    free(bitmap);
    if (ret)
        return ret;

    gd->bg_flags &= (uint16_t)~EXT4_BG_BLOCK_UNINIT;
    return ext_write_group_desc(fs, group);
}

static uint64_t ext_sb_reserved_blocks_count(const ext_super_block_t *sb) {
    if (!sb)
        return 0;

    return (uint64_t)sb->s_r_blocks_count_lo |
           ((uint64_t)sb->s_r_blocks_count_hi << 32);
}

static uint64_t ext_sb_free_blocks_count(const ext_super_block_t *sb) {
    if (!sb)
        return 0;

    return (uint64_t)sb->s_free_blocks_count_lo |
           ((uint64_t)sb->s_free_blocks_count_hi << 32);
}

static void ext_sb_free_blocks_count_set(ext_super_block_t *sb,
                                         uint64_t count) {
    if (!sb)
        return;

    sb->s_free_blocks_count_lo = (uint32_t)count;
    sb->s_free_blocks_count_hi = (uint32_t)(count >> 32);
}

static uint64_t ext_group_block_bitmap(const ext_group_desc_t *gd) {
    if (!gd)
        return 0;

    return (uint64_t)gd->bg_block_bitmap_lo |
           ((uint64_t)gd->bg_block_bitmap_hi << 32);
}

static uint64_t ext_group_inode_bitmap(const ext_group_desc_t *gd) {
    if (!gd)
        return 0;

    return (uint64_t)gd->bg_inode_bitmap_lo |
           ((uint64_t)gd->bg_inode_bitmap_hi << 32);
}

static uint64_t ext_group_inode_table(const ext_group_desc_t *gd) {
    if (!gd)
        return 0;

    return (uint64_t)gd->bg_inode_table_lo |
           ((uint64_t)gd->bg_inode_table_hi << 32);
}

static uint64_t ext_group_free_blocks_count(const ext_group_desc_t *gd) {
    if (!gd)
        return 0;

    return (uint64_t)gd->bg_free_blocks_count_lo |
           ((uint64_t)gd->bg_free_blocks_count_hi << 16);
}

static void ext_group_free_blocks_count_set(ext_group_desc_t *gd,
                                            uint64_t count) {
    if (!gd)
        return;

    gd->bg_free_blocks_count_lo = (uint16_t)count;
    gd->bg_free_blocks_count_hi = (uint16_t)(count >> 16);
}

static uint64_t ext_group_free_inodes_count(const ext_group_desc_t *gd) {
    if (!gd)
        return 0;

    return (uint64_t)gd->bg_free_inodes_count_lo |
           ((uint64_t)gd->bg_free_inodes_count_hi << 16);
}

static void ext_group_free_inodes_count_set(ext_group_desc_t *gd,
                                            uint64_t count) {
    if (!gd)
        return;

    gd->bg_free_inodes_count_lo = (uint16_t)count;
    gd->bg_free_inodes_count_hi = (uint16_t)(count >> 16);
}

static uint64_t ext_group_used_dirs_count(const ext_group_desc_t *gd) {
    if (!gd)
        return 0;

    return (uint64_t)gd->bg_used_dirs_count_lo |
           ((uint64_t)gd->bg_used_dirs_count_hi << 16);
}

static void ext_group_used_dirs_count_set(ext_group_desc_t *gd,
                                          uint64_t count) {
    if (!gd)
        return;

    gd->bg_used_dirs_count_lo = (uint16_t)count;
    gd->bg_used_dirs_count_hi = (uint16_t)(count >> 16);
}

static uint64_t ext_groups_free_blocks_sum(ext_mount_ctx_t *fs) {
    uint64_t free_blocks = 0;

    if (!fs)
        return 0;

    for (uint32_t group = 0; group < fs->group_count; group++)
        free_blocks += ext_group_free_blocks_count(&fs->groups[group]);

    return free_blocks;
}

static uint64_t ext_groups_free_inodes_sum(ext_mount_ctx_t *fs) {
    uint64_t free_inodes = 0;

    if (!fs)
        return 0;

    for (uint32_t group = 0; group < fs->group_count; group++)
        free_inodes += ext_group_free_inodes_count(&fs->groups[group]);

    return free_inodes;
}

static bool ext_reserved_blocks_available_to_caller(const ext_mount_ctx_t *fs) {
    task_t *task = current_task;

    if (!fs || !task)
        return false;

    return task->euid == 0 || task->fsuid == (int64_t)fs->sb.s_def_resuid ||
           task->fsgid == (int64_t)fs->sb.s_def_resgid;
}

static int ext_dev_read_direct(ext_mount_ctx_t *fs, uint64_t offset, void *buf,
                               size_t size) {
    ssize_t ret = device_read(fs->dev, buf, offset, size, 0);
    if (ret < 0)
        return (int)ret;
    if ((size_t)ret != size)
        return -EIO;
    return 0;
}

static int ext_dev_write_direct(ext_mount_ctx_t *fs, uint64_t offset,
                                const void *buf, size_t size) {
    ssize_t ret = device_write(fs->dev, (void *)buf, offset, size, 0);
    if (ret < 0)
        return (int)ret;
    if ((size_t)ret != size)
        return -EIO;
    return 0;
}

static uint32_t ext_map_cache_default_entries(uint32_t block_size) {
    uint32_t entries;

    if (!block_size)
        return 0;

    entries = EXT_MAP_CACHE_TARGET_BYTES / block_size;
    if (entries < EXT_MAP_CACHE_MIN_ENTRIES)
        entries = EXT_MAP_CACHE_MIN_ENTRIES;
    if (entries > EXT_MAP_CACHE_MAX_ENTRIES)
        entries = EXT_MAP_CACHE_MAX_ENTRIES;
    return entries;
}

static int ext_map_cache_init(ext_mount_ctx_t *fs) {
    if (!fs || !fs->block_size)
        return -EINVAL;

    fs->map_cache_entry_count = ext_map_cache_default_entries(fs->block_size);
    fs->map_cache_entries =
        calloc(fs->map_cache_entry_count, sizeof(*fs->map_cache_entries));
    if (!fs->map_cache_entries)
        return -ENOMEM;

    for (uint32_t i = 0; i < fs->map_cache_entry_count; i++) {
        fs->map_cache_entries[i].data = calloc(1, fs->block_size);
        if (!fs->map_cache_entries[i].data) {
            for (uint32_t j = 0; j < i; j++)
                free(fs->map_cache_entries[j].data);
            free(fs->map_cache_entries);
            fs->map_cache_entries = NULL;
            fs->map_cache_entry_count = 0;
            return -ENOMEM;
        }
    }

    return 0;
}

static void ext_map_cache_destroy(ext_mount_ctx_t *fs) {
    if (!fs || !fs->map_cache_entries)
        return;

    for (uint32_t i = 0; i < fs->map_cache_entry_count; i++)
        free(fs->map_cache_entries[i].data);
    free(fs->map_cache_entries);
    fs->map_cache_entries = NULL;
    fs->map_cache_entry_count = 0;
}

static inline ext_map_cache_entry_t *ext_map_cache_slot(ext_mount_ctx_t *fs,
                                                        uint64_t block) {
    if (!fs || !fs->map_cache_entries || !fs->map_cache_entry_count)
        return NULL;
    return &fs->map_cache_entries[block % fs->map_cache_entry_count];
}

static void ext_map_cache_invalidate_locked(ext_mount_ctx_t *fs,
                                            uint64_t block) {
    ext_map_cache_entry_t *entry = ext_map_cache_slot(fs, block);
    if (!entry)
        return;
    if (entry->valid && entry->block == block)
        entry->valid = false;
}

static int ext_map_cache_read_locked(ext_mount_ctx_t *fs, uint64_t block,
                                     void *buf) {
    ext_map_cache_entry_t *entry = ext_map_cache_slot(fs, block);
    if (entry && entry->valid && entry->block == block) {
        memcpy(buf, entry->data, fs->block_size);
        return 0;
    }

    int ret =
        ext_dev_read_direct(fs, block * fs->block_size, buf, fs->block_size);
    if (ret)
        return ret;

    if (entry) {
        memcpy(entry->data, buf, fs->block_size);
        entry->block = block;
        entry->valid = true;
    }
    return 0;
}

static int ext_map_cache_ref_locked(ext_mount_ctx_t *fs, uint64_t block,
                                    uint8_t **buf_out) {
    if (!buf_out)
        return -EINVAL;

    ext_map_cache_entry_t *entry = ext_map_cache_slot(fs, block);
    if (!entry || !entry->data)
        return -EINVAL;

    if (!entry->valid || entry->block != block) {
        int ret = ext_dev_read_direct(fs, block * fs->block_size, entry->data,
                                      fs->block_size);
        if (ret)
            return ret;
        entry->block = block;
        entry->valid = true;
    }

    *buf_out = entry->data;
    return 0;
}

static void ext_map_cache_store_locked(ext_mount_ctx_t *fs, uint64_t block,
                                       const void *buf) {
    ext_map_cache_entry_t *entry = ext_map_cache_slot(fs, block);
    if (!entry)
        return;

    memcpy(entry->data, buf, fs->block_size);
    entry->block = block;
    entry->valid = true;
}

static inline ext_mount_ctx_t *ext_sb_info(struct vfs_super_block *sb) {
    return sb ? (ext_mount_ctx_t *)sb->s_fs_info : NULL;
}

static inline ext_inode_info_t *ext_i(struct vfs_inode *inode) {
    return container_of_or_null(inode, ext_inode_info_t, vfs_inode);
}

static int ext_inode_offset(ext_mount_ctx_t *fs, uint32_t ino,
                            uint64_t *offset_out) {
    if (!fs || !offset_out || ino == 0 || ino > fs->inodes_count)
        return -EINVAL;

    uint32_t group = (ino - 1) / fs->sb.s_inodes_per_group;
    uint32_t index = (ino - 1) % fs->sb.s_inodes_per_group;
    uint64_t table_block = ext_group_inode_table(&fs->groups[group]);
    *offset_out =
        table_block * fs->block_size + (uint64_t)index * fs->inode_size;
    return 0;
}

static int ext_dev_read(ext_mount_ctx_t *fs, uint64_t offset, void *buf,
                        size_t size) {
    if (!size)
        return 0;
    if (!fs || !buf)
        return -EINVAL;
    return ext_dev_read_direct(fs, offset, buf, size);
}

static int ext_dev_write(ext_mount_ctx_t *fs, uint64_t offset, const void *buf,
                         size_t size) {
    if (!size)
        return 0;
    if (!fs || !buf)
        return -EINVAL;
    return ext_dev_write_direct(fs, offset, buf, size);
}

static int ext_read_block(ext_mount_ctx_t *fs, uint64_t block, void *buf) {
    return ext_dev_read(fs, block * fs->block_size, buf, fs->block_size);
}

static int ext_write_block(ext_mount_ctx_t *fs, uint64_t block,
                           const void *buf) {
    return ext_dev_write(fs, block * fs->block_size, buf, fs->block_size);
}

static int ext_zero_block(ext_mount_ctx_t *fs, uint64_t block) {
    void *zero = calloc(1, fs->block_size);
    if (!zero)
        return -ENOMEM;
    int ret = ext_write_block(fs, block, zero);
    free(zero);
    return ret;
}

static int ext_write_block_cached_locked(ext_mount_ctx_t *fs, uint64_t block,
                                         const void *buf) {
    int ret = ext_write_block(fs, block, buf);
    if (!ret)
        ext_map_cache_store_locked(fs, block, buf);
    return ret;
}

static int ext_write_super(ext_mount_ctx_t *fs) {
    fs->sb.s_wtime = (uint32_t)ext_now();
    ext_super_checksum_set(fs);
    return ext_dev_write(fs, 1024, &fs->sb, sizeof(fs->sb));
}

static int ext_write_group_desc(ext_mount_ctx_t *fs, uint32_t group) {
    uint64_t gdt_block = fs->block_size == 1024 ? 2 : 1;
    uint64_t offset =
        gdt_block * fs->block_size + (uint64_t)group * fs->desc_size;
    ext_group_descriptor_checksum_set(fs, group);
    return ext_dev_write(fs, offset, &fs->groups[group],
                         MIN((size_t)fs->desc_size, sizeof(ext_group_desc_t)));
}

static int ext_read_inode(ext_mount_ctx_t *fs, uint32_t ino,
                          ext_inode_disk_t *inode) {
    if (!fs || !inode || ino == 0 || ino > fs->inodes_count)
        return -EINVAL;

    uint64_t offset = 0;
    int ret = ext_inode_offset(fs, ino, &offset);
    if (ret)
        return ret;

    uint8_t *raw = calloc(1, fs->inode_size);
    if (!raw)
        return -ENOMEM;

    ret = ext_dev_read(fs, offset, raw, fs->inode_size);
    if (!ret)
        memcpy(inode, raw, MIN((size_t)fs->inode_size, sizeof(*inode)));

    free(raw);
    return ret;
}

static int ext_write_inode(ext_mount_ctx_t *fs, uint32_t ino,
                           const ext_inode_disk_t *inode) {
    if (!fs || !inode || ino == 0 || ino > fs->inodes_count)
        return -EINVAL;

    uint64_t offset = 0;
    int ret = ext_inode_offset(fs, ino, &offset);
    if (ret)
        return ret;

    uint8_t *raw = calloc(1, fs->inode_size);
    if (!raw)
        return -ENOMEM;

    ret = ext_dev_read(fs, offset, raw, fs->inode_size);
    if (ret) {
        free(raw);
        return ret;
    }

    memcpy(raw, inode, MIN((size_t)fs->inode_size, sizeof(*inode)));
    ext_inode_checksum_set(fs, ino, raw);
    ret = ext_dev_write(fs, offset, raw, fs->inode_size);
    free(raw);
    return ret;
}

static int ext_write_inode_zeroed(ext_mount_ctx_t *fs, uint32_t ino,
                                  const ext_inode_disk_t *inode) {
    if (!fs || !inode || ino == 0 || ino > fs->inodes_count)
        return -EINVAL;

    uint64_t offset = 0;
    int ret = ext_inode_offset(fs, ino, &offset);
    if (ret)
        return ret;

    uint8_t *raw = calloc(1, fs->inode_size);
    if (!raw)
        return -ENOMEM;

    memcpy(raw, inode, MIN((size_t)fs->inode_size, sizeof(*inode)));
    ext_inode_checksum_set(fs, ino, raw);
    ret = ext_dev_write(fs, offset, raw, fs->inode_size);
    free(raw);
    return ret;
}

static uint64_t ext_inode_xattr_block(const ext_inode_disk_t *inode) {
    return (uint64_t)inode->i_file_acl_lo |
           ((uint64_t)inode->i_file_acl_high << 32);
}

static void ext_inode_xattr_block_set(ext_inode_disk_t *inode, uint64_t block) {
    inode->i_file_acl_lo = (uint32_t)block;
    inode->i_file_acl_high = (uint16_t)(block >> 32);
}

static int ext_read_inode_raw(ext_mount_ctx_t *fs, uint32_t ino,
                              uint8_t **out) {
    uint64_t offset;
    uint8_t *raw;
    int ret;

    if (!fs || !out || !ino || ino > fs->inodes_count)
        return -EINVAL;
    ret = ext_inode_offset(fs, ino, &offset);
    if (ret)
        return ret;
    raw = calloc(1, fs->inode_size);
    if (!raw)
        return -ENOMEM;
    ret = ext_dev_read(fs, offset, raw, fs->inode_size);
    if (ret) {
        free(raw);
        return ret;
    }
    *out = raw;
    return 0;
}

static int ext_write_inode_raw(ext_mount_ctx_t *fs, uint32_t ino,
                               uint8_t *raw) {
    uint64_t offset;
    int ret;

    if (!fs || !raw || !ino || ino > fs->inodes_count)
        return -EINVAL;
    ret = ext_inode_offset(fs, ino, &offset);
    if (ret)
        return ret;
    ext_inode_checksum_set(fs, ino, raw);
    return ext_dev_write(fs, offset, raw, fs->inode_size);
}

static const char *ext_xattr_prefix(uint8_t index) {
    switch (index) {
    case EXT4_XATTR_INDEX_USER:
        return "user.";
    case EXT4_XATTR_INDEX_POSIX_ACL_ACCESS:
        return "system.posix_acl_access";
    case EXT4_XATTR_INDEX_POSIX_ACL_DEFAULT:
        return "system.posix_acl_default";
    case EXT4_XATTR_INDEX_TRUSTED:
        return "trusted.";
    case EXT4_XATTR_INDEX_SECURITY:
        return "security.";
    case EXT4_XATTR_INDEX_SYSTEM:
        return "system.";
    case EXT4_XATTR_INDEX_RICHACL:
        return "system.richacl";
    default:
        return NULL;
    }
}

static int ext_xattr_split_name(const char *full, uint8_t *index,
                                const char **name) {
    if (!full || !index || !name)
        return -EINVAL;
    if (!strncmp(full, "user.", 5)) {
        *index = EXT4_XATTR_INDEX_USER;
        *name = full + 5;
    } else if (!strcmp(full, "system.posix_acl_access")) {
        *index = EXT4_XATTR_INDEX_POSIX_ACL_ACCESS;
        *name = full + strlen(full);
    } else if (!strcmp(full, "system.posix_acl_default")) {
        *index = EXT4_XATTR_INDEX_POSIX_ACL_DEFAULT;
        *name = full + strlen(full);
    } else if (!strncmp(full, "trusted.", 8)) {
        *index = EXT4_XATTR_INDEX_TRUSTED;
        *name = full + 8;
    } else if (!strncmp(full, "security.", 9)) {
        *index = EXT4_XATTR_INDEX_SECURITY;
        *name = full + 9;
    } else if (!strcmp(full, "system.richacl")) {
        *index = EXT4_XATTR_INDEX_RICHACL;
        *name = full + strlen(full);
    } else if (!strncmp(full, "system.", 7)) {
        *index = EXT4_XATTR_INDEX_SYSTEM;
        *name = full + 7;
    } else {
        return -EOPNOTSUPP;
    }
    if (strlen(*name) > 255)
        return -ERANGE;
    return 0;
}

static size_t ext_xattr_entry_size(uint8_t name_len) {
    return PADDING_UP(sizeof(ext_xattr_entry_t) + name_len, 4);
}

static bool ext_xattr_last(const ext_xattr_entry_t *entry) {
    const uint32_t *word = (const uint32_t *)entry;
    return *word == 0;
}

static int ext_xattr_collect_area(const uint8_t *first, const uint8_t *end,
                                  const uint8_t *value_base,
                                  ext_xattr_record_t **records, size_t *count,
                                  size_t *capacity) {
    const ext_xattr_entry_t *entry = (const ext_xattr_entry_t *)first;

    while ((const uint8_t *)entry + sizeof(uint32_t) <= end &&
           !ext_xattr_last(entry)) {
        size_t entry_size = ext_xattr_entry_size(entry->e_name_len);
        uint32_t value_size = entry->e_value_size;
        uint16_t value_offs = entry->e_value_offs;
        ext_xattr_record_t *record;

        if ((const uint8_t *)entry + entry_size > end || entry->e_value_inum ||
            value_offs > (size_t)(end - value_base) ||
            value_size > (size_t)(end - value_base - value_offs))
            return -EIO;
        if (*count == *capacity) {
            size_t new_capacity = *capacity ? *capacity * 2 : 8;
            ext_xattr_record_t *new_records =
                realloc(*records, new_capacity * sizeof(**records));
            if (!new_records)
                return -ENOMEM;
            *records = new_records;
            *capacity = new_capacity;
        }
        record = &(*records)[(*count)++];
        memset(record, 0, sizeof(*record));
        record->index = entry->e_name_index;
        record->name = calloc(1, (size_t)entry->e_name_len + 1);
        if (!record->name)
            return -ENOMEM;
        memcpy(record->name, entry->e_name, entry->e_name_len);
        record->size = value_size;
        if (value_size) {
            record->value = malloc(value_size);
            if (!record->value)
                return -ENOMEM;
            memcpy(record->value, value_base + value_offs, value_size);
        }
        entry =
            (const ext_xattr_entry_t *)((const uint8_t *)entry + entry_size);
    }
    if ((const uint8_t *)entry + sizeof(uint32_t) > end)
        return -EIO;
    return 0;
}

static void ext_xattr_records_free(ext_xattr_record_t *records, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(records[i].name);
        free(records[i].value);
    }
    free(records);
}

static int ext_xattr_records_load_locked(ext_mount_ctx_t *fs, uint32_t ino,
                                         uint8_t **raw_out,
                                         ext_xattr_record_t **records_out,
                                         size_t *count_out) {
    ext_xattr_record_t *records = NULL;
    size_t count = 0, capacity = 0;
    uint8_t *raw = NULL;
    ext_inode_disk_t *disk_inode;
    uint64_t block;
    int ret;

    ret = ext_read_inode_raw(fs, ino, &raw);
    if (ret)
        return ret;
    disk_inode = (ext_inode_disk_t *)raw;
    if (fs->inode_size > EXT2_GOOD_OLD_INODE_SIZE &&
        disk_inode->i_extra_isize <=
            fs->inode_size - EXT2_GOOD_OLD_INODE_SIZE - sizeof(uint32_t)) {
        uint8_t *header =
            raw + EXT2_GOOD_OLD_INODE_SIZE + disk_inode->i_extra_isize;
        if (*(uint32_t *)header == EXT4_XATTR_MAGIC) {
            uint8_t *first = header + sizeof(uint32_t);
            ret = ext_xattr_collect_area(first, raw + fs->inode_size, first,
                                         &records, &count, &capacity);
            if (ret)
                goto fail;
        }
    }

    block = ext_inode_xattr_block(disk_inode);
    if (block) {
        uint8_t *buf = calloc(1, fs->block_size);
        ext_xattr_header_t *header;
        if (!buf) {
            ret = -ENOMEM;
            goto fail;
        }
        ret = ext_read_block(fs, block, buf);
        if (ret) {
            free(buf);
            goto fail;
        }
        header = (ext_xattr_header_t *)buf;
        if (header->h_magic != EXT4_XATTR_MAGIC || header->h_blocks != 1) {
            free(buf);
            ret = -EIO;
            goto fail;
        }
        ret =
            ext_xattr_collect_area(buf + sizeof(*header), buf + fs->block_size,
                                   buf, &records, &count, &capacity);
        free(buf);
        if (ret)
            goto fail;
    }

    *raw_out = raw;
    *records_out = records;
    *count_out = count;
    return 0;

fail:
    free(raw);
    ext_xattr_records_free(records, count);
    return ret;
}

static int ext_xattr_record_compare(const void *a, const void *b) {
    const ext_xattr_record_t *ra = a;
    const ext_xattr_record_t *rb = b;
    if (ra->index != rb->index)
        return (int)ra->index - (int)rb->index;
    return strcmp(ra->name, rb->name);
}

static uint32_t ext_xattr_block_checksum(ext_mount_ctx_t *fs, uint64_t block,
                                         uint8_t *buf) {
    ext_xattr_header_t *header = (ext_xattr_header_t *)buf;
    uint32_t old = header->h_checksum;
    uint32_t zero = 0;
    uint32_t crc = ext_crc32c_update(fs->checksum_seed, &block, sizeof(block));
    size_t off = offsetof(ext_xattr_header_t, h_checksum);
    crc = ext_crc32c_update(crc, buf, off);
    crc = ext_crc32c_update(crc, &zero, sizeof(zero));
    crc = ext_crc32c_update(crc, buf + off + sizeof(zero),
                            fs->block_size - off - sizeof(zero));
    header->h_checksum = old;
    return crc;
}

static int ext_xattr_serialize_block(ext_mount_ctx_t *fs, uint64_t block,
                                     ext_xattr_record_t *records, size_t count,
                                     uint8_t *buf) {
    ext_xattr_header_t *header = (ext_xattr_header_t *)buf;
    uint8_t *entry_pos = buf + sizeof(*header);
    uint8_t *value_pos = buf + fs->block_size;

    memset(buf, 0, fs->block_size);
    header->h_magic = EXT4_XATTR_MAGIC;
    header->h_refcount = 1;
    header->h_blocks = 1;
    qsort(records, count, sizeof(*records), ext_xattr_record_compare);

    for (size_t i = 0; i < count; i++) {
        size_t name_len = strlen(records[i].name);
        size_t entry_size = ext_xattr_entry_size((uint8_t)name_len);
        size_t value_size = PADDING_UP(records[i].size, 4);
        ext_xattr_entry_t *entry;

        if (entry_pos + entry_size + sizeof(uint32_t) > value_pos ||
            value_size >
                (size_t)(value_pos - entry_pos - entry_size - sizeof(uint32_t)))
            return -ENOSPC;
        value_pos -= value_size;
        entry = (ext_xattr_entry_t *)entry_pos;
        entry->e_name_len = (uint8_t)name_len;
        entry->e_name_index = records[i].index;
        entry->e_value_offs = (uint16_t)(value_pos - buf);
        entry->e_value_size = records[i].size;
        memcpy(entry->e_name, records[i].name, name_len);
        if (records[i].size)
            memcpy(value_pos, records[i].value, records[i].size);
        entry_pos += entry_size;
    }
    if (fs->has_metadata_csum)
        header->h_checksum = ext_xattr_block_checksum(fs, block, buf);
    return 0;
}

static int ext_xattr_records_store_locked(ext_mount_ctx_t *fs, uint32_t ino,
                                          uint8_t *raw,
                                          ext_xattr_record_t *records,
                                          size_t count) {
    ext_inode_disk_t *disk_inode = (ext_inode_disk_t *)raw;
    uint64_t old_block = ext_inode_xattr_block(disk_inode);
    uint64_t block = old_block;
    uint8_t *buf = NULL;
    uint8_t *old_buf = NULL;
    bool allocated_new = false;
    int ret = 0;

    if (fs->inode_size > EXT2_GOOD_OLD_INODE_SIZE &&
        disk_inode->i_extra_isize <=
            fs->inode_size - EXT2_GOOD_OLD_INODE_SIZE - sizeof(uint32_t)) {
        uint8_t *header =
            raw + EXT2_GOOD_OLD_INODE_SIZE + disk_inode->i_extra_isize;
        memset(header, 0, (raw + fs->inode_size) - header);
    }

    if (!count) {
        if (old_block) {
            old_buf = calloc(1, fs->block_size);
            if (!old_buf)
                return -ENOMEM;
            ret = ext_read_block(fs, old_block, old_buf);
            if (ret)
                goto out;
            ext_xattr_header_t *old_header = (ext_xattr_header_t *)old_buf;
            if (old_header->h_magic != EXT4_XATTR_MAGIC ||
                !old_header->h_refcount) {
                ret = -EIO;
                goto out;
            }
            if (old_header->h_refcount > 1) {
                old_header->h_refcount--;
                if (fs->has_metadata_csum)
                    old_header->h_checksum =
                        ext_xattr_block_checksum(fs, old_block, old_buf);
                ret = ext_write_block(fs, old_block, old_buf);
            } else {
                ret = ext_free_block_locked(fs, old_block);
            }
            if (ret)
                goto out;
            ext_inode_xattr_block_set(disk_inode, 0);
            uint64_t sectors = fs->block_size / 512;
            uint64_t blocks = ext_inode_blocks_get(disk_inode);
            ext_inode_blocks_set(disk_inode,
                                 blocks > sectors ? blocks - sectors : 0);
        }
        ret = ext_write_inode_raw(fs, ino, raw);
        goto out;
    }

    if (old_block) {
        old_buf = calloc(1, fs->block_size);
        if (!old_buf)
            return -ENOMEM;
        ret = ext_read_block(fs, old_block, old_buf);
        if (ret)
            goto out;
        ext_xattr_header_t *old_header = (ext_xattr_header_t *)old_buf;
        if (old_header->h_magic != EXT4_XATTR_MAGIC ||
            !old_header->h_refcount) {
            ret = -EIO;
            goto out;
        }
        if (old_header->h_refcount > 1) {
            ret = ext_alloc_block_locked(
                fs, (ino - 1) / fs->sb.s_inodes_per_group, &block);
            if (ret)
                goto out;
            allocated_new = true;
            ext_inode_xattr_block_set(disk_inode, block);
            buf = calloc(1, fs->block_size);
            if (!buf) {
                ret = -ENOMEM;
                goto out;
            }
        } else {
            buf = old_buf;
            old_buf = NULL;
        }
    } else {
        ret = ext_alloc_block_locked(fs, (ino - 1) / fs->sb.s_inodes_per_group,
                                     &block);
        if (ret)
            return ret;
        allocated_new = true;
        ext_inode_xattr_block_set(disk_inode, block);
        ext_inode_add_fs_blocks(fs, disk_inode, 1);
        buf = calloc(1, fs->block_size);
        if (!buf) {
            ext_free_block_locked(fs, block);
            return -ENOMEM;
        }
    }

    ret = ext_xattr_serialize_block(fs, block, records, count, buf);
    if (ret)
        goto out;
    ret = ext_write_block(fs, block, buf);
    if (ret)
        goto out;
    if (old_buf) {
        ext_xattr_header_t *old_header = (ext_xattr_header_t *)old_buf;
        old_header->h_refcount--;
        if (fs->has_metadata_csum)
            old_header->h_checksum =
                ext_xattr_block_checksum(fs, old_block, old_buf);
        ret = ext_write_block(fs, old_block, old_buf);
        if (ret)
            goto out;
    }
    ret = ext_write_inode_raw(fs, ino, raw);

out:
    if (ret && allocated_new)
        ext_free_block_locked(fs, block);
    free(buf);
    free(old_buf);
    return ret;
}

static void ext_copy_runtime_inode_state(ext_inode_disk_t *dst,
                                         const ext_inode_disk_t *src) {
    if (!dst || !src)
        return;

    ext_inode_size_set(dst, ext_inode_size_get(src));
    ext_inode_blocks_set(dst, ext_inode_blocks_get(src));
    memcpy(dst->i_block, src->i_block, sizeof(dst->i_block));
    dst->i_mtime = src->i_mtime;
    dst->i_ctime = src->i_ctime;
}

static void ext_inode_copy_size_blocks(ext_inode_disk_t *dst,
                                       const ext_inode_disk_t *src) {
    if (!dst || !src)
        return;

    ext_inode_size_set(dst, ext_inode_size_get(src));
    ext_inode_blocks_set(dst, ext_inode_blocks_get(src));
    memcpy(dst->i_block, src->i_block, sizeof(dst->i_block));
}

static uint32_t ext_block_bits(uint32_t block_size) {
    uint32_t bits = 0;

    while (block_size > 1) {
        bits++;
        block_size >>= 1;
    }
    return bits;
}

static void ext_sync_vfs_inode(struct vfs_inode *inode, ext_mount_ctx_t *fs,
                               const ext_inode_disk_t *disk_inode) {
    if (!inode || !fs || !disk_inode)
        return;

    inode->i_mode = disk_inode->i_mode;
    inode->i_uid = ext_inode_uid_get(disk_inode);
    inode->i_gid = ext_inode_gid_get(disk_inode);
    inode->i_nlink = disk_inode->i_links_count;
    inode->i_size = ext_inode_size_get(disk_inode);
    inode->i_blocks = ext_inode_blocks_get(disk_inode);
    inode->i_blkbits = ext_block_bits(fs->block_size);
    inode->i_atime.sec = disk_inode->i_atime;
    inode->i_btime.sec =
        disk_inode->i_crtime ? disk_inode->i_crtime : disk_inode->i_ctime;
    inode->i_ctime.sec = disk_inode->i_ctime;
    inode->i_mtime.sec = disk_inode->i_mtime;
    inode->inode = inode->i_ino;
    if (S_ISBLK(inode->i_mode) || S_ISCHR(inode->i_mode))
        inode->i_rdev = ext_inode_rdev_get(disk_inode);
    else
        inode->i_rdev = fs->dev;

    inode->i_op = &ext_inode_ops;
    inode->i_fop = S_ISDIR(inode->i_mode) ? &ext_dir_ops : &ext_file_ops;
}

static int ext_lookup_name_locked(ext_mount_ctx_t *fs, uint32_t dir_ino,
                                  const char *name, ext_dir_lookup_t *result) {
    size_t name_len;

    if (!fs || !name || !result)
        return -EINVAL;

    name_len = strlen(name);
    if (name_len > 255)
        return -ENAMETOOLONG;

    ext_inode_disk_t dir_inode = {0};
    int ret = ext_read_inode(fs, dir_ino, &dir_inode);
    if (ret)
        return ret;
    if ((dir_inode.i_mode & S_IFMT) != EXT2_S_IFDIR)
        return -ENOTDIR;

    memset(result, 0, sizeof(*result));
    uint64_t dir_size = ext_inode_size_get(&dir_inode);
    uint32_t blocks =
        (uint32_t)((dir_size + fs->block_size - 1) / fs->block_size);
    uint8_t *buf = calloc(1, fs->block_size);
    ext_dir_block_ctx_t block_ctx;
    if (!buf)
        return -ENOMEM;

    for (uint32_t lblock = 0; lblock < blocks; lblock++) {
        ret = ext_dir_block_prepare(fs, dir_ino, &dir_inode, lblock, buf,
                                    &block_ctx);
        if (ret) {
            free(buf);
            return ret;
        }
        if (!block_ctx.pblock)
            continue;

        bool has_prev = false;
        uint16_t prev_off = 0;
        uint16_t prev_rec = 0;
        uint32_t off = block_ctx.indexed_root ? 0 : 0;
        if (block_ctx.indexed_root) {
            ext_dir_entry_t *dot = (ext_dir_entry_t *)buf;
            ext_dir_entry_t *dotdot = (ext_dir_entry_t *)(buf + dot->rec_len);
            off = dot->rec_len + dotdot->rec_len;
        }
        for (; off + sizeof(ext_dir_entry_t) <= block_ctx.data_size;) {
            ext_dir_entry_t *entry = (ext_dir_entry_t *)(buf + off);
            if (!ext_dir_entry_valid(entry, off, block_ctx.data_size)) {
                free(buf);
                return -EIO;
            }
            if (ext_dir_entry_name_eq(entry, name, name_len)) {
                result->found = true;
                result->inode = entry->inode;
                result->file_type = entry->file_type;
                result->lblock = lblock;
                result->offset = off;
                result->rec_len = entry->rec_len;
                result->has_prev = has_prev;
                result->prev_offset = prev_off;
                result->prev_rec_len = prev_rec;
                free(buf);
                return 0;
            }
            has_prev = true;
            prev_off = off;
            prev_rec = entry->rec_len;
            off += entry->rec_len;
        }
    }

    free(buf);
    return 0;
}

static int ext_bitmap_update(ext_mount_ctx_t *fs, uint64_t block, uint32_t bit,
                             bool set) {
    uint32_t group;
    bool is_inode_bitmap = false;
    uint8_t *bitmap = calloc(1, fs->block_size);
    if (!bitmap)
        return -ENOMEM;
    int ret = ext_read_block(fs, block, bitmap);
    if (ret) {
        free(bitmap);
        return ret;
    }
    if (set)
        ext_bitmap_set_bit(bitmap, bit);
    else
        ext_bitmap_clear_bit(bitmap, bit);
    for (group = 0; group < fs->group_count; group++) {
        if (ext_group_block_bitmap(&fs->groups[group]) == block)
            break;
        if (ext_group_inode_bitmap(&fs->groups[group]) == block) {
            is_inode_bitmap = true;
            break;
        }
    }
    if (group < fs->group_count) {
        if (is_inode_bitmap)
            ext_bitmap_set_padding(fs, bitmap,
                                   ext_group_inodes_count(fs, group));
        else {
            ext_mark_block_group_metadata(fs, group, bitmap);
            ext_bitmap_set_padding(fs, bitmap,
                                   ext_group_blocks_count(fs, group));
        }
        ext_bitmap_checksum_set(fs, group, is_inode_bitmap, bitmap);
    }
    ret = ext_write_block(fs, block, bitmap);
    free(bitmap);
    return ret;
}

static int ext_alloc_inode_locked(ext_mount_ctx_t *fs, uint16_t mode,
                                  uint32_t *out_ino) {
    if (!fs || !out_ino)
        return -EINVAL;

    for (uint32_t group = 0; group < fs->group_count; group++) {
        ext_group_desc_t *gd = &fs->groups[group];
        uint64_t group_start = ext_group_inode_start(group, fs);
        uint32_t first_nonreserved = ext_first_nonreserved_inode(fs);
        uint64_t free_inodes;
        int ret;

        uint64_t inode_count = ext_group_inodes_count(fs, group);
        uint8_t *bitmap = calloc(1, fs->block_size);
        if (!bitmap)
            return -ENOMEM;
        ret = ext_init_inode_bitmap_locked(fs, group);
        if (ret) {
            free(bitmap);
            return ret;
        }
        ret = ext_read_block(fs, ext_group_inode_bitmap(gd), bitmap);
        if (ret) {
            free(bitmap);
            return ret;
        }
        ext_bitmap_set_padding(fs, bitmap, inode_count);
        free_inodes = ext_bitmap_count_free(bitmap, inode_count);
        if (!free_inodes) {
            free(bitmap);
            continue;
        }

        for (uint32_t bit = 0; bit < inode_count; bit++) {
            uint64_t ino = group_start + bit;
            if (ino < first_nonreserved)
                continue;
            if (ext_bitmap_test_bit(bitmap, bit))
                continue;
            ext_inode_disk_t empty_inode = {0};
            ext_inode_init_large_fields(fs, &empty_inode);
            ret = ext_write_inode_zeroed(fs, (uint32_t)ino, &empty_inode);
            if (ret) {
                free(bitmap);
                return ret;
            }

            ext_bitmap_set_bit(bitmap, bit);
            ext_bitmap_checksum_set(fs, group, true, bitmap);
            ret = ext_write_block(fs, ext_group_inode_bitmap(gd), bitmap);
            if (ret) {
                free(bitmap);
                return ret;
            }

            ext_group_free_inodes_count_set(
                gd, ext_bitmap_count_free(bitmap, inode_count));
            free(bitmap);
            if ((mode & S_IFMT) == EXT2_S_IFDIR)
                ext_group_used_dirs_count_set(
                    gd, ext_group_used_dirs_count(gd) + 1);
            fs->sb.s_free_inodes_count =
                (uint32_t)ext_groups_free_inodes_sum(fs);
            ext_group_itable_unused_refresh(fs, group);
            ret = ext_write_group_desc(fs, group);
            if (ret)
                return ret;
            ret = ext_write_super(fs);
            if (ret)
                return ret;

            *out_ino = (uint32_t)ino;
            return 0;
        }

        free(bitmap);
    }

    return -ENOSPC;
}

static int ext_free_inode_locked(ext_mount_ctx_t *fs, uint32_t ino,
                                 bool is_dir) {
    if (!fs || ino == 0 || ino > fs->inodes_count)
        return -EINVAL;

    uint32_t group = (ino - 1) / fs->sb.s_inodes_per_group;
    uint32_t bit = (ino - 1) % fs->sb.s_inodes_per_group;
    ext_group_desc_t *gd = &fs->groups[group];
    uint64_t used_dirs = ext_group_used_dirs_count(gd);
    uint64_t inode_count = ext_group_inodes_count(fs, group);
    uint8_t *bitmap = calloc(1, fs->block_size);
    int ret;
    if (!bitmap)
        return -ENOMEM;

    ret = ext_read_block(fs, ext_group_inode_bitmap(gd), bitmap);
    if (ret)
        goto out_bitmap;
    ext_bitmap_clear_bit(bitmap, bit);
    ext_bitmap_set_padding(fs, bitmap, inode_count);
    ext_bitmap_checksum_set(fs, group, true, bitmap);
    ret = ext_write_block(fs, ext_group_inode_bitmap(gd), bitmap);
    if (ret)
        goto out_bitmap;

    ext_group_free_inodes_count_set(gd,
                                    ext_bitmap_count_free(bitmap, inode_count));
    free(bitmap);
    bitmap = NULL;

    if (ret)
        return ret;

    if (is_dir && used_dirs)
        ext_group_used_dirs_count_set(gd, used_dirs - 1);
    fs->sb.s_free_inodes_count = (uint32_t)ext_groups_free_inodes_sum(fs);
    ext_group_itable_unused_refresh(fs, group);
    ret = ext_write_group_desc(fs, group);
    if (ret)
        return ret;
    return ext_write_super(fs);

out_bitmap:
    free(bitmap);
    return ret;
}

static int ext_alloc_blocks_locked(ext_mount_ctx_t *fs, uint32_t prefer_group,
                                   uint32_t want_blocks, uint64_t goal_block,
                                   uint64_t *out_block,
                                   uint32_t *out_allocated) {
    if (!fs || !out_block || !out_allocated || !want_blocks)
        return -EINVAL;

    for (uint32_t pass = 0; pass < fs->group_count; pass++) {
        uint32_t group = (prefer_group + pass) % fs->group_count;
        ext_group_desc_t *gd = &fs->groups[group];
        uint64_t free_blocks;
        int ret;

        uint64_t block_count = ext_group_blocks_count(fs, group);
        uint8_t *bitmap = calloc(1, fs->block_size);
        if (!bitmap)
            return -ENOMEM;
        ret = ext_init_block_bitmap_locked(fs, group);
        if (ret) {
            free(bitmap);
            return ret;
        }
        ret = ext_read_block(fs, ext_group_block_bitmap(gd), bitmap);
        if (ret) {
            free(bitmap);
            return ret;
        }

        ext_mark_block_group_metadata(fs, group, bitmap);
        ext_bitmap_set_padding(fs, bitmap, block_count);
        free_blocks = ext_bitmap_count_free(bitmap, block_count);
        if (!free_blocks) {
            free(bitmap);
            continue;
        }

        uint32_t start_bit = 0;
        uint64_t group_first = ext_group_first_block(fs, group);
        if (goal_block >= group_first && goal_block < group_first + block_count)
            start_bit = (uint32_t)(goal_block - group_first);

        for (uint32_t scan = 0; scan < block_count; scan++) {
            uint32_t bit = (start_bit + scan) % block_count;
            if (ext_bitmap_test_bit(bitmap, bit))
                continue;

            uint32_t run = 1;
            uint32_t max_run = MIN((uint32_t)(block_count - bit), want_blocks);
            while (run < max_run && !ext_bitmap_test_bit(bitmap, bit + run)) {
                run++;
            }

            *out_block = ext_group_first_block(fs, group) + bit;
            *out_allocated = run;
            for (uint32_t i = 0; i < run; i++) {
                ret = ext_zero_block(fs, *out_block + i);
                if (ret) {
                    free(bitmap);
                    return ret;
                }
            }

            for (uint32_t i = 0; i < run; i++)
                ext_bitmap_set_bit(bitmap, bit + i);

            ext_bitmap_checksum_set(fs, group, false, bitmap);
            ret = ext_write_block(fs, ext_group_block_bitmap(gd), bitmap);
            if (ret) {
                free(bitmap);
                return ret;
            }

            ext_group_free_blocks_count_set(
                gd, ext_bitmap_count_free(bitmap, block_count));
            free(bitmap);
            ext_sb_free_blocks_count_set(&fs->sb,
                                         ext_groups_free_blocks_sum(fs));
            ret = ext_write_group_desc(fs, group);
            if (ret)
                return ret;
            ret = ext_write_super(fs);
            if (ret)
                return ret;
            return 0;
        }

        free(bitmap);
    }

    return -ENOSPC;
}

static int ext_alloc_block_locked(ext_mount_ctx_t *fs, uint32_t prefer_group,
                                  uint64_t *out_block) {
    uint32_t allocated = 0;
    return ext_alloc_blocks_locked(fs, prefer_group, 1, 0, out_block,
                                   &allocated);
}

static int ext_free_block_locked(ext_mount_ctx_t *fs, uint64_t block) {
    if (!fs || block < fs->sb.s_first_data_block || block >= fs->blocks_count)
        return -EINVAL;

    ext_map_cache_invalidate_locked(fs, block);

    uint32_t group =
        (block - fs->sb.s_first_data_block) / fs->sb.s_blocks_per_group;
    uint32_t bit =
        (block - fs->sb.s_first_data_block) % fs->sb.s_blocks_per_group;
    ext_group_desc_t *gd = &fs->groups[group];
    uint64_t block_count = ext_group_blocks_count(fs, group);
    uint8_t *bitmap = calloc(1, fs->block_size);
    int ret;
    if (!bitmap)
        return -ENOMEM;

    ret = ext_read_block(fs, ext_group_block_bitmap(gd), bitmap);
    if (ret)
        goto out_bitmap;
    ext_bitmap_clear_bit(bitmap, bit);
    ext_mark_block_group_metadata(fs, group, bitmap);
    ext_bitmap_set_padding(fs, bitmap, block_count);
    ext_bitmap_checksum_set(fs, group, false, bitmap);
    ret = ext_write_block(fs, ext_group_block_bitmap(gd), bitmap);
    if (ret)
        goto out_bitmap;

    ext_group_free_blocks_count_set(gd,
                                    ext_bitmap_count_free(bitmap, block_count));
    free(bitmap);
    bitmap = NULL;

    ext_sb_free_blocks_count_set(&fs->sb, ext_groups_free_blocks_sum(fs));
    ret = ext_write_group_desc(fs, group);
    if (ret)
        return ret;
    return ext_write_super(fs);

out_bitmap:
    free(bitmap);
    return ret;
}

static int ext_lblock_path(ext_mount_ctx_t *fs, uint32_t lblock,
                           uint32_t offsets[4], uint32_t *depth) {
    uint32_t ptrs = fs->ptrs_per_block;
    if (lblock < EXT2_NDIR_BLOCKS) {
        offsets[0] = lblock;
        *depth = 1;
        return 0;
    }

    lblock -= EXT2_NDIR_BLOCKS;
    if (lblock < ptrs) {
        offsets[0] = EXT2_IND_BLOCK;
        offsets[1] = lblock;
        *depth = 2;
        return 0;
    }

    lblock -= ptrs;
    if (lblock < ptrs * ptrs) {
        offsets[0] = EXT2_DIND_BLOCK;
        offsets[1] = lblock / ptrs;
        offsets[2] = lblock % ptrs;
        *depth = 3;
        return 0;
    }

    lblock -= ptrs * ptrs;
    uint64_t tmax = (uint64_t)ptrs * ptrs * ptrs;
    if (lblock < tmax) {
        offsets[0] = EXT2_TIND_BLOCK;
        offsets[1] = lblock / (ptrs * ptrs);
        offsets[2] = (lblock / ptrs) % ptrs;
        offsets[3] = lblock % ptrs;
        *depth = 4;
        return 0;
    }

    return -EFBIG;
}

static int ext_block_all_zero(void *buf, uint32_t count) {
    uint32_t *entries = (uint32_t *)buf;
    for (uint32_t i = 0; i < count; i++) {
        if (entries[i])
            return false;
    }
    return true;
}

static uint16_t ext_extent_inode_max_entries(void) {
    return (uint16_t)((sizeof(((ext_inode_disk_t *)0)->i_block) -
                       sizeof(ext_extent_header_t)) /
                      sizeof(ext_extent_t));
}

static uint16_t ext_extent_block_max_entries(ext_mount_ctx_t *fs) {
    size_t tail_size =
        fs && fs->has_metadata_csum ? sizeof(ext_extent_tail_t) : 0;
    return (
        uint16_t)((fs->block_size - sizeof(ext_extent_header_t) - tail_size) /
                  sizeof(ext_extent_t));
}

static ext_extent_tail_t *ext_extent_tail(ext_mount_ctx_t *fs,
                                          ext_extent_header_t *hdr) {
    uint8_t *base;
    size_t off;

    if (!fs || !hdr || !fs->has_metadata_csum)
        return NULL;

    base = (uint8_t *)hdr;
    off = sizeof(*hdr) + (size_t)hdr->eh_max * sizeof(ext_extent_t);
    if (off + sizeof(ext_extent_tail_t) > fs->block_size)
        return NULL;

    return (ext_extent_tail_t *)(base + off);
}

static void ext_extent_block_checksum_set(ext_mount_ctx_t *fs, uint32_t ino,
                                          const ext_inode_disk_t *inode,
                                          uint8_t *buf) {
    ext_extent_header_t *hdr;
    ext_extent_tail_t *tail;
    uint32_t ino_le = ino;
    uint32_t gen;
    uint32_t crc;
    size_t crc_len;

    if (!fs || !inode || !buf || !fs->has_metadata_csum)
        return;

    hdr = (ext_extent_header_t *)buf;
    tail = ext_extent_tail(fs, hdr);
    if (!tail)
        return;

    /* External extent blocks store a checksum tail after eh_max entries. */
    gen = inode->i_generation;
    tail->et_checksum = 0;
    crc = ext_crc32c_update(fs->checksum_seed, &ino_le, sizeof(ino_le));
    crc = ext_crc32c_update(crc, &gen, sizeof(gen));
    crc_len = (uint8_t *)tail - buf;
    crc = ext_crc32c_update(crc, buf, crc_len);
    tail->et_checksum = crc;
}

static ext_extent_header_t *ext_inode_extent_header(ext_inode_disk_t *inode) {
    return (ext_extent_header_t *)inode->i_block;
}

static const ext_extent_header_t *
ext_inode_extent_header_const(const ext_inode_disk_t *inode) {
    return (const ext_extent_header_t *)inode->i_block;
}

static ext_extent_t *ext_extent_first(ext_extent_header_t *hdr) {
    return (ext_extent_t *)((uint8_t *)hdr + sizeof(*hdr));
}

static const ext_extent_t *
ext_extent_first_const(const ext_extent_header_t *hdr) {
    return (const ext_extent_t *)((const uint8_t *)hdr + sizeof(*hdr));
}

static ext_extent_idx_t *ext_extent_first_idx(ext_extent_header_t *hdr) {
    return (ext_extent_idx_t *)((uint8_t *)hdr + sizeof(*hdr));
}

static const ext_extent_idx_t *
ext_extent_first_idx_const(const ext_extent_header_t *hdr) {
    return (const ext_extent_idx_t *)((const uint8_t *)hdr + sizeof(*hdr));
}

static uint16_t ext_extent_len_get(const ext_extent_t *ext) {
    uint16_t raw = ext->ee_len;
    return raw > EXT4_EXT_INIT_MAX_LEN ? raw - EXT4_EXT_INIT_MAX_LEN : raw;
}

static bool ext_extent_is_unwritten(const ext_extent_t *ext) {
    return ext->ee_len > EXT4_EXT_INIT_MAX_LEN;
}

static uint64_t ext_extent_start_get(const ext_extent_t *ext) {
    return (uint64_t)ext->ee_start_lo | ((uint64_t)ext->ee_start_hi << 32);
}

static void ext_extent_start_set(ext_extent_t *ext, uint64_t block) {
    ext->ee_start_lo = (uint32_t)block;
    ext->ee_start_hi = (uint16_t)(block >> 32);
}

static uint64_t ext_extent_idx_leaf_get(const ext_extent_idx_t *idx) {
    return (uint64_t)idx->ei_leaf_lo | ((uint64_t)idx->ei_leaf_hi << 32);
}

static void ext_extent_idx_leaf_set(ext_extent_idx_t *idx, uint64_t block) {
    idx->ei_leaf_lo = (uint32_t)block;
    idx->ei_leaf_hi = (uint16_t)(block >> 32);
}

static void ext_extent_len_set(ext_extent_t *ext, uint16_t len) {
    ext->ee_len = len;
}

static bool ext_extent_header_valid(const ext_extent_header_t *hdr,
                                    uint16_t max_entries) {
    if (!hdr || hdr->eh_magic != EXT4_EXT_MAGIC)
        return false;
    if (hdr->eh_max > max_entries || hdr->eh_entries > hdr->eh_max)
        return false;
    if (hdr->eh_depth > EXT_EXTENT_MAX_DEPTH)
        return false;
    return true;
}

static uint32_t ext_extent_node_first_lblock(const ext_extent_header_t *hdr) {
    if (!hdr || !hdr->eh_entries)
        return 0;
    if (hdr->eh_depth)
        return ext_extent_first_idx_const(hdr)[0].ei_block;
    return ext_extent_first_const(hdr)[0].ee_block;
}

static void *ext_extent_node_entries(ext_extent_header_t *hdr) {
    return (uint8_t *)hdr + sizeof(*hdr);
}

static const void *
ext_extent_node_entries_const(const ext_extent_header_t *hdr) {
    return (const uint8_t *)hdr + sizeof(*hdr);
}

static size_t ext_extent_node_entry_size(const ext_extent_header_t *hdr) {
    return hdr && hdr->eh_depth ? sizeof(ext_extent_idx_t)
                                : sizeof(ext_extent_t);
}

static void ext_extent_path_release(ext_extent_path_t *path, uint16_t depth) {
    if (!path)
        return;

    for (uint16_t i = 1; i <= depth; i++) {
        free(path[i].buf);
        path[i].buf = NULL;
        path[i].hdr = NULL;
    }
}

static uint32_t ext_extent_next_lblock_from_path(ext_extent_path_t path[],
                                                 uint16_t depth) {
    if (!path)
        return UINT32_MAX;

    for (int level = depth; level > 0; level--) {
        ext_extent_header_t *parent = path[level - 1].hdr;
        uint16_t next_slot = path[level - 1].slot + 1;

        if (!parent || next_slot >= parent->eh_entries)
            continue;

        return ext_extent_first_idx(parent)[next_slot].ei_block;
    }

    return UINT32_MAX;
}

static void ext_inode_init_extent_root(ext_inode_disk_t *inode) {
    ext_extent_header_t *hdr;

    if (!inode)
        return;

    memset(inode->i_block, 0, sizeof(inode->i_block));
    inode->i_flags |= EXT4_EXTENTS_FL;
    hdr = ext_inode_extent_header(inode);
    hdr->eh_magic = EXT4_EXT_MAGIC;
    hdr->eh_entries = 0;
    hdr->eh_max = ext_extent_inode_max_entries();
    hdr->eh_depth = 0;
    hdr->eh_generation = 0;
}

static bool ext_inode_should_use_extents(ext_mount_ctx_t *fs, uint16_t mode,
                                         size_t payload_size) {
    if (!fs || !fs->has_extents)
        return false;

    switch (mode & S_IFMT) {
    case EXT2_S_IFREG:
    case EXT2_S_IFDIR:
        return true;
    case EXT2_S_IFLNK:
        return payload_size > sizeof(((ext_inode_disk_t *)0)->i_block);
    default:
        return false;
    }
}

static int ext_extent_load_path_locked(ext_mount_ctx_t *fs,
                                       ext_inode_disk_t *inode,
                                       uint32_t logical_block,
                                       ext_extent_path_t path[],
                                       uint16_t *depth_out) {
    ext_extent_header_t *hdr;
    uint16_t depth;
    int ret = 0;

    if (!fs || !inode || !path || !depth_out)
        return -EINVAL;

    memset(path, 0, sizeof(ext_extent_path_t) * (EXT_EXTENT_MAX_DEPTH + 1));
    hdr = ext_inode_extent_header(inode);
    if (!ext_extent_header_valid(hdr, ext_extent_inode_max_entries()))
        return -EIO;

    depth = hdr->eh_depth;
    path[0].hdr = hdr;

    for (uint16_t level = 0; level < depth; level++) {
        ext_extent_idx_t *idx;
        uint16_t entries = path[level].hdr->eh_entries;
        uint16_t slot = 0;
        uint64_t child;

        if (!entries)
            break;

        idx = ext_extent_first_idx(path[level].hdr);
        while (slot + 1 < entries && idx[slot + 1].ei_block <= logical_block)
            slot++;

        child = ext_extent_idx_leaf_get(&idx[slot]);
        path[level].slot = slot;
        path[level + 1].buf = calloc(1, fs->block_size);
        if (!path[level + 1].buf) {
            ret = -ENOMEM;
            goto fail;
        }

        ret = ext_read_block(fs, child, path[level + 1].buf);
        if (ret)
            goto fail;

        path[level + 1].block = child;
        path[level + 1].hdr = (ext_extent_header_t *)path[level + 1].buf;
        if (!ext_extent_header_valid(path[level + 1].hdr,
                                     ext_extent_block_max_entries(fs))) {
            ret = -EIO;
            goto fail;
        }
    }

    path[depth].slot = 0;
    if (path[depth].hdr->eh_entries) {
        ext_extent_t *ext = ext_extent_first(path[depth].hdr);
        while (path[depth].slot < path[depth].hdr->eh_entries &&
               ext[path[depth].slot].ee_block <= logical_block) {
            path[depth].slot++;
        }
    }

    *depth_out = depth;
    return 0;

fail:
    ext_extent_path_release(path, depth);
    return ret;
}

static int ext_extent_flush_path_locked(ext_mount_ctx_t *fs, uint32_t ino,
                                        const ext_inode_disk_t *inode,
                                        ext_extent_path_t path[],
                                        uint16_t depth) {
    for (int level = depth; level > 0; level--) {
        ext_extent_block_checksum_set(fs, ino, inode, path[level].buf);
        int ret = ext_write_block_cached_locked(fs, path[level].block,
                                                path[level].buf);
        if (ret)
            return ret;
    }
    return 0;
}

static int ext_extent_refresh_parents_locked(ext_mount_ctx_t *fs, uint32_t ino,
                                             const ext_inode_disk_t *inode,
                                             ext_extent_path_t path[],
                                             uint16_t depth) {
    for (int level = depth; level > 0; level--) {
        ext_extent_header_t *child = path[level].hdr;
        ext_extent_idx_t *parent_entries;
        uint16_t slot;

        if (!child->eh_entries)
            break;

        parent_entries = ext_extent_first_idx(path[level - 1].hdr);
        slot = path[level - 1].slot;
        if (slot >= path[level - 1].hdr->eh_entries)
            return -EIO;
        parent_entries[slot].ei_block = ext_extent_node_first_lblock(child);
    }

    return ext_extent_flush_path_locked(fs, ino, inode, path, depth);
}

static int ext_extent_grow_tree_locked(ext_mount_ctx_t *fs, uint32_t ino,
                                       ext_inode_disk_t *inode) {
    ext_extent_header_t *root;
    uint8_t *child_buf = NULL;
    ext_extent_header_t *child_hdr;
    uint64_t child_block = 0;
    uint16_t old_depth;
    size_t entry_size;
    int ret;

    if (!fs || !inode)
        return -EINVAL;

    root = ext_inode_extent_header(inode);
    if (!ext_extent_header_valid(root, ext_extent_inode_max_entries()))
        return -EIO;
    if (!root->eh_entries)
        return -EINVAL;
    if (root->eh_depth >= EXT_EXTENT_MAX_DEPTH)
        return -ENOSPC;

    old_depth = root->eh_depth;
    entry_size = ext_extent_node_entry_size(root);

    ret = ext_alloc_block_locked(fs, 0, &child_block);
    if (ret)
        return ret;

    child_buf = calloc(1, fs->block_size);
    if (!child_buf) {
        ext_free_block_locked(fs, child_block);
        return -ENOMEM;
    }

    child_hdr = (ext_extent_header_t *)child_buf;
    child_hdr->eh_magic = EXT4_EXT_MAGIC;
    child_hdr->eh_entries = root->eh_entries;
    child_hdr->eh_max = ext_extent_block_max_entries(fs);
    child_hdr->eh_depth = old_depth;
    child_hdr->eh_generation = root->eh_generation;
    memcpy(ext_extent_node_entries(child_hdr), ext_extent_node_entries(root),
           root->eh_entries * entry_size);

    ext_extent_block_checksum_set(fs, ino, inode, child_buf);
    ret = ext_write_block_cached_locked(fs, child_block, child_buf);
    if (ret) {
        free(child_buf);
        ext_free_block_locked(fs, child_block);
        return ret;
    }

    memset(inode->i_block, 0, sizeof(inode->i_block));
    root = ext_inode_extent_header(inode);
    root->eh_magic = EXT4_EXT_MAGIC;
    root->eh_entries = 1;
    root->eh_max = ext_extent_inode_max_entries();
    root->eh_depth = old_depth + 1;
    root->eh_generation = 0;
    ext_extent_first_idx(root)[0].ei_block =
        ext_extent_node_first_lblock(child_hdr);
    ext_extent_idx_leaf_set(&ext_extent_first_idx(root)[0], child_block);
    ext_inode_add_fs_blocks(fs, inode, 1);

    free(child_buf);
    return 0;
}

static int ext_extent_split_child_locked(ext_mount_ctx_t *fs, uint32_t ino,
                                         ext_inode_disk_t *inode,
                                         ext_extent_path_t path[],
                                         uint16_t parent_level,
                                         uint32_t prefer_group) {
    ext_extent_header_t *parent_hdr;
    ext_extent_header_t *child_hdr;
    ext_extent_header_t *new_hdr;
    ext_extent_idx_t *parent_idx;
    uint8_t *new_buf;
    uint64_t new_block = 0;
    uint16_t child_level = parent_level + 1;
    uint16_t entries;
    uint16_t split;
    uint16_t moved;
    uint16_t insert_slot;
    size_t entry_size;
    int ret;

    if (!fs || !inode || !path || child_level > EXT_EXTENT_MAX_DEPTH)
        return -EINVAL;

    parent_hdr = path[parent_level].hdr;
    child_hdr = path[child_level].hdr;
    if (!parent_hdr || !child_hdr || !parent_hdr->eh_depth)
        return -EIO;
    if (parent_hdr->eh_entries >= parent_hdr->eh_max)
        return -ENOSPC;
    if (!child_hdr->eh_entries)
        return -EIO;

    entries = child_hdr->eh_entries;
    split = entries / 2;
    if (!split)
        split = 1;
    if (split >= entries)
        split = entries - 1;
    moved = entries - split;
    entry_size = ext_extent_node_entry_size(child_hdr);

    ret = ext_alloc_block_locked(fs, prefer_group, &new_block);
    if (ret)
        return ret;

    new_buf = calloc(1, fs->block_size);
    if (!new_buf) {
        ext_free_block_locked(fs, new_block);
        return -ENOMEM;
    }

    new_hdr = (ext_extent_header_t *)new_buf;
    new_hdr->eh_magic = EXT4_EXT_MAGIC;
    new_hdr->eh_entries = moved;
    new_hdr->eh_max = ext_extent_block_max_entries(fs);
    new_hdr->eh_depth = child_hdr->eh_depth;
    new_hdr->eh_generation = child_hdr->eh_generation;
    memcpy(ext_extent_node_entries(new_hdr),
           (const uint8_t *)ext_extent_node_entries_const(child_hdr) +
               split * entry_size,
           moved * entry_size);

    parent_idx = ext_extent_first_idx(parent_hdr);
    insert_slot = path[parent_level].slot + 1;
    if (insert_slot > parent_hdr->eh_entries) {
        free(new_buf);
        ext_free_block_locked(fs, new_block);
        return -EIO;
    }

    ext_extent_block_checksum_set(fs, ino, inode, new_buf);
    ret = ext_write_block_cached_locked(fs, new_block, new_buf);
    if (ret) {
        free(new_buf);
        return ret;
    }

    child_hdr->eh_entries = split;
    memmove(&parent_idx[insert_slot + 1], &parent_idx[insert_slot],
            (parent_hdr->eh_entries - insert_slot) * sizeof(ext_extent_idx_t));
    parent_idx[insert_slot].ei_block = ext_extent_node_first_lblock(new_hdr);
    ext_extent_idx_leaf_set(&parent_idx[insert_slot], new_block);
    parent_hdr->eh_entries++;
    ext_inode_add_fs_blocks(fs, inode, 1);

    free(new_buf);
    return 0;
}

static int ext_extent_ensure_leaf_room_locked(ext_mount_ctx_t *fs, uint32_t ino,
                                              ext_inode_disk_t *inode,
                                              uint32_t logical_block) {
    uint32_t prefer_group;

    if (!fs || !inode)
        return -EINVAL;

    prefer_group = (ino - 1) / fs->sb.s_inodes_per_group;

    for (;;) {
        ext_extent_path_t path[EXT_EXTENT_MAX_DEPTH + 1];
        ext_extent_header_t *leaf_hdr;
        uint16_t depth = 0;
        bool split_done = false;
        int ret;

        ret =
            ext_extent_load_path_locked(fs, inode, logical_block, path, &depth);
        if (ret)
            return ret;

        leaf_hdr = path[depth].hdr;
        if (leaf_hdr->eh_entries < leaf_hdr->eh_max) {
            ext_extent_path_release(path, depth);
            return 0;
        }

        for (int level = (int)depth - 1; level >= 0; level--) {
            if (path[level].hdr->eh_entries < path[level].hdr->eh_max) {
                ret = ext_extent_split_child_locked(
                    fs, ino, inode, path, (uint16_t)level, prefer_group);
                if (!ret)
                    ret = ext_extent_flush_path_locked(fs, ino, inode, path,
                                                       depth);
                ext_extent_path_release(path, depth);
                if (ret)
                    return ret;
                split_done = true;
                break;
            }
        }

        if (split_done)
            continue;

        if (path[0].hdr->eh_entries >= path[0].hdr->eh_max) {
            ext_extent_path_release(path, depth);
            ret = ext_extent_grow_tree_locked(fs, ino, inode);
            if (ret)
                return ret;
            continue;
        }

        ext_extent_path_release(path, depth);
        return -EIO;
    }
}

static int ext_extent_map_blocks_locked(ext_mount_ctx_t *fs, uint32_t ino,
                                        ext_inode_disk_t *inode,
                                        uint32_t logical_block, bool create,
                                        uint64_t *out_block,
                                        uint32_t *out_run_blocks,
                                        uint32_t max_run_blocks) {
    ext_extent_path_t path[EXT_EXTENT_MAX_DEPTH + 1];
    ext_extent_header_t *leaf_hdr;
    ext_extent_t *ext;
    uint16_t depth = 0;
    uint16_t entries;
    uint16_t pos;
    int ret;

retry:
    ret = ext_extent_load_path_locked(fs, inode, logical_block, path, &depth);
    if (ret)
        return ret;

    if (!out_block || !out_run_blocks) {
        ext_extent_path_release(path, depth);
        return -EINVAL;
    }
    if (!max_run_blocks)
        max_run_blocks = 1;

    leaf_hdr = path[depth].hdr;
    entries = leaf_hdr->eh_entries;
    pos = path[depth].slot;
    ext = ext_extent_first(leaf_hdr);

    if (pos > 0) {
        ext_extent_t *prev = &ext[pos - 1];
        uint32_t prev_len = ext_extent_len_get(prev);
        uint32_t prev_start = prev->ee_block;
        if (logical_block >= prev_start &&
            logical_block < prev_start + prev_len) {
            uint32_t delta = logical_block - prev_start;
            *out_run_blocks = MIN(prev_len - delta, max_run_blocks);
            *out_block = ext_extent_is_unwritten(prev)
                             ? 0
                             : ext_extent_start_get(prev) + delta;
            ext_extent_path_release(path, depth);
            return 0;
        }
    }

    if (!create) {
        uint32_t hole = max_run_blocks;
        if (pos < entries) {
            hole = MIN(hole, ext[pos].ee_block - logical_block);
        } else if (depth > 0) {
            uint32_t next_lblock =
                ext_extent_next_lblock_from_path(path, depth);
            if (next_lblock != UINT32_MAX) {
                if (next_lblock <= logical_block) {
                    ext_extent_path_release(path, depth);
                    return -EIO;
                }
                hole = MIN(hole, next_lblock - logical_block);
            }
        }
        *out_block = 0;
        *out_run_blocks = hole ? hole : 1;
        ext_extent_path_release(path, depth);
        return 0;
    }

    if (entries >= leaf_hdr->eh_max) {
        ext_extent_path_release(path, depth);
        ret = ext_extent_ensure_leaf_room_locked(fs, ino, inode, logical_block);
        if (!ret)
            goto retry;
        return ret;
    }

    uint32_t prefer_group = (ino - 1) / fs->sb.s_inodes_per_group;
    uint32_t hole_limit = max_run_blocks;
    uint64_t goal_block = 0;
    uint64_t first_block = 0;
    uint32_t alloc_blocks = 0;

    if (pos > 0) {
        ext_extent_t *prev = &ext[pos - 1];
        uint32_t prev_len = ext_extent_len_get(prev);
        if (logical_block == prev->ee_block + prev_len)
            goal_block = ext_extent_start_get(prev) + prev_len;
    }
    if (pos < entries) {
        hole_limit = MIN(hole_limit, ext[pos].ee_block - logical_block);
    } else if (depth > 0) {
        uint32_t next_lblock = ext_extent_next_lblock_from_path(path, depth);
        if (next_lblock != UINT32_MAX) {
            if (next_lblock <= logical_block) {
                ext_extent_path_release(path, depth);
                return -EIO;
            }
            hole_limit = MIN(hole_limit, next_lblock - logical_block);
        }
    }
    hole_limit = MIN(hole_limit, (uint32_t)EXT4_EXT_INIT_MAX_LEN);
    if (!hole_limit)
        hole_limit = 1;

    ret = ext_alloc_blocks_locked(fs, prefer_group, hole_limit, goal_block,
                                  &first_block, &alloc_blocks);
    if (ret) {
        ext_extent_path_release(path, depth);
        return ret;
    }
    ext_inode_add_fs_blocks(fs, inode, alloc_blocks);

    if (pos > 0) {
        ext_extent_t *prev = &ext[pos - 1];
        uint32_t prev_len = ext_extent_len_get(prev);
        uint64_t prev_phys = ext_extent_start_get(prev);
        if (logical_block == prev->ee_block + prev_len &&
            prev_phys + prev_len == first_block &&
            prev_len + alloc_blocks <= EXT4_EXT_INIT_MAX_LEN) {
            ext_extent_len_set(prev, (uint16_t)(prev_len + alloc_blocks));
            *out_block = first_block;
            *out_run_blocks = alloc_blocks;
            ret =
                ext_extent_refresh_parents_locked(fs, ino, inode, path, depth);
            ext_extent_path_release(path, depth);
            return ret;
        }
    }

    memmove(&ext[pos + 1], &ext[pos], (entries - pos) * sizeof(ext_extent_t));
    ext[pos].ee_block = logical_block;
    ext_extent_len_set(&ext[pos], (uint16_t)alloc_blocks);
    ext_extent_start_set(&ext[pos], first_block);
    leaf_hdr->eh_entries++;
    *out_block = first_block;
    *out_run_blocks = alloc_blocks;
    ret = ext_extent_refresh_parents_locked(fs, ino, inode, path, depth);
    ext_extent_path_release(path, depth);
    return ret;
}

static int ext_inode_get_block_run_legacy_locked(
    ext_mount_ctx_t *fs, uint32_t ino, ext_inode_disk_t *inode,
    uint32_t logical_block, bool create, uint64_t *out_block,
    uint32_t *out_run_blocks, uint32_t max_run_blocks) {
    uint32_t offsets[4] = {0};
    uint32_t depth = 0;
    int ret = ext_lblock_path(fs, logical_block, offsets, &depth);
    if (ret)
        return ret;

    if (!out_block || !out_run_blocks)
        return -EINVAL;
    if (!max_run_blocks)
        max_run_blocks = 1;

    uint32_t prefer_group = (ino - 1) / fs->sb.s_inodes_per_group;

    if (depth == 1) {
        if (!inode->i_block[offsets[0]] && create) {
            uint64_t first = 0;
            ret = ext_alloc_block_locked(fs, prefer_group, &first);
            if (ret)
                return ret;
            inode->i_block[offsets[0]] = (uint32_t)first;
            ext_inode_add_fs_blocks(fs, inode, 1);
        }
        *out_block = inode->i_block[offsets[0]];
        *out_run_blocks = 1;

        uint32_t limit =
            MIN((uint32_t)EXT2_NDIR_BLOCKS - offsets[0], max_run_blocks);
        for (uint32_t run = 1; run < limit; run++) {
            uint32_t slot = inode->i_block[offsets[0] + run];
            if (!slot && create) {
                uint64_t new_block = 0;
                ret = ext_alloc_block_locked(fs, prefer_group, &new_block);
                if (ret)
                    return ret;
                slot = (uint32_t)new_block;
                inode->i_block[offsets[0] + run] = slot;
                ext_inode_add_fs_blocks(fs, inode, 1);
            }

            if (*out_block == 0) {
                if (slot != 0)
                    break;
            } else if ((uint64_t)slot != *out_block + run) {
                break;
            }
            *out_run_blocks = run + 1;
        }
        return 0;
    }

    uint32_t cur = inode->i_block[offsets[0]];
    if (!cur && create) {
        uint64_t new_block = 0;
        ret = ext_alloc_block_locked(fs, prefer_group, &new_block);
        if (ret)
            return ret;
        cur = (uint32_t)new_block;
        inode->i_block[offsets[0]] = cur;
        ext_inode_add_fs_blocks(fs, inode, 1);
    }
    if (!cur) {
        *out_block = 0;
        *out_run_blocks = 1;
        return 0;
    }

    uint8_t *buf = NULL;

    for (uint32_t level = 1; level < depth - 1; level++) {
        ret = ext_map_cache_ref_locked(fs, cur, &buf);
        if (ret)
            return ret;

        uint32_t *entries = (uint32_t *)buf;
        uint32_t next = entries[offsets[level]];
        if (!next && create) {
            uint64_t new_block = 0;
            ret = ext_alloc_block_locked(fs, prefer_group, &new_block);
            if (ret)
                return ret;
            next = (uint32_t)new_block;
            entries[offsets[level]] = next;
            ret = ext_write_block_cached_locked(fs, cur, buf);
            if (ret)
                return ret;
            ext_inode_add_fs_blocks(fs, inode, 1);
        }
        if (!next) {
            *out_block = 0;
            *out_run_blocks = 1;
            return 0;
        }
        cur = next;
    }

    ret = ext_map_cache_ref_locked(fs, cur, &buf);
    if (ret)
        return ret;

    uint32_t *entries = (uint32_t *)buf;
    uint32_t first = entries[offsets[depth - 1]];
    bool dirty = false;
    if (!first && create) {
        uint64_t new_block = 0;
        ret = ext_alloc_block_locked(fs, prefer_group, &new_block);
        if (ret)
            return ret;
        first = (uint32_t)new_block;
        entries[offsets[depth - 1]] = first;
        ext_inode_add_fs_blocks(fs, inode, 1);
        dirty = true;
    }

    *out_block = first;
    *out_run_blocks = 1;

    uint32_t limit =
        MIN(fs->ptrs_per_block - offsets[depth - 1], max_run_blocks);
    for (uint32_t run = 1; run < limit; run++) {
        uint32_t *slot = &entries[offsets[depth - 1] + run];
        if (!*slot && create) {
            uint64_t new_block = 0;
            ret = ext_alloc_block_locked(fs, prefer_group, &new_block);
            if (ret)
                return ret;
            *slot = (uint32_t)new_block;
            ext_inode_add_fs_blocks(fs, inode, 1);
            dirty = true;
        }

        if (first == 0) {
            if (*slot != 0)
                break;
        } else if ((uint64_t)*slot != *out_block + run) {
            break;
        }
        *out_run_blocks = run + 1;
    }

    if (dirty) {
        ret = ext_write_block_cached_locked(fs, cur, buf);
        if (ret)
            return ret;
    }

    return 0;
}

static int ext_inode_get_block_run_locked(ext_mount_ctx_t *fs, uint32_t ino,
                                          ext_inode_disk_t *inode,
                                          uint32_t logical_block, bool create,
                                          uint64_t *out_block,
                                          uint32_t *out_run_blocks,
                                          uint32_t max_run_blocks) {
    if (ext_inode_uses_extents(inode))
        return ext_extent_map_blocks_locked(fs, ino, inode, logical_block,
                                            create, out_block, out_run_blocks,
                                            max_run_blocks);

    return ext_inode_get_block_run_legacy_locked(
        fs, ino, inode, logical_block, create, out_block, out_run_blocks,
        max_run_blocks);
}

static int ext_inode_get_block_locked(ext_mount_ctx_t *fs, uint32_t ino,
                                      ext_inode_disk_t *inode,
                                      uint32_t logical_block, bool create,
                                      uint64_t *out_block) {
    uint32_t run_blocks = 0;
    return ext_inode_get_block_run_locked(fs, ino, inode, logical_block, create,
                                          out_block, &run_blocks, 1);
}

static int ext_inode_clear_block_legacy_locked(ext_mount_ctx_t *fs,
                                               ext_inode_disk_t *inode,
                                               uint32_t logical_block) {
    uint32_t offsets[4] = {0};
    uint32_t depth = 0;
    int ret = ext_lblock_path(fs, logical_block, offsets, &depth);
    if (ret)
        return ret;

    if (depth == 1) {
        uint64_t block = inode->i_block[offsets[0]];
        if (!block)
            return 0;
        ret = ext_free_block_locked(fs, block);
        if (ret)
            return ret;
        inode->i_block[offsets[0]] = 0;
        ext_inode_sub_fs_blocks(fs, inode, 1);
        return 0;
    }

    uint32_t blocks[3] = {0};
    uint8_t *bufs[3] = {0};
    uint32_t *entries[3] = {0};

    uint32_t cur = inode->i_block[offsets[0]];
    if (!cur)
        return 0;

    for (uint32_t level = 1; level < depth; level++) {
        bufs[level - 1] = calloc(1, fs->block_size);
        if (!bufs[level - 1]) {
            ret = -ENOMEM;
            goto cleanup;
        }
        ret = ext_map_cache_read_locked(fs, cur, bufs[level - 1]);
        if (ret)
            goto cleanup;
        blocks[level - 1] = cur;
        entries[level - 1] = (uint32_t *)bufs[level - 1];
        cur = entries[level - 1][offsets[level]];
        if (!cur) {
            ret = 0;
            goto cleanup;
        }
    }

    ret = ext_free_block_locked(fs, cur);
    if (ret)
        goto cleanup;
    ext_inode_sub_fs_blocks(fs, inode, 1);
    entries[depth - 2][offsets[depth - 1]] = 0;

    for (int level = (int)depth - 2; level >= 0; level--) {
        if (!ext_block_all_zero(bufs[level], fs->ptrs_per_block)) {
            ret = ext_write_block_cached_locked(fs, blocks[level], bufs[level]);
            if (ret)
                goto cleanup;
            break;
        }

        ret = ext_free_block_locked(fs, blocks[level]);
        if (ret)
            goto cleanup;
        ext_inode_sub_fs_blocks(fs, inode, 1);

        if (level == 0) {
            inode->i_block[offsets[0]] = 0;
            break;
        }
        entries[level - 1][offsets[level]] = 0;
    }

cleanup:
    for (size_t i = 0; i < 3; i++)
        free(bufs[i]);
    return ret;
}

static int ext_inode_clear_block_extent_locked(ext_mount_ctx_t *fs,
                                               uint32_t ino,
                                               ext_inode_disk_t *inode,
                                               uint32_t logical_block) {
    ext_extent_path_t path[EXT_EXTENT_MAX_DEPTH + 1];
    ext_extent_header_t *leaf_hdr;
    ext_extent_t *ext;
    uint16_t depth = 0;
    uint16_t pos;
    int ret;

    ret = ext_extent_load_path_locked(fs, inode, logical_block, path, &depth);
    if (ret)
        return ret;

    leaf_hdr = path[depth].hdr;
    pos = path[depth].slot;
    if (!pos) {
        ext_extent_path_release(path, depth);
        return 0;
    }

    ext = ext_extent_first(leaf_hdr);
    ext_extent_t *target = &ext[pos - 1];
    uint32_t len = ext_extent_len_get(target);
    uint32_t start = target->ee_block;
    uint64_t phys = ext_extent_start_get(target);
    uint32_t delta;

    if (logical_block < start || logical_block >= start + len) {
        ext_extent_path_release(path, depth);
        return 0;
    }

    delta = logical_block - start;
    if (delta > 0 && delta < len - 1 &&
        leaf_hdr->eh_entries >= leaf_hdr->eh_max) {
        ext_extent_path_release(path, depth);
        ret = ext_extent_ensure_leaf_room_locked(fs, ino, inode, logical_block);
        if (ret)
            return ret;
        return ext_inode_clear_block_extent_locked(fs, ino, inode,
                                                   logical_block);
    }

    ret = ext_free_block_locked(fs, phys + delta);
    if (ret) {
        ext_extent_path_release(path, depth);
        return ret;
    }
    ext_inode_sub_fs_blocks(fs, inode, 1);

    if (len == 1) {
        memmove(&ext[pos - 1], &ext[pos],
                (leaf_hdr->eh_entries - pos) * sizeof(ext_extent_t));
        leaf_hdr->eh_entries--;
    } else if (delta == 0) {
        target->ee_block++;
        ext_extent_start_set(target, phys + 1);
        ext_extent_len_set(target, (uint16_t)(len - 1));
    } else if (delta == len - 1) {
        ext_extent_len_set(target, (uint16_t)(len - 1));
    } else {
        memmove(&ext[pos + 1], &ext[pos],
                (leaf_hdr->eh_entries - pos) * sizeof(ext_extent_t));
        ext[pos].ee_block = logical_block + 1;
        ext_extent_len_set(&ext[pos], (uint16_t)(len - delta - 1));
        ext_extent_start_set(&ext[pos], phys + delta + 1);
        ext_extent_len_set(target, (uint16_t)delta);
        leaf_hdr->eh_entries++;
    }

    if (!leaf_hdr->eh_entries && depth == 1) {
        ext_extent_idx_t *root_idx = ext_extent_first_idx(path[0].hdr);

        ret = ext_free_block_locked(fs, path[1].block);
        if (ret) {
            ext_extent_path_release(path, depth);
            return ret;
        }
        ext_inode_sub_fs_blocks(fs, inode, 1);

        if (path[0].hdr->eh_entries > 1) {
            memmove(&root_idx[path[0].slot], &root_idx[path[0].slot + 1],
                    (path[0].hdr->eh_entries - path[0].slot - 1) *
                        sizeof(ext_extent_idx_t));
            path[0].hdr->eh_entries--;
        } else {
            ext_inode_init_extent_root(inode);
        }
        ext_extent_path_release(path, depth);
        return 0;
    }

    ret = ext_extent_refresh_parents_locked(fs, ino, inode, path, depth);
    ext_extent_path_release(path, depth);
    return ret;
}

static int ext_inode_clear_block_locked(ext_mount_ctx_t *fs, uint32_t ino,
                                        ext_inode_disk_t *inode,
                                        uint32_t logical_block) {
    if (ext_inode_uses_extents(inode))
        return ext_inode_clear_block_extent_locked(fs, ino, inode,
                                                   logical_block);

    return ext_inode_clear_block_legacy_locked(fs, inode, logical_block);
}

static int ext_zero_inode_tail_locked(ext_mount_ctx_t *fs, uint32_t ino,
                                      ext_inode_disk_t *inode,
                                      uint64_t new_size) {
    uint32_t tail;
    uint32_t lblock;
    uint64_t pblock = 0;
    uint8_t *buf;
    int ret;

    if (!fs || !inode || new_size == 0)
        return 0;

    tail = (uint32_t)(new_size % fs->block_size);
    if (!tail)
        return 0;

    lblock = (uint32_t)(new_size / fs->block_size);
    ret = ext_inode_get_block_locked(fs, ino, inode, lblock, false, &pblock);
    if (ret || !pblock)
        return ret;

    buf = calloc(1, fs->block_size);
    if (!buf)
        return -ENOMEM;

    ret = ext_read_block(fs, pblock, buf);
    if (!ret) {
        memset(buf + tail, 0, fs->block_size - tail);
        ret = ext_write_block(fs, pblock, buf);
    }

    free(buf);
    return ret;
}

static int ext_inode_truncate_locked(ext_mount_ctx_t *fs, uint32_t ino,
                                     ext_inode_disk_t *inode,
                                     uint64_t new_size) {
    return ext_inode_resize_locked(fs, ino, inode, new_size, true);
}

static int ext_inode_resize_locked(ext_mount_ctx_t *fs, uint32_t ino,
                                   ext_inode_disk_t *inode, uint64_t new_size,
                                   bool write_back) {
    uint64_t old_size = ext_inode_size_get(inode);
    uint64_t old_blocks = (old_size + fs->block_size - 1) / fs->block_size;
    uint64_t new_blocks = (new_size + fs->block_size - 1) / fs->block_size;
    int ret;

    if (new_size < old_size) {
        ret = ext_zero_inode_tail_locked(fs, ino, inode, new_size);
        if (ret)
            return ret;
    }

    while (old_blocks > new_blocks) {
        ret = ext_inode_clear_block_locked(fs, ino, inode,
                                           (uint32_t)(old_blocks - 1));
        if (ret)
            return ret;
        old_blocks--;
    }

    ext_inode_size_set(inode, new_size);
    ext_inode_touch(inode, false, true, true);
    return write_back ? ext_write_inode(fs, ino, inode) : 0;
}

static int ext_read_inode_data_locked(ext_mount_ctx_t *fs, uint32_t ino,
                                      ext_inode_disk_t *inode, void *buf,
                                      size_t offset, size_t size) {
    uint64_t file_size = ext_inode_size_get(inode);
    if (offset >= file_size)
        return 0;

    size_t total = MIN((size_t)(file_size - offset), size);
    size_t done = 0;
    uint8_t *block = calloc(1, fs->block_size);
    if (!block)
        return -ENOMEM;

    while (done < total) {
        uint64_t pos = offset + done;
        uint32_t lblock = (uint32_t)(pos / fs->block_size);
        uint32_t boff = (uint32_t)(pos % fs->block_size);
        size_t chunk = MIN((size_t)fs->block_size - boff, total - done);

        if (boff == 0 && chunk == fs->block_size) {
            uint64_t pblock = 0;
            uint32_t run_blocks = 0;
            uint32_t max_blocks = (uint32_t)((total - done) / fs->block_size);
            int ret = ext_inode_get_block_run_locked(fs, ino, inode, lblock,
                                                     false, &pblock,
                                                     &run_blocks, max_blocks);
            if (ret) {
                free(block);
                return ret;
            }
            size_t run_size = (size_t)run_blocks * fs->block_size;

            if (!pblock) {
                memset((uint8_t *)buf + done, 0, run_size);
            } else {
                ret = ext_dev_read_direct(fs, pblock * fs->block_size,
                                          (uint8_t *)buf + done, run_size);
                if (ret) {
                    free(block);
                    return ret;
                }
            }

            done += run_size;
            continue;
        }

        uint64_t pblock = 0;
        int ret =
            ext_inode_get_block_locked(fs, ino, inode, lblock, false, &pblock);
        if (ret) {
            free(block);
            return ret;
        }
        if (!pblock) {
            memset((uint8_t *)buf + done, 0, chunk);
        } else {
            ret = ext_dev_read_direct(fs, pblock * fs->block_size, block,
                                      fs->block_size);
            if (ret) {
                free(block);
                return ret;
            }
            memcpy((uint8_t *)buf + done, block + boff, chunk);
        }
        done += chunk;
    }

    free(block);
    return (int)done;
}

static int ext_quota_read_locked(ext_mount_ctx_t *fs, uint32_t quota_ino,
                                 ext_inode_disk_t *quota_inode, uint32_t block,
                                 void *buf) {
    int ret;

    if (!buf)
        return -EINVAL;

    ret = ext_read_inode_data_locked(fs, quota_ino, quota_inode, buf,
                                     (size_t)block * EXT_QUOTA_BLOCK_SIZE,
                                     EXT_QUOTA_BLOCK_SIZE);
    if (ret < 0)
        return ret;
    if ((size_t)ret != EXT_QUOTA_BLOCK_SIZE)
        return -EIO;
    return 0;
}

static unsigned int ext_quota_tree_depth(void) {
    uint64_t entries = EXT_QUOTA_PTRS_PER_BLOCK;
    unsigned int depth = 1;

    while (entries < (1ULL << 32)) {
        entries *= EXT_QUOTA_PTRS_PER_BLOCK;
        depth++;
    }

    return depth;
}

static void ext_quota_tree_path(uint32_t id, uint32_t *path,
                                unsigned int depth) {
    uint64_t div = 1;

    for (unsigned int i = 1; i < depth; i++)
        div *= EXT_QUOTA_PTRS_PER_BLOCK;

    for (unsigned int i = 0; i < depth; i++) {
        path[i] = (uint32_t)((id / div) % EXT_QUOTA_PTRS_PER_BLOCK);
        if (div > 1)
            div /= EXT_QUOTA_PTRS_PER_BLOCK;
    }
}

static int ext_quota_find_data_block_locked(ext_mount_ctx_t *fs,
                                            uint32_t quota_ino,
                                            ext_inode_disk_t *quota_inode,
                                            uint32_t id, uint32_t *block_out) {
    uint8_t block[EXT_QUOTA_BLOCK_SIZE];
    uint32_t tree_path[8] = {0};
    uint32_t block_no = EXT_QUOTA_TREEOFF;
    unsigned int depth = ext_quota_tree_depth();
    int ret;

    if (!block_out)
        return -EINVAL;
    if (depth > sizeof(tree_path) / sizeof(tree_path[0]))
        return -EIO;

    ext_quota_tree_path(id, tree_path, depth);

    for (unsigned int level = 0; level < depth; level++) {
        uint32_t *entries;

        ret =
            ext_quota_read_locked(fs, quota_ino, quota_inode, block_no, block);
        if (ret)
            return ret;

        entries = (uint32_t *)(block + EXT_QUOTA_BLOCK_HEADER_SIZE);
        block_no = entries[tree_path[level]];
        if (!block_no) {
            *block_out = 0;
            return 0;
        }
    }

    *block_out = block_no;
    return 0;
}

static bool ext_quota_entry_empty(const ext_quota_disk_entry_t *entry) {
    return entry && entry->dqb_id == 0 && entry->dqb_pad == 0 &&
           entry->dqb_ihardlimit == 0 && entry->dqb_isoftlimit == 0 &&
           entry->dqb_curinodes == 0 && entry->dqb_bhardlimit == 0 &&
           entry->dqb_bsoftlimit == 0 && entry->dqb_curspace == 0 &&
           entry->dqb_btime == 0 && entry->dqb_itime == 0;
}

static int ext_quota_extract_bhardlimit_locked(
    ext_mount_ctx_t *fs, uint32_t quota_ino, ext_inode_disk_t *quota_inode,
    uint32_t id, uint64_t *bhardlimit_out, uint64_t *bsoftlimit_out,
    uint32_t *valid_out) {
    uint8_t block[EXT_QUOTA_BLOCK_SIZE];
    uint64_t quota_size;
    uint32_t data_block = 0;
    int ret;

    if (!bhardlimit_out || !bsoftlimit_out || !valid_out)
        return -EINVAL;

    *bhardlimit_out = 0;
    *bsoftlimit_out = 0;
    *valid_out = QIF_BLIMITS;
    quota_size = ext_inode_size_get(quota_inode);

    ret = ext_quota_find_data_block_locked(fs, quota_ino, quota_inode, id,
                                           &data_block);
    if (ret)
        return ret;

    if (data_block) {
        ret = ext_quota_read_locked(fs, quota_ino, quota_inode, data_block,
                                    block);
        if (ret)
            return ret;

        for (uint32_t i = 0; i < EXT_QUOTA_ENTRIES_PER_BLOCK; i++) {
            ext_quota_disk_entry_t *entry =
                (ext_quota_disk_entry_t *)(block + EXT_QUOTA_BLOCK_HEADER_SIZE +
                                           i * sizeof(ext_quota_disk_entry_t));
            if (entry->dqb_id != id)
                continue;
            if (id == 0 && ext_quota_entry_empty(entry))
                continue;
            *bhardlimit_out = entry->dqb_bhardlimit;
            *bsoftlimit_out = entry->dqb_bsoftlimit;
            return 0;
        }
    }

    for (uint64_t off = EXT_QUOTA_BLOCK_SIZE;
         off + EXT_QUOTA_BLOCK_SIZE <= quota_size;
         off += EXT_QUOTA_BLOCK_SIZE) {
        ext_quota_block_header_t *hdr;

        ret = ext_read_inode_data_locked(fs, quota_ino, quota_inode, block,
                                         (size_t)off, EXT_QUOTA_BLOCK_SIZE);
        if (ret < 0)
            return ret;
        if ((size_t)ret != EXT_QUOTA_BLOCK_SIZE)
            return -EIO;

        hdr = (ext_quota_block_header_t *)block;
        if (hdr->dqdh_entries > EXT_QUOTA_ENTRIES_PER_BLOCK)
            continue;

        for (uint32_t i = 0; i < EXT_QUOTA_ENTRIES_PER_BLOCK; i++) {
            ext_quota_disk_entry_t *entry =
                (ext_quota_disk_entry_t *)(block + EXT_QUOTA_BLOCK_HEADER_SIZE +
                                           i * sizeof(ext_quota_disk_entry_t));
            if (entry->dqb_id != id)
                continue;
            if (id == 0 && ext_quota_entry_empty(entry))
                continue;
            *bhardlimit_out = entry->dqb_bhardlimit;
            *bsoftlimit_out = entry->dqb_bsoftlimit;
            return 0;
        }
    }

    return 0;
}

static int ext_get_quota(struct vfs_super_block *sb, unsigned int type,
                         uint32_t id, uint64_t *bhardlimit,
                         uint64_t *bsoftlimit, uint32_t *valid) {
    ext_mount_ctx_t *fs = ext_sb_info(sb);
    ext_inode_disk_t quota_inode = {0};
    ext_quota_header_t header = {0};
    uint32_t quota_ino;
    int ret;

    if (!fs || !bhardlimit || !bsoftlimit || !valid)
        return -EINVAL;
    if (type != USRQUOTA)
        return -EOPNOTSUPP;

    quota_ino = fs->sb.s_usr_quota_inum;
    if (!quota_ino)
        return -ESRCH;

    spin_lock(&fs->lock);
    ret = ext_read_inode(fs, quota_ino, &quota_inode);
    if (ret)
        goto out;

    ret = ext_read_inode_data_locked(fs, quota_ino, &quota_inode, &header, 0,
                                     sizeof(header));
    if (ret < 0)
        goto out;
    if ((size_t)ret != sizeof(header)) {
        ret = -EIO;
        goto out;
    }
    if (header.dqh_magic != EXT_QUOTA_USER_MAGIC) {
        ret = -ESRCH;
        goto out;
    }

    ret = ext_quota_extract_bhardlimit_locked(fs, quota_ino, &quota_inode, id,
                                              bhardlimit, bsoftlimit, valid);

out:
    spin_unlock(&fs->lock);
    return ret;
}

static int ext_write_inode_data_locked(ext_mount_ctx_t *fs, uint32_t ino,
                                       ext_inode_disk_t *inode, const void *buf,
                                       size_t offset, size_t size) {
    size_t done = 0;
    uint8_t *block = calloc(1, fs->block_size);
    if (!block)
        return -ENOMEM;

    while (done < size) {
        uint64_t pos = offset + done;
        uint32_t lblock = (uint32_t)(pos / fs->block_size);
        uint32_t boff = (uint32_t)(pos % fs->block_size);
        size_t chunk = MIN((size_t)fs->block_size - boff, size - done);

        if (boff == 0 && chunk == fs->block_size) {
            uint64_t pblock = 0;
            uint32_t run_blocks = 0;
            uint32_t max_blocks = (uint32_t)((size - done) / fs->block_size);
            int ret = ext_inode_get_block_run_locked(
                fs, ino, inode, lblock, true, &pblock, &run_blocks, max_blocks);
            if (ret) {
                free(block);
                return ret;
            }
            if (!pblock) {
                free(block);
                return -ENOSPC;
            }
            size_t run_size = (size_t)run_blocks * fs->block_size;

            ret = ext_dev_write_direct(fs, pblock * fs->block_size,
                                       (const uint8_t *)buf + done, run_size);
            if (ret) {
                free(block);
                return ret;
            }

            done += run_size;
            continue;
        }

        uint64_t pblock = 0;
        int ret =
            ext_inode_get_block_locked(fs, ino, inode, lblock, true, &pblock);
        if (ret) {
            free(block);
            return ret;
        }
        if (!pblock) {
            free(block);
            return -ENOSPC;
        }

        if (chunk != fs->block_size || boff != 0) {
            ret = ext_dev_read_direct(fs, pblock * fs->block_size, block,
                                      fs->block_size);
            if (ret) {
                free(block);
                return ret;
            }
        } else {
            memset(block, 0, fs->block_size);
        }

        memcpy(block + boff, (const uint8_t *)buf + done, chunk);
        ret = ext_dev_write_direct(fs, pblock * fs->block_size, block,
                                   fs->block_size);
        if (ret) {
            free(block);
            return ret;
        }
        done += chunk;
    }

    uint64_t end = (uint64_t)offset + size;
    if (end > ext_inode_size_get(inode))
        ext_inode_size_set(inode, end);
    ext_inode_touch(inode, false, true, true);
    free(block);
    return (int)done;
}

static int ext_dir_find_locked(ext_mount_ctx_t *fs, uint32_t dir_ino,
                               ext_inode_disk_t *dir_inode, const char *name,
                               ext_dir_lookup_t *result) {
    size_t name_len;

    if (!dir_inode)
        return -EINVAL;
    if (!fs || !name || !result)
        return -EINVAL;

    name_len = strlen(name);
    if (name_len > 255)
        return -ENAMETOOLONG;

    uint64_t dir_size = ext_inode_size_get(dir_inode);
    uint32_t blocks =
        (uint32_t)((dir_size + fs->block_size - 1) / fs->block_size);
    uint8_t *buf = calloc(1, fs->block_size);
    ext_dir_block_ctx_t block_ctx;
    if (!buf)
        return -ENOMEM;

    memset(result, 0, sizeof(*result));

    for (uint32_t lblock = 0; lblock < blocks; lblock++) {
        int ret = ext_dir_block_prepare(fs, dir_ino, dir_inode, lblock, buf,
                                        &block_ctx);
        if (ret) {
            free(buf);
            return ret;
        }
        if (!block_ctx.pblock)
            continue;

        bool has_prev = false;
        uint16_t prev_off = 0;
        uint16_t prev_rec = 0;
        uint32_t off = 0;
        if (block_ctx.indexed_root) {
            ext_dir_entry_t *dot = (ext_dir_entry_t *)buf;
            ext_dir_entry_t *dotdot = (ext_dir_entry_t *)(buf + dot->rec_len);
            off = dot->rec_len + dotdot->rec_len;
        }
        for (; off + sizeof(ext_dir_entry_t) <= block_ctx.data_size;) {
            ext_dir_entry_t *entry = (ext_dir_entry_t *)(buf + off);
            if (!ext_dir_entry_valid(entry, off, block_ctx.data_size)) {
                free(buf);
                return -EIO;
            }
            if (ext_dir_entry_name_eq(entry, name, name_len)) {
                result->found = true;
                result->inode = entry->inode;
                result->file_type = entry->file_type;
                result->lblock = lblock;
                result->offset = off;
                result->rec_len = entry->rec_len;
                result->has_prev = has_prev;
                result->prev_offset = prev_off;
                result->prev_rec_len = prev_rec;
                free(buf);
                return 0;
            }
            has_prev = true;
            prev_off = off;
            prev_rec = entry->rec_len;
            off += entry->rec_len;
        }
    }

    free(buf);
    return 0;
}

static int ext_dir_add_entry_locked(ext_mount_ctx_t *fs, uint32_t dir_ino,
                                    ext_inode_disk_t *dir_inode,
                                    uint32_t child_ino, const char *name,
                                    uint8_t file_type) {
    size_t name_len = strlen(name);
    ext_dir_lookup_t lookup = {0};
    int ret;

    if (!name_len || name_len > 255)
        return -EINVAL;
    ret = ext_require_mutable_linear_dir(dir_inode);
    if (ret)
        return ret;

    ret = ext_dir_find_locked(fs, dir_ino, dir_inode, name, &lookup);
    if (ret)
        return ret;
    if (lookup.found)
        return -EEXIST;

    uint16_t need = ext_dir_rec_len(name_len);
    uint64_t dir_size = ext_inode_size_get(dir_inode);
    uint32_t blocks =
        (uint32_t)((dir_size + fs->block_size - 1) / fs->block_size);
    uint8_t *buf = calloc(1, fs->block_size);
    ext_dir_block_ctx_t block_ctx;
    if (!buf)
        return -ENOMEM;

    for (uint32_t lblock = 0; lblock < blocks; lblock++) {
        ret = ext_dir_block_prepare(fs, dir_ino, dir_inode, lblock, buf,
                                    &block_ctx);
        if (ret) {
            free(buf);
            return ret;
        }
        if (!block_ctx.pblock)
            continue;

        uint32_t off = 0;
        if (block_ctx.indexed_root) {
            ext_dir_entry_t *dot = (ext_dir_entry_t *)buf;
            ext_dir_entry_t *dotdot = (ext_dir_entry_t *)(buf + dot->rec_len);
            off = dot->rec_len + dotdot->rec_len;
        }
        for (; off + sizeof(ext_dir_entry_t) <= block_ctx.data_size;) {
            ext_dir_entry_t *entry = (ext_dir_entry_t *)(buf + off);
            if (!ext_dir_entry_valid(entry, off, block_ctx.data_size)) {
                free(buf);
                return -EIO;
            }

            if (!entry->inode && entry->rec_len >= need) {
                uint16_t full_rec_len = entry->rec_len;
                memset(entry, 0, full_rec_len);
                entry->inode = child_ino;
                entry->rec_len = full_rec_len;
                entry->name_len = (uint8_t)name_len;
                entry->file_type = file_type;
                memcpy(entry->name, name, name_len);
                ret = ext_dir_block_write_locked(fs, dir_ino, dir_inode,
                                                 &block_ctx, buf);
                if (ret) {
                    free(buf);
                    return ret;
                }
                ext_inode_touch(dir_inode, false, true, true);
                free(buf);
                return ext_write_inode(fs, dir_ino, dir_inode);
            }

            if (entry->inode) {
                uint16_t ideal = ext_dir_rec_len(entry->name_len);
                if (ideal <= entry->rec_len && entry->rec_len >= ideal + need) {
                    uint16_t old_rec = entry->rec_len;
                    entry->rec_len = ideal;
                    ext_dir_entry_t *new_entry =
                        (ext_dir_entry_t *)((uint8_t *)entry + ideal);
                    memset(new_entry, 0, old_rec - ideal);
                    new_entry->inode = child_ino;
                    new_entry->rec_len = old_rec - ideal;
                    new_entry->name_len = (uint8_t)name_len;
                    new_entry->file_type = file_type;
                    memcpy(new_entry->name, name, name_len);
                    ret = ext_dir_block_write_locked(fs, dir_ino, dir_inode,
                                                     &block_ctx, buf);
                    if (ret) {
                        free(buf);
                        return ret;
                    }
                    ext_inode_touch(dir_inode, false, true, true);
                    free(buf);
                    return ext_write_inode(fs, dir_ino, dir_inode);
                }
            }
            off += entry->rec_len;
        }
    }

    uint32_t new_lblock = blocks;
    uint64_t pblock = 0;
    ret = ext_inode_get_block_locked(fs, dir_ino, dir_inode, new_lblock, true,
                                     &pblock);
    if (ret) {
        free(buf);
        return ret;
    }
    memset(buf, 0, fs->block_size);
    ext_dir_entry_t *entry = (ext_dir_entry_t *)buf;
    entry->inode = child_ino;
    entry->rec_len = (uint16_t)fs->block_size;
    entry->name_len = (uint8_t)name_len;
    entry->file_type = file_type;
    memcpy(entry->name, name, name_len);
    memset(&block_ctx, 0, sizeof(block_ctx));
    block_ctx.pblock = pblock;
    block_ctx.lblock = new_lblock;
    block_ctx.data_size = fs->block_size;
    if (ext_dir_has_tail(fs, dir_inode, fs->block_size)) {
        entry->rec_len =
            (uint16_t)(fs->block_size - sizeof(ext_dir_entry_tail_t));
        block_ctx.data_size = fs->block_size - sizeof(ext_dir_entry_tail_t);
        block_ctx.has_tail = true;
    }
    ret = ext_dir_block_write_locked(fs, dir_ino, dir_inode, &block_ctx, buf);
    if (ret) {
        free(buf);
        return ret;
    }
    if ((uint64_t)(new_lblock + 1) * fs->block_size >
        ext_inode_size_get(dir_inode))
        ext_inode_size_set(dir_inode,
                           (uint64_t)(new_lblock + 1) * fs->block_size);
    ext_inode_touch(dir_inode, false, true, true);
    free(buf);
    return ext_write_inode(fs, dir_ino, dir_inode);
}

static int ext_dir_remove_entry_locked(ext_mount_ctx_t *fs, uint32_t dir_ino,
                                       ext_inode_disk_t *dir_inode,
                                       const char *name,
                                       ext_dir_lookup_t *removed) {
    ext_dir_lookup_t lookup = {0};
    int ret = ext_require_mutable_linear_dir(dir_inode);
    if (ret)
        return ret;

    ret = ext_dir_find_locked(fs, dir_ino, dir_inode, name, &lookup);
    if (ret)
        return ret;
    if (!lookup.found)
        return -ENOENT;

    uint8_t *buf = calloc(1, fs->block_size);
    ext_dir_block_ctx_t block_ctx;
    if (!buf)
        return -ENOMEM;

    ret = ext_dir_block_prepare(fs, dir_ino, dir_inode, lookup.lblock, buf,
                                &block_ctx);
    if (ret) {
        free(buf);
        return ret;
    }
    if (!block_ctx.pblock) {
        free(buf);
        return -EIO;
    }

    ext_dir_entry_t *entry = (ext_dir_entry_t *)(buf + lookup.offset);
    if (lookup.has_prev) {
        ext_dir_entry_t *prev = (ext_dir_entry_t *)(buf + lookup.prev_offset);
        prev->rec_len += entry->rec_len;
    } else {
        uint16_t rec_len = entry->rec_len;
        memset(entry, 0, rec_len);
        entry->rec_len = rec_len;
    }

    ret = ext_dir_block_write_locked(fs, dir_ino, dir_inode, &block_ctx, buf);
    free(buf);
    if (ret)
        return ret;

    ext_inode_touch(dir_inode, false, true, true);
    ret = ext_write_inode(fs, dir_ino, dir_inode);
    if (!ret && removed)
        *removed = lookup;
    return ret;
}

static int ext_dir_replace_entry_locked(ext_mount_ctx_t *fs, uint32_t dir_ino,
                                        ext_inode_disk_t *dir_inode,
                                        const char *name, uint32_t child_ino,
                                        uint8_t file_type) {
    ext_dir_lookup_t lookup = {0};
    int ret = ext_require_mutable_linear_dir(dir_inode);
    if (ret)
        return ret;

    ret = ext_dir_find_locked(fs, dir_ino, dir_inode, name, &lookup);
    if (ret)
        return ret;
    if (!lookup.found)
        return -ENOENT;

    uint8_t *buf = calloc(1, fs->block_size);
    ext_dir_block_ctx_t block_ctx;
    if (!buf)
        return -ENOMEM;

    ret = ext_dir_block_prepare(fs, dir_ino, dir_inode, lookup.lblock, buf,
                                &block_ctx);
    if (ret) {
        free(buf);
        return ret;
    }
    if (!block_ctx.pblock) {
        free(buf);
        return -EIO;
    }

    ext_dir_entry_t *entry = (ext_dir_entry_t *)(buf + lookup.offset);
    entry->inode = child_ino;
    entry->file_type = file_type;

    ret = ext_dir_block_write_locked(fs, dir_ino, dir_inode, &block_ctx, buf);
    free(buf);
    if (ret)
        return ret;

    ext_inode_touch(dir_inode, false, true, true);
    return ext_write_inode(fs, dir_ino, dir_inode);
}

static int ext_dir_is_empty_locked(ext_mount_ctx_t *fs, uint32_t dir_ino,
                                   ext_inode_disk_t *dir_inode) {
    uint64_t dir_size = ext_inode_size_get(dir_inode);
    uint32_t blocks =
        (uint32_t)((dir_size + fs->block_size - 1) / fs->block_size);
    uint8_t *buf = calloc(1, fs->block_size);
    ext_dir_block_ctx_t block_ctx;
    int ret;
    if (!buf)
        return -ENOMEM;

    ret = ext_require_mutable_linear_dir(dir_inode);
    if (ret) {
        free(buf);
        return ret;
    }

    for (uint32_t lblock = 0; lblock < blocks; lblock++) {
        ret = ext_dir_block_prepare(fs, dir_ino, dir_inode, lblock, buf,
                                    &block_ctx);
        if (ret) {
            free(buf);
            return ret;
        }
        if (!block_ctx.pblock)
            continue;
        uint32_t off = 0;
        if (block_ctx.indexed_root) {
            ext_dir_entry_t *dot = (ext_dir_entry_t *)buf;
            ext_dir_entry_t *dotdot = (ext_dir_entry_t *)(buf + dot->rec_len);
            off = dot->rec_len + dotdot->rec_len;
        }
        for (; off + sizeof(ext_dir_entry_t) <= block_ctx.data_size;) {
            ext_dir_entry_t *entry = (ext_dir_entry_t *)(buf + off);
            if (!ext_dir_entry_valid(entry, off, block_ctx.data_size)) {
                free(buf);
                return -EIO;
            }
            if (entry->inode) {
                bool is_dot = entry->name_len == 1 && entry->name[0] == '.';
                bool is_dotdot = entry->name_len == 2 &&
                                 entry->name[0] == '.' && entry->name[1] == '.';
                if (!is_dot && !is_dotdot) {
                    free(buf);
                    return 0;
                }
            }
            off += entry->rec_len;
        }
    }

    free(buf);
    return 1;
}

static int ext_drop_link_locked(ext_mount_ctx_t *fs, uint32_t ino,
                                ext_inode_disk_t *inode,
                                struct vfs_inode *cached_inode);

static int ext_delete_directory_contents_locked(ext_mount_ctx_t *fs,
                                                uint32_t dir_ino,
                                                ext_inode_disk_t *dir_inode) {
    uint64_t dir_size = ext_inode_size_get(dir_inode);
    uint32_t blocks =
        (uint32_t)((dir_size + fs->block_size - 1) / fs->block_size);
    uint8_t *buf = calloc(1, fs->block_size);
    int ret;
    if (!buf)
        return -ENOMEM;

    ret = ext_require_mutable_linear_dir(dir_inode);
    if (ret) {
        free(buf);
        return ret;
    }

    for (uint32_t lblock = 0; lblock < blocks; lblock++) {
        uint64_t pblock = 0;
        ret = ext_inode_get_block_locked(fs, dir_ino, dir_inode, lblock, false,
                                         &pblock);
        if (ret) {
            free(buf);
            return ret;
        }
        if (!pblock)
            continue;
        ret = ext_read_block(fs, pblock, buf);
        if (ret) {
            free(buf);
            return ret;
        }
        for (uint32_t off = 0;
             off + sizeof(ext_dir_entry_t) <= fs->block_size;) {
            ext_dir_entry_t *entry = (ext_dir_entry_t *)(buf + off);
            if (!ext_dir_entry_valid(entry, off, fs->block_size)) {
                free(buf);
                return -EIO;
            }
            if (entry->inode) {
                bool is_dot = entry->name_len == 1 && entry->name[0] == '.';
                bool is_dotdot = entry->name_len == 2 &&
                                 entry->name[0] == '.' && entry->name[1] == '.';
                if (!is_dot && !is_dotdot) {
                    char name[256];
                    size_t name_len = MIN(entry->name_len, 255);
                    memcpy(name, entry->name, name_len);
                    name[name_len] = '\0';

                    ext_inode_disk_t child_inode = {0};
                    ret = ext_read_inode(fs, entry->inode, &child_inode);
                    if (ret) {
                        free(buf);
                        return ret;
                    }

                    if ((child_inode.i_mode & S_IFMT) == EXT2_S_IFDIR) {
                        ret = ext_delete_directory_contents_locked(
                            fs, entry->inode, &child_inode);
                        if (ret) {
                            free(buf);
                            return ret;
                        }
                    }

                    uint32_t child_ino = entry->inode;
                    ret = ext_dir_remove_entry_locked(fs, dir_ino, dir_inode,
                                                      name, NULL);
                    if (ret) {
                        free(buf);
                        return ret;
                    }
                    entry->inode = 0;

                    ret =
                        ext_drop_link_locked(fs, child_ino, &child_inode, NULL);
                    if (ret) {
                        free(buf);
                        return ret;
                    }
                }
            }
            off += entry->rec_len;
        }
    }

    free(buf);
    return 0;
}

static int ext_dir_set_dotdot_locked(ext_mount_ctx_t *fs, uint32_t dir_ino,
                                     ext_inode_disk_t *dir_inode,
                                     uint32_t new_parent_ino) {
    uint64_t dir_size = ext_inode_size_get(dir_inode);
    uint32_t blocks =
        (uint32_t)((dir_size + fs->block_size - 1) / fs->block_size);
    uint8_t *buf = calloc(1, fs->block_size);
    ext_dir_block_ctx_t block_ctx;
    int ret;
    if (!buf)
        return -ENOMEM;

    ret = ext_require_mutable_linear_dir(dir_inode);
    if (ret) {
        free(buf);
        return ret;
    }

    for (uint32_t lblock = 0; lblock < blocks; lblock++) {
        ret = ext_dir_block_prepare(fs, dir_ino, dir_inode, lblock, buf,
                                    &block_ctx);
        if (ret) {
            free(buf);
            return ret;
        }
        if (!block_ctx.pblock)
            continue;
        uint32_t off = 0;
        for (; off + sizeof(ext_dir_entry_t) <= block_ctx.data_size;) {
            ext_dir_entry_t *entry = (ext_dir_entry_t *)(buf + off);
            if (!ext_dir_entry_valid(entry, off, block_ctx.data_size)) {
                free(buf);
                return -EIO;
            }
            bool is_dotdot = entry->inode && entry->name_len == 2 &&
                             entry->name[0] == '.' && entry->name[1] == '.';
            if (is_dotdot) {
                entry->inode = new_parent_ino;
                ret = ext_dir_block_write_locked(fs, dir_ino, dir_inode,
                                                 &block_ctx, buf);
                free(buf);
                return ret;
            }
            off += entry->rec_len;
        }
    }

    free(buf);
    return -ENOENT;
}

static int ext_release_inode_locked(ext_mount_ctx_t *fs, uint32_t ino,
                                    ext_inode_disk_t *inode) {
    uint64_t blocks = 0;
    if (!((inode->i_mode & S_IFMT) == EXT2_S_IFLNK &&
          ext_inode_blocks_get(inode) == 0)) {
        blocks =
            (ext_inode_size_get(inode) + fs->block_size - 1) / fs->block_size;
    }
    while (blocks > 0) {
        int ret = ext_inode_clear_block_locked(fs, ino, inode,
                                               (uint32_t)(blocks - 1));
        if (ret)
            return ret;
        blocks--;
    }
    uint16_t old_mode = inode->i_mode;
    uint32_t old_generation = inode->i_generation;
    uint32_t old_dtime = (uint32_t)ext_now();

    memset(inode, 0, sizeof(*inode));
    inode->i_mode = old_mode;
    inode->i_generation = old_generation;
    inode->i_dtime = old_dtime;
    ext_inode_init_large_fields(fs, inode);
    int ret = ext_write_inode_zeroed(fs, ino, inode);
    if (ret)
        return ret;
    return ext_free_inode_locked(fs, ino, (old_mode & S_IFMT) == EXT2_S_IFDIR);
}

static int ext_drop_link_locked(ext_mount_ctx_t *fs, uint32_t ino,
                                ext_inode_disk_t *inode,
                                struct vfs_inode *cached_inode) {
    uint16_t drop_links;

    if (!fs || !inode)
        return -EINVAL;

    drop_links = S_ISDIR(inode->i_mode) ? 2 : 1;
    if (inode->i_links_count > drop_links)
        inode->i_links_count -= drop_links;
    else
        inode->i_links_count = 0;
    ext_inode_touch(inode, false, false, true);

    if (inode->i_links_count == 0) {
        if (cached_inode && vfs_ref_read(&cached_inode->i_ref) > 1) {
            inode->i_dtime = (uint32_t)ext_now();
            return ext_write_inode(fs, ino, inode);
        }
        return ext_release_inode_locked(fs, ino, inode);
    }

    return ext_write_inode(fs, ino, inode);
}
static int ext_load_inode_locked(struct vfs_inode *inode,
                                 ext_inode_disk_t *disk_inode) {
    ext_inode_info_t *info;
    ext_mount_ctx_t *fs;
    int ret;

    if (!inode || !disk_inode)
        return -EINVAL;

    info = ext_i(inode);
    fs = ext_sb_info(inode->i_sb);
    if (!info || !fs)
        return -EINVAL;

    if (!info->inode_valid) {
        ret = ext_read_inode(fs, (uint32_t)inode->i_ino, &info->inode_cache);
        if (ret)
            return ret;
        info->inode_valid = true;
        ext_sync_vfs_inode(inode, fs, &info->inode_cache);
    }

    *disk_inode = info->inode_cache;
    return 0;
}

static int ext_store_inode_locked(struct vfs_inode *inode,
                                  const ext_inode_disk_t *disk_inode,
                                  bool write_back) {
    ext_inode_info_t *info;
    ext_mount_ctx_t *fs;
    int ret = 0;

    if (!inode || !disk_inode)
        return -EINVAL;

    info = ext_i(inode);
    fs = ext_sb_info(inode->i_sb);
    if (!info || !fs)
        return -EINVAL;

    if (write_back) {
        ret = ext_write_inode(fs, (uint32_t)inode->i_ino, disk_inode);
        if (ret)
            return ret;
    }

    info->inode_cache = *disk_inode;
    info->inode_valid = true;
    ext_sync_vfs_inode(inode, fs, disk_inode);
    return 0;
}

static struct vfs_inode *
ext_find_cached_inode_locked(struct vfs_super_block *sb, uint32_t ino) {
    struct vfs_inode *inode, *tmp;

    if (!sb)
        return NULL;

    spin_lock(&sb->s_inode_lock);
    llist_for_each(inode, tmp, &sb->s_inodes, i_sb_list) {
        if (inode->i_ino == ino) {
            vfs_igrab(inode);
            spin_unlock(&sb->s_inode_lock);
            return inode;
        }
    }
    spin_unlock(&sb->s_inode_lock);
    return NULL;
}

static struct vfs_inode *ext_iget_locked(struct vfs_super_block *sb,
                                         uint32_t ino) {
    ext_mount_ctx_t *fs = ext_sb_info(sb);
    ext_inode_info_t *info;
    struct vfs_inode *inode;
    ext_inode_disk_t disk_inode = {0};
    int ret;

    if (!sb || !fs || !ino)
        return ERR_PTR(-EINVAL);

    inode = ext_find_cached_inode_locked(sb, ino);
    if (inode)
        return inode;

    inode = vfs_alloc_inode(sb);
    if (!inode)
        return ERR_PTR(-ENOMEM);

    ret = ext_read_inode(fs, ino, &disk_inode);
    if (ret) {
        vfs_iput(inode);
        return ERR_PTR(ret);
    }

    inode->i_ino = ino;
    info = ext_i(inode);
    info->inode_cache = disk_inode;
    info->inode_valid = true;
    ext_sync_vfs_inode(inode, fs, &disk_inode);
    return inode;
}

static int ext_create_inode_common_locked(
    ext_mount_ctx_t *fs, struct vfs_inode *parent, struct vfs_dentry *dentry,
    uint16_t mode, uint32_t rdev, const void *payload, size_t payload_size) {
    uint32_t ino = 0;
    struct vfs_inode *vinode = NULL;
    ext_inode_info_t *info = NULL;
    uid32_t uid = 0;
    gid32_t gid = 0;

    vfs_init_new_inode_owner(parent, &mode, &uid, &gid);
    int ret = ext_alloc_inode_locked(fs, mode, &ino);
    if (ret)
        return ret;

    ext_inode_disk_t disk_inode = {0};
    disk_inode.i_mode = mode;
    disk_inode.i_links_count = (mode & S_IFMT) == EXT2_S_IFDIR ? 2 : 1;
    ext_inode_uid_set(&disk_inode, uid);
    ext_inode_gid_set(&disk_inode, gid);
    ext_inode_init_large_fields(fs, &disk_inode);
    ext_inode_touch(&disk_inode, true, true, true);
    if (ext_inode_should_use_extents(fs, mode, payload_size))
        ext_inode_init_extent_root(&disk_inode);
    if ((mode & S_IFMT) == EXT2_S_IFCHR || (mode & S_IFMT) == EXT2_S_IFBLK)
        ext_inode_rdev_set(&disk_inode, rdev);

    if ((mode & S_IFMT) == EXT2_S_IFDIR) {
        uint64_t block = 0;
        ext_dir_block_ctx_t block_ctx = {0};
        ret = ext_inode_get_block_locked(fs, ino, &disk_inode, 0, true, &block);
        if (ret)
            goto rollback_inode;
        ext_inode_size_set(&disk_inode, fs->block_size);

        uint8_t *buf = calloc(1, fs->block_size);
        if (!buf) {
            ret = -ENOMEM;
            goto rollback_inode_release;
        }
        ext_dir_entry_t *dot = (ext_dir_entry_t *)buf;
        dot->inode = ino;
        dot->rec_len = ext_dir_rec_len(1);
        dot->name_len = 1;
        dot->file_type = EXT2_FT_DIR;
        dot->name[0] = '.';

        ext_dir_entry_t *dotdot = (ext_dir_entry_t *)(buf + dot->rec_len);
        dotdot->inode = parent ? (uint32_t)parent->i_ino : ino;
        dotdot->rec_len = fs->block_size - dot->rec_len;
        dotdot->name_len = 2;
        dotdot->file_type = EXT2_FT_DIR;
        dotdot->name[0] = '.';
        dotdot->name[1] = '.';

        block_ctx.pblock = block;
        block_ctx.lblock = 0;
        block_ctx.data_size = fs->block_size;
        if (ext_dir_has_tail(fs, &disk_inode, fs->block_size)) {
            dotdot->rec_len = (uint16_t)(fs->block_size - dot->rec_len -
                                         sizeof(ext_dir_entry_tail_t));
            block_ctx.data_size = fs->block_size - sizeof(ext_dir_entry_tail_t);
            block_ctx.has_tail = true;
        }
        ret = ext_dir_block_write_locked(fs, ino, &disk_inode, &block_ctx, buf);
        free(buf);
        if (ret)
            goto rollback_inode_release;
    } else if ((mode & S_IFMT) == EXT2_S_IFLNK) {
        if (payload_size <= sizeof(disk_inode.i_block) &&
            !ext_inode_uses_extents(&disk_inode)) {
            memcpy(disk_inode.i_block, payload, payload_size);
            ext_inode_size_set(&disk_inode, payload_size);
        } else {
            ret = ext_write_inode_data_locked(fs, ino, &disk_inode, payload, 0,
                                              payload_size);
            if (ret < 0)
                goto rollback_inode_release;
        }
    }

    ret = ext_write_inode(fs, ino, &disk_inode);
    if (ret)
        goto rollback_inode_release;

    ext_inode_disk_t parent_inode = {0};
    ret = ext_read_inode(fs, (uint32_t)parent->i_ino, &parent_inode);
    if (ret)
        goto rollback_inode_release;

    ret = ext_dir_add_entry_locked(
        fs, (uint32_t)parent->i_ino, &parent_inode, ino, dentry->d_name.name,
        ext_mode_to_dir_file_type(disk_inode.i_mode));
    if (ret)
        goto rollback_inode_release;

    if ((mode & S_IFMT) == EXT2_S_IFDIR) {
        parent_inode.i_links_count++;
        ext_inode_touch(&parent_inode, false, true, true);
        ret = ext_write_inode(fs, (uint32_t)parent->i_ino, &parent_inode);
        if (ret)
            return ret;
    }

    ret = ext_store_inode_locked(parent, &parent_inode, false);
    if (ret)
        return ret;

    vinode = ext_iget_locked(parent->i_sb, ino);
    if (IS_ERR(vinode))
        return PTR_ERR(vinode);

    info = ext_i(vinode);
    if ((mode & S_IFMT) == EXT2_S_IFLNK &&
        payload_size <= sizeof(disk_inode.i_block)) {
        info->symlink = calloc(1, payload_size + 1);
        if (info->symlink)
            memcpy(info->symlink, payload, payload_size);
    }

    vfs_d_instantiate(dentry, vinode);
    vfs_iput(vinode);
    return 0;

rollback_inode_release:
    ext_release_inode_locked(fs, ino, &disk_inode);
    return ret;
rollback_inode:
    ext_free_inode_locked(fs, ino, (mode & S_IFMT) == EXT2_S_IFDIR);
    return ret;
}

static int ext_mount_prepare_locked(ext_mount_ctx_t *fs, uint64_t dev);
static int ext_iterate_dir_locked(struct vfs_inode *dir,
                                  struct vfs_dir_context *ctx);
static struct vfs_dentry *ext_lookup(struct vfs_inode *dir,
                                     struct vfs_dentry *dentry,
                                     unsigned int flags);
static int ext_create(struct vfs_inode *dir, struct vfs_dentry *dentry,
                      umode_t mode, bool excl);
static int ext_mkdir(struct vfs_inode *dir, struct vfs_dentry *dentry,
                     umode_t mode);
static int ext_mknod(struct vfs_inode *dir, struct vfs_dentry *dentry,
                     umode_t mode, dev64_t dev);
static int ext_symlink(struct vfs_inode *dir, struct vfs_dentry *dentry,
                       const char *target);
static int ext_link(struct vfs_dentry *old_dentry, struct vfs_inode *dir,
                    struct vfs_dentry *new_dentry);
static int ext_unlink(struct vfs_inode *dir, struct vfs_dentry *dentry);
static int ext_rmdir(struct vfs_inode *dir, struct vfs_dentry *dentry);
static int ext_rename(struct vfs_rename_ctx *ctx);
static const char *ext_get_link(struct vfs_dentry *dentry,
                                struct vfs_inode *inode,
                                struct vfs_nameidata *nd);
static int ext_permission(struct vfs_inode *inode, int mask);
static int ext_getattr(const struct vfs_path *path, struct vfs_kstat *stat,
                       uint32_t request_mask, unsigned int flags);
static int ext_setattr(struct vfs_dentry *dentry, const struct vfs_kstat *stat);
static loff_t ext_llseek(struct vfs_file *file, loff_t offset, int whence);
static ssize_t ext_read(struct vfs_file *file, void *buf, size_t count,
                        loff_t *ppos);
static ssize_t ext_write(struct vfs_file *file, const void *buf, size_t count,
                         loff_t *ppos);
static int ext_iterate_shared(struct vfs_file *file,
                              struct vfs_dir_context *ctx);
static long ext_unlocked_ioctl(struct vfs_file *file, unsigned long cmd,
                               unsigned long arg);
static int ext_flush(struct vfs_file *file);
static int ext_fsync(struct vfs_file *file, loff_t start, loff_t end,
                     int datasync);
static __poll_t ext_poll(struct vfs_file *file, struct vfs_poll_table *pt);
static int ext_open(struct vfs_inode *inode, struct vfs_file *file);
static int ext_release(struct vfs_inode *inode, struct vfs_file *file);
static int ext_fsync(struct vfs_file *file, loff_t start, loff_t end,
                     int datasync);
static struct vfs_inode *ext_alloc_inode(struct vfs_super_block *sb);
static void ext_destroy_inode(struct vfs_inode *inode);
static void ext_put_super(struct vfs_super_block *sb);
static int ext_statfs(struct vfs_path *path, void *buf);
static int ext_resolve_mount_dev(struct vfs_fs_context *fc, uint64_t *dev_out);
static int ext_init_fs_context(struct vfs_fs_context *fc);
static int ext_get_tree(struct vfs_fs_context *fc);

static int ext_mount_prepare_locked(ext_mount_ctx_t *fs, uint64_t dev) {
    int ret;
    uint64_t gdt_block;
    uint32_t compat_ok;
    uint32_t ro_ok;
    uint32_t incompat_ok;

    if (!fs || !dev)
        return -EINVAL;

    fs->dev = dev;
    ret = ext_dev_read(fs, 1024, &fs->sb, sizeof(fs->sb));
    if (ret)
        return ret;
    if (fs->sb.s_magic != EXT_SUPER_MAGIC)
        return -EINVAL;

    if (fs->sb.s_log_block_size > 2)
        return -EINVAL;
    fs->block_size = 1024u << fs->sb.s_log_block_size;
    fs->inode_size =
        fs->sb.s_inode_size ? fs->sb.s_inode_size : EXT2_GOOD_OLD_INODE_SIZE;
    fs->desc_size = fs->sb.s_desc_size ? fs->sb.s_desc_size : 32;
    fs->has_extents =
        (fs->sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS) != 0;
    fs->has_64bit =
        (fs->sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) != 0;
    fs->has_metadata_csum = (fs->sb.s_feature_ro_compat &
                             EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) != 0;
    fs->has_dir_index =
        (fs->sb.s_feature_compat & EXT2_FEATURE_COMPAT_DIR_INDEX) != 0;
    fs->ptrs_per_block = fs->block_size / sizeof(uint32_t);
    fs->blocks_count = (uint64_t)fs->sb.s_blocks_count_lo |
                       ((uint64_t)fs->sb.s_blocks_count_hi << 32);
    fs->inodes_count = fs->sb.s_inodes_count;
    fs->checksum_seed =
        (fs->sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_CSUM_SEED)
            ? fs->sb.s_checksum_seed
            : ext_metadata_checksum_seed(&fs->sb);
    if (!ext_super_checksum_verify(fs))
        return -EINVAL;

    if (fs->block_size < 1024 || fs->block_size > EXT_MAX_BLOCK_SIZE)
        return -EINVAL;
    if (fs->inode_size < EXT2_GOOD_OLD_INODE_SIZE ||
        fs->inode_size > fs->block_size || (fs->inode_size & 3))
        return -EINVAL;
    if (fs->block_size % fs->inode_size)
        return -EINVAL;
    if (fs->desc_size < 32 || fs->desc_size > fs->block_size ||
        (fs->desc_size & 3))
        return -EINVAL;
    if (!fs->has_64bit && fs->desc_size != 32)
        return -EINVAL;
    if (!fs->sb.s_blocks_per_group || !fs->sb.s_inodes_per_group)
        return -EINVAL;
    if (!fs->blocks_count || fs->sb.s_first_data_block >= fs->blocks_count)
        return -EINVAL;
    if (!fs->inodes_count)
        return -EINVAL;
    if (fs->sb.s_blocks_per_group > fs->block_size * 8 ||
        fs->sb.s_inodes_per_group > fs->block_size * 8)
        return -EINVAL;
    if (fs->inodes_count > UINT32_MAX || fs->sb.s_inodes_per_group > UINT32_MAX)
        return -EINVAL;
    if (fs->sb.s_inodes_per_group &&
        fs->inodes_count % fs->sb.s_inodes_per_group)
        return -EINVAL;

    fs->group_count = (uint32_t)((fs->blocks_count - fs->sb.s_first_data_block +
                                  fs->sb.s_blocks_per_group - 1) /
                                 fs->sb.s_blocks_per_group);
    if (!fs->group_count)
        return -EINVAL;
    if (fs->group_count > UINT32_MAX / sizeof(ext_group_desc_t))
        return -EINVAL;

    compat_ok = EXT3_FEATURE_COMPAT_HAS_JOURNAL | EXT2_FEATURE_COMPAT_EXT_ATTR |
                EXT2_FEATURE_COMPAT_RESIZE_INODE |
                EXT2_FEATURE_COMPAT_DIR_INDEX;
    ro_ok =
        EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER |
        EXT2_FEATURE_RO_COMPAT_LARGE_FILE | EXT4_FEATURE_RO_COMPAT_HUGE_FILE |
        EXT4_FEATURE_RO_COMPAT_DIR_NLINK | EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE |
        EXT4_FEATURE_RO_COMPAT_METADATA_CSUM;
    incompat_ok = EXT2_FEATURE_INCOMPAT_FILETYPE |
                  EXT4_FEATURE_INCOMPAT_EXTENTS | EXT4_FEATURE_INCOMPAT_64BIT |
                  EXT4_FEATURE_INCOMPAT_FLEX_BG |
                  EXT4_FEATURE_INCOMPAT_CSUM_SEED;
    if ((fs->sb.s_feature_compat & ~compat_ok) ||
        (fs->sb.s_feature_ro_compat & ~ro_ok) ||
        (fs->sb.s_feature_incompat & ~incompat_ok))
        return -ENOTSUP;

    ret = ext_map_cache_init(fs);
    if (ret)
        return ret;

    fs->groups = calloc(fs->group_count, sizeof(ext_group_desc_t));
    if (!fs->groups) {
        ext_map_cache_destroy(fs);
        return -ENOMEM;
    }

    gdt_block = fs->block_size == 1024 ? 2 : 1;
    for (uint32_t i = 0; i < fs->group_count; i++) {
        uint64_t offset =
            gdt_block * fs->block_size + (uint64_t)i * fs->desc_size;
        ret =
            ext_dev_read(fs, offset, &fs->groups[i],
                         MIN((size_t)fs->desc_size, sizeof(ext_group_desc_t)));
        if (ret) {
            free(fs->groups);
            fs->groups = NULL;
            ext_map_cache_destroy(fs);
            return ret;
        }
    }

    return 0;
}

static int ext_iterate_dir_locked(struct vfs_inode *dir,
                                  struct vfs_dir_context *ctx) {
    ext_mount_ctx_t *fs = ext_sb_info(dir->i_sb);
    ext_inode_disk_t dir_inode = {0};
    uint64_t dir_size;
    uint32_t blocks;
    uint8_t *buf;
    ext_dir_block_ctx_t block_ctx;
    int ret;

    ret = ext_load_inode_locked(dir, &dir_inode);
    if (ret)
        return ret;
    if (!S_ISDIR(dir_inode.i_mode))
        return -ENOTDIR;

    dir_size = ext_inode_size_get(&dir_inode);
    blocks = (uint32_t)((dir_size + fs->block_size - 1) / fs->block_size);
    buf = calloc(1, fs->block_size);
    if (!buf)
        return -ENOMEM;

    for (uint32_t lblock = 0; lblock < blocks; lblock++) {
        ret = ext_dir_block_prepare(fs, (uint32_t)dir->i_ino, &dir_inode,
                                    lblock, buf, &block_ctx);
        if (ret) {
            free(buf);
            return ret;
        }
        if (!block_ctx.pblock)
            continue;

        uint32_t off = 0;
        if (block_ctx.indexed_root) {
            ext_dir_entry_t *dot = (ext_dir_entry_t *)buf;
            ext_dir_entry_t *dotdot = (ext_dir_entry_t *)(buf + dot->rec_len);
            off = dot->rec_len + dotdot->rec_len;
        }
        for (; off + sizeof(ext_dir_entry_t) <= block_ctx.data_size;) {
            ext_dir_entry_t *entry = (ext_dir_entry_t *)(buf + off);
            if (!ext_dir_entry_valid(entry, off, block_ctx.data_size)) {
                free(buf);
                return -EIO;
            }
            loff_t entry_pos = (loff_t)lblock * fs->block_size + off;
            loff_t next_pos = entry_pos + entry->rec_len;
            if (next_pos <= ctx->pos) {
                off += entry->rec_len;
                continue;
            }
            if (entry->inode &&
                !(entry->name_len == 1 && entry->name[0] == '.') &&
                !(entry->name_len == 2 && entry->name[0] == '.' &&
                  entry->name[1] == '.')) {
                if (ctx->actor(ctx, entry->name, entry->name_len, next_pos,
                               entry->inode,
                               ext_dir_file_type_to_dtype(entry->file_type))) {
                    free(buf);
                    return 0;
                }
                ctx->pos = next_pos;
            }
            off += entry->rec_len;
        }
    }

    free(buf);
    return 0;
}

static struct vfs_dentry *ext_lookup(struct vfs_inode *dir,
                                     struct vfs_dentry *dentry,
                                     unsigned int flags) {
    ext_mount_ctx_t *fs = ext_sb_info(dir->i_sb);
    ext_dir_lookup_t lookup = {0};
    struct vfs_inode *inode = NULL;
    int ret;

    (void)flags;
    if (!fs || !dir || !dentry)
        return ERR_PTR(-EINVAL);

    spin_lock(&fs->lock);
    ret = ext_lookup_name_locked(fs, (uint32_t)dir->i_ino, dentry->d_name.name,
                                 &lookup);
    if (!ret && lookup.found)
        inode = ext_iget_locked(dir->i_sb, lookup.inode);
    spin_unlock(&fs->lock);

    if (ret)
        return ERR_PTR(ret);
    if (inode && IS_ERR(inode))
        return ERR_CAST(inode);

    vfs_d_instantiate(dentry, inode);
    if (inode)
        vfs_iput(inode);
    return dentry;
}

static int ext_create(struct vfs_inode *dir, struct vfs_dentry *dentry,
                      umode_t mode, bool excl) {
    ext_mount_ctx_t *fs = ext_sb_info(dir->i_sb);
    int ret;
    (void)excl;
    spin_lock(&fs->lock);
    ret = ext_create_inode_common_locked(fs, dir, dentry,
                                         (mode & 07777) | S_IFREG, 0, NULL, 0);
    spin_unlock(&fs->lock);
    return ret;
}

static int ext_mkdir(struct vfs_inode *dir, struct vfs_dentry *dentry,
                     umode_t mode) {
    ext_mount_ctx_t *fs = ext_sb_info(dir->i_sb);
    int ret;
    spin_lock(&fs->lock);
    ret = ext_create_inode_common_locked(fs, dir, dentry,
                                         (mode & 07777) | S_IFDIR, 0, NULL, 0);
    spin_unlock(&fs->lock);
    return ret;
}

static int ext_mknod(struct vfs_inode *dir, struct vfs_dentry *dentry,
                     umode_t mode, dev64_t dev) {
    ext_mount_ctx_t *fs = ext_sb_info(dir->i_sb);
    int ret;
    spin_lock(&fs->lock);
    ret = ext_create_inode_common_locked(fs, dir, dentry, mode, (uint32_t)dev,
                                         NULL, 0);
    spin_unlock(&fs->lock);
    return ret;
}

static int ext_symlink(struct vfs_inode *dir, struct vfs_dentry *dentry,
                       const char *target) {
    ext_mount_ctx_t *fs = ext_sb_info(dir->i_sb);
    int ret;
    spin_lock(&fs->lock);
    ret = ext_create_inode_common_locked(fs, dir, dentry, S_IFLNK | 0777, 0,
                                         target, target ? strlen(target) : 0);
    spin_unlock(&fs->lock);
    return ret;
}

static int ext_link(struct vfs_dentry *old_dentry, struct vfs_inode *dir,
                    struct vfs_dentry *new_dentry) {
    ext_mount_ctx_t *fs = ext_sb_info(dir->i_sb);
    ext_inode_disk_t target_inode = {0};
    ext_inode_disk_t parent_inode = {0};
    int ret;

    if (!old_dentry || !old_dentry->d_inode || !dir || !new_dentry)
        return -EINVAL;
    if (old_dentry->d_inode->i_sb != dir->i_sb)
        return -EXDEV;

    spin_lock(&fs->lock);
    ret = ext_load_inode_locked(old_dentry->d_inode, &target_inode);
    if (ret)
        goto out;
    if (S_ISDIR(target_inode.i_mode)) {
        ret = -EPERM;
        goto out;
    }

    ret = ext_read_inode(fs, (uint32_t)dir->i_ino, &parent_inode);
    if (ret)
        goto out;

    ret = ext_dir_add_entry_locked(
        fs, (uint32_t)dir->i_ino, &parent_inode,
        (uint32_t)old_dentry->d_inode->i_ino, new_dentry->d_name.name,
        ext_mode_to_dir_file_type(target_inode.i_mode));
    if (ret)
        goto out;

    ret = ext_store_inode_locked(dir, &parent_inode, false);
    if (ret)
        goto out;

    target_inode.i_links_count++;
    target_inode.i_dtime = 0;
    ext_inode_touch(&target_inode, false, false, true);
    ret = ext_store_inode_locked(old_dentry->d_inode, &target_inode, true);
    if (!ret)
        vfs_d_instantiate(new_dentry, old_dentry->d_inode);

out:
    spin_unlock(&fs->lock);
    return ret;
}

static int ext_unlink(struct vfs_inode *dir, struct vfs_dentry *dentry) {
    ext_mount_ctx_t *fs = ext_sb_info(dir->i_sb);
    ext_inode_disk_t parent_inode = {0};
    ext_inode_disk_t disk_inode = {0};
    int ret;

    if (!dentry || !dentry->d_inode)
        return -ENOENT;

    spin_lock(&fs->lock);
    ret = ext_read_inode(fs, (uint32_t)dir->i_ino, &parent_inode);
    if (ret)
        goto out;
    ret = ext_load_inode_locked(dentry->d_inode, &disk_inode);
    if (ret)
        goto out;
    if (S_ISDIR(disk_inode.i_mode)) {
        ret = -EISDIR;
        goto out;
    }

    ret = ext_dir_remove_entry_locked(fs, (uint32_t)dir->i_ino, &parent_inode,
                                      dentry->d_name.name, NULL);
    if (ret)
        goto out;

    ret = ext_store_inode_locked(dir, &parent_inode, false);
    if (ret)
        goto out;

    ret = ext_drop_link_locked(fs, (uint32_t)dentry->d_inode->i_ino,
                               &disk_inode, dentry->d_inode);
    if (!ret)
        ret = ext_store_inode_locked(dentry->d_inode, &disk_inode, false);

out:
    spin_unlock(&fs->lock);
    return ret;
}

static int ext_rmdir(struct vfs_inode *dir, struct vfs_dentry *dentry) {
    ext_mount_ctx_t *fs = ext_sb_info(dir->i_sb);
    ext_inode_disk_t parent_inode = {0};
    ext_inode_disk_t disk_inode = {0};
    int ret;

    if (!dentry || !dentry->d_inode)
        return -ENOENT;

    spin_lock(&fs->lock);
    ret = ext_read_inode(fs, (uint32_t)dir->i_ino, &parent_inode);
    if (ret)
        goto out;
    ret = ext_load_inode_locked(dentry->d_inode, &disk_inode);
    if (ret)
        goto out;
    if (!S_ISDIR(disk_inode.i_mode)) {
        ret = -ENOTDIR;
        goto out;
    }

    ret = ext_dir_is_empty_locked(fs, (uint32_t)dentry->d_inode->i_ino,
                                  &disk_inode);
    if (ret <= 0) {
        ret = ret < 0 ? ret : -ENOTEMPTY;
        goto out;
    }

    ret = ext_dir_remove_entry_locked(fs, (uint32_t)dir->i_ino, &parent_inode,
                                      dentry->d_name.name, NULL);
    if (ret)
        goto out;

    if (parent_inode.i_links_count)
        parent_inode.i_links_count--;
    ext_inode_touch(&parent_inode, false, true, true);
    ret = ext_write_inode(fs, (uint32_t)dir->i_ino, &parent_inode);
    if (ret)
        goto out;

    ret = ext_store_inode_locked(dir, &parent_inode, false);
    if (ret)
        goto out;

    ret = ext_drop_link_locked(fs, (uint32_t)dentry->d_inode->i_ino,
                               &disk_inode, dentry->d_inode);
    if (!ret)
        ret = ext_store_inode_locked(dentry->d_inode, &disk_inode, false);

out:
    spin_unlock(&fs->lock);
    return ret;
}

static int ext_rename(struct vfs_rename_ctx *ctx) {
    ext_mount_ctx_t *fs;
    ext_inode_disk_t src_inode = {0};
    ext_inode_disk_t old_parent_inode = {0};
    ext_inode_disk_t new_parent_inode = {0};
    ext_inode_disk_t target_inode = {0};
    ext_dir_lookup_t target_lookup = {0};
    struct vfs_inode *cached_target = NULL;
    bool target_exists;
    bool source_is_dir;
    bool target_is_dir = false;
    int ret;

    if (!ctx || !ctx->old_dir || !ctx->new_dir || !ctx->old_dentry ||
        !ctx->old_dentry->d_inode || !ctx->new_dentry)
        return -EINVAL;
    if (ctx->flags & (VFS_RENAME_EXCHANGE | VFS_RENAME_WHITEOUT))
        return -EOPNOTSUPP;
    if (ctx->old_dir->i_sb != ctx->new_dir->i_sb)
        return -EXDEV;

    fs = ext_sb_info(ctx->old_dir->i_sb);
    spin_lock(&fs->lock);

    ret = ext_load_inode_locked(ctx->old_dentry->d_inode, &src_inode);
    if (ret)
        goto out;
    ret = ext_read_inode(fs, (uint32_t)ctx->old_dir->i_ino, &old_parent_inode);
    if (ret)
        goto out;
    ret = ext_read_inode(fs, (uint32_t)ctx->new_dir->i_ino, &new_parent_inode);
    if (ret)
        goto out;

    source_is_dir = S_ISDIR(src_inode.i_mode);
    ret = ext_dir_find_locked(fs, (uint32_t)ctx->new_dir->i_ino,
                              &new_parent_inode, ctx->new_dentry->d_name.name,
                              &target_lookup);
    if (ret)
        goto out;
    target_exists = target_lookup.found;

    if ((ctx->old_dir == ctx->new_dir &&
         streq(ctx->old_dentry->d_name.name, ctx->new_dentry->d_name.name)) ||
        (target_exists &&
         target_lookup.inode == (uint32_t)ctx->old_dentry->d_inode->i_ino)) {
        ret = 0;
        goto out;
    }

    if (target_exists) {
        ret = ext_read_inode(fs, target_lookup.inode, &target_inode);
        if (ret)
            goto out;

        target_is_dir = S_ISDIR(target_inode.i_mode);
        if (source_is_dir != target_is_dir) {
            ret = source_is_dir ? -ENOTDIR : -EISDIR;
            goto out;
        }
        if (target_is_dir) {
            ret =
                ext_dir_is_empty_locked(fs, target_lookup.inode, &target_inode);
            if (ret <= 0) {
                ret = ret < 0 ? ret : -ENOTEMPTY;
                goto out;
            }
        }

        ret = ext_dir_replace_entry_locked(
            fs, (uint32_t)ctx->new_dir->i_ino, &new_parent_inode,
            ctx->new_dentry->d_name.name,
            (uint32_t)ctx->old_dentry->d_inode->i_ino,
            ext_mode_to_dir_file_type(src_inode.i_mode));
        if (ret)
            goto out;

        cached_target = ext_find_cached_inode_locked(ctx->new_dir->i_sb,
                                                     target_lookup.inode);
        ret = ext_drop_link_locked(fs, target_lookup.inode, &target_inode,
                                   cached_target);
        if (cached_target) {
            if (!ret)
                (void)ext_store_inode_locked(cached_target, &target_inode,
                                             false);
            vfs_iput(cached_target);
            cached_target = NULL;
        }
        if (ret)
            goto out;
    } else {
        ret = ext_dir_add_entry_locked(
            fs, (uint32_t)ctx->new_dir->i_ino, &new_parent_inode,
            (uint32_t)ctx->old_dentry->d_inode->i_ino,
            ctx->new_dentry->d_name.name,
            ext_mode_to_dir_file_type(src_inode.i_mode));
        if (ret)
            goto out;
    }

    if (ctx->old_dir == ctx->new_dir)
        old_parent_inode = new_parent_inode;

    ret = ext_dir_remove_entry_locked(fs, (uint32_t)ctx->old_dir->i_ino,
                                      &old_parent_inode,
                                      ctx->old_dentry->d_name.name, NULL);
    if (ret)
        goto out;

    if (source_is_dir && ctx->new_dir != ctx->old_dir) {
        ret = ext_dir_set_dotdot_locked(
            fs, (uint32_t)ctx->old_dentry->d_inode->i_ino, &src_inode,
            (uint32_t)ctx->new_dir->i_ino);
        if (ret)
            goto out;

        if (old_parent_inode.i_links_count)
            old_parent_inode.i_links_count--;
        new_parent_inode.i_links_count++;
        ext_inode_touch(&old_parent_inode, false, true, true);
        ext_inode_touch(&new_parent_inode, false, true, true);
        ret = ext_write_inode(fs, (uint32_t)ctx->old_dir->i_ino,
                              &old_parent_inode);
        if (ret)
            goto out;
        ret = ext_write_inode(fs, (uint32_t)ctx->new_dir->i_ino,
                              &new_parent_inode);
        if (ret)
            goto out;
    }

    ext_inode_touch(&src_inode, false, false, true);
    ret = ext_store_inode_locked(ctx->old_dentry->d_inode, &src_inode, true);
    if (ret)
        goto out;

    ret = ext_store_inode_locked(ctx->old_dir, &old_parent_inode, false);
    if (ret)
        goto out;
    if (ctx->new_dir != ctx->old_dir) {
        ret = ext_store_inode_locked(ctx->new_dir, &new_parent_inode, false);
        if (ret)
            goto out;
    }

out:
    if (cached_target)
        vfs_iput(cached_target);
    spin_unlock(&fs->lock);
    return ret;
}

static const char *ext_get_link(struct vfs_dentry *dentry,
                                struct vfs_inode *inode,
                                struct vfs_nameidata *nd) {
    ext_inode_info_t *info;
    ext_mount_ctx_t *fs;
    ext_inode_disk_t disk_inode = {0};
    size_t link_size;
    int ret;

    (void)dentry;
    (void)nd;
    if (!inode)
        return ERR_PTR(-EINVAL);

    fs = ext_sb_info(inode->i_sb);
    spin_lock(&fs->lock);
    info = ext_i(inode);
    ret = ext_load_inode_locked(inode, &disk_inode);
    if (ret) {
        spin_unlock(&fs->lock);
        return ERR_PTR(ret);
    }

    link_size = (size_t)ext_inode_size_get(&disk_inode);
    free(info->symlink);
    info->symlink = calloc(1, link_size + 1);
    if (!info->symlink) {
        spin_unlock(&fs->lock);
        return ERR_PTR(-ENOMEM);
    }

    if (link_size <= sizeof(disk_inode.i_block)) {
        memcpy(info->symlink, disk_inode.i_block, link_size);
        spin_unlock(&fs->lock);
        return info->symlink;
    }

    ret = ext_read_inode_data_locked(fs, (uint32_t)inode->i_ino, &disk_inode,
                                     info->symlink, 0, link_size);
    spin_unlock(&fs->lock);
    return ret < 0 ? ERR_PTR(ret) : info->symlink;
}

static int ext_permission(struct vfs_inode *inode, int mask) {
    return vfs_inode_permission(inode, mask);
}

static ssize_t ext_getxattr(struct vfs_inode *inode, const char *full_name,
                            void *value, size_t size) {
    ext_mount_ctx_t *fs;
    ext_xattr_record_t *records = NULL;
    uint8_t *raw = NULL;
    size_t count = 0;
    uint8_t index;
    const char *name;
    ssize_t ret;

    if (!inode || !inode->i_sb || !full_name)
        return -EINVAL;
    ret = ext_xattr_split_name(full_name, &index, &name);
    if (ret)
        return ret;
    fs = inode->i_sb->s_fs_info;
    spin_lock(&fs->lock);
    ret = ext_xattr_records_load_locked(fs, (uint32_t)inode->i_ino, &raw,
                                        &records, &count);
    spin_unlock(&fs->lock);
    if (ret)
        return ret;
    ret = -ENODATA;
    for (size_t i = 0; i < count; i++) {
        if (records[i].index != index || strcmp(records[i].name, name))
            continue;
        if (size && size < records[i].size) {
            ret = -ERANGE;
            break;
        }
        if (value && records[i].size)
            memcpy(value, records[i].value, records[i].size);
        ret = records[i].size;
        break;
    }
    free(raw);
    ext_xattr_records_free(records, count);
    return ret;
}

static ssize_t ext_listxattr(struct vfs_inode *inode, char *list, size_t size) {
    ext_mount_ctx_t *fs;
    ext_xattr_record_t *records = NULL;
    uint8_t *raw = NULL;
    size_t count = 0, total = 0;
    int ret;

    if (!inode || !inode->i_sb)
        return -EINVAL;
    fs = inode->i_sb->s_fs_info;
    spin_lock(&fs->lock);
    ret = ext_xattr_records_load_locked(fs, (uint32_t)inode->i_ino, &raw,
                                        &records, &count);
    spin_unlock(&fs->lock);
    if (ret)
        return ret;
    for (size_t i = 0; i < count; i++) {
        const char *prefix = ext_xattr_prefix(records[i].index);
        size_t prefix_len, name_len, full_len;
        if (!prefix)
            continue;
        prefix_len = strlen(prefix);
        name_len = strlen(records[i].name);
        full_len = prefix_len + name_len + 1;
        if (size && total + full_len > size) {
            ret = -ERANGE;
            goto out;
        }
        if (list) {
            memcpy(list + total, prefix, prefix_len);
            memcpy(list + total + prefix_len, records[i].name, name_len);
            list[total + prefix_len + name_len] = '\0';
        }
        total += full_len;
    }
    ret = (int)total;
out:
    free(raw);
    ext_xattr_records_free(records, count);
    return ret;
}

static int ext_xattr_may_set(struct vfs_inode *inode, uint8_t index) {
    uid32_t fsuid = vfs_current_fsuid();
    if (fsuid != 0 && fsuid != inode->i_uid)
        return -EPERM;
    if ((index == EXT4_XATTR_INDEX_TRUSTED ||
         index == EXT4_XATTR_INDEX_SECURITY) &&
        fsuid != 0)
        return -EPERM;
    return 0;
}

static int ext_setxattr(struct vfs_inode *inode, const char *full_name,
                        const void *value, size_t size, int flags) {
    ext_mount_ctx_t *fs;
    ext_xattr_record_t *records = NULL;
    uint8_t *raw = NULL;
    size_t count = 0;
    uint8_t index;
    const char *name;
    ssize_t found = -1;
    int ret;

    if (!inode || !inode->i_sb || !full_name || (size && !value))
        return -EINVAL;
    ret = ext_xattr_split_name(full_name, &index, &name);
    if (ret)
        return ret;
    ret = ext_xattr_may_set(inode, index);
    if (ret)
        return ret;
    fs = inode->i_sb->s_fs_info;
    spin_lock(&fs->lock);
    ret = ext_xattr_records_load_locked(fs, (uint32_t)inode->i_ino, &raw,
                                        &records, &count);
    if (ret)
        goto out;
    for (size_t i = 0; i < count; i++)
        if (records[i].index == index && !strcmp(records[i].name, name)) {
            found = (ssize_t)i;
            break;
        }
    if ((flags & 1) && found >= 0) {
        ret = -EEXIST;
        goto out;
    }
    if ((flags & 2) && found < 0) {
        ret = -ENODATA;
        goto out;
    }
    if (found < 0) {
        ext_xattr_record_t *new_records =
            realloc(records, (count + 1) * sizeof(*records));
        if (!new_records) {
            ret = -ENOMEM;
            goto out;
        }
        records = new_records;
        found = (ssize_t)count++;
        memset(&records[found], 0, sizeof(records[found]));
        records[found].index = index;
        records[found].name = strdup(name);
        if (!records[found].name) {
            ret = -ENOMEM;
            goto out;
        }
    }
    free(records[found].value);
    records[found].value = NULL;
    records[found].size = (uint32_t)size;
    if (size) {
        records[found].value = malloc(size);
        if (!records[found].value) {
            ret = -ENOMEM;
            goto out;
        }
        memcpy(records[found].value, value, size);
    }
    ret = ext_xattr_records_store_locked(fs, (uint32_t)inode->i_ino, raw,
                                         records, count);
    if (!ret)
        ext_sync_vfs_inode(inode, fs, (ext_inode_disk_t *)raw);
out:
    spin_unlock(&fs->lock);
    free(raw);
    ext_xattr_records_free(records, count);
    return ret;
}

static int ext_removexattr(struct vfs_inode *inode, const char *full_name) {
    ext_mount_ctx_t *fs;
    ext_xattr_record_t *records = NULL;
    uint8_t *raw = NULL;
    size_t count = 0;
    uint8_t index;
    const char *name;
    ssize_t found = -1;
    int ret;

    if (!inode || !inode->i_sb || !full_name)
        return -EINVAL;
    ret = ext_xattr_split_name(full_name, &index, &name);
    if (ret)
        return ret;
    ret = ext_xattr_may_set(inode, index);
    if (ret)
        return ret;
    fs = inode->i_sb->s_fs_info;
    spin_lock(&fs->lock);
    ret = ext_xattr_records_load_locked(fs, (uint32_t)inode->i_ino, &raw,
                                        &records, &count);
    if (ret)
        goto out;
    for (size_t i = 0; i < count; i++)
        if (records[i].index == index && !strcmp(records[i].name, name)) {
            found = (ssize_t)i;
            break;
        }
    if (found < 0) {
        ret = -ENODATA;
        goto out;
    }
    free(records[found].name);
    free(records[found].value);
    if ((size_t)found + 1 < count)
        memmove(&records[found], &records[found + 1],
                (count - (size_t)found - 1) * sizeof(*records));
    count--;
    ret = ext_xattr_records_store_locked(fs, (uint32_t)inode->i_ino, raw,
                                         records, count);
    if (!ret)
        ext_sync_vfs_inode(inode, fs, (ext_inode_disk_t *)raw);
out:
    spin_unlock(&fs->lock);
    free(raw);
    ext_xattr_records_free(records, count);
    return ret;
}

static int ext_getattr(const struct vfs_path *path, struct vfs_kstat *stat,
                       uint32_t request_mask, unsigned int flags) {
    (void)request_mask;
    (void)flags;
    vfs_fill_generic_kstat(path, stat);
    return 0;
}

static int ext_setattr(struct vfs_dentry *dentry,
                       const struct vfs_kstat *stat) {
    struct vfs_inode *inode = dentry ? dentry->d_inode : NULL;
    ext_mount_ctx_t *fs;
    ext_inode_disk_t disk_inode = {0};
    uint64_t old_size;
    int ret;

    if (!inode || !stat)
        return -EINVAL;

    fs = ext_sb_info(inode->i_sb);
    spin_lock(&fs->lock);
    ret = ext_load_inode_locked(inode, &disk_inode);
    if (ret)
        goto out;
    old_size = ext_inode_size_get(&disk_inode);
    spin_unlock(&fs->lock);

    if (!S_ISDIR(disk_inode.i_mode) && stat->size != old_size) {
        uint64_t wb_end = stat->size < old_size ? stat->size : UINT64_MAX;
        ret = page_cache_writeback_range(&inode->i_mapping, 0, wb_end, false);
        if (ret < 0)
            return ret;
    }

    spin_lock(&fs->lock);
    ret = ext_load_inode_locked(inode, &disk_inode);
    if (ret)
        goto out;

    if (stat->mode)
        disk_inode.i_mode = (disk_inode.i_mode & S_IFMT) | (stat->mode & 07777);
    ext_inode_uid_set(&disk_inode, stat->uid);
    ext_inode_gid_set(&disk_inode, stat->gid);
    old_size = ext_inode_size_get(&disk_inode);
    if (!S_ISDIR(disk_inode.i_mode) && stat->size != old_size) {
        ret = ext_inode_truncate_locked(fs, (uint32_t)inode->i_ino, &disk_inode,
                                        stat->size);
        if (ret)
            goto out;
    } else {
        ext_inode_touch(&disk_inode, false, false, true);
        ret = ext_write_inode(fs, (uint32_t)inode->i_ino, &disk_inode);
        if (ret)
            goto out;
    }

    ret = ext_store_inode_locked(inode, &disk_inode, false);
out:
    spin_unlock(&fs->lock);
    if (!ret && !S_ISDIR(disk_inode.i_mode) && stat->size != old_size)
        page_cache_truncate(&inode->i_mapping, stat->size);
    return ret;
}

static loff_t ext_llseek(struct vfs_file *file, loff_t offset, int whence) {
    loff_t pos;

    if (!file || !file->f_inode)
        return -EBADF;

    spin_lock(&file->f_pos_lock);
    switch (whence) {
    case SEEK_SET:
        pos = offset;
        break;
    case SEEK_CUR:
        pos = file->f_pos + offset;
        break;
    case SEEK_END:
        pos = (loff_t)file->f_inode->i_size + offset;
        break;
    default:
        spin_unlock(&file->f_pos_lock);
        return -EINVAL;
    }
    if (pos < 0) {
        spin_unlock(&file->f_pos_lock);
        return -EINVAL;
    }
    file->f_pos = pos;
    spin_unlock(&file->f_pos_lock);
    return pos;
}

static ssize_t ext_read(struct vfs_file *file, void *buf, size_t count,
                        loff_t *ppos) {
    ext_mount_ctx_t *fs;
    ext_inode_disk_t disk_inode = {0};
    int ret;

    if (!file || !file->f_inode || !buf || !ppos)
        return -EINVAL;
    if (S_ISBLK(file->f_inode->i_mode) || S_ISCHR(file->f_inode->i_mode))
        return device_read(file->f_inode->i_rdev, buf, (size_t)*ppos, count,
                           file);
    if (!S_ISREG(file->f_inode->i_mode))
        return -EINVAL;

    return page_cache_read(file, buf, count, ppos);
}

static ssize_t ext_write(struct vfs_file *file, const void *buf, size_t count,
                         loff_t *ppos) {
    ext_mount_ctx_t *fs;
    ext_inode_disk_t disk_inode = {0};
    int ret;

    if (!file || !file->f_inode || !buf || !ppos)
        return -EINVAL;
    if (S_ISBLK(file->f_inode->i_mode) || S_ISCHR(file->f_inode->i_mode))
        return device_write(file->f_inode->i_rdev, (void *)buf, (size_t)*ppos,
                            count, file);
    if (!S_ISREG(file->f_inode->i_mode))
        return -EINVAL;

    fs = ext_sb_info(file->f_inode->i_sb);
    spin_lock(&fs->lock);
    ret = ext_load_inode_locked(file->f_inode, &disk_inode);
    if (!ret) {
        uint64_t old_size = ext_inode_size_get(&disk_inode);
        loff_t old_pos = *ppos;
        if ((uint64_t)old_pos > old_size) {
            ret =
                ext_inode_resize_locked(fs, (uint32_t)file->f_inode->i_ino,
                                        &disk_inode, (uint64_t)old_pos, false);
            if (ret)
                goto out_unlock;
            ret = ext_store_inode_locked(file->f_inode, &disk_inode, false);
            if (ret)
                goto out_unlock;
        }
    }
out_unlock:
    spin_unlock(&fs->lock);

    if (ret < 0)
        return ret;

    ret = page_cache_write(file, buf, count, ppos);
    if (ret < 0)
        return ret;

    spin_lock(&fs->lock);
    int load_ret = ext_load_inode_locked(file->f_inode, &disk_inode);
    if (!load_ret) {
        if (file->f_inode->i_size > ext_inode_size_get(&disk_inode))
            ext_inode_size_set(&disk_inode, file->f_inode->i_size);
        ext_inode_touch(&disk_inode, false, true, true);
        load_ret = ext_store_inode_locked(file->f_inode, &disk_inode, false);
    }
    spin_unlock(&fs->lock);
    if (load_ret < 0)
        return load_ret;
    if (file->f_flags & (O_SYNC | O_DSYNC | O_TRUNC)) {
        bool datasync = (file->f_flags & O_DSYNC) && !(file->f_flags & O_TRUNC);
        load_ret = ext_fsync(file, 0, LLONG_MAX, datasync);
        if (load_ret < 0)
            return load_ret;
    }
    return ret;
}

static int ext_iterate_shared(struct vfs_file *file,
                              struct vfs_dir_context *ctx) {
    int ret;

    if (!file || !file->f_inode || !ctx)
        return -EINVAL;

    spin_lock(&ext_sb_info(file->f_inode->i_sb)->lock);
    ret = ext_iterate_dir_locked(file->f_inode, ctx);
    spin_unlock(&ext_sb_info(file->f_inode->i_sb)->lock);
    if (!ret)
        file->f_pos = ctx->pos;
    return ret;
}

static long ext_unlocked_ioctl(struct vfs_file *file, unsigned long cmd,
                               unsigned long arg) {
    if (!file || !file->f_inode)
        return -EBADF;
    if (S_ISBLK(file->f_inode->i_mode) || S_ISCHR(file->f_inode->i_mode))
        return device_ioctl(file->f_inode->i_rdev, (ssize_t)cmd, (void *)arg,
                            file);
    return -ENOTTY;
}

static __poll_t ext_poll(struct vfs_file *file, struct vfs_poll_table *pt) {
    (void)pt;
    if (!file || !file->f_inode)
        return EPOLLNVAL;
    if (S_ISBLK(file->f_inode->i_mode) || S_ISCHR(file->f_inode->i_mode))
        return device_poll(file->f_inode->i_rdev,
                           EPOLLIN | EPOLLOUT | EPOLLRDNORM | EPOLLWRNORM,
                           file);
    return EPOLLIN | EPOLLOUT | EPOLLRDNORM | EPOLLWRNORM;
}

static int ext_open(struct vfs_inode *inode, struct vfs_file *file) {
    if (!inode || !file)
        return -EINVAL;
    file->f_op = inode->i_fop;
    if (S_ISBLK(inode->i_mode) || S_ISCHR(inode->i_mode))
        return device_open(inode->i_rdev, file);
    return 0;
}

static int ext_release(struct vfs_inode *inode, struct vfs_file *file) {
    (void)file;
    if (!inode)
        return 0;
    if (S_ISBLK(inode->i_mode) || S_ISCHR(inode->i_mode))
        device_close(inode->i_rdev, file);
    return 0;
}

static void *ext_mmap(struct vfs_file *file, void *addr, size_t offset,
                      size_t size, size_t prot, uint64_t flags) {
    if (!file || !file->f_inode)
        return (void *)(int64_t)-EINVAL;
    if ((flags & MAP_TYPE) == MAP_SHARED ||
        (flags & MAP_TYPE) == MAP_SHARED_VALIDATE) {
        return addr;
    }
    return general_map(file, (uint64_t)addr, size, prot, flags, offset);
}

static int ext_fsync(struct vfs_file *file, loff_t start, loff_t end,
                     int datasync) {
    ext_mount_ctx_t *fs;
    ext_inode_disk_t disk_inode = {0};
    ext_inode_disk_t stable_inode = {0};
    int ret;

    if (!file || !file->f_inode)
        return -EINVAL;
    if (start < 0)
        start = 0;
    if (end < start)
        return 0;
    ret = page_cache_writeback_range(&file->f_inode->i_mapping, (uint64_t)start,
                                     (uint64_t)end + 1, datasync != 0);
    if (ret < 0)
        return ret;

    if (!S_ISREG(file->f_inode->i_mode))
        return 0;

    fs = ext_sb_info(file->f_inode->i_sb);
    spin_lock(&fs->lock);
    ret = ext_load_inode_locked(file->f_inode, &disk_inode);
    if (!ret) {
        ret = ext_read_inode(fs, (uint32_t)file->f_inode->i_ino, &stable_inode);
        if (!ret) {
            if (datasync)
                ext_inode_copy_size_blocks(&stable_inode, &disk_inode);
            else
                stable_inode = disk_inode;
            ret = ext_store_inode_locked(file->f_inode, &stable_inode, true);
        }
    }
    spin_unlock(&fs->lock);
    return ret;
}

static int ext_flush(struct vfs_file *file) {
    if (!file || !file->f_inode || !S_ISREG(file->f_inode->i_mode))
        return 0;
    return ext_fsync(file, 0, LLONG_MAX, 0);
}

static int ext_readpage(struct vfs_file *file,
                        struct vfs_address_space *mapping, uint64_t index,
                        void *page) {
    ext_mount_ctx_t *fs;
    ext_inode_disk_t disk_inode = {0};
    int ret;

    struct vfs_inode *inode =
        file && file->f_inode ? file->f_inode : mapping->host;
    if (!inode || !page)
        return -EINVAL;
    memset(page, 0, PAGE_SIZE);
    if (S_ISBLK(inode->i_mode) || S_ISCHR(inode->i_mode))
        return device_read(inode->i_rdev, page, index * PAGE_SIZE, PAGE_SIZE,
                           file);
    if (!S_ISREG(inode->i_mode))
        return -EINVAL;

    fs = ext_sb_info(inode->i_sb);
    spin_lock(&fs->lock);
    ret = ext_load_inode_locked(inode, &disk_inode);
    if (!ret)
        ret =
            ext_read_inode_data_locked(fs, (uint32_t)inode->i_ino, &disk_inode,
                                       page, index * PAGE_SIZE, PAGE_SIZE);
    spin_unlock(&fs->lock);
    return ret < 0 ? ret : 0;
}

static int ext_writepage(struct vfs_file *file,
                         struct vfs_address_space *mapping, uint64_t index,
                         const void *page) {
    ext_mount_ctx_t *fs;
    ext_inode_disk_t disk_inode = {0};
    int ret;

    struct vfs_inode *inode =
        file && file->f_inode ? file->f_inode : mapping->host;
    if (!inode || !page)
        return -EINVAL;
    if (S_ISBLK(inode->i_mode) || S_ISCHR(inode->i_mode))
        return device_write(inode->i_rdev, (void *)page, index * PAGE_SIZE,
                            PAGE_SIZE, file);
    if (!S_ISREG(inode->i_mode))
        return -EINVAL;

    fs = ext_sb_info(inode->i_sb);
    spin_lock(&fs->lock);
    ret = ext_load_inode_locked(inode, &disk_inode);
    if (!ret) {
        uint64_t page_off = index * PAGE_SIZE;
        uint64_t inode_size = inode->i_size;
        uint64_t disk_size = ext_inode_size_get(&disk_inode);
        uint64_t logical_end = page_off + PAGE_SIZE;
        uint64_t write_end = MIN(logical_end, inode_size);
        uint64_t alloc_end = MAX(disk_size, write_end);

        if (alloc_end > disk_size) {
            ext_inode_size_set(&disk_inode, alloc_end);
            ext_inode_touch(&disk_inode, false, true, true);
        }
        if (page_off >= alloc_end)
            goto out_unlock;

        uint64_t page_end =
            MIN(logical_end, PADDING_UP(alloc_end, fs->block_size));
        for (uint64_t off = page_off; off < page_end;) {
            uint32_t lblock = (uint32_t)(off / fs->block_size);
            uint64_t pblock = 0;
            uint32_t run_blocks = 0;
            uint32_t max_blocks = (uint32_t)((page_end - off) / fs->block_size);
            size_t run_size;

            ret = ext_inode_get_block_run_locked(
                fs, (uint32_t)inode->i_ino, &disk_inode, lblock, true, &pblock,
                &run_blocks, max_blocks);
            if (ret)
                break;
            if (!pblock || !run_blocks) {
                ret = -ENOSPC;
                break;
            }
            run_size = (size_t)run_blocks * fs->block_size;
            ret = ext_dev_write_direct(fs, pblock * fs->block_size,
                                       (const uint8_t *)page + (off - page_off),
                                       run_size);
            if (ret)
                break;
            off += run_size;
        }
        if (ret >= 0)
            ret = ext_store_inode_locked(inode, &disk_inode, false);
    }
out_unlock:
    spin_unlock(&fs->lock);
    return ret < 0 ? ret : 0;
}

static const struct vfs_address_space_operations ext_a_ops = {
    .readpage = ext_readpage,
    .writepage = ext_writepage,
    .invalidatepage = NULL,
};

static struct vfs_inode *ext_alloc_inode(struct vfs_super_block *sb) {
    ext_inode_info_t *info = calloc(1, sizeof(*info));
    if (!info)
        return NULL;
    info->vfs_inode.i_mapping.a_ops = &ext_a_ops;
    return &info->vfs_inode;
}

static void ext_destroy_inode(struct vfs_inode *inode) {
    ext_inode_info_t *info = ext_i(inode);
    if (!info)
        return;
    if (info->symlink)
        free(info->symlink);
    info->symlink = NULL;
    free(info);
}

static void ext_evict_inode(struct vfs_inode *inode) {
    ext_inode_info_t *info;
    ext_mount_ctx_t *fs;
    ext_inode_disk_t disk_inode = {0};

    if (!inode || !inode->i_sb)
        return;

    info = ext_i(inode);
    fs = ext_sb_info(inode->i_sb);
    if (!info || !fs)
        return;

    if (inode->i_nlink != 0) {
        page_cache_evict(&inode->i_mapping);
        return;
    }

    (void)page_cache_invalidate_range(&inode->i_mapping, 0, UINT64_MAX, false);

    spin_lock(&fs->lock);
    if (!info->inode_valid) {
        if (ext_read_inode(fs, (uint32_t)inode->i_ino, &info->inode_cache)) {
            spin_unlock(&fs->lock);
            return;
        }
        info->inode_valid = true;
    }

    disk_inode = info->inode_cache;
    if (disk_inode.i_dtime)
        (void)ext_release_inode_locked(fs, (uint32_t)inode->i_ino, &disk_inode);
    spin_unlock(&fs->lock);
}

static void ext_put_super(struct vfs_super_block *sb) {
    ext_mount_ctx_t *fs = ext_sb_info(sb);
    if (!fs)
        return;
    ext_map_cache_destroy(fs);
    free(fs->groups);
    free(fs);
}

static int ext_statfs(struct vfs_path *path, void *buf) {
    struct statfs *st = (struct statfs *)buf;
    struct vfs_super_block *sb;
    ext_mount_ctx_t *fs;
    uint64_t free_blocks = 0;
    uint64_t free_inodes = 0;
    uint64_t reserved_blocks;

    if (!path || !path->dentry || !path->dentry->d_inode || !st)
        return -EINVAL;

    sb = path->dentry->d_inode->i_sb;
    fs = ext_sb_info(sb);
    if (!fs)
        return -EINVAL;

    memset(st, 0, sizeof(*st));

    spin_lock(&fs->lock);
    for (uint32_t i = 0; i < fs->group_count; i++) {
        free_blocks += ext_group_free_blocks_count(&fs->groups[i]);
        free_inodes += ext_group_free_inodes_count(&fs->groups[i]);
    }

    reserved_blocks = ext_sb_reserved_blocks_count(&fs->sb);

    st->f_bsize = fs->block_size;
    st->f_frsize = fs->block_size;
    st->f_blocks = fs->blocks_count;
    st->f_bfree = free_blocks;
    st->f_bavail =
        ext_reserved_blocks_available_to_caller(fs)
            ? free_blocks
            : (free_blocks > reserved_blocks ? free_blocks - reserved_blocks
                                             : 0);
    st->f_files = fs->inodes_count;
    st->f_ffree = free_inodes;
    st->f_namelen = VFS_NAME_MAX;
    spin_unlock(&fs->lock);

    return 0;
}

static int ext_resolve_mount_dev(struct vfs_fs_context *fc, uint64_t *dev_out) {
    struct vfs_path path = {0};
    char *end = NULL;
    uint64_t dev;
    int ret;

    if (!fc || !dev_out)
        return -EINVAL;

    dev = (uint64_t)(uintptr_t)fc->data;
    if (dev) {
        *dev_out = dev;
        return 0;
    }

    if (!fc->source || !fc->source[0])
        return -EINVAL;

    dev = strtoul(fc->source, &end, 0);
    if (end && *end == '\0' && dev) {
        *dev_out = dev;
        return 0;
    }

    ret = vfs_filename_lookup(AT_FDCWD, fc->source, LOOKUP_FOLLOW, &path);
    if (ret < 0)
        return ret;

    if (!path.dentry || !path.dentry->d_inode) {
        vfs_path_put(&path);
        return -ENOENT;
    }

    dev = path.dentry->d_inode->i_rdev ? path.dentry->d_inode->i_rdev
                                       : path.dentry->d_inode->i_sb->s_dev;
    vfs_path_put(&path);
    if (!dev)
        return -EINVAL;

    *dev_out = dev;
    return 0;
}

static int ext_init_fs_context(struct vfs_fs_context *fc) {
    fc->sb = vfs_alloc_super(fc->fs_type, fc->sb_flags);
    if (!fc->sb)
        return -ENOMEM;
    return 0;
}

static int ext_get_tree(struct vfs_fs_context *fc) {
    struct vfs_super_block *sb = fc ? fc->sb : NULL;
    ext_mount_ctx_t *fs = NULL;
    struct vfs_inode *root_inode = NULL;
    struct vfs_dentry *root_dentry = NULL;
    struct vfs_qstr root_name = {.name = "", .len = 0, .hash = 0};
    uint64_t dev = 0;
    int ret;

    if (!sb)
        return -EINVAL;

    ret = ext_resolve_mount_dev(fc, &dev);
    if (ret)
        return ret;

    fs = calloc(1, sizeof(*fs));
    if (!fs)
        return -ENOMEM;
    spin_init(&fs->lock);

    spin_lock(&fs->lock);
    ret = ext_mount_prepare_locked(fs, dev);
    spin_unlock(&fs->lock);
    if (ret) {
        ext_map_cache_destroy(fs);
        free(fs->groups);
        free(fs);
        return ret;
    }

    sb->s_fs_info = fs;
    sb->s_op = &ext_super_ops;
    sb->s_type = &ext_fs_type;
    sb->s_magic = EXT_SUPER_MAGIC;
    sb->s_dev = dev;

    spin_lock(&fs->lock);
    root_inode = ext_iget_locked(sb, EXT_ROOT_INO);
    spin_unlock(&fs->lock);
    if (IS_ERR(root_inode))
        return PTR_ERR(root_inode);

    root_dentry = vfs_d_alloc(sb, NULL, &root_name);
    if (!root_dentry) {
        vfs_iput(root_inode);
        return -ENOMEM;
    }

    vfs_d_instantiate(root_dentry, root_inode);
    sb->s_root = root_dentry;
    vfs_iput(root_inode);
    return 0;
}

static const struct vfs_super_operations ext_super_ops = {
    .alloc_inode = ext_alloc_inode,
    .destroy_inode = ext_destroy_inode,
    .evict_inode = ext_evict_inode,
    .put_super = ext_put_super,
    .statfs = ext_statfs,
    .get_quota = ext_get_quota,
};

static const struct vfs_file_operations ext_dir_ops = {
    .llseek = ext_llseek,
    .iterate_shared = ext_iterate_shared,
    .open = ext_open,
    .release = ext_release,
    .fsync = ext_fsync,
    .poll = ext_poll,
};

static const struct vfs_file_operations ext_file_ops = {
    .llseek = ext_llseek,
    .read = ext_read,
    .write = ext_write,
    .unlocked_ioctl = ext_unlocked_ioctl,
    .open = ext_open,
    .flush = ext_flush,
    .release = ext_release,
    .mmap = ext_mmap,
    .fsync = ext_fsync,
    .poll = ext_poll,
};

static const struct vfs_inode_operations ext_inode_ops = {
    .lookup = ext_lookup,
    .create = ext_create,
    .link = ext_link,
    .unlink = ext_unlink,
    .symlink = ext_symlink,
    .mkdir = ext_mkdir,
    .rmdir = ext_rmdir,
    .mknod = ext_mknod,
    .rename = ext_rename,
    .get_link = ext_get_link,
    .permission = ext_permission,
    .getattr = ext_getattr,
    .setattr = ext_setattr,
    .getxattr = ext_getxattr,
    .listxattr = ext_listxattr,
    .setxattr = ext_setxattr,
    .removexattr = ext_removexattr,
};

static struct vfs_file_system_type ext_fs_type = {
    .name = "ext",
    .fs_flags = VFS_FS_REQUIRES_DEV,
    .init_fs_context = ext_init_fs_context,
    .get_tree = ext_get_tree,
};

void ext_init(void) { vfs_register_filesystem(&ext_fs_type); }

int dlmain() {
    ext_init();
    return 0;
}
