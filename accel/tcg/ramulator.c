#include "qemu/osdep.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "exec/ramulator_log.h"
#include "hw/core/cpu.h"
#include "exec/tb-flush.h"
#include "hw/boards.h"
#include "qemu/notify.h"
#include "system/system.h"
#include "tcg/tcg-op-common.h"
#include "monitor/monitor.h"

static void *global_shm_base = NULL;
static int shm_fd = -1;
static size_t global_shm_size = 0;
static Notifier ramulator_exit_notifier;
bool ramulator_trace_active = false;

void ramulator_trigger_global_flush(void)
{
    CPUState *cpu = first_cpu;
    if (cpu) {
        tb_flush(cpu);
    }
}

static void ramulator_shm_cleanup(Notifier *n, void *data)
{
    if (global_shm_base && global_shm_base != MAP_FAILED) {
        munmap(global_shm_base, global_shm_size);
        global_shm_base = NULL;
    }

    if (shm_fd != -1) {
        close(shm_fd);
        shm_fd = -1;
    }

    shm_unlink(RAMULATOR_SHM_NAME);
    
    printf("Ramulator SHM: Memoire partagee nettoyee\n");
}

void ramulator_init_shm_for_cpu(int cpu_index, void *cpu_state_ptr)
{
    CPUState *cpu = (CPUState *)cpu_state_ptr;
    MachineState *ms = MACHINE(qdev_get_machine());
    unsigned int max_cpus = ms->smp.max_cpus;
    size_t buf_size_per_cpu = LOG_BUFFER_SIZE_PER_CPU;

    char *env_size = getenv("RAMULATOR_BUF_SIZE");
    if (env_size) {
        long mbytes = atol(env_size);
        if (mbytes > 0) {
            buf_size_per_cpu = (size_t)mbytes * 1024 * 1024;
        }
    }

    size_t global_shm_size = (size_t)max_cpus * buf_size_per_cpu;
    if (global_shm_base == NULL) {
        shm_fd = shm_open(RAMULATOR_SHM_NAME, O_CREAT | O_RDWR, 0666);
        if (shm_fd == -1) {
            perror("Ramulator SHM: shm_open failed");
            return;
        }

        if (ftruncate(shm_fd, global_shm_size) == -1) {
            perror("Ramulator SHM: ftruncate failed");
            return;
        }

        global_shm_base = mmap(NULL, global_shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
        if (global_shm_base == MAP_FAILED) {
            perror("Ramulator SHM: mmap failed");
            global_shm_base = NULL;
            return;
        }

        ramulator_exit_notifier.notify = ramulator_shm_cleanup;
        qemu_add_exit_notifier(&ramulator_exit_notifier);
        
        printf("Ramulator SHM: Fichier initialise pour %u CPUs (Taille totale : %zu octets)\n", max_cpus, global_shm_size);
    }

    uint8_t *cpu_shm_start = (uint8_t *)global_shm_base + (cpu_index * LOG_BUFFER_SIZE_PER_CPU);
    cpu->ramulator_log_ptr   = (uint64_t *)cpu_shm_start;
    cpu->ramulator_log_end   = (uint64_t *)(cpu_shm_start + LOG_BUFFER_SIZE_PER_CPU);
    cpu->ramulator_insn_count = 0;

    printf("Ramulator SHM: CPU %d connecte au slot SHM\n", cpu_index);
}

void gen_ramulator_count_instruction(void)
{
    if (!ramulator_trace_active) {
        return;
    }
    TCGv_i64 insn_count = tcg_temp_new_i64();
    tcg_gen_ld_i64(insn_count, tcg_env, offsetof(CPUState, ramulator_insn_count) - sizeof(CPUState));
    tcg_gen_addi_i64(insn_count, insn_count, 1);
    tcg_gen_st_i64(insn_count, tcg_env, offsetof(CPUState, ramulator_insn_count) - sizeof(CPUState));
}

void gen_ramulator_ptr_increment(int is_store, int size, TCGv_i64 vaddr)
{
    if (!ramulator_trace_active) {
        return;
    }
    TCGv_ptr log_ptr = tcg_temp_new_ptr();
    TCGv_ptr log_end = tcg_temp_new_ptr();
    TCGv_i64 host_clock = tcg_temp_new_i64();
    TCGv_i64 local_insn = tcg_temp_new_i64();
    TCGv_i32 cpu_index = tcg_temp_new_i32();
    TCGLabel *label_buffer_full = gen_new_label();

    tcg_gen_ld_ptr(log_ptr, tcg_env, offsetof(CPUState, ramulator_log_ptr) - sizeof(CPUState));
    tcg_gen_ld_ptr(log_end, tcg_env, offsetof(CPUState, ramulator_log_end) - sizeof(CPUState));
    tcg_gen_brcond_i64(TCG_COND_GEU, (TCGv_i64)log_ptr, (TCGv_i64)log_end, label_buffer_full);

    gen_helper_ramulator_write_phys_test(tcg_env, vaddr);
    
    gen_helper_ramulator_get_clock(host_clock);
    tcg_gen_st_i64(host_clock, log_ptr, offsetof(LogRecord, logical_clock));

    tcg_gen_ld_i64(local_insn, tcg_env, offsetof(CPUState, ramulator_insn_count) - sizeof(CPUState));
    tcg_gen_st_i64(local_insn, log_ptr, offsetof(LogRecord, insn_count));

    tcg_gen_ld_i32(cpu_index, tcg_env, offsetof(CPUState, cpu_index) - sizeof(CPUState));
    tcg_gen_st8_i32(cpu_index, log_ptr, offsetof(LogRecord, cpu));

    TCGv_i32 store_val = tcg_constant_i32(is_store);
    tcg_gen_st8_i32(store_val, log_ptr, offsetof(LogRecord, store));

    TCGv_i32 size_val = tcg_constant_i32(size);
    tcg_gen_st8_i32(size_val, log_ptr, offsetof(LogRecord, access_size));

    tcg_gen_addi_ptr(log_ptr, log_ptr, sizeof(LogRecord));
    tcg_gen_st_ptr(log_ptr, tcg_env, offsetof(CPUState, ramulator_log_ptr) - sizeof(CPUState));


    gen_set_label(label_buffer_full);
}

void ramulator_reset_counters(void)
{
    CPUState *cpu;
    CPU_FOREACH(cpu) {
        cpu->ramulator_insn_count = 0;
        uint8_t *base_shm_cpu = (uint8_t *)cpu->ramulator_log_end - LOG_BUFFER_SIZE_PER_CPU;
        cpu->ramulator_log_ptr = (uint64_t *)base_shm_cpu;
    }
}