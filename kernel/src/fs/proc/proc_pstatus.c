#include <fs/proc/proc.h>
#include <task/task.h>
#include <libs/string_builder.h>

static char task_state_to_proc_state(task_t *task) {
    if (!task) {
        return 'R';
    }

    switch (task->state) {
    case TASK_RUNNING:
        return 'R';
    case TASK_BLOCKING:
        return 'S';
    case TASK_UNINTERRUPTABLE:
        return 'D';
    case TASK_DIED:
        return 'Z';
    default:
        return 'S';
    }
}

static void proc_status_sig_masks(task_t *task, unsigned long long *ignored,
                                  unsigned long long *caught) {
    if (ignored)
        *ignored = 0;
    if (caught)
        *caught = 0;
    if (!task || !task->signal || !task->signal->sighand)
        return;

    spin_lock(&task->signal->sighand->siglock);
    for (int sig = MINSIG; sig < MAXSIG; sig++) {
        sigaction_t *action = &task->signal->sighand->actions[sig];
        uint64_t bit = 1ULL << (uint64_t)(sig - 1);
        if (action->sa_handler == SIG_IGN) {
            if (ignored)
                *ignored |= bit;
        } else if (action->sa_handler != SIG_DFL) {
            if (caught)
                *caught |= bit;
        }
    }
    spin_unlock(&task->signal->sighand->siglock);
}

char *proc_gen_status_file(task_t *task, size_t *content_len) {
    uint64_t tgid = task_effective_tgid(task);
    size_t threads = task_thread_group_count(tgid);
    unsigned long long ignored = 0;
    unsigned long long caught = 0;
    procfs_task_mem_stats_t mem;
    proc_status_sig_masks(task, &ignored, &caught);
    procfs_task_mem_stats(task, &mem);
    string_builder_t *builder = create_string_builder(1024);
    if (!builder) {
        *content_len = 0;
        return NULL;
    }

    unsigned long long vm_size_kb = mem.size_pages * (PAGE_SIZE / 1024);
    unsigned long long vm_rss_kb = mem.resident_pages * (PAGE_SIZE / 1024);
    unsigned long long rss_file_kb = mem.file_pages * (PAGE_SIZE / 1024);
    unsigned long long rss_shmem_kb = mem.shared_pages * (PAGE_SIZE / 1024);
    unsigned long long rss_anon_pages = mem.resident_pages;
    if (rss_anon_pages > mem.file_pages)
        rss_anon_pages -= mem.file_pages;
    else
        rss_anon_pages = 0;
    if (rss_anon_pages > mem.shared_pages)
        rss_anon_pages -= mem.shared_pages;
    else
        rss_anon_pages = 0;

    string_builder_append(
        builder,
        "Name:\t%s\n"
        "State:\t%c\n"
        "Tgid:\t%llu\n"
        "Pid:\t%llu\n"
        "PPid:\t%llu\n"
        "Uid:\t%lld\t%lld\t%lld\t%lld\n"
        "Gid:\t%lld\t%lld\t%lld\t%lld\n"
        "Threads:\t%llu\n"
        "NStgid:\t%llu\n"
        "NSpid:\t%llu\n"
        "NSpgid:\t%lld\n"
        "NSsid:\t%lld\n"
        "VmPeak:\t%llu kB\n"
        "VmSize:\t%llu kB\n"
        "VmLck:\t0 kB\n"
        "VmPin:\t0 kB\n"
        "VmHWM:\t%llu kB\n"
        "VmRSS:\t%llu kB\n"
        "RssAnon:\t%llu kB\n"
        "RssFile:\t%llu kB\n"
        "RssShmem:\t%llu kB\n"
        "VmData:\t%llu kB\n"
        "VmStk:\t%llu kB\n"
        "VmExe:\t%llu kB\n"
        "VmLib:\t0 kB\n"
        "VmPTE:\t%llu kB\n"
        "VmSwap:\t0 kB\n"
        "HugetlbPages:\t0 kB\n"
        "SigPnd:\t%016llx\n"
        "ShdPnd:\t%016llx\n"
        "SigBlk:\t%016llx\n"
        "SigIgn:\t%016llx\n"
        "SigCgt:\t%016llx\n"
        "CapInh:\t%016llx\n"
        "CapPrm:\t%016llx\n"
        "CapEff:\t%016llx\n"
        "CapBnd:\t%016llx\n"
        "CapAmb:\t%016llx\n",
        task ? task->name : "unknown", task_state_to_proc_state(task),
        (unsigned long long)tgid, (unsigned long long)(task ? task->pid : 0),
        (unsigned long long)task_parent_pid(task), task ? task->uid : 0,
        task ? task->euid : 0, task ? task->suid : 0, task ? task->fsuid : 0,
        task ? task->gid : 0, task ? task->egid : 0, task ? task->sgid : 0,
        task ? task->fsgid : 0, (unsigned long long)threads,
        (unsigned long long)tgid, (unsigned long long)(task ? task->pid : 0),
        task ? task->pgid : 0, task ? task->sid : 0, vm_size_kb, vm_size_kb,
        vm_rss_kb, vm_rss_kb,
        (unsigned long long)(rss_anon_pages * (PAGE_SIZE / 1024)), rss_file_kb,
        rss_shmem_kb, (unsigned long long)(mem.data_pages * (PAGE_SIZE / 1024)),
        (unsigned long long)(mem.stack_pages * (PAGE_SIZE / 1024)),
        (unsigned long long)(mem.text_pages * (PAGE_SIZE / 1024)),
        (unsigned long long)(mem.pte_pages * (PAGE_SIZE / 1024)),
        (unsigned long long)(task && task->signal
                                 ? sigset_kernel_to_user(task->signal->signal)
                                 : 0),
        0ULL,
        (unsigned long long)(task && task->signal
                                 ? sigset_kernel_to_user(task->signal->blocked)
                                 : 0),
        ignored, caught, (unsigned long long)(task ? task->cap_inheritable : 0),
        (unsigned long long)(task ? task->cap_permitted : 0),
        (unsigned long long)(task ? task->cap_effective : 0),
        (unsigned long long)(task ? task->cap_bounding : 0),
        (unsigned long long)(task ? task->cap_ambient : 0));

    int group_count = task_groups_copy(task, NULL, 0);
    uint32_t *groups = NULL;
    if (group_count > 0) {
        groups = malloc((size_t)group_count * sizeof(*groups));
        if (groups) {
            int copied = task_groups_copy(task, groups, (size_t)group_count);
            if (copied >= 0)
                group_count = copied;
            else
                group_count = 0;
        } else {
            group_count = 0;
        }
    }

    string_builder_append(builder, "Groups:\t");
    for (int i = 0; i < group_count; i++)
        string_builder_append(builder, "%u ", groups[i]);
    string_builder_append(builder, "\n");
    free(groups);

    char *data = builder->data;
    *content_len = builder->size;
    free(builder);
    return data;
}

size_t proc_pstatus_stat(proc_handle_t *handle) {
    task_t *task = procfs_handle_task_or_current(handle);
    size_t content_len = 0;
    char *content = proc_gen_status_file(task, &content_len);
    if (!content)
        return 0;
    free(content);
    return content_len;
}

size_t proc_pstatus_read(proc_handle_t *handle, void *addr, size_t offset,
                         size_t size) {
    task_t *task = procfs_handle_task_or_current(handle);
    size_t content_len = 0;
    char *content = proc_gen_status_file(task, &content_len);
    if (!content)
        return 0;
    if (offset >= content_len) {
        free(content);
        return 0;
    }

    size_t to_copy = MIN(size, content_len - offset);
    memcpy(addr, content + offset, to_copy);
    free(content);
    return to_copy;
}
