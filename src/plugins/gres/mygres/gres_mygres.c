#define _GNU_SOURCE

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "slurm/slurm.h"
#include "slurm/slurm_errno.h"

#include "src/common/slurm_xlator.h"
#include "src/common/assoc_mgr.h"
#include "src/common/bitstring.h"
#include "src/common/env.h"
#include "src/interfaces/npu.h"
#include "src/interfaces/gres.h"
#include "src/common/list.h"
#include "src/common/strnatcmp.h"
#include "src/common/xstring.h"

#include "../common/gres_common.h"

/*
 * These variables are required by the generic plugin interface.  If they
 * are not found in the plugin, the plugin loader will ignore it.
 *
 * plugin_name - A string giving a human-readable description of the
 * plugin.  There is no maximum length, but the symbol must refer to
 * a valid string.
 *
 * plugin_type - A string suggesting the type of the plugin or its
 * applicability to a particular form of data or method of data handling.
 * If the low-level plugin API is used, the contents of this string are
 * unimportant and may be anything.  Slurm uses the higher-level plugin
 * interface which requires this string to be of the form
 *
 *	<application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "auth" for Slurm authentication) and <method> is a
 * description of how this plugin satisfies that application.  Slurm will
 * only load authentication plugins if the plugin_type string has a prefix
 * of "auth/".
 *
 * plugin_version - an unsigned 32-bit integer containing the Slurm version
 * (major.minor.micro combined into a single number).
 */

const char plugin_name[] = "My Gres Plugin";
const char plugin_type[] = "gres/mygres";
const uint32_t plugin_version = SLURM_VERSION_NUMBER;
static list_t* gres_devices = NULL;
static uint32_t node_flags = 0;

extern int init(void) {
    debug("loaded");

    return SLURM_SUCCESS;
}

extern void fini(void) {
    debug("unloading");
    npu_plugin_fini();  // TODO: check
    FREE_NULL_LIST(gres_devices);
}

// "p" stands for "plugin"
// parse gres.conf/slurm.conf, get gres config -> config1
// query system devices -> config2
// merge config1 and config2 -> final config, store in gres_devices
extern int gres_p_node_config_load(list_t* gres_conf_list, node_config_load_t* node_config) {
    int rc = SLURM_SUCCESS;
    list_t* gres_list_system = NULL;
    log_level_t log_lvl = LOG_LEVEL_DEBUG;

    // assume scontrol reconfigure is called
    if (gres_devices) {
        debug("%s: Resetting gres_devices", plugin_name);
        FREE_NULL_LIST(gres_devices);
    }

    gres_list_system = NULL;
    // query real system devices if running in slurmd, slurmctld will not do this
    if (node_config->in_slurmd) {
        gres_list_system = npu_g_get_system_npu_list(node_config);  // TODO: check
    }
    if (gres_list_system) {
        if (list_is_empty(gres_list_system)) {
            log_var(log_lvl, "There were 0 NPUs detected on the system");
        }
        log_var(log_lvl, "%s: Merging configured GRES with system NPUs", plugin_name);
        _merge_system_gres_conf(gres_conf_list, gres_list_system);
        FREE_NULL_LIST(gres_list_system);

        if (!gres_conf_list || list_is_empty(gres_conf_list)) {
            log_var(log_lvl, "%s: Final merged GRES list is empty", plugin_name);
        } else {
            log_var(log_lvl, "%s: Final merged GRES list:", plugin_name);
            print_gres_list(gres_conf_list, log_lvl);
        }
    }

    rc = gres_node_config_load(gres_conf_list, node_config, &gres_devices);

    // check what envs the gres_slurmd_conf records want to set
    // if one record wants an env, assume every record on this node wants that env
    // node_flags stores env info, check it when setting envs later in stepd
    node_flags = 0;
    (void)list_for_each(gres_conf_list, gres_common_set_env_types_on_node_flags, &node_flags);

    if (rc != SLURM_SUCCESS) {
        fatal("%s failed to load configuration", plugin_name);
    }
    return rc;
}

// set environment variables for job (i.e. all tasks) based on job's GRES
extern void gres_p_job_set_env(char ***job_env_ptr, bitstr_t* gres_bit_alloc, uint64_t gres_cnt, bitstr_t* usable_gres, gres_internal_flags_t flags) {
    common_gres_env_t gres_env = {
        .bit_alloc = gres_bit_alloc,
        .env_ptr = job_env_ptr,
        .flags = flags,
        .gres_cnt = gres_cnt,
        .gres_conf_flags = node_flags,  // node_flags has env info
        .gres_devices = gres_devices,
        .is_task = false,
        .usable_gres = usable_gres,
    };

    gres_common_gpu_set_env(&gres_env); // TODO: update gres/common/gres_common.c, gres_common_gpu_set_env()
}

// set environment variables for step (i.e. all tasks) base on job step's GRES
extern void gres_p_step_set_env(char ***step_env_ptr, bitstr_t* gres_bit_alloc, uint64_t gres_cnt, bitstr_t* usable_gres, gres_internal_flags_t flags) {
    common_gres_env_t gres_env = {
        .bit_alloc = gres_bit_alloc,
        .env_ptr = step_env_ptr,
        .flags = flags,
        .gres_cnt = gres_cnt,
        .gres_conf_flags = node_flags,  // node_flags has env info
        .gres_devices = gres_devices,
        .is_task = false,
        .usable_gres = usable_gres,
    };

    gres_common_gpu_set_env(&gres_env); // TODO: update gres/common/gres_common.c, gres_common_gpu_set_env()
}

// set environment variables for one task base on job step's GRES
extern void gres_p_task_set_env(char ***task_env_ptr, bitstr_t* gres_bit_alloc, uint64_t gres_cnt, bitstr_t* usable_gres, gres_internal_flags_t flags) {
    common_gres_env_t gres_env = {
        .bit_alloc = gres_bit_alloc,
        .env_ptr = task_env_ptr,
        .flags = flags,
        .gres_cnt = gres_cnt,
        .gres_conf_flags = node_flags,  // node_flags has env info
        .gres_devices = gres_devices,
        .is_task = true,
        .usable_gres = usable_gres,
    };

    gres_common_gpu_set_env(&gres_env); // TODO: update gres/common/gres_common.c, gres_common_gpu_set_env()
}

// slurmstepd is forked from slurmd, we need to send/recv GRES info between slurmd/slurmstepd
// when job starts, send GRES list from slurmd to slurmstepd via a buffer
extern void gres_p_send_stepd(buf_t* buffer) {
    gres_send_stepd(buffer, gres_devices);
    pack32(node_flags, buffer);
}

// for slurmctepd, recv GRES list from slurmd via a buffer
extern void gres_p_recv_stepd(buf_t* buffer) {
    gres_recv_stepd(buffer, &gres_devices);
    safe_unpack32(&node_flags, buffer);
    return;

unpack_error:
    error("%s: gres_p_recv_stepd failed to unpack buffer", __func__);
}

// return device list, element's type is "gres_device_t"
extern list_t* gres_p_get_device_list(void) {
    return gres_devices;
}

// build record, set prolog/epilog envs later
// 
extern gres_prep_t* gres_p_prep_build_env(gres_job_state_t* gres_js) {
    int i;
    gres_prep_t* gres_prep;

    gres_prep = xmalloc(sizeof(gres_prep_t));
    gres_prep->node_cnt = 0;
    gres_prep->gres_bit_alloc = xcalloc(gres_prep->node_cnt, sizeof(bitstr_t*));
	for (i = 0; i < gres_prep->node_cnt; i++) {
		if (gres_js->gres_bit_alloc && gres_js->gres_bit_alloc[i]) {
			gres_prep->gres_bit_alloc[i] = bit_copy(gres_js->gres_bit_alloc[i]);
		}
	}

	return gres_prep;
}

// set environment variables for a job's prolog or epilog based GRES allocated to the job
extern void gres_p_prep_set_env(char*** prep_env_ptr, gres_prep_t* gres_prep, int node_inx) {
    (void)gres_common_prep_set_env(prep_env_ptr, gres_prep, node_inx, node_flags, gres_devices);
}

/************************************* TAINTED ************************************* */

/*

// init func
extern int init(void)  {
    verbose("mygres plugin initialized");
    return SLURM_SUCCESS;
}

// cleanup func
extern void fini(void) {
    verbose("mygres plugin finalized");
    return SLURM_SUCCESS;
}

// get GRES info
extern List get_gres_info(void) {
    List gres_list = list_create(NULL);
    return gres_list;
}

// configure GRES
extern int gres_config(List gres_list) {
    // ...
    return SLURM_SUCCESS;
}

// allocate GRES
extern int gres_alloc(List gres_list) {
    // ...
    return SLURM_SUCCESS;
}

// release GRES
extern int gres_release(List gres_list) {
    // ...
    return SLURM_SUCCESS;
}

// SLURM plugin symbol table


const struct plugin_ops gres_ops = {
    .init = init,
    .fini = fini,
    .get_gres_info = get_gres_info,
    .gres_config = gres_config,
    .gres_alloc = gres_alloc,
    .gres_release = gres_release
};

*/