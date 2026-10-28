# Revision-4 protocol notes

Reference: dougg3/hd60s-linux-driver at
34621f7184b602ae7797d206ab2c9627cc045d1a. These observations guide this
implementation; they do not imply support for other hardware revisions.

- VID:PID 0fd9:0076. USB 3 SuperSpeed link observed at 5 Gbit/s.
- Interface 0 EP 0x83 IN: alternate 1/2/3 isochronous capacities of
  16384/32768/39936 bytes per service interval; alternate 4 bulk.
- Interface 1 EP 0x81 IN: interrupt, 64 bytes. Not opened by this implementation.
- Vendor requests use device recipient, 0x40 OUT or 0xc0 IN.
- C0 value 0064: MCU register file. Status index 0, length 32:
  vertical total LE16 at 4, horizontal total at 6, active height at 8,
  width at 10, rate at 12, flags at 13, audio kHz at 14.
- Mode flags: bit 0 interlaced; bit 1 fractional 1000/1001; bit 3 HDCP;
  bits 5:4 color matrix (1=709, 2=601).
- Registers: stream 10, range 12 (0=bypass), picture 13 (four neutral 80 bytes),
  audio source 3a (0=HDMI), HDMI audio gain 3b (80 neutral).
- C6 value 0 index 0100 disarms events. C2 value 0 configures stream pool;
  C7 value 0064 configures I2C. EC initialization packet and MCU/GPIO handshake
  follow the reference sequence. All commands are volatile, not flash writes.
- Start: disarm, enable stream, wait 105 ms, select alternate 4, queue reads.
- Stop: select alternate 0 before canceling reads, drain callbacks, wait 35 ms,
  disarm and disable stream. Ordering avoids filling FX3 DMA while EP0 waits.
- Data records have 4-byte headers: FF 00 00 XY then width*2 YUY2 bytes for
  each active OR blanking line. XY=80 active and AB blank for progressive input.
- FF 00 FF nn introduces nn*4 bytes of little-endian signed stereo 16-bit PCM.
  Records can cross USB read boundaries. Video payload is never marker-scanned.
- Frame start/end are blanking transitions. Incomplete frames are discarded.
  Audio is batched into approximately 10 ms blocks for OBS.
- Timing uses one monotonic epoch and counted video frames/audio samples.
  Long-term drift correction and discontinuity-aware audio recovery remain work.
