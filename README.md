# CCSDS EYE demo

This demo uses two ESP32-S3-EYE boards to capture and exchange camera images
over acknowledged CCSDS CFDP. CFDP Space Packets are carried in bidirectional,
flow-controlled USLP frames over Wi-Fi UDP to simulate a noisy, intermittent,
slow space-radio link.

COP-1 retransmits lost USLP frames before CFDP needs to recover missing file
data. Retransmission details are written to the serial diagnostics; they do not
replace the main display status. On its initial acquisition after boot, a board
adopts the peer's clean CLCW Report Value and may send `BC_UNLOCK` if that peer
is locked. A later sequence mismatch, lockout, or terminal retry exhaustion is
reported as `LINK ERROR` and requires resetting both boards together. The demo
does not automatically send `SET_VR`. `TRANSFER FAILED` is reserved for an
image transfer that actually failed. SDLS is not enabled.

Periodic peer-presence packets are sent only while the link and CFDP service
are idle and the COP-1 Sent Queue is empty. CLCW feedback caused by transfer
traffic remains immediate.

## Controls

Wait until both displays have registered to Wi-Fi and show `READY / PEER OK`.

- **SEND** (upper-left): capture a fresh image on this board and send it to the
  other board.
- **REQUEST** (lower-left): ask the other board to capture a fresh image and
  send it back.
- **SHOW** (either right-side button): display the latest valid image, whether
  it was captured locally or received from the other board. Press again to
  return to the protocol view.

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
