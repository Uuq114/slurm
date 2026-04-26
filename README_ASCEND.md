# Slurm 适配昇腾 NPU 总结

本文基于 Slurm 25.05 的 GPU GRES 实现、`playground` 分支中的 NPU 适配代码、适配记录，以及《Slurm GRES 插件工作机制：以GPU为例》的推文整理。目标是在 Slurm 中把昇腾 NPU 作为 `gres/npu` 资源调度，并在作业运行时设置昇腾运行时需要的可见设备环境变量。

## GRES 运行主线

Slurm 采用中心化控制结构：

- `slurmctld` 负责资源状态和调度决策，每个计算节点运行 `slurmd`，作业 step 启动后由 `slurmstepd` 管理进程。
- GPU、NPU、MIC、NIC 等设备通常以 GRES（Generic RESource）建模。

### 1. 配置声明资源

管理员先在配置中声明“集群认为这个节点有什么资源”：

- `slurm.conf`：通过 `GresTypes=npu` 声明资源类型，通过 `NodeName ... Gres=npu:Ascend-910B3:8` 声明节点资源数量。
- `gres.conf`：把声明的资源落到具体设备，例如 `Name=npu Type=Ascend-910B3 File=/dev/davinci[0-7] AutoDetect=dcmi`。
- `cgroup.conf`：通过 `ConstrainDevices=yes` 控制作业实际能访问哪些设备文件。

这里的关键点是：`slurm.conf` 解决“调度层面有多少资源”，`gres.conf` 解决“这些资源对应哪些设备文件”，`cgroup.conf` 解决“进程实际是否能访问未分配设备”。

### 2. slurmd 加载插件

`slurmd` 启动后读取配置，GRES 框架会按 `GresTypes` 加载对应插件。GPU 的集成插件是 `gres/gpu`，NPU 的集成插件是本分支新增的 `gres/npu`。

每种 GRES 又会有一个硬件接口层。GPU 走 `src/interfaces/gpu.c`，NPU 走 `src/interfaces/npu.c`。接口层根据 `AutoDetect` 选择具体后端：

- GPU：`AutoDetect=nvml|nvidia|rsmi|oneapi|nrt`
- NPU：`AutoDetect=dcmi`

接口层会先尝试 `dlopen()` 对应厂商动态库。这样编译环境不必和每个计算节点的硬件完全一致。GPU 已有成熟的 generic 后端可兜底；NPU 当前实际探测依赖 DCMI，generic 后端主要是占位。

### 3. 探测系统资源

如果启用了 `AutoDetect`，`slurmd` 会通过硬件后端查询本机真实设备。

GPU 典型路径是：

```text
gres/gpu -> interfaces/gpu.c -> gpu/nvml -> NVML API
```

NPU 适配后的路径是：

```text
gres/npu -> interfaces/npu.c -> npu/dcmi -> DCMI API
```

以昇腾 NPU 为例，`npu/dcmi` 会调用 DCMI 初始化和查询接口，得到设备数量与芯片信息，然后生成系统探测列表。每个 NPU 会被描述为一条类似这样的内部记录：

```text
Name=npu Type=Ascend-910B3 File=/dev/davinci0 Flags=AutoDetect,ENV_DCMI
```

### 4. 合并配置和真实设备

探测完成后，`gres/npu` 和 `gres/gpu` 都会做同一件事：把 `gres.conf` 中的配置记录和系统探测记录合并。

合并时主要校验：

- `Type` 是否匹配。配置中的类型可以是系统完整设备名的子串。
- `File` 是否匹配。NPU 示例中就是 `/dev/davinci0` 到 `/dev/davinci7`。
- `CPUs/Cores` 和 `Links` 是否与系统探测结果冲突。

合并成功后，插件会生成 `gres_devices`。这份列表是 `slurmd` 本地后续启动作业、传递给 `slurmstepd`、设置环境变量和 cgroup 设备限制的依据。

### 5. 向 slurmctld 注册节点状态

`slurmd` 完成本地配置加载和设备校验后，会向 `slurmctld` 注册节点并持续发送心跳。`slurmctld` 维护调度层面的节点 GRES 状态，后续作业提交时根据 `NodeName ... Gres=...`、节点状态和作业请求做资源分配。

如果本地 `gres.conf` 与实际设备不一致，问题通常会在 `slurmd` 加载配置阶段暴露。开启 `DebugFlags=Gres` 后，日志中可以看到自动探测、配置合并、忽略设备等信息。

### 6. 分配资源并启动作业

用户提交作业时，例如：

```bash
srun -N1 --gres=npu:1 ...
```

`slurmctld` 会选择满足请求的节点，并在 job 和 step 的 GRES 状态中记录分配数量和设备位图。`slurmd` 收到启动请求后，会把本地 `gres_devices` 以及本次 step 的 GRES 分配信息传给 `slurmstepd`。

### 7. 设置环境变量和设备隔离

`slurmstepd` 启动作业进程前，GRES 插件会根据分配位图设置环境变量。

GPU 常见变量包括：

- `SLURM_JOB_GPUS`
- `SLURM_STEP_GPUS`
- `SLURM_GPUS_ON_NODE`
- `CUDA_VISIBLE_DEVICES`、`ROCR_VISIBLE_DEVICES`、`ZE_AFFINITY_MASK` 等厂商变量

NPU 适配新增变量包括：

- `SLURM_JOB_NPUS`
- `SLURM_STEP_NPUS`
- `SLURM_NPUS_ON_NODE`
- `ASCEND_RT_VISIBLE_DEVICES`

其中 `ASCEND_RT_VISIBLE_DEVICES` 是昇腾运行时识别可见 NPU 的关键变量。使用 `AutoDetect=dcmi` 时，DCMI 探测出的设备会自动带上 `ENV_DCMI` 标志，因此会自动设置该变量。如果完全手写 NPU 配置而不使用自动探测，需要在 `gres.conf` 中显式写 `Flags=ascend_npu_env`。

环境变量告诉应用“应该使用哪些设备编号”，cgroup device 约束则负责从内核层面限制进程实际能打开哪些设备文件。二者配合后，作业既能看到正确的设备编号，也不能越权访问未分配设备。

### 8. 记账和回收

作业结束后，`slurmstepd/slurmd` 回收 step 状态，`slurmctld` 释放调度层面的 GRES 占用，后续记账信息可通过 `slurmdbd` 写入数据库。当前 NPU DCMI 插件已完成设备发现和环境变量设置，但按进程读取 `npumem`、`npuutil` 仍是占位逻辑；DCMI 25.2.0 也没有用于当前适配的频率设置能力，因此 NPU 频率设置没有实际动作。

## GPU 插件工作方式

GPU 是 Slurm GRES 中最成熟的参考实现。本次 NPU 适配主要参考它的三层结构：

- `gres/gpu`：处理 Slurm 资源模型，包括配置合并、设备列表、job/step/task 环境变量、stepd 传递。
- `interfaces/gpu.c`：把 GRES 层需要的 GPU 操作抽象为统一函数表。
- `gpu/nvml`、`gpu/rsmi`、`gpu/oneapi` 等后端：调用厂商 API 探测和控制设备。

这套结构的好处是调度逻辑和硬件 API 解耦。Slurm 核心只关心 GRES 的数量、类型和设备文件；具体如何从 NVML、RSMI 或 oneAPI 获取设备信息，由硬件后端负责。

## NPU 插件工作方式

本分支把昇腾 NPU 按同样结构接入 Slurm：

- `gres/npu`：新增 NPU GRES 集成插件，复用 GPU 插件的资源合并、设备列表、stepd 传递和环境变量设置思路。
- `interfaces/npu.c`：新增 NPU 接口层，根据 `AutoDetect=dcmi` 选择 `npu/dcmi`。
- `npu/dcmi`：调用昇腾 DCMI 查询设备数量和芯片型号，生成 `Name=npu`、`Type=Ascend-910B3`、`File=/dev/davinciX` 的系统设备列表。
- `gres_common_npu_set_env()`：根据分配结果设置 `SLURM_*_NPUS` 和 `ASCEND_RT_VISIBLE_DEVICES`。

为了让这条链路生效，本分支还接入了配置解析和构建系统：新增 `GRES_AUTODETECT_NPU_DCMI`、`GRES_CONF_ENV_DCMI`，支持 `AutoDetect=dcmi` 和 `Flags=ascend_npu_env`，并通过 `auxdir/x_ac_dcmi.m4`、`configure.ac`、各级 `Makefile.am` 把 DCMI 插件纳入构建。

## 编译安装步骤

以下步骤以 openEuler 22.03、昇腾 910B3 环境为例。

### 1. 准备系统依赖

```bash
yum install rpmdevtools
yum install -y readline-devel munge-devel pam-devel perl-ExtUtils-MakeMaker \
    lua-devel mariadb-devel munge-libs libssh-devel libssh2-devel \
    hwloc-devel http-parser-devel json-c-devel libjwt-devel glib2-devel
```

openEuler 22.03 仓库中的 PMIx 包可能没有使用 hwloc2 编译。如需 PMIx 支持，需要自行编译 PMIx，例如基于 openEuler 的 `pmix-4.2.6` 源码包。

### 2. 准备昇腾 DCMI 动态库

编译机和计算节点需要安装昇腾驱动或 DCMI，并确保头文件、动态库位于当前 `configure` 可探测的位置。

当前适配会查找：

- 头文件：`/usr/local/dcmi`、`/usr/local/dcmi/include`、`/usr/local/Ascend/driver/kernel/inc/driver`
- 动态库：`/usr/local/dcmi/lib/libdcmi.so`
- 动态库：`/usr/local/Ascend/driver/lib64/driver/libdrvdsmi_host.so`

运行时还需要 loader 能找到 `libdrvdsmi_host.so`。如果不在系统默认动态库路径中，可配置 `LD_LIBRARY_PATH` 或 `/etc/ld.so.conf.d/`。

如果构建环境没有昇腾 DCMI 库，可用 `--without-dcmi` 跳过 `npu/dcmi` 插件，但这样无法使用 `AutoDetect=dcmi`。

### 3. 生成源码包并构建 RPM

在源码目录同级放置构建脚本，例如：

```bash
#!/bin/bash

set -e

cd /opt/git/dev/slurm
autoreconf -ivf

cd /opt/git/dev
rm -rf ./slurm-25.05.0*
cp -r ./slurm ./slurm-25.05.0
tar cjpvf slurm-25.05.0.tar.bz2 slurm-25.05.0/

# 如果主机 HDF5 包不兼容，可显式禁用 hdf5。
rpmbuild --with mysql --with lua --with jwt --with slurmrestd \
    --without hdf5 -ta slurm-25.05.0.tar.bz2
```

如遇到 RPM `check-rpaths` 报错，可在 `~/.rpmmacros` 中临时关闭 rpath 检查：

```rpm
%_topdir %(echo $HOME)/rpmbuild

#%__arch_install_post \
#    [ "%{buildarch}" = "noarch" ] || QA_CHECK_RPATHS=1 ; \
#    case "${QA_CHECK_RPATHS:-}" in [1yY]*) /usr/lib/rpm/check-rpaths ;; esac \
#    /usr/lib/rpm/check-buildroot
```

`slurm.spec` 中已经把 `libdrvdsmi_host`、`libascend_hal`、`libdcmi` 从自动 RPM 依赖中过滤掉，避免把昇腾驱动库错误打成系统 RPM 依赖。实际部署时仍需由昇腾驱动或 DCMI 安装这些库。

### 4. 安装服务包

主控节点通常安装：

```bash
yum -y install ./slurm-25.05.0-*.rpm \
    ./slurm-devel-25.05.0-*.rpm \
    ./slurm-slurmctld-25.05.0-*.rpm \
    ./slurm-slurmdbd-25.05.0-*.rpm \
    ./slurm-contribs-25.05.0-*.rpm \
    ./slurm-example-configs-25.05.0-*.rpm \
    ./slurm-libpmi-25.05.0-*.rpm \
    ./slurm-perlapi-25.05.0-*.rpm \
    ./slurm-slurmrestd-25.05.0-*.rpm
```

计算节点至少需要安装 Slurm 基础包、`slurmd` 包和包含插件的库包，并确保 `/dev/davinci*`、`/dev/davinci_manager` 等设备节点以及昇腾驱动库对 slurmd/slurmstepd 可访问。

测试环境中为了避免设备权限问题，曾将 `slurmctld`、`slurmd`、`slurmdbd` 的 systemd service 改为 root 运行，即注释掉服务文件中的：

```ini
User=slurm
Group=slurm
```

生产环境更建议通过 udev 规则、用户组和 cgroup 设备白名单授予所需设备权限。

## 示例配置

以下示例为单个 910B3 节点 `ascend31`，192 CPU，8 张昇腾 NPU。

### `/etc/slurm/slurm.conf`

```ini
ClusterName=sjtupi
SlurmctldHost=ascend31

AuthType=auth/munge
ProctrackType=proctrack/cgroup
TaskPlugin=task/cgroup
JobAcctGatherType=jobacct_gather/cgroup

AccountingStorageType=accounting_storage/slurmdbd
AccountingStorageHost=ascend31
AccountingStorageTRES=gres/npu

DebugFlags=Gres
SlurmctldDebug=debug3
SlurmdDebug=debug3
SlurmctldLogFile=/var/log/slurmctld.log
SlurmdLogFile=/var/log/slurmd.log
SlurmctldPidFile=/var/run/slurmctld.pid
SlurmdPidFile=/var/run/slurmd.pid
SlurmdSpoolDir=/tmp/slurmd
StateSaveLocation=/etc/slurm/state

SlurmctldPort=6817
SlurmdPort=6818
SlurmctldTimeout=60
SlurmdTimeout=600
SrunPortRange=60001-63000
TCPTimeout=5
Waittime=0

GresTypes=npu
SelectType=select/cons_tres

NodeName=ascend31 CPUs=192 SocketsPerBoard=4 CoresPerSocket=48 ThreadsPerCore=1 RealMemory=512000 Gres=npu:Ascend-910B3:8 Weight=60

PartitionName=ascend Nodes=ascend31 Default=yes State=UP MaxTime=4:00:00 MaxCPUsPerNode=192 DefMemPerCPU=2500 MaxMemPerCPU=2500 AllowQos=ALL
```

如果集群中同时有 GPU 和 NPU，可写成：

```ini
GresTypes=gpu,npu
AccountingStorageTRES=gres/gpu,gres/npu
```

### `/etc/slurm/gres.conf`

使用 DCMI 自动探测并校验设备：

```ini
NodeName=ascend31 Name=npu Type=Ascend-910B3 File=/dev/davinci[0-7] CPUs=0-191 AutoDetect=dcmi
```

完全手写、不使用 DCMI 自动探测时，需要显式设置昇腾环境变量 flag：

```ini
NodeName=ascend31 Name=npu Type=Ascend-910B3 File=/dev/davinci[0-7] CPUs=0-191 Flags=ascend_npu_env
```

### `/etc/slurm/cgroup.conf`

```ini
AllowedRAMSpace=100
AllowedSwapSpace=100
CgroupMountpoint=/sys/fs/cgroup
ConstrainCores=yes
ConstrainDevices=yes
ConstrainRAMSpace=yes
ConstrainSwapSpace=yes
MaxRAMPercent=100
MaxSwapPercent=100
```

310P3 环境曾遇到不支持 cgroup 设备约束的限制，相关配置需要按实际驱动和系统能力调整。910B3 环境建议开启 `ConstrainDevices=yes`，确保作业只能访问被分配的设备文件。

### `/etc/slurm/slurmdbd.conf`

```ini
AuthType=auth/munge
DbdAddr=localhost
DbdHost=localhost
DebugLevel=verbose
LogFile=/var/log/slurmdbd.log
Parameters=PreserveCaseUser
PidFile=/var/run/slurmdbd.pid
PrivateData=accounts,jobs,usage,users,reservations
SlurmUser=root
StorageHost=localhost
StorageLoc=slurmdb
StoragePass=slurm@ascend
StorageType=accounting_storage/mysql
StorageUser=slurm
```

MariaDB 基础初始化示例：

```sql
CREATE DATABASE slurmdb;
CREATE USER 'slurm'@'%' IDENTIFIED BY 'slurm@ascend';
GRANT ALL PRIVILEGES ON slurmdb.* TO 'slurm'@'%' WITH GRANT OPTION;
```

### 验证命令

在计算节点上检查自动探测输出：

```bash
slurmd -C
```

启动服务后检查节点 GRES：

```bash
scontrol show node ascend31
sinfo -Nel
```

提交一个 NPU 作业并检查环境变量：

```bash
srun -N1 --gres=npu:1 env | grep -E 'SLURM_.*NPUS|ASCEND_RT_VISIBLE_DEVICES'
```

期望能看到类似输出：

```text
SLURM_JOB_NPUS=0
SLURM_STEP_NPUS=0
SLURM_NPUS_ON_NODE=1
ASCEND_RT_VISIBLE_DEVICES=0
```

如需排查 GRES 合并和分配过程，开启：

```ini
DebugFlags=Gres
SlurmdDebug=debug3
SlurmctldDebug=debug3
```

如果 `slurmd` 启动或重配置时崩溃，可以直接前台运行调试：

```bash
gdb --args /usr/sbin/slurmd -D -f /etc/slurm/slurm.conf
```

对于 systemd service 产生的 core dump，可用：

```bash
coredumpctl gdb slurmd
```

## 参考资料

- Slurm GRES 插件工作机制：以GPU为例：<https://mp.weixin.qq.com/s/BaiUqVOPM4S0lvGn4VU5sw>
- Slurm Plugin API：<https://slurm.schedmd.com/plugins.html>
- Ascend slurm-atlas-plugin：<https://github.com/Ascend/slurm-atlas-plugin>
