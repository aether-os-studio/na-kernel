#include <arch/arch.h>
#include <task/task.h>
#include <drivers/bus/pci.h>
#include <drivers/fdt/fdt.h>
#include <fs/dev.h>
#include <fs/sys.h>
#include <fs/vfs/notify.h>
#include <block/partition.h>
#include <net/real_socket.h>
#include <drivers/fb.h>
#include <drivers/drm/drm.h>
#include <drivers/virtio/virtio.h>
#include <irq/softirq.h>

extern void acpi_init_after_pci();

bool system_initialized = false;

extern void pidfd_init();
extern void mountfd_init();

extern void sysfs_init();
extern void fsfdfs_init();
extern void cgroupfs_init();
extern void socketfs_init();
extern void pipefs_init();
extern void configfs_init();

extern void epoll_init();
extern void eventfd_init();
extern void signalfd_init();
extern void timerfd_init();
extern void memfd_init();

void fs_syscall_init() {}

void init_thread(uint64_t arg) {
    printk("NAOS init thread is running...\n");

    arch_init_after_thread();

    pci_controller_init();
    virtio_bus_init();

#if !defined(__x86_64__)
    fdt_init();
#endif

    pidfd_init();
    mountfd_init();
    epoll_init();
    eventfd_init();
    signalfd_init();
    timerfd_init();
    memfd_init();
    configfs_init();
    socketfs_init();
    pipefs_init();
    fsfdfs_init();
    cgroupfs_init();

    pci_init();

    acpi_init_after_pci();

    arch_init_after_acpi_pci();

    arch_input_dev_init();

    fbdev_init();

    drm_init_after_pci_sysfs();

    real_socket_init();

    devtmpfs_init_umount();
    sysfs_init_umount();

    system_initialized = true;

    printk("System initialized, ready to go to userland.\n");

    current_task->nice = NORMAL_PRIORITY;

    /* 进入用户态前处理初始化阶段积压的定时器和延迟任务。 */
    schedule(SCHED_FLAG_YIELD);
    if (softirq_has_pending()) {
        softirq_handle_pending();
    }
    schedule(SCHED_FLAG_YIELD);

    const char *argvs[2];
    memset(argvs, 0, sizeof(argvs));
    argvs[0] = "/init";
    task_execve("/init", argvs, NULL);

    printk("run init failed\n");

    while (1) {
        arch_enable_interrupt();
        arch_wait_for_interrupt();
    }
}
