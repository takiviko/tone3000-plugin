# Zoom AMS-22: descriptor and entity-graph notes (plan M1.3)

Device under test for the low-latency USB driver. Facts marked **confirmed**
come from `dumpsys usb` on the Galaxy Tab S10 FE (2026-10-17) and the probe
app's enumeration; facts marked **pending** come from the raw descriptor
dump (`am start -e mode dump`), which needs the tablet unlocked for the USB
permission dialog. When that lands, save the blob as
`plugin/usb/test/fixtures/ams22-descriptors.bin` and replace the pending
rows.

## Device identity (confirmed)

| Property | Value |
| --- | --- |
| VID:PID | `0x1686:0x066F` (5766:1647) |
| Strings | ZOOM Corporation / AMS-22 |
| Device class | 0xEF / 0x02 / 0x01 (Misc, IAD) |
| USB speed | USB 2.0 high speed |
| Config | value 1, bus-powered, bMaxPower 500 (1000 mA at 2 mA units) |

## Interfaces (confirmed, from `dumpsys usb` and the probe's enumeration)

| Iface | Subclass / protocol | Role | Endpoints |
| --- | --- | --- | --- |
| 0 | AudioControl 1/1/32 (UAC2) | entity graph | none |
| 1 | AudioStreaming 1/2/32 | **playback** | alt 0 zero-bandwidth; alt 1: `0x02` isoc OUT (async data, bmAttributes 0x05, 104 B/microframe, interval 1) + `0x82` isoc **explicit feedback** (bmAttributes 0x11, 4 B, interval 4) |
| 2 | AudioStreaming 1/2/32 | **capture** | alt 0 zero-bandwidth; alt 1: `0x81` isoc IN (async data, bmAttributes 0x05, 104 B/microframe, interval 1) |
| 3 | AudioControl 1/1/0 | second AC block (protocol 0) | none |
| 4 | MIDIStreaming 1/3/0 | MIDI (out of scope) | `0x03` / `0x84` bulk 512 B |

- **Feedback is explicit**: endpoint `0x82`, isochronous, usage type 01
  (feedback). UAC2 feedback values are 16.16 (verify the format on first
  readback; the plan's ClockModel::parseFeedback handles both).
- Interface 4 (MIDI) is bulk: leaving it bound to `snd-usb-audio` costs no
  isochronous bandwidth. Interface 3 is never claimed. Both are in the quirk
  registry (`plugin/usb/UsbQuirks.cpp`).
- `max_packet_size` 104 B/microframe covers 96 kHz / 2 ch / subslot 3
  (72 B needed) and 48 kHz with room to spare; 125 us packet cadence
  (`interval=1`) = 8 packets per ms at high speed.

## Pending: raw descriptor dump

- UAC2 clock source ID and its bmControls (the rate knob's entity id).
- Subslot size (3 packed vs 4-with-24-significant) and bBitResolution.
- The full AC entity graph: input/output terminals, feature unit wiring.
- The AS Type I format descriptors (channels, per-rate info if any).
- Altsettings for 44.1 / 48 / 88.2 / 96 kHz: UAC2 usually keeps one active
  altsetting per direction and switches rate on the clock source, but the
  Type I descriptor will confirm; `GET_RANGE` on SAM_FREQ_CONTROL lists the
  supported rates.

## Kernel driver state before claiming (M1.3 checklist)

`USBDEVFS_GETDRIVER` per interface runs before `claimInterface(force=true)`
and logs what held each interface (`usb: iface N kernel driver='...'` in
the probe log). Fill this table from the first dump run:

| Iface | Kernel driver before claim |
| --- | --- |
| 0 | pending |
| 1 | pending |
| 2 | pending |
| 3 | pending (leave bound) |
| 4 | pending (leave bound) |
