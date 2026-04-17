# Linux InfiniBand 链路利用率实时监控（us 粒度，高频轮询原型）

## 1. 目标与边界

这份实现面向 **InfiniBand 端口带宽利用率** 的高频采样，目标是把采样粒度推进到 **微秒量级**，并将结果持续记录到 CSV 文件。

先给出最重要的结论：

- 常规用户态工具，如 `sar`、`ibstat`、`perf`，**不能稳定做到 us 级采样**
- Python 版 sysfs 轮询也**不适合** us 级采样
- 本目录当前实现的是你给定思路中的 **方案 A**：
  - **IB 硬件计数器**
  - **`CLOCK_MONOTONIC_RAW`**
  - **busy wait**
  - **用户态直接轮询 sysfs counter**

但也必须明确：

- 这不是硬实时采样
- 这不是 `<1 us` 的严格观测工具
- **sysfs read 本身通常就需要数微秒到十几微秒**

因此，这份代码的准确定位应当是：

- **微秒参数输入**
- **微秒量级高频轮询**
- **基于实际观测间隔计算带宽利用率**
- **适合做原型验证、burst 分析和集合通信窗口观测**

如果你要稳定逼近 `<1 us` 或更低开销，需要转向 **DevX / mlx5dv / ibverbs 直接访问硬件计数器**。

---

## 2. 经典定义

链路利用率的经典定义仍然是：

\[
util = \frac{\Delta bytes / \Delta t}{link\ bandwidth}
\]

对 IB 端口，分子使用 PMA 标准计数器：

- `port_xmit_data`
- `port_rcv_data`

Linux sysfs 路径：

- `/sys/class/infiniband/<device>/ports/<port>/counters/port_xmit_data`
- `/sys/class/infiniband/<device>/ports/<port>/counters/port_rcv_data`

根据 Linux kernel 文档，这两个计数器的语义是：

- `port_xmit_data`: transmitted octets divided by 4
- `port_rcv_data`: received octets divided by 4

因此：

- `Δxmit_bytes = Δport_xmit_data × 4`
- `Δrcv_bytes = Δport_rcv_data × 4`

设：

- `Δt`：两次采样之间的**实际观测时间差**
- `rate_bps`：端口当前链路速率

则：

- `xmit_bps = Δxmit_bytes × 8 / Δt`
- `rcv_bps = Δrcv_bytes × 8 / Δt`
- `xmit_util_pct = xmit_bps / rate_bps × 100`
- `rcv_util_pct = rcv_bps / rate_bps × 100`

由于 IB 是全双工，脚本同时输出：

- `xmit_util_pct`
- `rcv_util_pct`
- `summary_util_pct = max(xmit_util_pct, rcv_util_pct)`
- `aggregate_util_pct = (xmit_bps + rcv_bps) / (2 × rate_bps) × 100`

其中：

- `summary_util_pct` 更适合判断繁忙方向
- `aggregate_util_pct` 更适合看双向平均负载

---

## 3. 为什么改成 C，而不是继续用 Python

你给的判断是对的：如果目标是 us 级采样，Python 版本即使把参数改成 `us`，也不具备可信的采样开销控制能力。

原因主要有三类：

1. Python 调度与解释器开销太大  
2. 高频文件读取与对象构造开销明显  
3. `print` / `csv.writerow` / `flush` 在 us 级窗口里会迅速成为主瓶颈

所以这里把主实现切换成了 C，并采用：

- `clock_gettime(CLOCK_MONOTONIC_RAW)`
- `pread()` 轮询 sysfs counter
- busy wait 到目标 deadline
- 使用 **实际采样间隔** 而不是目标间隔做计算

---

## 4. 方案对比

### 4.1 已实现：方案 A

本目录已实现的版本是：

- `ib_util_monitor_us.c`

特点：

- 实现简单
- 不依赖 DevX
- 只要 sysfs counter 可读就能运行
- 适合快速落地

限制：

- sysfs 读取延迟明显
- 1 us 目标周期通常达不到
- 高频记录文件时，I/O 会影响可达频率

### 4.2 未实现：方案 B

若要进一步降低读数开销，应使用：

- `libibverbs`
- `mlx5dv`
- DevX

这样可以绕开 sysfs 文本读取，直接访问更低层的计数器或寄存器。这是更专业、也更复杂的路线。

### 4.3 不作为主方案：方案 C / D

- eBPF：更适合 10–50 us 量级统计，不适合严格 us 级链路利用率
- CQE timestamp：更适合 flow 或完成事件分析，不适合全局端口利用率

---

## 5. 文件结构

本目录当前包含：

- `ib_util_monitor_us.c`
- `Makefile`
- `ib_util_monitor.py`
- `README.md`

说明：

- `ib_util_monitor_us.c`：当前推荐的 us 级实现
- `ib_util_monitor.py`：保留的 Python 版本，更适合 ms 级采样，不建议用于 us 级

---

## 6. 依赖安装

### 6.1 系统依赖

在 Ubuntu 上安装编译与 RDMA 基础工具：

```bash
sudo apt update
sudo apt install -y build-essential rdma-core infiniband-diags ibverbs-utils util-linux
```

这些包的用途：

- `build-essential`：提供 `gcc` 和基本构建工具
- `rdma-core`：提供 RDMA 用户态基础组件
- `infiniband-diags`：提供 `perfquery`、`ibstat` 等工具
- `ibverbs-utils`：提供 `ibv_devinfo` 等工具
- `util-linux`：通常提供 `taskset`、`chrt`

### 6.2 可选依赖

若要做链路压测对照，可另外安装：

```bash
sudo apt install -y perftest
```

---

## 7. 编译步骤

进入目录：

```bash
cd /Users/zdl/work/code/ib_util_monitor
```

直接编译：

```bash
make
```

生成可执行文件：

```bash
./ib_util_monitor_us
```

如果想手工编译，也可以：

```bash
gcc -O3 -std=c11 -Wall -Wextra -pedantic -o ib_util_monitor_us ib_util_monitor_us.c
```

---

## 8. 运行前检查

先确认本机存在可用的 IB 端口：

```bash
./ib_util_monitor_us --list-ports
```

或交叉验证：

```bash
ibstat
ibv_devinfo
```

还可以直接看 sysfs：

```bash
cat /sys/class/infiniband/mlx5_0/ports/1/link_layer
cat /sys/class/infiniband/mlx5_0/ports/1/state
cat /sys/class/infiniband/mlx5_0/ports/1/phys_state
cat /sys/class/infiniband/mlx5_0/ports/1/rate
```

你需要确认：

- `link_layer = InfiniBand`
- `state = ACTIVE`
- `phys_state = LinkUp`
- `rate` 可正常读出，例如 `100 Gb/sec (4X EDR)`

---

## 9. 运行步骤

### 9.1 最小可运行命令

```bash

./ib_util_monitor_us \
  --device ib_0 \
  --port 1 \
  --interval-us 5 \
  --output ./logs/ib_syccl_ag.csv
```

### 9.2 固定采样次数

例如采样 200000 次，目标周期 50 us：

```bash
./ib_util_monitor_us \
  --device mlx5_0 \
  --port 1 \
  --interval-us 50 \
  --count 200000 \
  --output ./logs/ib_utilization_us.csv
```

### 9.3 追加写入

```bash
./ib_util_monitor_us \
  --device mlx5_0 \
  --port 1 \
  --interval-us 100 \
  --output ./logs/ib_utilization_us.csv \
  --append
```

### 9.4 控制刷盘频率

高频采样时，不建议每条记录都 flush。默认每 1000 条 flush 一次。  
如果你要更保守地落盘：

```bash
./ib_util_monitor_us \
  --device mlx5_0 \
  --port 1 \
  --interval-us 100 \
  --flush-every 100 \
  --output ./logs/ib_utilization_us.csv
```

### 9.5 控制终端打印频率

默认不做周期性打印，以减少 stdout 对采样的影响。  
如果想每 10000 个样本打印一次摘要：

```bash
./ib_util_monitor_us \
  --device mlx5_0 \
  --port 1 \
  --interval-us 100 \
  --print-every 10000 \
  --output ./logs/ib_utilization_us.csv
```

### 9.6 推荐的高频运行方式

若想尽量降低调度抖动，建议将进程绑定到单独 CPU core：

```bash
taskset -c 0 ./ib_util_monitor_us \
  --device mlx5_0 \
  --port 1 \
  --interval-us 50 \
  --output ./logs/ib_utilization_us.csv
```

若你有 root 权限，并希望进一步减少被普通进程抢占的概率，可选：

```bash
sudo chrt -f 90 taskset -c 0 ./ib_util_monitor_us \
  --device mlx5_0 \
  --port 1 \
  --interval-us 50 \
  --output ./logs/ib_utilization_us.csv
```

这不是必须步骤，但对高频采样更有利。

---

## 10. 输出字段

CSV 表头如下：

```text
sample_index,monotonic_raw_ns,target_interval_us,interval_ns,interval_us,read_cost_ns,late_ns,device,port,rate_text,rate_bps,state,phys_state,link_layer,xmit_data_dwords,rcv_data_dwords,xmit_data_dwords_delta,rcv_data_dwords_delta,xmit_bytes_delta,rcv_bytes_delta,xmit_bps,rcv_bps,total_bps,xmit_util_pct,rcv_util_pct,aggregate_util_pct,summary_util_pct
```

重点字段解释：

- `sample_index`
  - 样本编号

- `monotonic_raw_ns`
  - 采样时间戳，来自 `CLOCK_MONOTONIC_RAW`

- `target_interval_us`
  - 目标采样周期，即你传入的 `--interval-us`

- `interval_ns` / `interval_us`
  - 当前样本与前一样本之间的**实际观测时间差**
  - 计算带宽利用率时，用的是这个实际值

- `read_cost_ns`
  - 本次读取两个 sysfs counter 的总耗时
  - 它直接反映了 sysfs 读取开销

- `late_ns`
  - 相对于目标 deadline 的滞后量
  - 若长期较大，说明目标采样周期已经超过当前实现可承受范围

- `xmit_data_dwords_delta` / `rcv_data_dwords_delta`
  - `port_xmit_data` / `port_rcv_data` 的增量

- `xmit_bytes_delta` / `rcv_bytes_delta`
  - 将 dword 换算成字节后的增量

- `xmit_bps` / `rcv_bps`
  - 当前窗口的发送、接收速率

- `xmit_util_pct` / `rcv_util_pct`
  - 单方向链路利用率

- `summary_util_pct`
  - `max(xmit_util_pct, rcv_util_pct)`

- `aggregate_util_pct`
  - 双向平均利用率

---

## 11. 如何理解“us 粒度”

这份代码支持的是：

- **微秒参数输入**
- **微秒量级 deadline**
- **busy wait 触发采样**

但最终真正可信的，是：

- `interval_us`
- `read_cost_ns`
- `late_ns`

也就是说，**是否真的达到 10 us、5 us、1 us，不是看你命令行传了多少，而是看 CSV 里实际测到了多少。**

例如：

- 你设置 `--interval-us 10`
- 但 CSV 里长期显示 `interval_us ≈ 17~30`

这说明：

- 目标周期是 10 us
- 但当前 sysfs 读取与调度开销让你只能做到 17~30 us

这不是公式问题，而是实现路径本身的上限。

---

## 12. 参数建议

### 12.1 建议起点

建议从下面几个点开始试：

- `1000 us`：稳妥
- `100 us`：高频但通常仍可运行
- `50 us`：适合 burst 观察
- `10 us`：实验性
- `1 us`：通常无法由 sysfs 路径稳定达到

### 12.2 如何判断已经逼近系统极限

重点看三列：

- `interval_us`
- `read_cost_ns`
- `late_ns`

如果出现以下现象：

- `read_cost_ns` 已接近甚至超过 `target_interval_us`
- `late_ns` 经常显著大于 0
- `interval_us` 明显大于目标值

说明当前实现已经被：

- sysfs 文本读取
- busy wait 的 CPU 抢占
- 文件写入或打印开销

共同限制。

---

## 13. 实时输出与结束摘要

如果设置了 `--print-every`，终端会输出类似：

```text
[sample=10000] interval=52.417 us read_cost=11.274 us late=2.417 us tx=81.203 Gb/s (81.203%) rx=80.994 Gb/s (80.994%) agg=81.099% summary=81.203%
```

结束时会输出摘要，例如：

```text
监控结束: samples=200000, avg_interval=53.102 us, min_interval=41.881 us, max_interval=219.507 us, avg_read_cost=10.734 us, max_summary_util=96.318%, port=mlx5_0:1, rate=100 Gb/sec (4X EDR)
```

---

## 14. 与你给定方案的对应关系

你给出的判断里有一个核心点：**sysfs 读取太慢，真正 1 us 采样不可行。**

这份实现对此做了两件事：

1. **不伪装成“严格 1 us”工具**  
README 和输出里都显式记录实际 `interval_us` 与 `read_cost_ns`

2. **保留方案 A 的最短落地路径**  
即使用 sysfs counter + `CLOCK_MONOTONIC_RAW` + busy wait 先把实验做起来

因此，这版代码适合作为：

- 方案 A 的可运行原型
- 方案 B 之前的基线测量工具
- 用于判断是否值得继续投入 DevX / verbs 直接计数器访问

---

## 15. 如果你要继续往下做

若你后续要把这条链路真正推进到“专业 us 级采样”，建议路线是：

1. 先用本工具测 `read_cost_ns`
2. 确认 sysfs 路径的可达极限
3. 如果 `read_cost_ns` 明显过大，再转向：
   - `mlx5dv`
   - DevX
   - `ibv_read_counters` / 更低层 API

也就是说，这份实现更像：

- **可运行、可验证、可量化上限的第一阶段工具**

---

## 16. 一条命令快速启动

```bash
cd /Users/zdl/work/code/ib_util_monitor
sudo apt update
sudo apt install -y build-essential rdma-core infiniband-diags ibverbs-utils util-linux
make
mkdir -p ./logs
taskset -c 0 ./ib_util_monitor_us --device mlx5_0 --port 1 --interval-us 100 --output ./logs/ib_utilization_us.csv
```

---

## 17. 参考依据

这份实现的定义和接口依据如下：

- Linux kernel sysfs-class-infiniband:
  - `port_xmit_data` / `port_rcv_data` 是 “data octets divided by 4”
  - `rate` 是 “Port data rate (active width * active speed)”
  - 来源: [Linux Kernel ABI stable](https://docs.kernel.org/6.0/admin-guide/abi-stable.html)

- `perfquery(8)`：
  - 可读取 IB PMA performance counters
  - 来源: [perfquery(8)](https://man7.org/linux/man-pages/man8/perfquery.8.html)

- `CLOCK_MONOTONIC_RAW`：
  - 原始硬件时钟，不受 NTP 渐进校时影响
  - 来源: [clock_gettime(3)](https://man7.org/linux/man-pages/man3/clock_gettime.3.html)
