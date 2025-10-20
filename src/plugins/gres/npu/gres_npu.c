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

#include "src/common/assoc_mgr.h"
#include "src/common/bitstring.h"
#include "src/common/env.h"
#include "src/common/list.h"
#include "src/common/slurm_xlator.h"
#include "src/common/strnatcmp.h"
#include "src/common/xstring.h"
#include "src/interfaces/gres.h"
#include "src/interfaces/npu.h"

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

const char plugin_name[] = "NPU Plugin";
const char plugin_type[] = "gres/npu";
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

extern void gres_p_step_hardware_init(bitstr_t* usable_gres, char* tres_freq) {
    npu_g_step_hardware_init(usable_gres, tres_freq);  // TODO: check
}

extern void gres_p_step_hardware_fini(void) {
    npu_g_step_hardware_fini();  // TODO: check
}

/* Sort strings in natural sort ascending order, except sort nulls last */
static int _sort_string_null_last(char* x, char* y) {
    /* Make NULLs greater than non-NULLs, so NULLs are sorted last */
    if (!x && y)
        return 1;
    else if (x && !y)
        return -1;
    else if (!x && !y)
        return 0;

    return strnatcmp(x, y);
}

/*
 * Sort gres/gpu records by descending length of type_name. If length is equal,
 * sort by ascending type_name. If still equal, sort by ascending file name.
 */
static int _sort_npu_by_type_name(void* x, void* y) {
    gres_slurmd_conf_t* gres_slurmd_conf1 = *(gres_slurmd_conf_t**)x;
    gres_slurmd_conf_t* gres_slurmd_conf2 = *(gres_slurmd_conf_t**)y;
    int val1, val2, ret;

    if (!gres_slurmd_conf1->type_name && !gres_slurmd_conf2->type_name)
        return 0;

    if (gres_slurmd_conf1->type_name && !gres_slurmd_conf2->type_name)
        return 1;

    if (!gres_slurmd_conf1->type_name && gres_slurmd_conf2->type_name)
        return -1;

    val1 = strlen(gres_slurmd_conf1->type_name);
    val2 = strlen(gres_slurmd_conf2->type_name);
    /*
     * By default, qsort orders in ascending order (smallest first). We want
     * descending order (longest first), so invert order by negating.
     */
    ret = slurm_sort_int_list_desc(&val1, &val2);

    /* Sort by type name value if type name length is equal */
    if (ret == 0)
        ret = xstrcmp(gres_slurmd_conf1->type_name,
                      gres_slurmd_conf2->type_name);

    /* Sort by file name if type name value is equal */
    if (ret == 0)
        ret = _sort_string_null_last(gres_slurmd_conf1->file,
                                     gres_slurmd_conf2->file);

    return ret;
}

static int _find_nonnull_type_in_gres_list(void* x, void* key) {
    gres_slurmd_conf_t* gres_slurmd_conf = (gres_slurmd_conf_t*)x;

    if (!gres_slurmd_conf)
        return 0;

    if (gres_slurmd_conf->type_name && gres_slurmd_conf->type_name[0])
        return 1;

    return 0;
}

/*
 * Find the first conf_gres record (x) with a GRES type that is a substring
 * of the GRES type of sys_gres (key).
 */
static int _find_type_in_gres_list(void* x, void* key) {
    gres_slurmd_conf_t* gres_slurmd_conf = (gres_slurmd_conf_t*)x;
    char* sys_gres_type = (char*)key;

    if (!gres_slurmd_conf)
        return 0;

    /* If count is 0, then we already accounted for it */
    if (gres_slurmd_conf->count == 0)
        return 0;

    xassert(gres_slurmd_conf->count == 1);

    if (!sys_gres_type) {
        debug3("gres/npu: _find_type_in_gres_list(): no sys_gres_type specified, defaulting to %s", gres_slurmd_conf->type_name);
        return 1;
    }

    if (xstrcasestr(sys_gres_type, gres_slurmd_conf->type_name))
        return 1;
    else
        return 0;
}

/*
 * Sync the GRES type of each device detected on the system (gres_list_system)
 * to its corresponding GRES type specified in [gres|slurm].conf. In effect, the
 * sys GRES type will be cut down to match the corresponding conf GRES type.
 *
 * NOTE: Both lists will be sorted in descending order by type_name
 * length. gres_list_conf_single is assumed to only have records of count == 1.
 */
static void _normalize_sys_gres_types(list_t* gres_list_system,
                                      list_t* gres_list_conf_single) {
    gres_slurmd_conf_t *sys_gres, *conf_gres;
    list_itr_t* itr;
    bool strip_type = true;

    /* No need to sync anything if configured GRES list is empty */
    if (!gres_list_conf_single || list_count(gres_list_conf_single) == 0)
        return;

    /*
     * Determine if any of the existing GRES have their types defined. If
     * have a type, then all GRES must have a type defined and stripping the
     * type is not helpful
     */
    if (list_find_first(gres_list_conf_single, _find_nonnull_type_in_gres_list, NULL))
        strip_type = false;

    /*
     * Sort conf and sys gres lists by longest GRES type to shortest, so we
     * can avoid issues if e.g. `k20m` and `k20m1` are both specified.
     */
    list_sort(gres_list_conf_single, _sort_npu_by_type_name);
    list_sort(gres_list_system, _sort_npu_by_type_name);

    /* Only match devices if the conf gres count isn't exceeded */
    itr = list_iterator_create(gres_list_system);
    while ((sys_gres = list_next(itr))) {
        conf_gres = list_find_first(gres_list_conf_single,
                                    _find_type_in_gres_list,
                                    sys_gres->type_name);
        if (!conf_gres) {
            if (strip_type) {
                info("Could not find an unused configuration record with a GRES type that is a substring of system device `%s`. Setting system GRES type to NULL",
                     sys_gres->type_name);
                xfree(sys_gres->type_name);
                sys_gres->config_flags &= ~GRES_CONF_HAS_TYPE;
            }
            continue;
        }

        xassert(conf_gres->count == 1);

        /* Temporarily set count to 0 to account for it */
        conf_gres->count = 0;

        /* Overwrite sys_gres type to match conf_gres type */
        xfree(sys_gres->type_name);
        sys_gres->type_name = xstrdup(conf_gres->type_name);
    }
    list_iterator_destroy(itr);

    /* Reset counts back to 1 */
    itr = list_iterator_create(gres_list_conf_single);
    while ((conf_gres = list_next(itr)))
        conf_gres->count = 1;
    list_iterator_destroy(itr);
}

/* See if the conf GRES matches the system GRES */
static int _match_gres(gres_slurmd_conf_t* conf_gres,
                       gres_slurmd_conf_t* sys_gres) {
    /* This record has already been "taken" (matched another conf GRES) */
    if (sys_gres->count == 0)
        return 0;

    /*
     * If the config gres has a type check it with what is found on the
     * system.
     */
    if (conf_gres->type_name &&
        xstrcmp(conf_gres->type_name, sys_gres->type_name))
        return 0;

    /*
     * If the config gres has a file check it with what is found on the
     * system.
     */
    if (conf_gres->file && xstrcmp(conf_gres->file, sys_gres->file))
        return 0;

    /* If all checks out above or nothing was defined return */
    return 1;
}

/*
 * Check that a gres.conf GRES has the same CPUs and Links as a system GRES, if
 * specified
 */
static int _validate_cpus_links(gres_slurmd_conf_t* conf_gres, gres_slurmd_conf_t* sys_gres) {
    /*
     * If conf_gres->cpus doesn't convert into conf_gres->cpus_bitmap, then
     * the configuration is messed up, and we should never validate it
     * against any system device.
     */
    if (conf_gres->cpus && !conf_gres->cpus_bitmap)
        return 0;

    /*
     * If the config gres has cpus defined check it with what is found on
     * the system.
     */
    if (conf_gres->cpus_bitmap && sys_gres->cpus_bitmap &&
        !bit_equal(conf_gres->cpus_bitmap, sys_gres->cpus_bitmap))
        return 0;

    /*
     * If the config gres has links defined check it with what is found on
     * the system.
     */
    if (conf_gres->links && sys_gres->links &&
        xstrcmp(conf_gres->links, sys_gres->links))
        return 0;

    /* If all checks out above, return */
    return 1;
}

/* Sort gres/gpu records by "File" value in ascending order, with nulls last */
static int _sort_npu_by_file(void* x, void* y) {
    gres_slurmd_conf_t* gres_slurmd_conf1 = *(gres_slurmd_conf_t**)x;
    gres_slurmd_conf_t* gres_slurmd_conf2 = *(gres_slurmd_conf_t**)y;

    return _sort_string_null_last(gres_slurmd_conf1->file, gres_slurmd_conf2->file);
}

/*
 * Sort GPUs by the order they are specified in links.
 *
 * It is assumed that each links string has a -1 to indicate the position of the
 * current GPU at the position it was enumerated in. The GPUs will be sorted so
 * the links matrix looks like this:
 *
 * -1, 0, ...  0, 0
 *  0,-1, ...  0, 0
 *  0, 0, ... -1, 0
 *  0, 0, ...  0,-1
 *
 * This should preserve the original enumeration order of NVML (which is in
 * order of PCI bus ID).
 */
static int _sort_npu_by_links_order(void* x, void* y) {
    gres_slurmd_conf_t* gres_slurmd_conf_x = *(gres_slurmd_conf_t**)x;
    gres_slurmd_conf_t* gres_slurmd_conf_y = *(gres_slurmd_conf_t**)y;
    int index_x, index_y;

    /* Make null links appear last in sort */
    if (!gres_slurmd_conf_x->links && gres_slurmd_conf_y->links)
        return 1;
    if (gres_slurmd_conf_x->links && !gres_slurmd_conf_y->links)
        return -1;

    index_x = gres_links_validate(gres_slurmd_conf_x->links);
    index_y = gres_links_validate(gres_slurmd_conf_y->links);

    if (index_x < -1 || index_y < -1)
        error("%s: invalid links value found", __func__);

    return slurm_sort_int_list_asc(&index_x, &index_y);
}

// split gres.conf list into single records
// match recprds with system devices
// merge valid records into unified list
extern void _merge_system_gres_conf(list_t* gres_list_conf, list_t* gres_list_system) {
    list_itr_t *itr, *itr2;
    gres_slurmd_conf_t *gres_slurmd_conf, *gres_slurmd_conf_sys;
    list_t *gres_list_conf_single, *gres_list_npu = NULL, *gres_list_non_npu;

    if (gres_list_conf == NULL) {
        error("gres_list_conf is NULL. This shouldn't happen");
        return;
    }

    gres_list_conf_single = list_create(destroy_gres_slurmd_conf);
    gres_list_non_npu = list_create(destroy_gres_slurmd_conf);
    gres_list_npu = list_create(destroy_gres_slurmd_conf);

    debug2("gres_list_conf:");
    print_gres_list(gres_list_conf, LOG_LEVEL_DEBUG2);

    // Break down gres_list_conf into 1 device per record
    itr = list_iterator_create(gres_list_conf);
    while ((gres_slurmd_conf = list_next(itr))) {
        int i;
        hostlist_t* hl;
        char *hl_name, *tmp_file;
        int count = gres_slurmd_conf->count;

        if (!gres_slurmd_conf->count)
            continue;

        if (xstrcasecmp(gres_slurmd_conf->name, "npu")) {
            /* Move record into non-NPU GRES list */
            gres_slurmd_conf = list_remove(itr);
            debug2("preserving original `%s` GRES record",
                   gres_slurmd_conf->name);
            list_append(gres_list_non_npu, gres_slurmd_conf);
            continue;
        }

        if (gres_slurmd_conf->count == 1) {
            /* Already count of 1; move into single-NPU GRES list */
            gres_slurmd_conf = list_remove(itr);
            list_append(gres_list_conf_single, gres_slurmd_conf);
            continue;
        } else if (!gres_slurmd_conf->file) {
            gres_slurmd_conf->count = 1;
            /*
             * Split this record into multiple single-NPU records
             * and add them to the single-NPU GRES list
             */
            for (i = 0; i < count; i++)
                add_gres_to_list(gres_list_conf_single,
                                 gres_slurmd_conf);
            gres_slurmd_conf->count = count;
            continue;
        }

        /*
         * count > 1 and we have devices;
         * Break down record into individual devices.
         */
        hl = hostlist_create(gres_slurmd_conf->file);
        tmp_file = gres_slurmd_conf->file;
        while ((hl_name = hostlist_shift(hl))) {
            gres_slurmd_conf->count = 1;
            gres_slurmd_conf->file = hl_name;
            /*
             * Split this record into multiple single-NPU,
             * single-file records and add to single-NPU GRES list
             */
            add_gres_to_list(gres_list_conf_single,
                             gres_slurmd_conf);
            free(hl_name);
            gres_slurmd_conf->file = NULL;
        }
        hostlist_destroy(hl);
        gres_slurmd_conf->count = count;
        gres_slurmd_conf->file = tmp_file;
    }
    list_iterator_destroy(itr);

    /*
     * Truncate the full system device types to match types in conf records
     */
    _normalize_sys_gres_types(gres_list_system, gres_list_conf_single);

    /*
     *  Sort null files last, so that conf records with a specified File
     *  are matched first in _match_gres(). Then, conf records without a
     *  File can fill in any remaining holes.
     */
    list_sort(gres_list_conf_single, _sort_npu_by_file);
    /* Sort system devices in the same way for convenience */
    list_sort(gres_list_system, _sort_npu_by_file);

    itr = list_iterator_create(gres_list_conf_single);
    itr2 = list_iterator_create(gres_list_system);
    while ((gres_slurmd_conf = list_next(itr))) {
        list_iterator_reset(itr2);
        while ((gres_slurmd_conf_sys = list_next(itr2))) {
            if (!_match_gres(gres_slurmd_conf,
                             gres_slurmd_conf_sys)) {
                continue;
            }

            /*
             * We have a match, so if CPUs and Links are specified,
             * see if they too match. If a value is specified and
             * does not match the system, emit error. If null, just
             * use the system-detected value.
             */
            if (!_validate_cpus_links(gres_slurmd_conf, gres_slurmd_conf_sys)) {
                /* What was specified differs from system */
                error("This NPU specified in [slurm|gres].conf has mismatching Cores or Links from the device found on the system. Ignoring it.");
                error("[slurm|gres].conf:");
                print_gres_conf(gres_slurmd_conf,
                                LOG_LEVEL_ERROR);
                error("system:");
                print_gres_conf(gres_slurmd_conf_sys,
                                LOG_LEVEL_ERROR);

                xassert(gres_slurmd_conf_sys->count == 1);

                /*
                 * Temporarily set the sys record count to 0 to
                 * mark it as already "used up"
                 */
                gres_slurmd_conf_sys->count = 0;
                break;
            }

            /* We found a match! */
            break;
        }

        if (gres_slurmd_conf_sys) {
            /*
             * Completely ignore this conf record if Cores and/or
             * Links do not match the corresponding system NPU
             */
            if (gres_slurmd_conf_sys->count == 0)
                continue;

            /*
             * Since the system NPU matches up completely with a
             * configured NPU, add the system NPU to the final list
             */
            debug("Including the following NPU matched between system and configuration:");
            print_gres_conf(gres_slurmd_conf_sys, LOG_LEVEL_DEBUG);

            /*
             * If the conf record did not fall back to default env
             * flags (i.e. it explicitly set env flags), then use
             * the conf's env flags. Otherwise, use the AutoDetected
             * env flags.
             */
            if (!(gres_slurmd_conf->config_flags &
                  GRES_CONF_ENV_DEF)) {
                gres_slurmd_conf_sys->config_flags &=
                    ~GRES_CONF_ENV_SET;
                gres_slurmd_conf_sys->config_flags |=
                    gres_slurmd_conf->config_flags &
                    GRES_CONF_ENV_SET;
            }

            list_remove(itr2);
            list_append(gres_list_npu, gres_slurmd_conf_sys);
            continue;
        }

        /* Else, config-only NPU */
        if (gres_slurmd_conf->file) {
            /*
             * Add the "extra" configured NPU to the final list, but
             * only if file is specified
             */
            debug("Including the following config-only NPU:");
            print_gres_conf(gres_slurmd_conf, LOG_LEVEL_DEBUG);
            list_remove(itr);
            list_append(gres_list_npu, gres_slurmd_conf);
        } else {
            /*
             * Either the conf NPU was specified in slurm.conf only,
             * or File (a required parameter for NPUs) was not
             * specified in gres.conf. Either way, ignore it.
             */
            error("Discarding the following config-only NPU due to lack of File specification:");
            print_gres_conf(gres_slurmd_conf, LOG_LEVEL_ERROR);
        }
    }
    list_iterator_destroy(itr);

    /* Reset the system NPU counts, in case system list is used after */
    list_iterator_reset(itr2);
    while ((gres_slurmd_conf_sys = list_next(itr2)))
        if (gres_slurmd_conf_sys->count == 0)
            gres_slurmd_conf_sys->count = 1;
    list_iterator_destroy(itr2);

    /* Print out all the leftover system NPUs that are not being used */
    if (gres_list_system && list_count(gres_list_system)) {
        warning("The following autodetected NPUs are being ignored:");
        print_gres_list(gres_list_system, LOG_LEVEL_INFO);
    }

    /* Add NPUs + non-NPUs to gres_list_conf */
    list_flush(gres_list_conf);
    if (gres_list_npu && list_count(gres_list_npu)) {
        /* Sort by device file first, in case no links */
        list_sort(gres_list_npu, _sort_npu_by_file);
        /* Sort by links, which is a stand-in for PCI bus ID order */
        list_sort(gres_list_npu, _sort_npu_by_links_order);
        debug2("gres_list_npu");
        print_gres_list(gres_list_npu, LOG_LEVEL_DEBUG2);
        list_transfer(gres_list_conf, gres_list_npu);
    }
    if (gres_list_non_npu && list_count(gres_list_non_npu))
        list_transfer(gres_list_conf, gres_list_non_npu);
    FREE_NULL_LIST(gres_list_npu);
    FREE_NULL_LIST(gres_list_conf_single);
    FREE_NULL_LIST(gres_list_non_npu);
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
        debug("node config in slurmd, cpu_cnt: %u, gres_name: %s", node_config->cpu_cnt, node_config->gres_name);
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
extern void gres_p_job_set_env(char*** job_env_ptr, bitstr_t* gres_bit_alloc, uint64_t gres_cnt, bitstr_t* usable_gres, gres_internal_flags_t flags) {
    common_gres_env_t gres_env = {
        .bit_alloc = gres_bit_alloc,
        .env_ptr = job_env_ptr,
        .flags = flags,
        .gres_cnt = gres_cnt,
        .gres_conf_flags = node_flags,  // node_flags has env info
        .gres_devices = gres_devices,
        .is_job = true,
        .usable_gres = usable_gres,
    };

    gres_common_npu_set_env(&gres_env);
}

// set environment variables for step (i.e. all tasks) base on job step's GRES
extern void gres_p_step_set_env(char*** step_env_ptr, bitstr_t* gres_bit_alloc, uint64_t gres_cnt, bitstr_t* usable_gres, gres_internal_flags_t flags) {
    common_gres_env_t gres_env = {
        .bit_alloc = gres_bit_alloc,
        .env_ptr = step_env_ptr,
        .flags = flags,
        .gres_cnt = gres_cnt,
        .gres_conf_flags = node_flags,  // node_flags has env info
        .gres_devices = gres_devices,
        .usable_gres = usable_gres,
    };

    gres_common_npu_set_env(&gres_env);
}

// set environment variables for one task base on job step's GRES
extern void gres_p_task_set_env(char*** task_env_ptr, bitstr_t* gres_bit_alloc, uint64_t gres_cnt, bitstr_t* usable_gres, gres_internal_flags_t flags) {
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

    gres_common_npu_set_env(&gres_env);
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

// return a list of devices of this type, element's type is "gres_device_t"
// the list should be freed using FREE_NULL_LIST()
extern list_t* gres_p_get_devices(void) {
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
    (void)gres_common_prep_set_env(prep_env_ptr, gres_prep, node_inx, node_flags, gres_devices);  // TODO: do we need reimplement this for NPU?
}

/************************************* TAINTED ************************************* */

/*

// init func
extern int init(void)  {
    verbose("npu plugin initialized");
    return SLURM_SUCCESS;
}

// cleanup func
extern void fini(void) {
    verbose("npu plugin finalized");
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