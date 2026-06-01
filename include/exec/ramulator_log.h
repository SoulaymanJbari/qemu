#ifndef RAMULATOR_LOG_H
#define RAMULATOR_LOG_H

#include <stdint.h>

#define RAMULATOR_SHM_NAME "/ramulator_qemu_shm"
#define LOG_BUFFER_SIZE_PER_CPU (1024*1024)

typedef struct LogRecord {
    uint64_t logical_clock;
    uint64_t insn_count;
    char cpu;
    char store;
    char access_size;
    char padding[5];
    uint64_t address;
} LogRecord;

void ramulator_trigger_global_flush(void);
void ramulator_init_shm_for_cpu(int cpu_index, void *cpu_state_ptr);
void gen_ramulator_count_instruction(void);

#endif