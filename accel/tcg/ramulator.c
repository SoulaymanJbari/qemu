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
    TCGv_i64 insn_count = tcg_temp_new_i64();
    tcg_gen_ld_i64(insn_count, tcg_env, offsetof(CPUState, ramulator_insn_count) - sizeof(CPUState));
    tcg_gen_addi_i64(insn_count, insn_count, 1);
    tcg_gen_st_i64(insn_count, tcg_env, offsetof(CPUState, ramulator_insn_count) - sizeof(CPUState));
}