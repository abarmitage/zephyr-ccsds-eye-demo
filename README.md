# zephyr-ccsds EYE demo

This standalone Zephyr application demonstrates symmetric acknowledged CFDP
and CCSDS telecommand exchange between two ESP32-S3-EYE boards. Button A sends a
fixed test object to the configured peer. Button B sends a versioned
`CAPTURE_AND_RETURN` TC; the peer accepts it and returns the same object through
the same worker-owned send operation.

Both builds run identical protocol behavior. The role fragments select only
callsigns and reciprocal CFDP entity identities. Each display always places
its local entity on the left and its peer on the right. Identity colors remain
stable across both displays: EYE-1 is cyan and EYE-2 is orange. CFDP packet
animation uses the source entity's identity color; the local/peer perspective
is conveyed by position rather than additional labels.

## Site configuration

Copy the documentation-only templates before building:

```sh
cp conf/site-a.example.conf conf/site-a.conf
cp conf/site-b.example.conf conf/site-b.conf
```

Edit both ignored files with:

- the same 2.4 GHz WPA2 SSID and password;
- two known-unused static IPv4 addresses reserved outside the DHCP pool;
- the LAN netmask and gateway;
- reciprocal UDP ports: A's local port is B's peer port and vice versa;
- reciprocal IPv4 endpoints: A's peer address is B's local address and vice
  versa.

The application does not start DHCP, mDNS, broadcast discovery, or any other
discovery protocol. The example addresses are from the RFC 5737 documentation
range and are not usable site settings. `conf/site-a.conf` and
`conf/site-b.conf` are git-ignored so credentials cannot be committed normally.

## Build and flash

From this directory, build both boards with their role and site fragments:

```sh
west build -p always -b esp32s3_eye/esp32s3/procpu . -d build-role-a -- \
  -DEXTRA_CONF_FILE='conf/role-a.conf;conf/site-a.conf'
west build -p always -b esp32s3_eye/esp32s3/procpu . -d build-role-b -- \
  -DEXTRA_CONF_FILE='conf/role-b.conf;conf/site-b.conf'
```

Put each board into its serial bootloader and replace the device paths as
needed:

```sh
west flash -d build-role-a --runner esp32 --esp-device /dev/ttyACM0
west flash -d build-role-b --runner esp32 --esp-device /dev/ttyACM1
```

To build both roles incrementally inside the development container, run:

```sh
./build_both.sh
```

The workspace is bind-mounted from the Fedora host, so the resulting images
are immediately available outside the container. Install `esptool` on the
host if necessary:

```sh
pipx install esptool
```

Then, from the host copy of this directory, flash both existing images without
restarting VS Code or granting the container access to the serial devices:

```sh
./flash_both.sh /dev/ttyACM0 /dev/ttyACM1
```

Stable `/dev/serial/by-id/...` paths are preferred when available. The build
script verifies both ignored site files. The flash script verifies both build
images and requires two distinct serial devices. The original
`build_and_flash.sh` remains available for environments where container USB
access works reliably.

Run the focused protocol tests with:

```sh
west build -p always -b native_sim tests -d build-tests
./build-tests/zephyr/zephyr.exe
```

## On-device behavior

Protocol services start only after Wi-Fi association and successful manual
IPv4 configuration. Each board unicasts a bounded status Space Packet once per
second. The peer is usable only when protocol version, callsign, entity IDs,
addresses, reciprocal ports, and APIDs agree. A missing peer expires after a
bounded timeout; duplicate entity identity and reciprocal configuration errors
are shown separately.

Every UDP datagram contains exactly one bounded, unsegmented CCSDS Space Packet.
The 1,536-byte versioned deterministic object is transferred in multiple CFDP
File Data PDUs and accepted only after its version, declared size, and every
byte have been verified.

The display keeps raw input diagnostics and shows the configured local IP and
entity ID, peer availability, real TC and CFDP directions, coarse packet
activity, verification, completion, timeout, busy, duplicate, invalid, and
failure states. It intentionally does not display byte percentages.

## Two-board acceptance checklist

1. Confirm each display shows the intended callsign, entity ID, local IP, and
   side placement.
2. Confirm both boards reach `READY / PEER OK`. Power one peer off and verify
   the other reaches `PEER UNAVAILABLE`, then recovers after it returns.
3. Flash the same role/site settings on both boards and confirm a visible peer
   identity or configuration error instead of `PEER OK`; then restore the
   reciprocal configurations.
4. Press and release Button A on each board. Check raw key diagnostics, CFDP
   direction, receive verification, and `CHECKSUM OK / COMPLETE` on each run.
5. Press and release Button B on each board. Check TC direction and acceptance,
   the reverse CFDP direction, verification, and completion.
6. Repeat a request ID using a packet-injection tool if available. Confirm the
   duplicate status is visible and no second CFDP transaction starts.
7. Start an outbound transfer and issue another request during it. Confirm the
   bounded busy response and that both boards remain usable afterward.
8. Remove or block the peer during a request and confirm bounded timeout or
   failure presentation and recovery after connectivity returns.
9. Exercise Buttons A and B repeatedly on both boards while watching for input,
   UI, queue, socket, or CFDP failures on the serial console.

This checklist requires two physical boards and is not satisfied by compilation
or native tests alone.
