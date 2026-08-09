#include "nvme.h"

volatile uint64_t nvme_drive_id = 0;

// Memory allocation (DMA-capable)
void *naos_dma_alloc(size_t size, uint64_t *phys_addr) {
    void *addr = alloc_frames_bytes(size);
    if (addr) {
        if (phys_addr)
            *phys_addr = virt_to_phys((const void *)addr);
        memset(addr, 0, size);
    }
    return addr;
}
void naos_dma_free(void *virt, size_t size) { free_frames_bytes(virt, size); }

void naos_memory_barrier(void) { dma_mb(); }

void naos_read_barrier(void) { dma_rmb(); }

void naos_write_barrier(void) { dma_wmb(); }

void naos_udelay(uint32_t us) {
    uint64_t ns = nano_time() + (uint64_t)us * 1000;
    while (nano_time() < ns) {
        arch_pause();
    }
}
uint64_t naos_get_time_ms(void) { return nano_time() / 1000000; }

// Locking (for multi-threaded environments)
void *naos_mutex_create(void) { return NULL; }
void naos_spin_lock(void *mutex) {}
void naos_spin_unlock(void *mutex) {}
void naos_mutex_destroy(void *mutex) {}

int naos_printk(const char *fmt, ...) {
    char buf[2048];
    memset(buf, 0, sizeof(buf));
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    printk(buf);
    return n;
}

nvme_platform_ops_t naos_nvme_platform_ops = {
    .dma_alloc = naos_dma_alloc,
    .dma_free = naos_dma_free,
    .mb = naos_memory_barrier,
    .rmb = naos_read_barrier,
    .wmb = naos_write_barrier,
    .udelay = naos_udelay,
    .get_time_ms = naos_get_time_ms,
    .mutex_create = naos_mutex_create,
    .spin_lock = naos_spin_lock,
    .spin_unlock = naos_spin_unlock,
    .mutex_destroy = naos_mutex_destroy,
    .log = naos_printk,
};

nvme_platform_ops_t *nvme_platform_ops = NULL;

// Helper macros with validation
#define NVME_READ32(ctrl, offset)                                              \
    (*(volatile uint32_t *)((ctrl)->bar0 + (offset)))

#define NVME_WRITE32(ctrl, offset, value)                                      \
    do {                                                                       \
        *(volatile uint32_t *)((ctrl)->bar0 + (offset)) = (value);             \
        nvme_platform_ops->mb();                                               \
    } while (0)

#define NVME_READ64(ctrl, offset)                                              \
    (*(volatile uint64_t *)((ctrl)->bar0 + (offset)))

#define NVME_WRITE64(ctrl, offset, value)                                      \
    do {                                                                       \
        *(volatile uint64_t *)((ctrl)->bar0 + (offset)) = (value);             \
        nvme_platform_ops->mb();                                               \
    } while (0)

void nvme_set_platform_ops(nvme_platform_ops_t *ops) {
    nvme_platform_ops = ops;
}

// Dump controller status for debugging
static void nvme_dump_status(nvme_controller_t *ctrl) {
    uint32_t csts = NVME_READ32(ctrl, NVME_REG_CSTS);
    uint32_t cc = NVME_READ32(ctrl, NVME_REG_CC);

    nvme_platform_ops->log("NVMe: CSTS=0x%08x CC=0x%08x\n", csts, cc);
    nvme_platform_ops->log("  RDY=%d CFS=%d SHST=%d NSSRO=%d\n",
                           !!(csts & NVME_CSTS_RDY), !!(csts & NVME_CSTS_CFS),
                           (csts >> 2) & 0x3, !!(csts & (1 << 4)));
    nvme_platform_ops->log(
        "  EN=%d CSS=%d MPS=%d AMS=%d SHN=%d IOSQES=%d IOCQES=%d\n",
        !!(cc & NVME_CC_ENABLE), (cc >> 4) & 0x7, (cc >> 7) & 0xF,
        (cc >> 11) & 0x7, (cc >> 14) & 0x3, (cc >> 16) & 0xF, (cc >> 20) & 0xF);
}

// Enhanced wait for ready with better error reporting
static int nvme_wait_ready(nvme_controller_t *ctrl, bool ready,
                           uint32_t timeout_ms) {
    uint64_t start = nvme_platform_ops->get_time_ms();
    uint32_t last_csts = 0;

    while (1) {
        uint32_t csts = NVME_READ32(ctrl, NVME_REG_CSTS);

        // Check for fatal status first
        if (csts & NVME_CSTS_CFS) {
            nvme_platform_ops->log("NVMe: Controller Fatal Status detected!\n");
            nvme_dump_status(ctrl);
            return -1;
        }

        bool is_ready = (csts & NVME_CSTS_RDY) != 0;

        if (is_ready == ready) {
            nvme_platform_ops->log("NVMe: Controller ready state reached: %d\n",
                                   ready);
            return 0;
        }

        // Log status changes
        if (csts != last_csts) {
            nvme_platform_ops->log("NVMe: CSTS changed: 0x%08x -> 0x%08x\n",
                                   last_csts, csts);
            last_csts = csts;
        }

        if (nvme_platform_ops->get_time_ms() - start > timeout_ms) {
            nvme_platform_ops->log("NVMe: Timeout waiting for ready=%d\n",
                                   ready);
            nvme_dump_status(ctrl);
            return -1;
        }

        nvme_platform_ops->udelay(100);
    }
}

// Enhanced controller reset
static int nvme_reset_controller(nvme_controller_t *ctrl) {
    nvme_platform_ops->log("NVMe: Resetting controller...\n");

    // Read current state
    uint32_t cc = NVME_READ32(ctrl, NVME_REG_CC);
    uint32_t csts = NVME_READ32(ctrl, NVME_REG_CSTS);

    nvme_platform_ops->log("NVMe: Initial state - CC=0x%08x CSTS=0x%08x\n", cc,
                           csts);

    // If controller is enabled, disable it
    if (cc & NVME_CC_ENABLE) {
        nvme_platform_ops->log("NVMe: Controller is enabled, disabling...\n");

        // Clear enable bit
        cc &= ~NVME_CC_ENABLE;
        NVME_WRITE32(ctrl, NVME_REG_CC, cc);

        // Wait for ready to clear
        if (nvme_wait_ready(ctrl, false, 5000) != 0) {
            nvme_platform_ops->log("NVMe: Failed to disable controller\n");
            return -1;
        }
    } else {
        // Even if not enabled, ensure RDY is clear
        if (csts & NVME_CSTS_RDY) {
            nvme_platform_ops->log(
                "NVMe: Warning - EN=0 but RDY=1, waiting...\n");
            if (nvme_wait_ready(ctrl, false, 5000) != 0) {
                return -1;
            }
        }
    }

    nvme_platform_ops->log("NVMe: Controller disabled successfully\n");
    return 0;
}

// Disable controller
static int nvme_disable_controller(nvme_controller_t *ctrl) {
    return nvme_reset_controller(ctrl);
}

// Enable controller with proper configuration
static int nvme_enable_controller(nvme_controller_t *ctrl) {
    nvme_platform_ops->log("NVMe: Enabling controller...\n");

    // Ensure controller is disabled first
    uint32_t csts = NVME_READ32(ctrl, NVME_REG_CSTS);
    if (csts & NVME_CSTS_RDY) {
        nvme_platform_ops->log("NVMe: Controller still ready, cannot enable\n");
        return -1;
    }

    // Calculate MPS (Memory Page Size)
    // Use host page size (typically 4KB = 2^12, so MPS = 0)
    // MPS value = (log2(page_size) - 12)
    uint32_t mps = 0; // 4KB pages

    // Build CC register value
    uint32_t cc = 0;
    cc |= NVME_CC_ENABLE;             // Enable
    cc |= NVME_CC_CSS_NVM;            // NVM command set
    cc |= (mps << NVME_CC_MPS_SHIFT); // Memory page size
    cc |= NVME_CC_AMS_RR;             // Arbitration: Round Robin
    cc |= NVME_CC_SHN_NONE;           // No shutdown notification
    cc |= NVME_CC_IOSQES; // I/O Submission Queue Entry Size (64 bytes)
    cc |= NVME_CC_IOCQES; // I/O Completion Queue Entry Size (16 bytes)

    nvme_platform_ops->log("NVMe: Writing CC=0x%08x\n", cc);
    NVME_WRITE32(ctrl, NVME_REG_CC, cc);

    // Verify write
    uint32_t cc_read = NVME_READ32(ctrl, NVME_REG_CC);
    if (cc_read != cc) {
        nvme_platform_ops->log(
            "NVMe: CC register write failed! Expected 0x%08x, got 0x%08x\n", cc,
            cc_read);
        return -1;
    }

    // Wait for ready
    nvme_platform_ops->log("NVMe: Waiting for controller ready...\n");
    return nvme_wait_ready(ctrl, true, 10000); // Increased timeout to 10s
}

// Initialize queue with alignment checks
static int nvme_init_queue(nvme_controller_t *ctrl, nvme_queue_t *queue,
                           uint16_t queue_id, uint16_t queue_depth) {
    queue->ctrl = ctrl;

    spin_init(&queue->lock);

    queue->queue_id = queue_id;
    queue->queue_depth = queue_depth;
    queue->sq_head = 0;
    queue->sq_tail = 0;
    queue->cq_head = 0;
    queue->cq_phase = 1;

    size_t sq_size = sizeof(nvme_sqe_t) * queue_depth;
    size_t cq_size = sizeof(nvme_cqe_t) * queue_depth;

    // Allocate submission queue (must be aligned to page size)
    queue->sq = nvme_platform_ops->dma_alloc(sq_size, &queue->sq_phys);
    if (!queue->sq) {
        nvme_platform_ops->log("NVMe: Failed to allocate SQ\n");
        return -1;
    }

    // Check alignment
    if (queue->sq_phys & 0xFFF) {
        nvme_platform_ops->log("NVMe: Warning - SQ not page aligned: 0x%llx\n",
                               queue->sq_phys);
    }

    memset(queue->sq, 0, sq_size);
    nvme_platform_ops->log(
        "NVMe: SQ allocated at virt=%p phys=0x%llx size=%d\n", queue->sq,
        queue->sq_phys, sq_size);

    // Allocate completion queue
    queue->cq = nvme_platform_ops->dma_alloc(cq_size, &queue->cq_phys);
    if (!queue->cq) {
        nvme_platform_ops->log("NVMe: Failed to allocate CQ\n");
        nvme_platform_ops->dma_free(queue->sq, sq_size);
        return -1;
    }

    if (queue->cq_phys & 0xFFF) {
        nvme_platform_ops->log("NVMe: Warning - CQ not page aligned: 0x%llx\n",
                               queue->cq_phys);
    }

    memset(queue->cq, 0, cq_size);
    nvme_platform_ops->log(
        "NVMe: CQ allocated at virt=%p phys=0x%llx size=%d\n", queue->cq,
        queue->cq_phys, cq_size);

    // Calculate doorbell addresses
    uint32_t doorbell_offset =
        NVME_REG_DBS + (2 * queue_id * ctrl->doorbell_stride);
    queue->sq_doorbell = (volatile uint32_t *)(ctrl->bar0 + doorbell_offset);
    queue->cq_doorbell = (volatile uint32_t *)(ctrl->bar0 + doorbell_offset +
                                               ctrl->doorbell_stride);

    nvme_platform_ops->log(
        "NVMe: Queue %d doorbells - SQ offset=0x%x CQ offset=0x%x\n", queue_id,
        doorbell_offset, doorbell_offset + ctrl->doorbell_stride);

    queue->vector = 0;

    return 0;
}

static int nvme_submit_cmd(nvme_queue_t *queue, nvme_sqe_t *cmd) {
    spin_lock(&queue->lock);
    uint16_t tail = queue->sq_tail;
    uint16_t next_tail = (tail + 1) % queue->queue_depth;
    if (next_tail == queue->sq_head) {
        spin_unlock(&queue->lock);
        return -1;
    }

    memcpy(&queue->sq[tail], cmd, sizeof(nvme_sqe_t));
    queue->sq_tail = next_tail;

    nvme_platform_ops->wmb();
    *queue->sq_doorbell = next_tail;
    nvme_platform_ops->mb();
    spin_unlock(&queue->lock);
    return 0;
}

static inline void nvme_reset_request(nvme_request_t *req) {
    req->callback = NULL;
    req->ctx = NULL;
    req->next = NULL;
    if (req->prp_list)
        memset(req->prp_list, 0, sizeof(nvme_prp_list_t));
}

static inline void nvme_complete_request(nvme_controller_t *ctrl, uint16_t cid,
                                         bool success, uint32_t result) {
    if (cid >= (sizeof(ctrl->requests) / sizeof(ctrl->requests[0])))
        return;

    nvme_request_t *req =
        __atomic_exchange_n(&ctrl->requests[cid], NULL, __ATOMIC_ACQ_REL);
    if (!req)
        return;

    nvme_io_callback_t callback = req->callback;
    void *ctx = req->ctx;
    nvme_reset_request(req);

    if (callback)
        callback(ctx, success, result);
}

static void nvme_log_cqe_error(nvme_queue_t *queue, const nvme_cqe_t *cqe) {
    uint16_t status_code = (cqe->status >> 1) & 0xFF;
    uint16_t status_type = (cqe->status >> 9) & 0x7;
    uint16_t crd = (cqe->status >> 12) & 0x3;
    bool more = (cqe->status & (1u << 14)) != 0;
    bool dnr = (cqe->status & (1u << 15)) != 0;

    nvme_platform_ops->log(
        "NVMe: CQE error qid=%u cid=%u sq_head=%u sct=%u sc=%u crd=%u more=%d "
        "dnr=%d dw0=0x%08x\n",
        queue->queue_id, cqe->cid, cqe->sq_head, status_type, status_code, crd,
        more, dnr, cqe->dw0);
}

static int nvme_process_queue_completions(nvme_controller_t *ctrl,
                                          nvme_queue_t *queue) {
    int count = 0;

    while (1) {
        uint16_t cid;
        uint32_t result;
        bool success;

        spin_lock(&queue->lock);
        nvme_platform_ops->rmb();

        nvme_cqe_t *cqe = &queue->cq[queue->cq_head];
        uint16_t phase = cqe->status & 1;
        if (phase != queue->cq_phase) {
            spin_unlock(&queue->lock);
            break;
        }

        uint16_t status_code = (cqe->status >> 1) & 0xFF;
        uint16_t status_type = (cqe->status >> 9) & 0x7;
        success = (status_code == 0 && status_type == 0);
        cid = cqe->cid;
        result = cqe->dw0;

        if (!success)
            nvme_log_cqe_error(queue, cqe);

        queue->sq_head = cqe->sq_head;
        queue->cq_head++;
        if (queue->cq_head >= queue->queue_depth) {
            queue->cq_head = 0;
            queue->cq_phase = !queue->cq_phase;
        }

        nvme_platform_ops->wmb();
        *queue->cq_doorbell = queue->cq_head;
        nvme_platform_ops->mb();
        spin_unlock(&queue->lock);

        nvme_complete_request(ctrl, cid, success, result);
        count++;
    }

    return count;
}

static uint16_t nvme_alloc_cid(nvme_controller_t *ctrl, nvme_io_callback_t cb,
                               void *ctx) {
    spin_lock(&ctrl->cid_alloc_lock);

    uint16_t total = sizeof(ctrl->requests) / sizeof(ctrl->requests[0]);
    for (uint16_t i = 0; i < total; i++) {
        uint16_t cid = (ctrl->cid_alloc_pos + i) % total;
        if (__atomic_load_n(&ctrl->requests[cid], __ATOMIC_ACQUIRE))
            continue;

        nvme_request_t *slot = &ctrl->request_slots[cid];
        slot->cid = cid;
        slot->callback = cb;
        slot->ctx = ctx;
        slot->next = NULL;

        __atomic_store_n(&ctrl->requests[cid], slot, __ATOMIC_RELEASE);
        ctrl->cid_alloc_pos = (cid + 1) % total;
        spin_unlock(&ctrl->cid_alloc_lock);
        return cid;
    }

    spin_unlock(&ctrl->cid_alloc_lock);
    return 0xFFFF;
}

static inline void nvme_release_cid(nvme_controller_t *ctrl, uint16_t cid) {
    if (cid >= (sizeof(ctrl->requests) / sizeof(ctrl->requests[0])))
        return;

    nvme_request_t *req =
        __atomic_exchange_n(&ctrl->requests[cid], NULL, __ATOMIC_ACQ_REL);
    if (req)
        nvme_reset_request(req);
}

// Execute admin command (synchronous helper)
typedef struct {
    bool done;
    bool success;
    uint32_t result;
} admin_sync_ctx_t;

static void admin_sync_callback(void *ctx, bool success, uint32_t result) {
    admin_sync_ctx_t *sync_ctx = (admin_sync_ctx_t *)ctx;
    sync_ctx->success = success;
    sync_ctx->result = result;
    __atomic_store_n(&sync_ctx->done, true, __ATOMIC_RELEASE);
}

static int nvme_admin_cmd_sync(nvme_controller_t *ctrl, nvme_sqe_t *cmd,
                               uint32_t *result, uint32_t timeout_ms) {
    admin_sync_ctx_t sync_ctx = {0};

    uint16_t cid = nvme_alloc_cid(ctrl, admin_sync_callback, &sync_ctx);
    if (cid == 0xFFFF)
        goto error_ctx;

    uint64_t start = nvme_platform_ops->get_time_ms();

    if (timeout_ms == 0)
        timeout_ms = 5000;

    cmd->cdw0 = (cmd->cdw0 & 0x0000FFFF) | (cid << 16);

    if (nvme_submit_cmd(&ctrl->admin_queue, cmd) != 0) {
        nvme_release_cid(ctrl, cid);
        goto error_ctx;
    }

    while (!__atomic_load_n(&sync_ctx.done, __ATOMIC_ACQUIRE)) {
        if (!nvme_process_queue_completions(ctrl, &ctrl->admin_queue))
            arch_pause();

        if (nvme_platform_ops->get_time_ms() - start > timeout_ms) {
            nvme_platform_ops->log(
                "NVMe: admin command opcode=0x%02x cid=%u timed out after %u "
                "ms\n",
                cmd->cdw0 & 0xFF, cid, timeout_ms);
            nvme_dump_status(ctrl);
            return -1;
        }
    }

    if (result)
        *result = sync_ctx.result;

    bool success = sync_ctx.success;

    return success ? 0 : -1;

error_ctx:
    return -1;
}

// Identify Controller
static int nvme_identify_controller(nvme_controller_t *ctrl,
                                    nvme_identify_ctrl_t *id_ctrl) {
    uint64_t buffer_phys;
    void *buffer = nvme_platform_ops->dma_alloc(PAGE_SIZE, &buffer_phys);
    if (!buffer) {
        return -1;
    }
    memset(buffer, 0, PAGE_SIZE);

    nvme_sqe_t cmd = {0};
    cmd.cdw0 = NVME_ADMIN_IDENTIFY;
    cmd.prp1 = buffer_phys;
    cmd.cdw10 = 1; // CNS = 01h (Identify Controller)

    int ret = nvme_admin_cmd_sync(ctrl, &cmd, NULL, 5000);

    if (ret == 0) {
        memcpy(id_ctrl, buffer, sizeof(nvme_identify_ctrl_t));
    }

    nvme_platform_ops->dma_free(buffer, PAGE_SIZE);
    return ret;
}

static int nvme_identify_namespace(nvme_controller_t *ctrl, uint32_t nsid,
                                   nvme_identify_ns_t *id_ns) {
    uint64_t buffer_phys;
    void *buffer = nvme_platform_ops->dma_alloc(PAGE_SIZE, &buffer_phys);
    if (!buffer) {
        return -1;
    }
    memset(buffer, 0, PAGE_SIZE);

    nvme_sqe_t cmd = {0};
    cmd.cdw0 = NVME_ADMIN_IDENTIFY;
    cmd.nsid = nsid;
    cmd.prp1 = buffer_phys;
    cmd.cdw10 = 0; // CNS = 00h (Identify Namespace)

    int ret = nvme_admin_cmd_sync(ctrl, &cmd, NULL, 5000);

    if (ret == 0) {
        memcpy(id_ns, buffer, sizeof(nvme_identify_ns_t));
    }

    nvme_platform_ops->dma_free(buffer, PAGE_SIZE);
    return ret;
}

// create I/O Completion Queue
static int nvme_create_io_cq(nvme_controller_t *ctrl, nvme_queue_t *queue) {
    nvme_sqe_t cmd = {0};
    cmd.cdw0 = NVME_ADMIN_CREATE_CQ;
    cmd.prp1 = queue->cq_phys;
    cmd.cdw10 = ((queue->queue_depth - 1) << 16) | queue->queue_id;

    uint32_t cdw11 = 0;

    cdw11 |= (1 << 0);

    cmd.cdw11 = cdw11;

    nvme_platform_ops->log("NVMe: Creating I/O CQ %d (depth=%d, phys=0x%llx)\n",
                           queue->queue_id, queue->queue_depth, queue->cq_phys);

    return nvme_admin_cmd_sync(ctrl, &cmd, NULL, 5000);
}

//  Create I/O Submission Queue
static int nvme_create_io_sq(nvme_controller_t *ctrl, nvme_queue_t *queue) {
    nvme_sqe_t cmd = {0};
    cmd.cdw0 = NVME_ADMIN_CREATE_SQ;
    cmd.prp1 = queue->sq_phys;
    cmd.cdw10 = ((queue->queue_depth - 1) << 16) | queue->queue_id;
    cmd.cdw11 = ((uint32_t)queue->queue_id << 16) | 0x1; // CQID, PC=1

    nvme_platform_ops->log("NVMe: Creating I/O SQ %d (depth=%d, phys=0x%llx)\n",
                           queue->queue_id, queue->queue_depth, queue->sq_phys);

    return nvme_admin_cmd_sync(ctrl, &cmd, NULL, 5000);
}

// Check PCI configuration
static int nvme_check_pci_config(pci_device_t *device) {
    nvme_platform_ops->log("NVMe: Checking PCI configuration...\n");

    // Check BAR0
    if (!device->bars[0].address || device->bars[0].size < 0x2000) {
        nvme_platform_ops->log("NVMe: Invalid BAR0 (addr=0x%llx size=0x%llx)\n",
                               device->bars[0].address, device->bars[0].size);
        return -1;
    }

    if (!device->bars[0].mmio) {
        nvme_platform_ops->log("NVMe: BAR0 is not MMIO\n");
        return -1;
    }

    nvme_platform_ops->log("NVMe: BAR0 at 0x%llx, size 0x%llx\n",
                           device->bars[0].address, device->bars[0].size);

    // TODO: Enable PCI bus mastering and memory space if your platform requires
    // it This is platform-specific

    return 0;
}

static inline uint32_t nvme_calc_num_pages(uint64_t addr, uint32_t size) {
    uint64_t end = addr + size - 1;
    return (end >> 12) - (addr >> 12) + 1;
}

static inline uint32_t nvme_page_offset(uint64_t addr) {
    return addr & NVME_PAGE_MASK;
}

static int nvme_prepare_prp_list(nvme_request_t *req) {
    if (req->prp_list)
        return 0;

    req->prp_list = (nvme_prp_list_t *)nvme_platform_ops->dma_alloc(
        sizeof(nvme_prp_list_t), &req->prp_list_phys);
    if (!req->prp_list)
        return -1;

    memset(req->prp_list, 0, sizeof(nvme_prp_list_t));
    return 0;
}

static inline uint64_t nvme_translate_page_phys(uint64_t page_va) {
    return virt_to_phys((const void *)(uintptr_t)page_va);
}

static int nvme_setup_prp(nvme_controller_t *ctrl, nvme_request_t *req,
                          nvme_sqe_t *cmd, const void *buffer,
                          uint64_t buffer_phys, uint32_t size) {
    if (!buffer || size == 0)
        return -1;
    if (ctrl->max_transfer_size > 0 && size > ctrl->max_transfer_size)
        return -1;

    uint64_t vaddr = (uint64_t)buffer;
    uint32_t num_pages = nvme_calc_num_pages(vaddr, size);

    uint64_t first_page_phys = nvme_translate_page_phys(vaddr);
    if (buffer_phys) {
        uint64_t hinted = buffer_phys & ~((uint64_t)NVME_PAGE_MASK);
        if (hinted == first_page_phys)
            first_page_phys = hinted;
    }
    if (!first_page_phys) {
        printk("NVMe: first PRP page not mapped, vaddr=%#018lx\n", vaddr);
        return -1;
    }

    cmd->prp1 = first_page_phys;
    cmd->prp2 = 0;
    if (num_pages == 1)
        return 0;

    uint64_t second_page_phys =
        nvme_translate_page_phys(vaddr + NVME_PAGE_SIZE);
    if (!second_page_phys) {
        printk("NVMe: second PRP page not mapped, vaddr=%#018lx\n", vaddr);
        return -1;
    }
    if (num_pages == 2) {
        cmd->prp2 = second_page_phys;
        return 0;
    }
    if (num_pages - 1 > NVME_MAX_PRP_LIST_ENTRIES) {
        printk("NVMe: PRP page count too large (%u pages)\n", num_pages);
        return -1;
    }
    if (nvme_prepare_prp_list(req) != 0) {
        printk("NVMe: failed to allocate request PRP list\n");
        return -1;
    }

    memset(req->prp_list, 0, sizeof(nvme_prp_list_t));
    cmd->prp2 = req->prp_list_phys;
    req->prp_list->prp[0] = second_page_phys;

    for (uint32_t i = 2; i < num_pages; i++) {
        uint64_t pa =
            nvme_translate_page_phys(vaddr + ((uint64_t)i * NVME_PAGE_SIZE));
        if (!pa) {
            printk("NVMe: PRP page %u not mapped, vaddr=%#018lx\n", i,
                   vaddr + ((uint64_t)i * NVME_PAGE_SIZE));
            return -1;
        }
        req->prp_list->prp[i - 1] = pa;
    }

    nvme_platform_ops->wmb();
    return 0;
}

// 异步读取
static inline nvme_queue_t *nvme_pick_io_queue(nvme_controller_t *ctrl) {
    return &ctrl->io_queues[current_cpu_id % ctrl->num_io_queues];
}

static int nvme_submit_io_async_on_queue(
    nvme_controller_t *ctrl, nvme_queue_t *queue, uint8_t opcode, uint32_t nsid,
    uint64_t lba, uint32_t block_count, void *buffer, uint64_t buffer_phys,
    nvme_io_callback_t callback, void *ctx) {
    if (!ctrl || !ctrl->initialized || !queue || !buffer || block_count == 0)
        return -1;
    if (nsid == 0 || nsid > ctrl->num_namespaces)
        return -1;

    nvme_namespace_t *ns = &ctrl->namespaces[nsid - 1];
    if (!ns->valid || ns->block_size == 0 || ns->block_count == 0)
        return -1;

    if (block_count - 1 > UINT16_MAX)
        return -1;
    if (lba >= ns->block_count || block_count > (ns->block_count - lba))
        return -1;

    uint32_t block_size = ns->block_size;
    uint64_t transfer_size_u64 = (uint64_t)block_count * block_size;
    if (transfer_size_u64 == 0 || transfer_size_u64 > UINT32_MAX)
        return -1;

    uint32_t transfer_size = (uint32_t)transfer_size_u64;
    if (ctrl->max_transfer_size > 0 && transfer_size > ctrl->max_transfer_size)
        return -1;

    uint16_t cid = nvme_alloc_cid(ctrl, callback, ctx);
    if (cid == 0xFFFF)
        return -1;

    nvme_request_t *req = &ctrl->request_slots[cid];

    nvme_sqe_t cmd = {0};
    cmd.cdw0 = opcode | (cid << 16);
    cmd.nsid = nsid;
    cmd.cdw10 = (uint32_t)(lba & 0xFFFFFFFF);
    cmd.cdw11 = (uint32_t)(lba >> 32);
    cmd.cdw12 = (block_count - 1) & 0xFFFF;

    if (nvme_setup_prp(ctrl, req, &cmd, buffer, buffer_phys, transfer_size) !=
        0) {
        nvme_release_cid(ctrl, cid);
        printk("NVMe: setting up PRP failed\n");
        return -1;
    }

    if (opcode == NVME_CMD_WRITE)
        nvme_platform_ops->wmb();

    if (nvme_submit_cmd(queue, &cmd) != 0) {
        printk("NVMe: submit command failed\n");
        nvme_release_cid(ctrl, cid);
        return -1;
    }

    return 0;
}

int nvme_read_async(nvme_controller_t *ctrl, uint32_t nsid, uint64_t lba,
                    uint32_t block_count, void *buffer, uint64_t buffer_phys,
                    nvme_io_callback_t callback, void *ctx) {
    nvme_queue_t *queue = nvme_pick_io_queue(ctrl);
    return nvme_submit_io_async_on_queue(ctrl, queue, NVME_CMD_READ, nsid, lba,
                                         block_count, buffer, buffer_phys,
                                         callback, ctx);
}

int nvme_write_async(nvme_controller_t *ctrl, uint32_t nsid, uint64_t lba,
                     uint32_t block_count, const void *buffer,
                     uint64_t buffer_phys, nvme_io_callback_t callback,
                     void *ctx) {
    nvme_queue_t *queue = nvme_pick_io_queue(ctrl);
    return nvme_submit_io_async_on_queue(ctrl, queue, NVME_CMD_WRITE, nsid, lba,
                                         block_count, (void *)buffer,
                                         buffer_phys, callback, ctx);
}

typedef struct nvme_callback_ctx {
    bool completed;
    bool success;
    uint32_t result;
} nvme_callback_ctx_t;

void nvme_io_callback(void *ctx, bool success, uint32_t result) {
    nvme_callback_ctx_t *cb_ctx = ctx;
    cb_ctx->success = success;
    cb_ctx->result = result;
    __atomic_store_n(&cb_ctx->completed, true, __ATOMIC_RELEASE);
}

typedef struct nvme_ns {
    nvme_controller_t *ctrl;
    nvme_namespace_t *ns;
} nvme_ns_t;

static uint64_t nvme_wait_io_done(nvme_controller_t *ctrl, nvme_queue_t *queue,
                                  nvme_callback_ctx_t *cb_ctx, uint64_t ok_ret,
                                  const char *op_name, uint32_t timeout_ms) {
    uint64_t start = nvme_platform_ops->get_time_ms();

    while (!__atomic_load_n(&cb_ctx->completed, __ATOMIC_ACQUIRE)) {
        if (!nvme_process_queue_completions(ctrl, queue))
            arch_pause();

        if ((timeout_ms != (uint32_t)-1) &&
            nvme_platform_ops->get_time_ms() - start > timeout_ms) {
            printk("NVMe: %s command timed out after %u ms\n", op_name,
                   timeout_ms);
            nvme_dump_status(ctrl);
            return 0;
        }
    }

    bool success = cb_ctx->success;
    uint32_t result = cb_ctx->result;

    if (success)
        return ok_ret;

    printk("NVMe: %s command failed, result=0x%08x\n", op_name, result);
    return 0;
}

static int nvme_flush(void *data) {
    nvme_ns_t *ns = data;
    nvme_queue_t *queue = nvme_pick_io_queue(ns->ctrl);
    nvme_callback_ctx_t cb_ctx = {0};
    nvme_sqe_t cmd = {0};
    uint16_t cid = nvme_alloc_cid(ns->ctrl, nvme_io_callback, &cb_ctx);
    if (cid == UINT16_MAX)
        return -1;

    cmd.cdw0 = NVME_CMD_FLUSH | ((uint32_t)cid << 16);
    cmd.nsid = ns->ns->nsid;
    if (nvme_submit_cmd(queue, &cmd) != 0) {
        nvme_release_cid(ns->ctrl, cid);
        return -1;
    }
    return nvme_wait_io_done(ns->ctrl, queue, &cb_ctx, 1, "flush", 5000) == 1
               ? 0
               : -1;
}

uint64_t nvme_read(void *data, uint64_t lba, void *buffer, uint64_t size) {
    nvme_ns_t *ns = data;
    nvme_queue_t *queue = nvme_pick_io_queue(ns->ctrl);

    nvme_callback_ctx_t cb_ctx = {0};
    int r = nvme_submit_io_async_on_queue(ns->ctrl, queue, NVME_CMD_READ,
                                          ns->ns->nsid, lba, size, buffer, 0,
                                          nvme_io_callback, &cb_ctx);
    if (r < 0) {
        printk("NVMe: submit read command failed\n");
        return 0;
    }
    return nvme_wait_io_done(ns->ctrl, queue, &cb_ctx, size, "read",
                             (uint32_t)-1);
}

uint64_t nvme_write(void *data, uint64_t lba, void *buffer, uint64_t size) {
    nvme_ns_t *ns = data;
    nvme_queue_t *queue = nvme_pick_io_queue(ns->ctrl);

    nvme_callback_ctx_t cb_ctx = {0};
    int r = nvme_submit_io_async_on_queue(
        ns->ctrl, queue, NVME_CMD_WRITE, ns->ns->nsid, lba, size,
        (void *)buffer, 0, nvme_io_callback, &cb_ctx);
    if (r < 0) {
        printk("NVMe: submit write command failed\n");
        return 0;
    }
    return nvme_wait_io_done(ns->ctrl, queue, &cb_ctx, size, "write",
                             (uint32_t)-1);
}

// Main probe function
int nvme_probe(pci_device_t *device) {
    if (!nvme_platform_ops) {
        return -1;
    }
    if (nvme_check_pci_config(device) != 0)
        return -1;

    nvme_platform_ops->log("NVMe: Probing device %04x:%04x\n",
                           device->vendor_id, device->device_id);

    // Allocate controller structure
    nvme_controller_t *ctrl = (nvme_controller_t *)nvme_platform_ops->dma_alloc(
        sizeof(nvme_controller_t), NULL);
    if (!ctrl) {
        return -1;
    }
    memset(ctrl, 0, sizeof(nvme_controller_t));

    spin_init(&ctrl->cid_alloc_lock);

    ctrl->pci_dev = device;
    ctrl->bar0 = (volatile uint8_t *)phys_to_virt(device->bars[0].address);
    map_page_range(get_current_page_dir(false), (uint64_t)ctrl->bar0,
                   device->bars[0].address, device->bars[0].size,
                   PT_FLAG_R | PT_FLAG_W | PT_FLAG_UNCACHEABLE |
                       PT_FLAG_DEVICE);

    if (!ctrl->bar0) {
        nvme_platform_ops->log("NVMe: BAR0 not mapped\n");
        goto error;
    }

    // Read capabilities
    uint64_t cap = NVME_READ64(ctrl, NVME_REG_CAP);
    ctrl->doorbell_stride = 4 << ((cap >> 32) & 0xF);
    uint32_t mpsmin = (cap >> 48) & 0xF;
    uint32_t mpsmax = (cap >> 52) & 0xF;

    nvme_platform_ops->log(
        "NVMe: CAP=%016llx, doorbell_stride=%d, mpsmin=%u, mpsmax=%u\n", cap,
        ctrl->doorbell_stride, mpsmin, mpsmax);

    if (mpsmin != 0) {
        nvme_platform_ops->log(
            "NVMe: Unsupported controller minimum page size %u KiB\n",
            1U << mpsmin);
        goto error;
    }

    // Disable controller
    if (nvme_disable_controller(ctrl) != 0) {
        nvme_platform_ops->log("NVMe: Failed to disable controller\n");
        goto error;
    }

    // Initialize admin queue
    if (nvme_init_queue(ctrl, &ctrl->admin_queue, 0, NVME_ADMIN_QUEUE_SIZE) !=
        0) {
        nvme_platform_ops->log("NVMe: Failed to initialize admin queue\n");
        goto error;
    }

    // Configure admin queues
    NVME_WRITE32(ctrl, NVME_REG_AQA,
                 ((NVME_ADMIN_QUEUE_SIZE - 1) << 16) |
                     (NVME_ADMIN_QUEUE_SIZE - 1));
    NVME_WRITE64(ctrl, NVME_REG_ASQ, ctrl->admin_queue.sq_phys);
    NVME_WRITE64(ctrl, NVME_REG_ACQ, ctrl->admin_queue.cq_phys);

    // Enable controller
    if (nvme_enable_controller(ctrl) != 0) {
        nvme_platform_ops->log("NVMe: Failed to enable controller\n");
        goto error;
    }

    nvme_platform_ops->log("NVMe: Controller ready\n");

    // Identify controller
    nvme_identify_ctrl_t id_ctrl;
    if (nvme_identify_controller(ctrl, &id_ctrl) != 0) {
        nvme_platform_ops->log("NVMe: Failed to identify controller\n");
        goto error;
    }

    ctrl->num_namespaces =
        MIN((uint32_t)(sizeof(ctrl->namespaces) / sizeof(ctrl->namespaces[0])),
            id_ctrl.nn);

    if (id_ctrl.mdts) {
        ctrl->max_transfer_size = ((1ULL << id_ctrl.mdts) * PAGE_SIZE);
    } else {
        ctrl->max_transfer_size = -1;
    }
    nvme_platform_ops->log("NVMe: Model=%.40s, Namespaces=%d\n", id_ctrl.mn,
                           ctrl->num_namespaces);

    ctrl->num_io_queues = MIN(MAX_IO_CPU_NUM, cpu_count);
    ctrl->page_size = NVME_PAGE_SIZE;

    // Create I/O queue pair
    for (uint32_t qid = 0; qid < ctrl->num_io_queues; qid++) {
        if (nvme_init_queue(ctrl, &ctrl->io_queues[qid], 1 + qid,
                            NVME_IO_QUEUE_SIZE) != 0) {
            nvme_platform_ops->log("NVMe: Failed to create I/O queue\n");
            goto error;
        }

        if (nvme_create_io_cq(ctrl, &ctrl->io_queues[qid]) != 0) {
            nvme_platform_ops->log("NVMe: Failed to create I/O CQ\n");
            goto error;
        }

        if (nvme_create_io_sq(ctrl, &ctrl->io_queues[qid]) != 0) {
            nvme_platform_ops->log("NVMe: Failed to create I/O SQ\n");
            goto error;
        }
    }

    nvme_platform_ops->log("NVMe: I/O queues created\n");

    ctrl->initialized = true;

    // Identify namespaces
    for (uint32_t i = 1; i <= ctrl->num_namespaces; i++) {
        nvme_identify_ns_t id_ns;
        if (nvme_identify_namespace(ctrl, i, &id_ns) == 0 && id_ns.nsze > 0) {
            ctrl->namespaces[i - 1].nsid = i;
            ctrl->namespaces[i - 1].block_count = id_ns.nsze;

            uint8_t lba_format = id_ns.flbas & 0xF;
            if (lba_format < 16) {
                ctrl->namespaces[i - 1].block_size =
                    1 << id_ns.lbaf[lba_format].lbads;
            }

            if (ctrl->namespaces[i - 1].block_size == 0) {
                nvme_platform_ops->log(
                    "NVMe: NS%d has invalid block size, skipping\n", i);
                continue;
            }

            ctrl->namespaces[i - 1].valid = true;

            nvme_platform_ops->log("NVMe: NS%d: %lld blocks x %d bytes\n", i,
                                   id_ns.nsze,
                                   ctrl->namespaces[i - 1].block_size);

            nvme_ns_t *ns = malloc(sizeof(nvme_ns_t));
            if (!ns) {
                nvme_platform_ops->log("NVMe: Failed to allocate namespace\n");
                continue;
            }
            ns->ctrl = ctrl;
            ns->ns = &ctrl->namespaces[i - 1];

            char name[16];
            snprintf(name, sizeof(name), "nvme%d", nvme_drive_id++);

            regist_blkdev(name, ns, ns->ns->block_size,
                          ns->ns->block_count * ns->ns->block_size,
                          MIN(ns->ctrl->max_transfer_size,
                              NVME_MAX_PRP_LIST_ENTRIES * PAGE_SIZE),
                          nvme_read, nvme_write, nvme_flush);
        }
    }

    device->desc = ctrl;

    nvme_platform_ops->log("NVMe: Initialization complete\n");
    return 0;

error:
    // Cleanup on error
    if (ctrl) {
        nvme_dump_status(ctrl);
        nvme_platform_ops->dma_free(ctrl, sizeof(nvme_controller_t));
    }
    return -1;
}

void nvme_remove(pci_device_t *dev) {}

void nvme_shutdown(pci_device_t *dev) {}

pci_driver_t nvme_driver = {
    .name = "nvme_driver",
    .class_id = 0x00010802,
    .match = NULL,
    .probe = nvme_probe,
    .remove = nvme_remove,
    .shutdown = nvme_shutdown,
    .flags = 0,
    .private_data = NULL,
};

int dlmain() {
    nvme_set_platform_ops(&naos_nvme_platform_ops);

    regist_pci_driver(&nvme_driver);

    return 0;
}
