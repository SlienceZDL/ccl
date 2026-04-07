#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import signal
import sys
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Optional


BITS_PER_BYTE = 8
MBPS_TO_BPS = 1_000_000
DEFAULT_INTERVAL = 1.0


class MonitorError(RuntimeError):
    pass


@dataclass(frozen=True)
class Sample:
    timestamp: str
    interface: str
    operstate: str
    duplex: str
    link_speed_mbps: int
    interval_seconds: float
    rx_bytes_delta: int
    tx_bytes_delta: int
    rx_bps: float
    tx_bps: float
    total_bps: float
    rx_util_pct: float
    tx_util_pct: float
    aggregate_util_pct: Optional[float]
    summary_util_pct: float


class NicUtilMonitor:
    def __init__(self, interface: str, speed_override_mbps: Optional[int]) -> None:
        self.interface = interface
        self.speed_override_mbps = speed_override_mbps
        self.iface_dir = Path("/sys/class/net") / interface
        self.stats_dir = self.iface_dir / "statistics"

        if not self.iface_dir.exists():
            raise MonitorError(
                f"接口 {interface!r} 不存在。请先执行 `ip -br link` 确认网卡名称。"
            )

    def read_counter(self, name: str) -> int:
        try:
            return int((self.stats_dir / name).read_text(encoding="utf-8").strip())
        except FileNotFoundError as exc:
            raise MonitorError(
                f"接口 {self.interface!r} 缺少统计项 {name!r}。当前系统可能不是标准 Linux sysfs 接口。"
            ) from exc
        except ValueError as exc:
            raise MonitorError(
                f"接口 {self.interface!r} 的统计项 {name!r} 不是有效整数。"
            ) from exc

    def read_text(self, name: str, default: str = "unknown") -> str:
        try:
            value = (self.iface_dir / name).read_text(encoding="utf-8").strip()
            return value or default
        except OSError:
            return default

    def get_operstate(self) -> str:
        return self.read_text("operstate")

    def get_duplex(self) -> str:
        duplex = self.read_text("duplex")
        return duplex if duplex in {"full", "half"} else "unknown"

    def get_speed_mbps(self) -> int:
        if self.speed_override_mbps is not None:
            return self.speed_override_mbps

        try:
            speed_text = (self.iface_dir / "speed").read_text(encoding="utf-8").strip()
        except OSError as exc:
            raise MonitorError(
                f"无法从 /sys/class/net/{self.interface}/speed 读取链路速率。"
                " 请使用 `--speed-mbps` 手动指定，例如 1000。"
            ) from exc

        try:
            speed_mbps = int(speed_text)
        except ValueError as exc:
            raise MonitorError(
                f"接口 {self.interface!r} 的 speed 值 {speed_text!r} 非法。"
            ) from exc

        if speed_mbps <= 0:
            raise MonitorError(
                f"接口 {self.interface!r} 的链路速率为 {speed_mbps} Mbps，无法计算利用率。"
                " 请改用物理网卡，或通过 `--speed-mbps` 手动指定链路速率。"
            )

        return speed_mbps

    def get_counters(self) -> tuple[int, int]:
        return self.read_counter("rx_bytes"), self.read_counter("tx_bytes")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="实时监控 Linux 网卡利用率，并将结果持续写入 CSV 文件。"
    )
    parser.add_argument(
        "-i",
        "--interface",
        default=None,
        help="网卡名称，例如 eno1、ens33。默认自动选择默认路由对应的接口。",
    )
    parser.add_argument(
        "-o",
        "--output",
        default="nic_utilization.csv",
        help="输出 CSV 文件路径，默认值为 nic_utilization.csv。",
    )
    parser.add_argument(
        "-t",
        "--interval",
        type=float,
        default=DEFAULT_INTERVAL,
        help="采样周期，单位秒，默认值为 1.0。",
    )
    parser.add_argument(
        "--speed-mbps",
        type=int,
        default=None,
        help="手动指定链路速率，单位 Mbps。当 /sys/class/net/<iface>/speed 不可用时使用。",
    )
    parser.add_argument(
        "--append",
        action="store_true",
        help="追加写入已有文件；默认覆盖旧文件。",
    )
    return parser.parse_args()


def pick_default_interface() -> str:
    route_file = Path("/proc/net/route")
    if not route_file.exists():
        raise MonitorError(
            "当前系统不存在 /proc/net/route，无法自动识别默认网卡。请显式传入 `--interface`。"
        )

    with route_file.open("r", encoding="utf-8") as handle:
        next(handle, None)
        for line in handle:
            fields = line.strip().split()
            if len(fields) < 11:
                continue
            iface, destination, _, flags = fields[:4]
            if destination != "00000000":
                continue
            if int(flags, 16) & 0x2:
                return iface

    raise MonitorError(
        "未发现默认路由对应的网卡。请先执行 `ip route show default` 检查网络，或显式传入 `--interface`。"
    )


def format_float(value: Optional[float]) -> str:
    if value is None:
        return ""
    return f"{value:.6f}"


def write_header_if_needed(writer: csv.DictWriter, file_exists: bool) -> None:
    if not file_exists:
        writer.writeheader()


def build_sample(
    interface: str,
    interval_seconds: float,
    rx_prev: int,
    tx_prev: int,
    rx_curr: int,
    tx_curr: int,
    speed_mbps: int,
    duplex: str,
    operstate: str,
) -> Sample:
    rx_delta = rx_curr - rx_prev
    tx_delta = tx_curr - tx_prev

    if rx_delta < 0 or tx_delta < 0:
        raise MonitorError(
            "检测到计数器回退，通常意味着网卡重置、驱动重载或计数器不连续。"
        )

    speed_bps = speed_mbps * MBPS_TO_BPS
    rx_bps = rx_delta * BITS_PER_BYTE / interval_seconds
    tx_bps = tx_delta * BITS_PER_BYTE / interval_seconds
    total_bps = rx_bps + tx_bps
    rx_util_pct = rx_bps / speed_bps * 100
    tx_util_pct = tx_bps / speed_bps * 100

    aggregate_util_pct: Optional[float]
    if duplex == "full":
        aggregate_util_pct = total_bps / (2 * speed_bps) * 100
        summary_util_pct = max(rx_util_pct, tx_util_pct)
    elif duplex == "half":
        aggregate_util_pct = total_bps / speed_bps * 100
        summary_util_pct = aggregate_util_pct
    else:
        aggregate_util_pct = None
        summary_util_pct = max(rx_util_pct, tx_util_pct)

    return Sample(
        timestamp=datetime.now().astimezone().isoformat(timespec="seconds"),
        interface=interface,
        operstate=operstate,
        duplex=duplex,
        link_speed_mbps=speed_mbps,
        interval_seconds=interval_seconds,
        rx_bytes_delta=rx_delta,
        tx_bytes_delta=tx_delta,
        rx_bps=rx_bps,
        tx_bps=tx_bps,
        total_bps=total_bps,
        rx_util_pct=rx_util_pct,
        tx_util_pct=tx_util_pct,
        aggregate_util_pct=aggregate_util_pct,
        summary_util_pct=summary_util_pct,
    )


def sample_to_row(sample: Sample) -> dict[str, str]:
    return {
        "timestamp": sample.timestamp,
        "interface": sample.interface,
        "operstate": sample.operstate,
        "duplex": sample.duplex,
        "link_speed_mbps": str(sample.link_speed_mbps),
        "interval_seconds": f"{sample.interval_seconds:.6f}",
        "rx_bytes_delta": str(sample.rx_bytes_delta),
        "tx_bytes_delta": str(sample.tx_bytes_delta),
        "rx_bps": format_float(sample.rx_bps),
        "tx_bps": format_float(sample.tx_bps),
        "total_bps": format_float(sample.total_bps),
        "rx_util_pct": format_float(sample.rx_util_pct),
        "tx_util_pct": format_float(sample.tx_util_pct),
        "aggregate_util_pct": format_float(sample.aggregate_util_pct),
        "summary_util_pct": format_float(sample.summary_util_pct),
    }


def print_sample(sample: Sample) -> None:
    aggregate = "N/A" if sample.aggregate_util_pct is None else f"{sample.aggregate_util_pct:8.3f}%"
    print(
        f"[{sample.timestamp}] "
        f"iface={sample.interface} state={sample.operstate} duplex={sample.duplex} "
        f"speed={sample.link_speed_mbps}Mbps "
        f"rx={sample.rx_bps / MBPS_TO_BPS:10.3f}Mb/s ({sample.rx_util_pct:7.3f}%) "
        f"tx={sample.tx_bps / MBPS_TO_BPS:10.3f}Mb/s ({sample.tx_util_pct:7.3f}%) "
        f"agg={aggregate} "
        f"summary={sample.summary_util_pct:7.3f}%"
    )


def ensure_output_parent(output_path: Path) -> None:
    if output_path.parent != Path("."):
        output_path.parent.mkdir(parents=True, exist_ok=True)


def main() -> int:
    args = parse_args()

    if args.interval <= 0:
        raise MonitorError("`--interval` 必须大于 0。")

    interface = args.interface or pick_default_interface()
    monitor = NicUtilMonitor(
        interface=interface,
        speed_override_mbps=args.speed_mbps,
    )
    output_path = Path(args.output).expanduser().resolve()
    ensure_output_parent(output_path)

    speed_mbps = monitor.get_speed_mbps()
    duplex = monitor.get_duplex()
    operstate = monitor.get_operstate()
    rx_prev, tx_prev = monitor.get_counters()
    t_prev = time.monotonic()

    print(f"开始监控接口 {interface}，输出文件: {output_path}")
    print(
        f"链路信息: speed={speed_mbps}Mbps duplex={duplex} operstate={operstate} interval={args.interval}s"
    )
    print("按 Ctrl+C 停止。")

    stop = False

    def handle_stop(signum: int, frame: object) -> None:
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT, handle_stop)
    signal.signal(signal.SIGTERM, handle_stop)

    file_exists = output_path.exists() and args.append
    mode = "a" if args.append else "w"
    fieldnames = [
        "timestamp",
        "interface",
        "operstate",
        "duplex",
        "link_speed_mbps",
        "interval_seconds",
        "rx_bytes_delta",
        "tx_bytes_delta",
        "rx_bps",
        "tx_bps",
        "total_bps",
        "rx_util_pct",
        "tx_util_pct",
        "aggregate_util_pct",
        "summary_util_pct",
    ]

    with output_path.open(mode, encoding="utf-8", newline="", buffering=1) as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        write_header_if_needed(writer, file_exists)

        while not stop:
            time.sleep(args.interval)

            rx_curr, tx_curr = monitor.get_counters()
            t_curr = time.monotonic()
            interval_seconds = t_curr - t_prev
            speed_mbps = monitor.get_speed_mbps()
            duplex = monitor.get_duplex()
            operstate = monitor.get_operstate()

            try:
                sample = build_sample(
                    interface=interface,
                    interval_seconds=interval_seconds,
                    rx_prev=rx_prev,
                    tx_prev=tx_prev,
                    rx_curr=rx_curr,
                    tx_curr=tx_curr,
                    speed_mbps=speed_mbps,
                    duplex=duplex,
                    operstate=operstate,
                )
            except MonitorError as exc:
                print(f"警告: {exc}；该采样点已跳过。", file=sys.stderr)
                rx_prev, tx_prev = rx_curr, tx_curr
                t_prev = t_curr
                continue

            writer.writerow(sample_to_row(sample))
            print_sample(sample)

            rx_prev, tx_prev = rx_curr, tx_curr
            t_prev = t_curr

    print("监控已停止。")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except MonitorError as exc:
        print(f"错误: {exc}", file=sys.stderr)
        raise SystemExit(1)
