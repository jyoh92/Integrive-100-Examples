#!/usr/bin/env bash
# =============================================================================
#  board_bringup_from_pc.sh -- push the config to the board and bring it up,
#                              driven entirely over SSH from the PC.
#
#  Works even when the board's SD rootfs is mounted READ-ONLY (a corrupted
#  ext4 remounts itself ro): every file is copied to /tmp, which is tmpfs, and
#  the scripts are run from there.  Nothing is written to the SD.
#
#      ./board_bringup_from_pc.sh [IP] [TX_dBm] [RX_dB]
#      IP default 192.168.200.2, TX default -27, RX default 31 (cable loopback).
#
#  Requires SSH access to root@IP (key or agent).  The board's rootfs must
#  still hold /root/fint2.ftr readable; if that inode is corrupt too, this
#  script ships its own copy of fint2.ftr from the repo.
# =============================================================================
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
IP=${1:-192.168.200.2}
TX=${2:--27}
RX=${3:-31}
SSHOPT="-o ConnectTimeout=8 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ServerAliveInterval=15"
SSH="ssh $SSHOPT root@$IP"

echo "== push config -> /tmp on $IP =="
for f in ad9361_autoconfig.sh rf_setup.sh bringup.sh fint2.ftr; do
    $SSH "cat > /tmp/$f && chmod +x /tmp/$f" < "$HERE/$f"
    echo "  /tmp/$f"
done

echo "== run bring-up on board =="
$SSH "cd /tmp && sh bringup.sh $TX $RX"
