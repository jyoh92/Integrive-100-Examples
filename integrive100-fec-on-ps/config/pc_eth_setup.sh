#!/bin/sh
# =============================================================================
#  pc_eth_setup.sh -- prepare the PC's Ethernet NIC to control the board.
#
#  Run on the PC (needs sudo).  Two things matter:
#    1. Static IP 192.168.200.1/24 on the direct cable (no DHCP server here).
#    2. FORCE 100 Mbps.  The board's eth0 (GEM0, MIO) works fine at 100 Mbps but
#       its RX path is broken at 1 Gbps: the kernel binds the PHY with the
#       "Generic PHY" driver in rgmii-id mode, which does NOT program the PHY's
#       RGMII RX skew, so at gigabit the RX sampling is mis-timed and every
#       received frame is corrupted ("not whole frame pointed by descriptor" +
#       "macb ... DMA bus error: HRESP not OK", rx_dropped climbing, TX still OK).
#       At 100 Mbps the timing margin is wide and RX is clean (rx_dropped=0).
#       Advertising 100M-only makes autoneg settle at 100 Mbps FULL duplex.
#
#  Usage:  sudo sh pc_eth_setup.sh [NIC]        # default NIC: enp0s31f6
# =============================================================================
set -e
NIC=${1:-enp0s31f6}

ip addr add 192.168.200.1/24 dev "$NIC" 2>/dev/null || true
ip link set "$NIC" up
# advertise 100baseT full+half only -> link negotiates 100 Mbps full duplex
ethtool -s "$NIC" autoneg on advertise 0x00c
sleep 5

echo "== $NIC now =="
ethtool "$NIC" | grep -iE 'speed|duplex|link detected'
ip -br addr show "$NIC"
echo "Ready.  Board is at 192.168.200.2 (ssh root@192.168.200.2, password: root)."
