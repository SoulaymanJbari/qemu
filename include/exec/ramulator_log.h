#ifndef RAMULATOR_LOG_H
#define RAMULATOR_LOG_H

#include <stdint.h>

#define RAMULATOR_SHM_NAME "/ramulator_qemu_shm"
#define LOG_BUFFER_SIZE_PER_CPU (1024*1024)

extern bool ramulator_trace_active;

typedef struct TCGv_i64_d *TCGv_i64;

void ramulator_trigger_global_flush(void);
void ramulator_init_shm_for_cpu(int cpu_index, void *cpu_state_ptr);
void gen_ramulator_count_instruction(void);
void gen_ramulator_ptr_increment(int is_store, int size, TCGv_i64 vaddr, unsigned oi);
void ramulator_reset_counters(void);
void ramulator_write_metadata(void);

#endif