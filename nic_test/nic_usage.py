#!/usr/bin/env python3
from bcc import BPF
import time
import socket
import subprocess
from collections import defaultdict

IFACES = ["ibp63s0"]          
READ_INTERVAL = 1.0        
BUCKET_NS = 1_000_000      

bpf_program = r"""
#include <uapi/linux/ptrace.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>

struct key_t {
    u32 ifindex;
    u64 bucket;
};

BPF_HASH(bytes, struct key_t, u64);

int handle_xmit(struct pt_regs *ctx, struct sk_buff *skb)
{
    if (skb == NULL) return 0;

    u32 len = 0;
    bpf_probe_read_kernel(&len, sizeof(len), &skb->len);

    struct net_device *dev = NULL;
    bpf_probe_read_kernel(&dev, sizeof(dev), &skb->dev);
    if (dev == NULL) return 0;

    u32 ifindex = 0;
    bpf_probe_read_kernel(&ifindex, sizeof(ifindex), &dev->ifindex);

    u64 now = bpf_ktime_get_ns();
    u64 bucket = now / """ + str(BUCKET_NS) + r""";

    struct key_t key = {};
    key.ifindex = ifindex;
    key.bucket = bucket;

    u64 v = (u64)len;
    u64 *old = bytes.lookup(&key);
    if (old) {
        __sync_fetch_and_add(old, v);
    } else {
        bytes.update(&key, &v);
    }
    return 0;
}
"""

def ifindex(name: str) -> int:
    return socket.if_nametoindex(name)

def get_speed_gbps(iface: str) -> float:
    out = subprocess.check_output(["ethtool", iface], stderr=subprocess.DEVNULL, text=True)
    for line in out.splitlines():
        if "Speed:" in line and "Mb/s" in line:
            mbps = int(line.split()[1].replace("Mb/s", ""))
            return mbps / 1000.0
    raise RuntimeError(f"Cannot determine link speed for {iface} (ethtool shows Unknown?)")

def pick_xmit_symbol():
    if BPF.get_kprobe_functions(b"__dev_queue_xmit"):
        return "__dev_queue_xmit"
    if BPF.get_kprobe_functions(b"dev_queue_xmit"):
        return "dev_queue_xmit"
    return None

def main():
    idx_to_name = {ifindex(n): n for n in IFACES}
    idx_to_speed = {ifindex(n): get_speed_gbps(n) for n in IFACES}

    sym = pick_xmit_symbol()
    if sym is None:
        raise RuntimeError("Cannot find __dev_queue_xmit or dev_queue_xmit to attach kprobe")

    b = BPF(text=bpf_program)
    b.attach_kprobe(event=sym, fn_name="handle_xmit")
    print(f"Attached kprobe to {sym}")

    print("bucket(ms) iface tx_Mbps util")

    while True:
        time.sleep(READ_INTERVAL)

        # bucket -> per-iface bytes
        agg = defaultdict(lambda: defaultdict(int))
        for k, v in b["bytes"].items():
            if k.ifindex in idx_to_name:
                agg[k.bucket][k.ifindex] += v.value

        for bucket in sorted(agg.keys()):
            for idx, byte_cnt in agg[bucket].items():
                tx_mbps = (byte_cnt * 8) / 1_000_000.0  
                speed_gbps = idx_to_speed[idx]
                util = tx_mbps / (speed_gbps * 1000.0)
                print(f"{bucket:>12d} {idx_to_name[idx]:>6s} {tx_mbps:>8.2f} {util:>6.3f}")

        b["bytes"].clear()

if __name__ == "__main__":
    main()
