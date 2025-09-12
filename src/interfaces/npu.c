/* Driver for NPU plugin */

#include <dlfcn.h>

#include "src/common/assoc_mgr.h"
#include "src/interfaces/npu.h"
#include "src/common/plugin.h"

/* Gres symbols provided by the plugin */
typedef struct slurm_ops {
	list_t* (*get_system_npu_list)   (node_config_load_t *node_conf);
	void    (*step_hardware_init)    (bitstr_t *usable_npus, char *tres_freq);
	void    (*step_hardware_fini)    (void);
	char*   (*test_cpu_conv)         (char *cpu_range);
	int     (*energy_read)           (uint32_t dv_ind, npu_status_t *npu);
	void    (*get_device_count)      (uint32_t *device_count);
	int     (*usage_read)            (pid_t pid, acct_gather_data_t *data);

} slurm_ops_t;  // TODO: check if huawei has same func

/*
 * These strings must be kept in the same order as the fields
 * declared for slurm_ops_t.
 */
static const char *syms[] = {
	"npu_p_get_system_npu_list",
	"npu_p_step_hardware_init",
	"npu_p_step_hardware_fini",
	"npu_p_test_cpu_conv",
	"npu_p_energy_read",
	"npu_p_get_device_count",
	"npu_p_usage_read",
};

/* Local variables */
static slurm_ops_t ops;
static plugin_context_t *g_context = NULL;
static pthread_mutex_t g_context_lock =	PTHREAD_MUTEX_INITIALIZER;
static void *ext_lib_handle = NULL;

// detect NPU hardware, dlopen .so files for 2 reason:
// 1. if no .so file exists, slurmd can still run on nodes without NPU
// 2. use libraries actually installed on compute nodes, instead of compiling env
static char* _get_npu_type(void) {
    uint32_t autodetect_flags = gres_get_autodetect_flags();

    if (autodetect_flags & GRES_AUTODETECT_NPU_DCMI) {
#ifdef HAVE_DCMI
        (void)dlerror();
        if (!(ext_lib_handle = dlopen("libdrvdsmi_host.so", RTLD_NOW | RTLD_GLOBAL))) {
            error("Failed to dlopen libdrvdsmi_host.so, error: %s", dlerror());
        } else {
            return "npu/dcmi";
        }
#else
        info("Slurm is configured to autodetect with DCMI, but libs are not found.");
#endif
    }
    return "npu/generic";
}

extern int npu_plugin_init(void) {
    int rc = SLURM_SUCCESS;
    char* plugin_type = "npu";
    char* type = NULL;

    slurm_mutex_lock(&g_context_lock);

    if (g_context) {
        goto done;
    }

    type = _get_npu_type();

    g_context = plugin_context_create(plugin_type, type, (void**)&ops, syms, sizeof(syms));

    if (!g_context) {
        error("cannot create %s context for %s", plugin_type, type);
        rc = SLURM_ERROR;
        goto done;
    }

done:
    slurm_mutex_unlock(&g_context_lock);

    return rc;
}

extern int npu_plugin_fini(void) {
    int rc;

    if (!g_context) {
        return SLURM_SUCCESS;
    }

    slurm_mutex_lock(&g_context_lock);

    if (ext_lib_handle) {
        dlclose(ext_lib_handle);
        ext_lib_handle = NULL;
    }

    rc = plugin_context_destroy(g_context);
    g_context = NULL;
    slurm_mutex_unlock(&g_context_lock);

    return rc;
}

extern void npu_get_tres_pos(int* npumem_pos, int* npuutil_pos) {
    static int loc_npumem_pos = -1; // use static to cache the value
    static int loc_npuutil_pos = -1;
    static bool inited = false;

    if (!inited) {
        slurmdb_tres_rec_t tres_rec;

        memset(&tres_rec, 0, sizeof(slurmdb_tres_rec_t));
        tres_rec.type = "gres";
        tres_rec.name = "npuutil";
        loc_npuutil_pos = assoc_mgr_find_tres_pos(&tres_rec, false);
        tres_rec.name = "npumem";
        loc_npumem_pos = assoc_mgr_find_tres_pos(&tres_rec, false);
        inited = true;
    }

    if (npumem_pos) {
        *npumem_pos = loc_npumem_pos;
    }
    if (npuutil_pos) {
        *npuutil_pos = loc_npuutil_pos;
    }
}

extern list_t *npu_g_get_system_npu_list(node_config_load_t *node_conf) {
    xassert(g_context);
    return (*(ops.get_system_npu_list))(node_conf);
}

extern void npu_g_step_hardware_init(bitstr_t *usable_npus, char *tres_freq) {
    xassert(g_context);
    (*(ops.step_hardware_init))(usable_npus, tres_freq);    // TODO: check if huawei has same func 
}

extern void npu_g_step_hardware_fini(void) {
    xassert(g_context);
    (*(ops.step_hardware_fini))();
}

extern char* npu_g_test_cpu_conv(char *cpu_range) {
    xassert(g_context);
    return (*(ops.test_cpu_conv))(cpu_range);
}

extern int npu_g_energy_read(uint32_t dv_ind, npu_status_t *npu) {
    xassert(g_context);
    return (*(ops.energy_read))(dv_ind, npu);
}

extern void npu_g_get_device_count(uint32_t *device_count) {
    xassert(g_context);
    (*(ops.get_device_count))(device_count);
}

extern int npu_g_usage_read(pid_t pid, acct_gather_data_t *data) {
    xassert(g_context);
    return (*(ops.usage_read))(pid, data);
}
