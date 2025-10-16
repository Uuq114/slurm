/* Support DCMI interfaces to Huawei NPU. */

#define _GNU_SOURCE

#include "dcmi_interface_api.h"  // sys path: /usr/local/Ascend/driver/include/dsmi_common_interface.h
#include "src/common/log.h"
#include "src/common/timers.h"
#include "src/common/xstring.h"
#include "src/interfaces/gres.h"
#include "src/interfaces/jobacct_gather.h"
#include "src/interfaces/npu.h"

/* variables, required by generic plugin interface */
const char plugin_name[] = "NPU DCMI plugin";
const char plugin_type[] = "npu/dcmi";
const uint32_t plugin_version = SLURM_VERSION_NUMBER;

/* DCMI macros */
#ifndef MAX_CARD_NUM
#define MAX_CARD_NUM 16
#endif

/* other global variables */
static bitstr_t* saved_npus = NULL;
static int npumem_pos = -1;
static int npuutil_pos = -1;
int card_list[MAX_CARD_NUM] = {0};
int device_id_max[MAX_CARD_NUM] = {0};
uint32_t card_count;
uint32_t device_count;

extern int init(void) {
    debug("%s: %s loaded", __func__, plugin_name);
    return SLURM_SUCCESS;
}

extern void fini(void) {
    debug("%s: unloading %s", __func__, plugin_name);
}

/* NPU DCMI return code, predefined in dcmi_interface_api.h */
/*
typedef enum {
    // ok
    DCMI_OK = 0,
    // invalid parameter, device malfunction
    DCMI_ERR_CODE_INVALID_PARAMETER = -8001,
    DCMI_ERR_CODE_MEM_OPERATE_FAIL = -8003,
    DCMI_ERR_CODE_INVALID_DEVICE_ID = -8007,
    DCMI_ERR_CODE_DEVICE_NOT_EXIST = -8008,
    DCMI_ERR_CODE_CONFIG_INFO_NOT_EXIST = -8023,
    // need permission, unsupported
    DCMI_ERR_CODE_OPER_NOT_PERMITTED = -8002,
    DCMI_ERR_CODE_NOT_SUPPORT_IN_CONTAINER = -8013,
    DCMI_ERR_CODE_NOT_SUPPORT = -8255,
    // timeout
    DCMI_ERR_CODE_TIME_OUT = -8006,
    DCMI_ERR_CODE_NOT_REDAY = -8012,
    DCMI_ERR_CODE_IS_UPGRADING = -8017,
    DCMI_ERR_CODE_RESOURCE_OCCUPIED = -8020,
    // internal error
    DCMI_ERR_CODE_SECURE_FUN_FAIL = -8004,
    DCMI_ERR_CODE_INNER_ERR = -8005,
    DCMI_ERR_CODE_IOCTL_FAIL = -8009,
    DCMI_ERR_CODE_SEND_MSG_FAIL = -8010,
    DCMI_ERR_CODE_RECV_MSG_FAIL = -8011,
    DCMI_ERR_CODE_RESET_FAIL = -8015,
    DCMI_ERR_CODE_ABORT_OPERATE = -8016,
} dcmi_rc_t;
*/
typedef int dcmi_rc_t;

static const char* _dcmi_strerror(dcmi_rc_t rc) {
    switch (rc) {
        case DCMI_OK:
            return "Success";
        case DCMI_ERR_CODE_INVALID_PARAMETER:
            return "Invalid parameter";
        case DCMI_ERR_CODE_MEM_OPERATE_FAIL:
            return "Memory operation failed";
        case DCMI_ERR_CODE_INVALID_DEVICE_ID:
            return "Invalid device ID";
        case DCMI_ERR_CODE_DEVICE_NOT_EXIST:
            return "Device does not exist";
        case DCMI_ERR_CODE_CONFIG_INFO_NOT_EXIST:
            return "Configuration information does not exist";
        case DCMI_ERR_CODE_OPER_NOT_PERMITTED:
            return "Operation not permitted";
        case DCMI_ERR_CODE_NOT_SUPPORT_IN_CONTAINER:
            return "Not supported in container";
        case DCMI_ERR_CODE_NOT_SUPPORT:
            return "Not supported";
        case DCMI_ERR_CODE_TIME_OUT:
            return "Operation timed out";
        case DCMI_ERR_CODE_NOT_REDAY:
            return "Device not ready";
        case DCMI_ERR_CODE_IS_UPGRADING:
            return "Device is upgrading";
        case DCMI_ERR_CODE_RESOURCE_OCCUPIED:
            return "Resource occupied";
        case DCMI_ERR_CODE_SECURE_FUN_FAIL:
            return "Secure function failed";
        case DCMI_ERR_CODE_INNER_ERR:
            return "Internal error";
        case DCMI_ERR_CODE_IOCTL_FAIL:
            return "IOCTL failed";
        case DCMI_ERR_CODE_SEND_MSG_FAIL:
            return "Failed to send message to device";
        case DCMI_ERR_CODE_RECV_MSG_FAIL:
            return "Failed to receive message from device";
        case DCMI_ERR_CODE_RESET_FAIL:
            return "Device reset failed";
        case DCMI_ERR_CODE_ABORT_OPERATE:
            return "Operation aborted";
        default:
            return "Unknown error code";
    }
}

/* DCMI lib func wrapper */

// init before call other DCMI func
static int _dcmi_init(void) {
    int rc;

    DEF_TIMERS;
    START_TIMER;
    rc = dcmi_init();
    END_TIMER;
    debug3("dcmi_init() took %ld microseconds", DELTA_TIMER);
    if (rc != DCMI_OK) {
        error("DCMI init failed, rc=%d, msg=%s", rc, _dcmi_strerror(rc));
    } else {
        debug2("Successfully initialized DCMI");
    }
    return rc;
}

/* NPU interface func */

// detect npus on node, return a gres conf list, return NULL if an error occurs
// caller is responsible for freeing the list
static list_t* _get_system_npu_list_dcmi(node_config_load_t* node_config) {
    list_t* npu_list = list_create(destroy_gres_slurmd_conf);
    uint32_t device_count = 0;
    int rc;

    _dcmi_init();
    // get device count
    rc = dcmi_get_all_device_count(&device_count);
    if (rc != DCMI_OK) {
        error("DCMI get device count failed, rc=%d, msg=%s", rc, _dcmi_strerror(rc));
        return npu_list;
    }
    info("Detected NPU count: %u", device_count);
    // loop to get each device info
    for (uint32_t i = 0; i < device_count; i++) {
        char dev_path[20];
        snprintf(dev_path, sizeof(dev_path), "/dev/davinci%u", i);
        gres_slurmd_conf_t npu_conf = {
            .config_flags = GRES_CONF_ENV_DCMI,  // TODO: add this var
            .count = 1,
            .name = "npu",
            .cpu_cnt = node_config->cpu_cnt,
            .file = xstrdup(dev_path),
        };
        add_gres_to_list(npu_list, &npu_conf);
    }

    // TODO: free dcmi resources, npu_shutdown()?
    return npu_list;
}

extern list_t* npu_p_get_system_npu_list(node_config_load_t* node_config) {
    list_t* gres_list_system = NULL;
    if (!(gres_list_system = _get_system_npu_list_dcmi(node_config))) {
        error("System NPU detection failed");
    }
    return gres_list_system;
}

extern void npu_p_step_hardware_init(bitstr_t* usable_npus, char* tres_freq) {
    char* freq = NULL;
    char* tmp = NULL;

    xassert(tres_freq);
    xassert(usable_npus);

    if (!usable_npus) {
        return; /* Job allocated no NPUs */
    }
    if (!tres_freq) {
        return; /* No TRES frequency spec */
    }
    if (!(tmp = strstr(tres_freq, "npu:"))) {
        return; /* No NPU frequency spec */
    }

    // dcmi does not support setting frequency in 25.2.0 version
    return;
}

extern void npu_p_step_hardware_fini(void) {
    if (!saved_npus) {
        return;
    }
    // dcmi does not support setting frequency in 25.2.0 version, skip reset NPU freq
    FREE_NULL_BITMAP(saved_npus);
    // TODO: check if dcmi has npu_shutdown() to free resources, cannot find now
}

// a function for test, not used now
extern char* npu_p_test_cpu_conv(char* cpu_range) {
    return NULL;
}

// DCMI lib only supports reading device power, skip this
extern int npu_p_energy_read(uint32_t dv_ind, npu_status_t* npu) {
    return SLURM_SUCCESS;
}

extern int npu_p_get_device_count(uint32_t* device_count) {
    int rc;
    _dcmi_init();
    rc = dcmi_get_all_device_count(device_count);
    if (rc != DCMI_OK) {
        error("DCMI get device count failed, rc=%d, msg=%s", rc, _dcmi_strerror(rc));
        *device_count = 0;
        return rc;
    }
    return SLURM_SUCCESS;
}

extern int npu_p_usage_read(pid_t pid, acct_gather_data_t* data) {
    int rc;
    bool track_npumem = (npumem_pos != -1);
    bool track_npuutil = (npuutil_pos != -1);

    if (!track_npuutil && !track_npumem) {
        debug2("%s: We are not tracking TRES npuutil/npumem", __func__);
        return SLURM_SUCCESS;
    }

    _dcmi_init();
    // get card/device count
    rc = dcmi_get_card_num_list(&card_count, card_list, MAX_CARD_NUM);
    if (rc != DCMI_OK) {
        error("DCMI get card num list failed, rc=%d, msg=%s", rc, _dcmi_strerror(rc));
        return SLURM_ERROR;
    }
    rc = dcmi_get_all_device_count(&device_count);
    if (rc != DCMI_OK) {
        error("DCMI get device count failed, rc=%d, msg=%s", rc, _dcmi_strerror(rc));
        return SLURM_ERROR;
    }
    // get device_id_max
    for (int i = 0; i < card_count; i++) {
        int mcu_id, cpu_id;
        rc = dcmi_get_device_id_in_card(card_list[i], &device_id_max[i], &mcu_id, &cpu_id);
        if (rc != DCMI_OK) {
            error("DCMI get device id max failed, rc=%d, msg=%s", rc, _dcmi_strerror(rc));
            return SLURM_ERROR;
        }
        debug3("[Card %d] device_id_max: %d, mcu_id: %d, cpu_id: %d\n", card_list[i], device_id_max[i], mcu_id, cpu_id);  // device index: (0~card_count-1, 0~device_id_max-1)
    }

    // DCMI does not support per-process usage reading, skip this
    data[npumem_pos].size_read = 0;
    data[npuutil_pos].size_read = 0;
    // NOTE: sum up pid's usage on all devices, may count some other processes' usage
    return SLURM_SUCCESS;
}