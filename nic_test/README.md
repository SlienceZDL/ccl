# Linux 网卡利用率实时监控

## 1. 定义口径

这里采用网络管理中最经典的接口利用率定义：在采样区间 \(\Delta t\) 内，接口吞吐量除以接口当前链路带宽。

- 接收方向利用率：

  `rx_util_pct = (Δrx_bytes × 8) / (Δt × link_speed_bps) × 100`

- 发送方向利用率：

  `tx_util_pct = (Δtx_bytes × 8) / (Δt × link_speed_bps) × 100`

- 聚合利用率：

  - 全双工：`aggregate_util_pct = (rx_bps + tx_bps) / (2 × link_speed_bps) × 100`
  - 半双工：`aggregate_util_pct = (rx_bps + tx_bps) / link_speed_bps × 100`

工程上更推荐同时记录 `rx_util_pct` 和 `tx_util_pct`。对于现代以太网，全双工链路的拥塞通常具有方向性，因此脚本额外给出 `summary_util_pct`：

- 全双工时：`summary_util_pct = max(rx_util_pct, tx_util_pct)`
- 半双工时：`summary_util_pct = aggregate_util_pct`

这使单值摘要既不低估单方向瓶颈，也不把全双工的双向容量错误压缩为单一分母。

## 2. 实现原理

脚本直接读取 Linux 内核暴露的 sysfs 统计文件：

- `/sys/class/net/<iface>/statistics/rx_bytes`
- `/sys/class/net/<iface>/statistics/tx_bytes`
- `/sys/class/net/<iface>/speed`
- `/sys/class/net/<iface>/duplex`
- `/sys/class/net/<iface>/operstate`

其中 `rx_bytes` 和 `tx_bytes` 是标准接口统计计数器，链路速率取自当前接口 speed；若该文件不可用，可通过 `--speed-mbps` 手动指定。

## 3. 依赖

### 3.1 Python 依赖

无第三方 Python 依赖，仅使用标准库。

### 3.2 Ubuntu 系统工具

建议安装以下工具：

```bash
sudo apt update
sudo apt install -y iproute2 ethtool
```

说明：

- `iproute2` 用于查看网卡与路由信息。
- `ethtool` 用于在 speed 文件不可用时辅助确认链路速率。

如果你还希望做压测验证，可以额外安装：

```bash
sudo apt install -y iperf3
```

## 4. 文件结构

本目录包含两个文件：

- `nic_util_monitor.py`
- `README.md`

## 5. 部署步骤

### 5.1 进入目录

```bash
cd /Users/zdl/work/code/nic_util_monitor
```

### 5.2 确认 Python 版本

```bash
python3 --version
```

期望输出为 `Python 3.13.5`。如果 `python3` 未指向该版本，请改用你本机的 Python 3.13.5 解释器路径执行脚本。

### 5.3 给脚本执行权限

```bash
chmod +x nic_util.py
```

### 5.4 找到目标网卡

先查看全部接口：

```bash
ip -br link
```

再查看默认路由对应的接口：

```bash
ip route show default
```

典型输出类似：

```text
default via 192.168.1.1 dev eno1 proto dhcp src 192.168.1.10 metric 100
```

其中 `eno1` 就是待监控网卡。

### 5.5 确认链路速率

优先读取 sysfs：

```bash
cat /sys/class/net/eno1/speed
cat /sys/class/net/eno1/duplex
```

若 `speed` 返回正常值，例如 `1000`，表示 1000 Mbps。

若该文件报错、返回 `-1`，或接口为虚拟网卡，可改用：

```bash
ethtool eno1 | grep -E 'Speed|Duplex'
```

如果依然无法得到链路速率，请在运行脚本时手动指定 `--speed-mbps`。

## 6. 运行方式

### 6.1 自动选择默认网卡

```bash
python3 nic_util.py --output ./logs/nic_utilization.csv --interval 0.001
```

脚本会自动从默认路由推断网卡名称。

### 6.2 显式指定网卡

```bash
python3 nic_util.py --interface eno1 --output ./logs/nic_utilization.csv --interval 0.001
```

### 6.3 手动指定链路速率

适用于虚拟接口、容器网卡或 speed 文件不可用的场景：

```bash
python3 nic_util.py \
  --interface eth0 \
  --speed-mbps 1000 \
  --output ./logs/nic_utilization.csv \
  --interval 1
```

### 6.4 追加写入已有结果文件

```bash
python3 nic_util_monitor.py \
  --interface eno1 \
  --output ./logs/nic_utilization.csv \
  --interval 1 \
  --append
```

默认行为是覆盖旧文件；只有加上 `--append` 才会追加。

## 7. 实时输出示例

脚本运行后会在终端持续打印采样结果，例如：

```text
[2026-04-07T09:30:01+08:00] iface=eno1 state=up duplex=full speed=1000Mbps rx=     12.350Mb/s (  1.235%) tx=      4.120Mb/s (  0.412%) agg=   0.824% summary=  1.235%
```

字段解释：

- `rx` / `tx`：当前采样区间上的平均接收、发送速率。
- `rx_util_pct` / `tx_util_pct`：接收、发送方向利用率。
- `agg`：聚合利用率。
- `summary`：推荐关注的单值摘要。

## 8. 输出文件格式

输出为 CSV，表头如下：

```text
timestamp,interface,operstate,duplex,link_speed_mbps,interval_seconds,rx_bytes_delta,tx_bytes_delta,rx_bps,tx_bps,total_bps,rx_util_pct,tx_util_pct,aggregate_util_pct,summary_util_pct
```

可直接用以下命令查看：

```bash
tail -f ./logs/nic_utilization.csv
```

也可以导入 pandas、Excel 或数据库做后续分析。

## 9. 压测验证方法

如果你希望验证脚本是否能正确反映链路负载，推荐使用 `iperf3`。

### 9.1 在另一台机器启动服务端

```bash
iperf3 -s
```

### 9.2 在当前机器发起发送压测

```bash
iperf3 -c <server_ip> -t 60
```

### 9.3 观察监控输出

此时：

- `tx_bps` 与 `tx_util_pct` 应显著上升。
- 若做反向压测 `iperf3 -c <server_ip> -R -t 60`，则 `rx_bps` 与 `rx_util_pct` 应显著上升。

## 10. 常见问题

### 10.1 为什么不直接把收发利用率相加再除以单个链路速率？

因为现代以太网通常是全双工。接收与发送各自拥有一套独立带宽预算，直接把双向流量相加再除以单个 `link_speed_bps`，会在双向同时繁忙时得到大于 100% 的结果，从而混淆物理含义。

### 10.2 为什么有时 `aggregate_util_pct` 为空？

当系统无法可靠识别 `duplex` 时，脚本不会伪造聚合分母，而是保留方向利用率，并以 `summary_util_pct = max(rx_util_pct, tx_util_pct)` 作为保守摘要。

### 10.3 为什么脚本提示计数器回退？

这通常表示：

- 网卡被重置；
- 驱动重新加载；
- 接口短暂 down/up；
- 虚拟化环境中统计口径发生切换。

脚本会跳过该采样点，随后继续监控。

## 11. 一条命令快速启动

如果你的网卡是 `eno1`，链路速率可从 sysfs 正常读取，最常用命令如下：

```bash
cd /Users/zdl/work/code/nic_util_monitor
python3 nic_util_monitor.py --interface eno1 --output ./logs/nic_utilization.csv --interval 1
```
