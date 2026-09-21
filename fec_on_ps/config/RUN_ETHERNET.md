# Running FEC-on-PS OVER ETHERNET (span65 + image rejection)

**Verified on hardware 2026-09-17:** the whole chain run over SSH/Ethernet,
`run_fecps.sh` = **1200/1200 codewords byte-exact (100.0%)**, PER 0.0000. neptunesdr
board (Zynq-7020 + AD9364), SMA cable TX↔RX, Ethernet cable board↔PC, span65
atomic-fix, this board's image-rejection coefficients.

---

## ⚠️ The single most important point: the link MUST run at 100 Mbps

The board's eth0 is **GEM0 on MIO** (not through the PL — the span65 bitstream is NOT
involved). But at **1 Gbps** the RX path is broken: the kernel binds the PHY with the
**"Generic PHY"** driver in `rgmii-id` mode, which does not program the PHY's RGMII skew
registers → RX samples off-timing → every RX frame is corrupted (`not whole frame pointed
by descriptor` + `macb ... DMA bus error: HRESP not OK`, rx_dropped climbing, **TX still
OK**). At **100 Mbps** the timing margin is wide and RX is clean (rx_dropped=0).

➡️ How to force it: have the **PC advertise 100M-only** → autoneg settles at **100 Mbps
Full**. This is packaged in `pc_eth_setup.sh`.

---

## Addresses / password

| | |
|---|---|
| PC NIC | `enp0s31f6` — static IP **192.168.200.1/24** |
| Board  | static IP **192.168.200.2/24** (fixed in `/etc/network/interfaces`) |
| SSH    | `root@192.168.200.2`, password **`root`** (the PC's public key is pre-installed) |

---

## Steps to re-run

### 1. On the PC — prepare the network (each time the PC reboots)
```sh
sudo sh config/pc_eth_setup.sh          # static IP + force 100M full
```
Expected: `Speed: 100Mb/s  Duplex: Full  Link detected: yes`.

### 2. Boot the board to Linux — ONLY this step needs UART
Power-cycle. The board stops at `zynq-uboot>` (qspiboot fails on its own). Type:
```
run uenvboot
```
→ loads span65 (cold fpga load) + full init → eth0 comes up at **100Mbps**, static IP
192.168.200.2, dropbear running. (Automatic autoboot needs a flash write — **blocked/unsafe**
on this hardware, so you still type this one line over UART.)

Check from the PC:
```sh
ping -c3 192.168.200.2
```

### 3. Run the whole FEC-on-PS flow — OVER ETHERNET
```sh
ssh root@192.168.200.2 'cd /root && sh run_fecps.sh'
```
Expected:
```
== 1. AD9361 + FPGA config (rf_loopback 15.36 MSPS, no initialize) ==
  DECODE-READY
== 2. image rejection (apply this board's coefficients) ==
image rejection: 0x79020414=0x3FFE0229 0x79020454=0x02254002 enb=0x00000251 qtrack=0
  RS codewords processed         : 1200
  codewords UNRECOVERABLE        : 0
  codewords BYTE-EXACT vs golden : 1200/1200  (100.0%)
  PER                            : 0.0000
  ★ 1200 codewords decoded on the ARM, not in the FPGA.
```

### 4. Quick re-run (no reconfiguration)
```sh
ssh root@192.168.200.2 'cd /root && ./fec_on_ps 40'
```

---

## Notes

- **Check RX is clean:** `ssh root@192.168.200.2 'dmesg | grep -c "HRESP not OK"; cat /sys/class/net/eth0/statistics/rx_dropped'` → both must be **0**. If either is non-zero → the link is running at 1Gbps, recheck step 1.
- **If you forget to force 100M:** the board will autoneg up to 1Gbps and RX dies (ping 100% loss even though the link is "up"). Re-run `pc_eth_setup.sh`.
- **Durable fix (optional, not yet done):** pin 100M on the board side by adding
  `max-speed = <100>;` to the ethernet node in `devicetree.dtb` (needs the SD in a PC to
  edit). Then it no longer depends on the PC advertising 100M, and plugging into a gigabit
  switch is still safe.
- **UART fallback:** if Ethernet is not up, log in over UART (ttyUSB1, 115200) `root`/`root`
  and run `sh /root/run_fecps.sh` as in `RUN_FECPS.md`.
