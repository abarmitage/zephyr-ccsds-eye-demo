# CCSDS EYE demo

This demo turns two ESP32-S3-EYE boards into a pair of small spacecraft. They
communicate over Wi-Fi at configured IP endpoints and exchange a test object
using CCSDS telecommands and acknowledged CFDP file transfer.

## Using the demo

Wait until both displays show `READY / PEER OK`. The two buttons used by the
demo are on the **left side of the display**:

- **Button A — Send:** this board sends its test object directly to the peer.
- **Button B — Request:** this board sends a `CAPTURE_AND_RETURN` telecommand.
  The peer responds by sending its test object back to this board.

In short, A means **send from here**; B means **ask the other board to send**.

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
peer can become available. Pressing A starts an acknowledged CFDP transfer
immediately. Pressing B first sends a CCSDS telecommand to the peer; acceptance
of that command starts the CFDP transfer in the opposite direction.

The test object is 1,536 bytes and is divided into several CFDP File Data PDUs.
The receiving board checks its version, declared size, contents, and checksum
before reporting completion. A successful exchange ends with `TX DONE` on the
sender and `RX DONE` on the receiver. Errors and unavailable peers are reported
on the display.

Each UDP datagram carries one bounded, unsegmented CCSDS Space Packet. The IP
and UDP connection is only the link used to carry the CCSDS packets; peer
status, telecommands, and CFDP traffic all use CCSDS packet formats.

## Quick check

With both boards showing `READY / PEER OK`:

1. Press A on EYE-1. The animation should travel from EYE-1 to EYE-2, followed
   by successful TX and RX completion.
2. Press B on EYE-1. A short telecommand animation should travel to EYE-2,
   followed by a file-transfer animation back to EYE-1.
3. Repeat both actions from EYE-2. The directions should reverse while the
   board in your hand remains on the left of its own display.

## Protocol tests

Run the focused native tests inside the development container:

```sh
west build -p always -b native_sim tests -d build-tests
./build-tests/zephyr/zephyr.exe
```
