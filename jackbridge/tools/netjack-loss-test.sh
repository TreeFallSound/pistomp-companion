#!/usr/bin/env bash
# Paired netjack UDP capture from pi + mac, then per-side stats:
#   total packets, gap-detected drops, out-of-order, inter-arrival jitter.
#
# Usage:  tools/netjack-loss-test.sh [duration_seconds]
# Env overrides: PI_HOST, PI_IFACE, MAC_IFACE, PI_IP, MAC_IP, NETJACK_PORT
set -euo pipefail

DURATION=${1:-30}
PI_HOST=${PI_HOST:-pistomp@pistomp.local}
PI_IFACE=${PI_IFACE:-end0}
MAC_IFACE=${MAC_IFACE:-en7}
PI_IP=${PI_IP:-169.254.161.200}
MAC_IP=${MAC_IP:-169.254.56.24}
NETJACK_PORT=${NETJACK_PORT:-19000}

OUTDIR=$(mktemp -d -t netjack-loss)
echo "out: $OUTDIR"
echo "iface pi=$PI_IFACE mac=$MAC_IFACE  duration=${DURATION}s"

FILTER="udp and host $PI_IP and host $MAC_IP"

# Start pi capture (background on remote).
ssh "$PI_HOST" "sudo rm -f /tmp/netjack-pi.pcap; \
                sudo nohup tcpdump -i $PI_IFACE -nn -s 96 -w /tmp/netjack-pi.pcap '$FILTER' \
                  >/tmp/netjack-pi.log 2>&1 & echo \$! >/tmp/netjack-pi.pid"

# Start mac capture (background local).
sudo tcpdump -i "$MAC_IFACE" -nn -s 96 -w "$OUTDIR/mac.pcap" "$FILTER" >/dev/null 2>&1 &
MAC_PID=$!

# Run for DURATION. Use a sentinel so we kill cleanly even on Ctrl-C.
trap 'sudo kill "$MAC_PID" 2>/dev/null || true; \
      ssh "$PI_HOST" "sudo kill \$(cat /tmp/netjack-pi.pid)" 2>/dev/null || true' EXIT

sleep "$DURATION"

# Stop both, wait briefly for flush.
sudo kill "$MAC_PID" 2>/dev/null || true
ssh "$PI_HOST" 'sudo kill $(cat /tmp/netjack-pi.pid)' || true
sleep 1

# Pull pi pcap.
scp -q "$PI_HOST:/tmp/netjack-pi.pcap" "$OUTDIR/pi.pcap"

echo
echo "=== analysis ==="
python3 - "$OUTDIR/pi.pcap" "$OUTDIR/mac.pcap" "$PI_IP" "$MAC_IP" <<'PYEOF'
import struct, sys
from collections import defaultdict

pi_pcap, mac_pcap, pi_ip, mac_ip = sys.argv[1:5]

def pcap_iter(path):
    """Yield (ts_us, src_ip, dst_ip, sport, dport, payload) for UDP/IPv4."""
    with open(path, 'rb') as f:
        gh = f.read(24)
        if len(gh) < 24: return
        magic = struct.unpack('<I', gh[:4])[0]
        if magic == 0xa1b2c3d4: end, nano = '<', False
        elif magic == 0xd4c3b2a1: end, nano = '>', False
        elif magic == 0xa1b23c4d: end, nano = '<', True
        elif magic == 0x4d3cb2a1: end, nano = '>', True
        else:
            print(f"unknown pcap magic {magic:#x}", file=sys.stderr); return
        linktype = struct.unpack(end+'I', gh[20:24])[0]
        # 1=Ethernet (14B), 0=null (4B), 12=raw IP (0B)
        l2 = {1: 14, 0: 4, 12: 0}.get(linktype, 14)
        while True:
            rec = f.read(16)
            if len(rec) < 16: return
            ts_s, ts_sub, caplen, _ = struct.unpack(end+'IIII', rec)
            ts_us = ts_s * 1_000_000 + (ts_sub // 1000 if nano else ts_sub)
            pkt = f.read(caplen)
            if len(pkt) < caplen: return
            if l2 == 14:
                if struct.unpack('>H', pkt[12:14])[0] != 0x0800: continue
            ip = pkt[l2:]
            if len(ip) < 20 or (ip[0] >> 4) != 4 or ip[9] != 17: continue
            ihl = (ip[0] & 0xf) * 4
            src = '.'.join(str(b) for b in ip[12:16])
            dst = '.'.join(str(b) for b in ip[16:20])
            udp = ip[ihl:]
            if len(udp) < 8: continue
            sport, dport, ulen, _ = struct.unpack('>HHHH', udp[:8])
            yield (ts_us, src, dst, sport, dport, udp[8:ulen] if ulen >= 8 else udp[8:])

def parse_netjack(payload):
    """Returns (pkt_type, data_type, data_stream, cycle, subcycle, is_last) or None.
    packet_header_t over the wire (each char field is sent as a uint32 with
    only the low byte populated, due to jack2's htonl roundtrip):
      [0:8]   fPacketType char[8]   ('header' / 'follow' / ...)
      [8:12]  fDataType   uint32    low byte = 's' sync, 'a' audio, 'm' midi
      [12:16] fDataStream uint32    low byte = 's' send, 'r' return
      [16:20] fID
      [20:24] fNumPacket
      [24:28] fPacketSize
      [28:32] fActivePorts
      [32:36] fCycle
      [36:40] fSubCycle
      [40:44] fIsLastPckt
      [44:48] fFrames
    """
    if len(payload) < 48: return None
    pt = payload[:8].rstrip(b'\x00').decode('ascii', 'replace')
    fields = struct.unpack('>10I', payload[8:48])
    data_type   = chr(fields[0] & 0xff)
    data_stream = chr(fields[1] & 0xff)
    cycle       = fields[6]
    subcycle    = fields[7]
    is_last     = fields[8]
    return (pt, data_type, data_stream, cycle, subcycle, is_last)

def summarize(path, label):
    print(f"\n--- {label}: {path} ---")
    # Group by (src, dst, data_stream) — 's' send and 'r' return are
    # independent streams each with their own cycle counter.
    streams = defaultdict(list)  # key -> [(ts_us, cycle, subcycle, data_type, is_last)]
    non_netjack = 0
    for ts, src, dst, sport, dport, payload in pcap_iter(path):
        hdr = parse_netjack(payload)
        if hdr is None:
            non_netjack += 1
            continue
        pt, data_type, data_stream, cycle, subcycle, is_last = hdr
        streams[(src, dst, data_stream)].append(
            (ts, cycle, subcycle, data_type, is_last))
    if non_netjack:
        print(f"  ({non_netjack} non-netjack UDP packets ignored)")
    for key, pkts in sorted(streams.items()):
        src, dst, ds = key
        pkts.sort()
        cycles = [c for _, c, _, _, _ in pkts]
        uniq_cycles = sorted(set(cycles))
        first, last = uniq_cycles[0], uniq_cycles[-1]
        expected = last - first + 1
        missing = expected - len(uniq_cycles)
        gap_runs = sum(1 for a, b in zip(uniq_cycles, uniq_cycles[1:]) if b - a > 1)
        ooo = sum(1 for a, b in zip(cycles, cycles[1:]) if b < a)
        ts_list = [t for t, _, _, _, _ in pkts]
        deltas = sorted(ts_list[i + 1] - ts_list[i] for i in range(len(ts_list) - 1))
        n = len(deltas)
        p50 = deltas[n // 2] if n else 0
        p99 = deltas[min(n - 1, n * 99 // 100)] if n else 0
        mx = deltas[-1] if n else 0
        # Subcycles-per-cycle: each cycle should produce a fixed number of
        # data subpackets. Cycles with fewer = partial delivery (also a click).
        by_cycle = defaultdict(set)
        for _, c, sc, _, _ in pkts:
            by_cycle[c].add(sc)
        sc_hist = defaultdict(int)
        for scs in by_cycle.values():
            sc_hist[len(scs)] += 1
        mode = max(sc_hist, key=sc_hist.get) if sc_hist else 0
        partial = sum(v for k, v in sc_hist.items() if k != mode)
        print(f"  {src} -> {dst}  stream='{ds}':")
        print(f"    packets={len(pkts)}  unique cycles={len(uniq_cycles)} "
              f"(range {first}..{last}, expected {expected})")
        print(f"    missing cycles={missing} across {gap_runs} gaps; "
              f"out-of-order arrivals={ooo}")
        print(f"    subcycles-per-cycle: {dict(sc_hist)}  partial cycles={partial}")
        print(f"    inter-arrival us: n={n} p50={p50} p99={p99} max={mx}")

summarize(pi_pcap, f"PI capture (on pi {pi_ip})")
summarize(mac_pcap, f"MAC capture (on mac {mac_ip})")

# Cross-side: for each direction, what arrived at the other side?
print("\n--- cross-side reconciliation ---")
def load(path):
    seen = defaultdict(set)  # (src, dst, stream) -> {(cycle, subcycle)}
    for ts, src, dst, sport, dport, payload in pcap_iter(path):
        hdr = parse_netjack(payload)
        if hdr is None: continue
        pt, dt, ds, c, sc, _ = hdr
        seen[(src, dst, ds)].add((c, sc))
    return seen

pi_seen = load(pi_pcap)
mac_seen = load(mac_pcap)
all_dirs = set(pi_seen.keys()) | set(mac_seen.keys())
for d in sorted(all_dirs):
    src, dst, ds = d
    p = pi_seen.get(d, set())
    m = mac_seen.get(d, set())
    if not p and not m: continue
    if not p or not m:
        print(f"  {src} -> {dst} stream='{ds}': only one side (pi:{len(p)} mac:{len(m)})")
        continue
    p_cycles = {c for c, _ in p}; m_cycles = {c for c, _ in m}
    lo = max(min(p_cycles), min(m_cycles))
    hi = min(max(p_cycles), max(m_cycles))
    p_in = {(c, sc) for c, sc in p if lo <= c <= hi}
    m_in = {(c, sc) for c, sc in m if lo <= c <= hi}
    pi_only  = p_in - m_in    # in pi capture only
    mac_only = m_in - p_in    # in mac capture only
    sender_is_pi = (src == pi_ip)
    sender_cap_only   = pi_only  if sender_is_pi else mac_only
    receiver_cap_only = mac_only if sender_is_pi else pi_only
    print(f"  {src} -> {dst} stream='{ds}' (cycle window {lo}..{hi}):")
    print(f"    pi-cap: {len(p_in)}   mac-cap: {len(m_in)}")
    print(f"    seen by sender-cap but NOT receiver-cap (= lost on link): {len(sender_cap_only)}")
    print(f"    seen by receiver-cap but NOT sender-cap (capture artifact): {len(receiver_cap_only)}")
PYEOF

echo
echo "pcaps left in: $OUTDIR  (delete when done)"
