#pragma once

#include <libs/klibc.h>
#include <fs/vfs/vfs.h>

#define MAX_PARTITIONS_NUM 128

#define MBR_MAX_PARTITION_NUM 4

struct MBR_DPTE {
    uint8_t flags;
    uint8_t start_head;
    uint16_t start_sector : 6, // 0~5
        start_cylinder : 10;   // 6~15
    uint8_t type;
    uint8_t end_head;
    uint16_t end_sector : 6, // 0~5
        end_cylinder : 10;   // 6~15
    uint32_t start_lba;
    uint32_t sectors_limit;
} __attribute__((packed));

struct MBR_DPT {
    uint8_t bs_reserved[446];
    struct MBR_DPTE dpte[4];
    uint16_t bs_trail_sig;
} __attribute__((packed));

#define GPT_MAX_PARTITION_NUM 128
#define GPT_HEADER_SIGNATURE "EFI PART"

struct GPT_DPT {
    char signature[8];                // 签名，必须是 "EFI PART"
    uint32_t revision;                // 修订版本，通常为 0x00010000
    uint32_t header_size;             // 头部大小，通常为 92 字节
    uint32_t header_crc32;            // 头部CRC32校验值
    uint32_t reserved;                // 保留字段，必须为 0
    uint64_t my_lba;                  // 当前LBA（这个头部所在的LBA）
    uint64_t alternate_lba;           // 备份头部所在的LBA
    uint64_t first_usable_lba;        // 第一个可用的LBA（用于分区）
    uint64_t last_usable_lba;         // 最后一个可用的LBA（用于分区）
    uint8_t disk_guid[16];            // 磁盘的GUID
    uint64_t partition_entry_lba;     // 分区表项的起始LBA
    uint32_t num_partition_entries;   // 分区表项的数量
    uint32_t size_of_partition_entry; // 每个分区表项的大小（通常为 128 字节）
    uint32_t partition_entry_array_crc32; // 分区表项的CRC32校验值
} __attribute__((packed));

struct GPT_DPTE {
    uint8_t partition_type_guid[16];   // Partition type GUID
    uint8_t unique_partition_guid[16]; // Unique partition GUID
    uint64_t starting_lba;             // Starting LBA of the partition
    uint64_t ending_lba;               // Ending LBA of the partition
    uint64_t attributes;               // Partition attributes
    uint16_t partition_name[36]; // Partition name (UTF-16LE, null-terminated)
};

typedef struct partition {
    uint64_t dev;
    uint64_t blkdev_id;
    uint64_t starting_lba;
    uint64_t ending_lba;
    enum {
        MBR = 1,
        GPT,
        RAW,
    } type;
} partition_t;

extern partition_t partitions[MAX_PARTITIONS_NUM];
extern uint64_t partition_num;
extern spinlock_t partition_lock;

ssize_t partition_read(void *data, void *buf, uint64_t offset, uint64_t len,
                       uint64_t flags);
ssize_t partition_write(void *data, const void *buf, uint64_t offset,
                        uint64_t len, uint64_t flags);
int partition_ioctl(void *data, int cmd, void *args, fd_t *fd);
