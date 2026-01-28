#!/usr/bin/env python3
import time
from datetime import datetime

DEV = "mlx5_1"
PORT = 1
INTERVAL_S = 0.01
LOG = "ib_port_util.csv"

BASE = f"/sys/class/infiniband/{DEV}/ports/{PORT}"
TX_DATA = f"{BASE}/counters/port_xmit_data"
RX_DATA = f"{BASE}/counters/port_rcv_data"
RATE_FILE = f"{BASE}/rate"

def read_u64(path: str) -> int:
    with open(path, "r") as f:
        return int(f.read().strip())

def read_rate_gbps() -> float:
    s = open(RATE_FILE, "r").read().strip()
    num = ""
    for ch in s:
        if ch.isdigit() or ch == ".":
            num += ch
        elif num:
            break
    if not num:
        raise RuntimeError(f"Cannot parse rate from: {s}")
    return float(num)

def main():
    rate_gbps = read_rate_gbps()
    rate_bps = rate_gbps * 1e9

    prev_tx_dw = read_u64(TX_DATA)
    prev_rx_dw = read_u64(RX_DATA)

    with open(LOG, "w") as f:
        f.write("ts,bucket_ms,tx_mbps,rx_mbps,util\n")
        while True:
            t0 = time.monotonic_ns()
            time.sleep(INTERVAL_S)

            tx_dw = read_u64(TX_DATA)
            rx_dw = read_u64(RX_DATA)

            d_tx_bytes = (tx_dw - prev_tx_dw) * 4  # PortXmitData: octets/4
            d_rx_bytes = (rx_dw - prev_rx_dw) * 4  # PortRcvData: octets/4

            t1 = time.monotonic_ns()
            dt = (t1 - t0) / 1e9
            if dt <= 0:
                dt = INTERVAL_S

            tx_bps = d_tx_bytes * 8 / dt
            rx_bps = d_rx_bytes * 8 / dt

            tx_mbps = tx_bps / 1e6
            rx_mbps = rx_bps / 1e6
            util = (tx_bps + rx_bps) / rate_bps

            bucket_ms = t1 // 1_000_000
            ts = datetime.now().isoformat(timespec="milliseconds")
            f.write(f"{ts},{bucket_ms},{tx_mbps:.2f},{rx_mbps:.2f},{util:.6f}\n")
            f.flush()

            prev_tx_dw, prev_rx_dw = tx_dw, rx_dw

if __name__ == "__main__":
    main()
