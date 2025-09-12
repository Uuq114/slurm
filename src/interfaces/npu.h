/* Driver for NPU plugin */

#ifndef _INTERFACES_NPU_H
#define _INTERFACES_NPU_H

#include "slurm/slurm.h"
#include "src/interfaces/gres.h"
#include "src/interfaces/acct_gather.h"

// array of struct to track the status of a NPU
typedef struct {
	uint32_t last_update_watt;
	time_t last_update_time;
	time_t previous_update_time;
	acct_gather_energy_t energy;
} npu_status_t;

extern int npu_plugin_init(void);
extern int npu_plugin_fini(void);
extern void npu_get_tres_pos(int* npumem_pos, int* npuutil_pos);
extern list_t* npu_g_get_system_npu_list(node_config_load_t* node_conf);
extern void npu_g_step_hardware_init(bitstr_t* usable_npus, char* tres_freq);
extern void npu_g_step_hardware_fini(void);
extern char* npu_g_test_cpu_conv(char* cpu_range);
extern int npu_g_energy_read(uint32_t dv_ind, npu_status_t* npu);
extern void npu_g_get_device_count(uint32_t* device_count);
extern int npu_g_usage_read(pid_t pid, acct_gather_data_t* data);

#endif