# CCSDS EYE demo

This demo uses two ESP32-S3-EYE boards to capture and exchange camera images
over acknowledged CCSDS CFDP. CFDP Space Packets are carried in bidirectional,
COP-1 flow-controlled USLP frames over Wi-Fi UDP to simulate a noisy,
intermittent, slow space-radio link. SDLS is not yet used.

Periodic peer-presence "ping" packets are sent while the link and CFDP service
are idle and the COP-1 Sent Queue is empty. CLCW feedback caused by transfer
traffic is immediate.

## Controls

Wait until both displays have registered to Wi-Fi and show `READY / PEER OK`.

- **SEND** (upper-left): capture a fresh image on this board and send it to the
  other board.
- **REQUEST** (lower-left): ask the other board to capture a fresh image and
  send it back.
- **SHOW** (upper-right): display the latest valid image, whether
  it was captured locally or received from the other board. Press again to
  return to the protocol view.
- **LINK** (lower-right): show link and recovery status. Press **BACK** to
  return.

`SHOW` reports `NO IMAGE` until an image is available. A failed transfer does
not replace the last valid image.

![ESP32-S3-EYE button locations and display layout](assets/eye-demo-guide.svg)

## Configure

Create the local configuration files:

```sh
cp conf/eye-1.example.conf conf/eye-1.conf
cp conf/eye-2.example.conf conf/eye-2.conf
```

Set both files to use:

- the same 2.4 GHz WPA2 network;
- two unused static IPv4 addresses outside the DHCP pool;
- the correct netmask and gateway;
- reciprocal peer addresses and UDP ports.

The example IP addresses must be replaced. The local configuration files are
ignored by Git so Wi-Fi credentials are not committed.

## Build and flash

Build both roles in the development container:

```sh
./build_both.sh
```

Flash the built images from the host:

```sh
./flash_both.sh /dev/ttyACM0 /dev/ttyACM1
```

The first device becomes EYE-1 and the second becomes EYE-2. Stable
`/dev/serial/by-id/...` paths are preferable when available. Install `esptool`
with `pipx install esptool` if it is not already present.

If the container can access both serial devices directly, build and flash in
one step:

```sh
./build_and_flash.sh /dev/ttyACM0 /dev/ttyACM1
```

## Try it

1. Press **SEND** on EYE-1. When EYE-2 reports successful reception, press
   **SHOW** on EYE-2 to see the captured EYE-1 image.
2. Repeat from EYE-2 to EYE-1.
3. Press **REQUEST** on EYE-1. EYE-2 captures an image and returns it; use
   **SHOW** on EYE-1 to view it.
4. Repeat the request in the opposite direction.

The progress bars show transferred image bytes. Reaching 100 percent means all
image data has arrived; CFDP checksum and completion confirmation follow as a
separate final step.

## Diagnostics

The **LINK** screen shows USLP/COP-1, link, and CFDP status and counters. More
detailed diagnostics are written to the serial log.

## Protocol Comparison & Tradeoff

COP-1 retransmits any lost or damaged USLP frames before CFDP needs to recover
missing file data at space packet level.

> **Note:** No channel-coding layer is implemented in this demo (no BCH, no R-S, 
> no FECF). A damaged UDP datagram will normally be rejected by the UDP/IP
> stack and therefore appear to COP-1 as a missing frame in the
> flow-controlled sequence. COP-1 will then retransmit it. The outcome is 
> similar to a frame that channel coding detected but could not correct.

CFDP validates the completed image with a file checksum. With
COP-1 enabled at USLP frame level, CFDP gap recovery will rarely (if ever) be
needed, because lost frames are recovered by COP-1.

A packet-only version, in which CFDP performs this recovery, is
preserved by the [`demo-cfdp-packet`](#tagged-demonstration-versions) tag
described below.

It's interesting to compare the two configurations:

- **demo-uslp-cop1** - lost packets & frames are automatically recovered by
  COP-1 during transmission. The sender can attempt a higher transmit rate
  because the receiver's capacity to absorb frames is enforced by
  COP-1 flow control. This can improve throughput, at the cost of feedback 
  of CLCW in returned frames, even if the return link is idle.
- **demo-cfdp-packet** - after splitting the file into packets, and sending,
  CFDP automatically requests to fill any "gaps" in the received file.
  This requires CFDP to maintain a record of any missing areas
  in the received file, which uses memory, especially if missing packets are highly dispersed.
  Furthermore, the sender must impose a higher, fixed, per-packet transmit
  delay to ensure it does not overwhelm the receiver. The value of this delay
  depends on arbitrary platform & link constraints.

Clearly, flow-controlled is preferable, although in the demo, it mostly "hides"
CFDP's ability to recover missing sections of a file.

## Initial Startup Approach

The demo COP-1 implementation tolerates an initial difference in Frame Sequence
Numbers between peers on first boot. Attempts to start sending in this state 
would result in Lockout. Preferably, we could avoid user intervention to reconcile
any differences automatically on first start.

On initial CLCW acquisition after boot, a board adopts the peer's CLCW Report Value 
and may send `BC_UNLOCK` if that peer is in Lockout state. 

A later lockout or retry exhaustion is reported as `LINK ERROR`. The **LINK**
screen provides manual unlock and sequence synchronization controls.

The demo does not automatically send `SET_VR`. `TRANSFER FAILED` is reserved for 
an image transfer that actually failed at CFDP level.

## Tagged demonstration versions

Two annotated Git tags preserve the working demonstrations at different
protocol layers:

- `demo-cfdp-packet`: CFDP Space Packets are carried directly in UDP
  datagrams, with missing file data recovered by CFDP.
- `demo-uslp-cop1`: CFDP Space Packets are carried in USLP Transfer Frames,
  with link loss normally recovered by COP-1 before CFDP observes a gap.

To build either version, check out its tag and use the same role build script:

```sh
git switch --detach demo-cfdp-packet
./build_both.sh
```

or:

```sh
git switch --detach demo-uslp-cop1
./build_both.sh
```

Return to current development with `git switch main`. The ignored local Wi-Fi
configuration files remain in the working directory when switching versions.
