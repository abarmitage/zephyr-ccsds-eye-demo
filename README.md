# CCSDS EYE demo

This demo turns two ESP32-S3-EYE boards into a pair of small spacecraft. They
communicate over Wi-Fi at configured IP endpoints and exchange a test object
using CCSDS telecommands and acknowledged CFDP file transfer.

## Using the demo

Wait until both displays show `READY / PEER OK`. The labels at the bottom of
the screen align with the controls on the corresponding side:

- **Upper-left button — Send:** this board captures one fresh still, keeps it as
  its latest image, and sends the deterministic test object to the peer.
- **Lower-left button — Request:** this board sends a `CAPTURE_AND_RETURN`
  telecommand. The peer captures one fresh still and then sends its
  deterministic test object back to this board.
- **Either right-side button — Show:** displays the latest valid local capture
  full-screen. Press either right-side button again to return to the protocol
  view. Before the first successful capture it reports `NO IMAGE` without
  changing views.

In short, the upper-left control means **capture here, then send**; the
lower-left control means **ask the other board to capture, then send**. Camera
pixels are deliberately not the CFDP object in this version.

![ESP32-S3-EYE button locations and display layout](assets/eye-demo-guide.svg)

The display is egocentric: the board in your hand is always on the left and
its peer is on the right. Identity colours do not change between displays:
EYE-1 is cyan and EYE-2 is orange. The moving packet shows the actual direction
of the telecommand or file transfer. The diagram shows the view from EYE-1;
EYE-2 shows itself in orange on the left and EYE-1 in cyan on the right.

## Configure the two boards

Create local configuration files from the supplied templates:

```sh
cp conf/site-a.example.conf conf/site-a.conf
cp conf/site-b.example.conf conf/site-b.conf
```

Edit both files with:

- the same 2.4 GHz WPA2 SSID and password;
- two known-unused static IPv4 addresses outside the DHCP pool;
- the LAN netmask and gateway;
- reciprocal UDP ports: A's local port is B's peer port, and vice versa;
- reciprocal IPv4 endpoints: A's peer address is B's local address, and vice
  versa.

The boards use these fixed settings; they do not use DHCP, mDNS, or broadcast
discovery. The example addresses are documentation addresses and must be
replaced. The two site files are ignored by Git to avoid committing Wi-Fi
credentials.

## Build and flash

Build both firmware roles inside the development container:

```sh
./build_both.sh
```

The resulting images are available to the Fedora host through the bind-mounted
workspace. Install `esptool` on the host if needed:

```sh
pipx install esptool
```

Put both boards into their serial bootloaders, then flash the already-built
images from the host (replace the device paths if necessary):

```sh
./flash_both.sh /dev/ttyACM0 /dev/ttyACM1
```

Stable `/dev/serial/by-id/...` paths are preferable when available. The first
device receives the EYE-1 image and the second receives the EYE-2 image. The
scripts reject missing site files, missing images, and duplicate serial-device
paths.

If the container has reliable access to both serial devices, the combined
`build_and_flash.sh` script remains available.

## What happens during a transfer

Each board sends a small status packet once per second so that its configured
peer can become available. Pressing the upper-left control captures and
validates one 240 x 240 RGB565 still before starting an acknowledged CFDP
transfer. Pressing the lower-left control first sends a CCSDS telecommand to
the peer; acceptance makes the peer run the same capture-first operation before
the CFDP transfer in the opposite direction.
Capture briefly discards nine frames so the camera's automatic adjustments can
settle, then saves the tenth complete frame.
Capture failure preserves the previous valid image and does not start CFDP.

The test object is 1,536 bytes and is divided into several CFDP File Data PDUs.
The receiving board checks its version, declared size, contents, and checksum
before reporting completion. The thin TX and RX bars show real CFDP byte
progress copied from the generic module callback and refreshed at a bounded
cadence. Duplicate or retransmitted data does not inflate the RX bar, recovery
activity is labelled separately, and 100 percent remains distinct from final
checksum/Finished completion. A successful exchange ends with `TX DONE` on the
sender and `RX DONE` on the receiver. Errors and unavailable peers are reported
on the display.

SHOW changes only the active screen. Peer monitoring, command handling, CFDP
polling, transfer progress, and timeouts continue while the image fills the
display. Pressing either left-side control while an image is shown returns to
the protocol view before queuing that action. Raw press/release diagnostics
remain visible in the protocol view.

Each UDP datagram carries one bounded, unsegmented CCSDS Space Packet. The IP
and UDP connection is only the link used to carry the CCSDS packets; peer
status, telecommands, and CFDP traffic all use CCSDS packet formats.

## Quick check

With both boards showing `READY / PEER OK`:

1. Press the upper-left control on EYE-1. The animation should travel from
   EYE-1 to EYE-2, followed by successful TX and RX completion.
2. Press the lower-left control on EYE-1. A short telecommand animation should
   travel to EYE-2, followed by a file-transfer animation back to EYE-1.
3. Repeat both actions from EYE-2. The directions should reverse while the
   board in your hand remains on the left of its own display.
4. Press either right-side button to show the latest capture, press it again
   to return, then repeat using the other right-side button.

## Protocol tests

Run the focused native tests inside the development container:

```sh
west build -p always -b native_sim tests -d build-tests
./build-tests/zephyr/zephyr.exe
```

## On-device acceptance checklist

- On each board, complete 50 sequential upper-left captures with stable memory
  use and no corrupt image or stale display pointer.
- Trigger capture with the upper-left control on each board and with the
  lower-left control from each peer.
- Before any successful capture, press SHOW and confirm `NO IMAGE` remains on
  the protocol screen.
- After capture, confirm both right-side buttons show the 240 x 240 image and
  a second SHOW returns to the unchanged protocol view.
- From the image view, press both left-side controls separately; confirm the
  protocol view is restored before the requested operation starts.
- Force or simulate a camera failure and confirm the prior valid image remains
  available through SHOW and no placeholder CFDP send begins.
- Confirm every intended button still logs raw press and release diagnostics
  over at least 50 presses.
- While repeatedly capturing and leaving SHOW active, confirm peer presence
  remains stable and TC, CFDP progress, completion, and timeouts continue.
