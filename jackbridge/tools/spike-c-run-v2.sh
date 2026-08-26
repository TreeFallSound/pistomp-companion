#!/usr/bin/env bash
# Spike C latency matrix runner V2.
# Drives jackd on both Mac (local) and Pi (over ssh), measures round-trip
# latency with jack_iodelay across a period × netjack2-cycles matrix.
set -u

PI=pistomp@pistomp.local
SR=48000
# Using device name directly since it's default and unambiguous in this setup
MAC_DEV='AppleUSBAudioEngine:Yamaha Corporation:Steinberg UR22C:120000:1,2'
MAC_IP='192.168.99.1'
RESULTS=/tmp/spike-c-results.tsv
LOG=/tmp/spike-c.log
: > "$RESULTS"
: > "$LOG"

# Convergence sample window for jack_iodelay (seconds)
SAMPLE_SECS=15

log()   { echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }
ssh_pi(){ ssh -o BatchMode=yes -o ConnectTimeout=5 "$PI" "$@"; }

teardown() {
  log "teardown"
  pkill -9 -f jack_iodelay 2>/dev/null
  pkill -9 -f jackd 2>/dev/null
  ssh_pi 'pkill -9 -f jack_iodelay 2>/dev/null; pkill -9 -f jackd 2>/dev/null; true'
  sleep 2
}

final_restore() {
  teardown
  log "restarting pi-stomp services"
  ssh_pi 'sudo systemctl start jack mod-host mod-ui mod-ala-pi-stomp mod-amidithru' || true
}
trap final_restore EXIT

log "stopping pi-stomp services"
ssh_pi 'sudo systemctl stop mod-ala-pi-stomp mod-ui mod-host mod-amidithru jack'
teardown

run_one() {
  local period=$1 cycles=$2
  log "=== period=$period cycles=$cycles ==="
  local pi_log=/tmp/spike-c-pi-${period}-${cycles}.log
  local mac_log=/tmp/spike-c-mac-${period}-${cycles}.log
  local iod_log=/tmp/spike-c-iodelay-${period}-${cycles}.log

  # Mac jackd
  ( jackd -R -P 75 -d coreaudio -d "$MAC_DEV" -r $SR -p $period \
      >"$mac_log" 2>&1 ) &
  local MAC_JACK=$!
  sleep 3
  if ! kill -0 $MAC_JACK 2>/dev/null; then
    log "  FAIL: mac jackd died; tail:"; tail -5 "$mac_log" | sed 's/^/    /' | tee -a "$LOG"
    echo -e "${period}\t${cycles}\tFAIL_MAC_JACK\t-\t-\t-\t-" >> "$RESULTS"
    teardown; return
  fi
  jack_load netmanager >>"$mac_log" 2>&1
  sleep 1

  # Pi jackd
  # Using -l for cycles and -a for Mac IP
  ssh_pi "nohup jackd -R -P 75 -d alsa -d hw:0 -r $SR -p $period -n 2 \
            >$pi_log 2>&1 </dev/null &
          sleep 3
          jack_load netadapter -i '-a $MAC_IP -C 2 -P 2 -l $cycles' >>$pi_log 2>&1
          sleep 1
          jack_connect netadapter:capture_1 netadapter:playback_1 >>$pi_log 2>&1
          echo 'pi-ready'"
  sleep 3

  # Wait up to 10s for slave ports to appear on Mac
  local t=0 slave=""
  while [ $t -lt 20 ]; do
    slave=$(jack_lsp | grep ':to_slave_1$' | head -1 | cut -d: -f1)
    [ -n "$slave" ] && break
    sleep 0.5; t=$((t+1))
  done
  if [ -z "$slave" ]; then
    log "  FAIL: slave ports never appeared. mac jackd tail:"
    tail -10 "$mac_log" | sed 's/^/    /' | tee -a "$LOG"
    log "  pi jackd tail:"
    ssh_pi "tail -10 $pi_log" | sed 's/^/    /' | tee -a "$LOG"
    echo -e "${period}\t${cycles}\tFAIL_NO_SLAVE\t-\t-\t-\t-" >> "$RESULTS"
    teardown; return
  fi
  log "  Found slave: $slave"

  # Start jack_iodelay with line buffering
  ( /opt/homebrew/bin/gstdbuf -oL -eL jack_iodelay >"$iod_log" 2>&1 ) &
  local IOD=$!
  sleep 2
  jack_connect jack_delay:out "${slave}:to_slave_1" >>"$iod_log" 2>&1 || true
  jack_connect "${slave}:from_slave_1" jack_delay:in >>"$iod_log" 2>&1 || true

  log "  sampling for ${SAMPLE_SECS}s"
  sleep $SAMPLE_SECS

  kill $IOD 2>/dev/null
  wait $IOD 2>/dev/null

  # jack_iodelay lines look like:
  #    4145.967 frames    86.374 ms
  local last_line frames ms
  last_line=$(grep -E '^\s*[0-9.]+ frames' "$iod_log" | tail -1)
  if [ -z "$last_line" ]; then
    log "  FAIL: no iodelay output. tail:"; tail -10 "$iod_log" | sed 's/^/    /' | tee -a "$LOG"
    # Also check if it was "Signal below threshold"
    if grep -q "Signal below threshold" "$iod_log"; then
      log "  (Signal below threshold - check wiring/loopback)"
    fi
    echo -e "${period}\t${cycles}\tFAIL_NO_IODELAY\t-\t-\t-\t-" >> "$RESULTS"
    teardown; return
  fi
  frames=$(echo "$last_line" | awk '{print $1}')
  ms=$(echo "$last_line" | awk '{print $3}')

  # Count xruns observed in mac log during the run
  local mac_xruns pi_xruns
  mac_xruns=$(grep -ci 'xrun' "$mac_log" || true)
  pi_xruns=$(ssh_pi "grep -ci 'xrun' $pi_log" 2>/dev/null || echo 0)

  log "  result: ${frames} frames / ${ms} ms  (mac xruns=$mac_xruns pi xruns=$pi_xruns)"
  echo -e "${period}\t${cycles}\tOK\t${frames}\t${ms}\t${mac_xruns}\t${pi_xruns}" >> "$RESULTS"
  rm "$pi_log" "$mac_log" "$iod_log" 2>/dev/null || true
  teardown
}

# Run the matrix
for period in 128 256 512 1024; do
  for cycles in 1 2 3; do
    run_one $period $cycles
  done
done

log "DONE — results:"
printf "Period\tCycles\tStatus\tFrames\tms\tMacXrun\tPiXrun\n"
cat "$RESULTS" | tee -a "$LOG"
