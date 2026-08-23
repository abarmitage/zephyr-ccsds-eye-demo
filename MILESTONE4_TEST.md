# Milestone 4 two-board verification record

Status: **physical verification pending**. The software and both role images
build, but no physical-board result is recorded until the commands and counter
checks below are run with two ESP32-S3-EYE devices.

## Build record

From this repository:

```sh
west twister -T tests -p native_sim --inline-logs
./build_both.sh
./flash_both.sh /dev/serial/by-id/eye-1 /dev/serial/by-id/eye-2
```

Capture both serial streams to separate timestamped files. Do not rely on the
displayed image alone. Each boot must show reciprocal USLP identities and both
displays must reach `PEER OK` before testing.

Configured storage is four 1024-byte peer admission slots, four 1037-byte Sent
Queue slots, sixteen 1037-byte UDP ingress slots, and no deferred packet queue.
The linked role-A image reports 400 bytes for the peer instance, 4096 bytes for
peer packet storage, 4148 bytes for Sent Queue frame storage, 16704 bytes for
the ingress queue backing store, 4336 bytes for the UDP adapter instance, 3976
bytes for the CFDP service, and 8224 bytes for the existing router queue backing
store.
Configured stacks are 6144 bytes main, 7168 bytes protocol worker, 3072 bytes
UDP receive, and 3072 bytes system work queue. The initial route interval is
8 ms; it remains provisional until the measurements below pass.

The 2026-08-22 clean linked-size comparison against the pre-integration
revision used the same board configuration and workspace module:

| Role | Text | Text change | Data | Data change | BSS | BSS change |
|---|---:|---:|---:|---:|---:|---:|
| A | 582074 | +6860 | 24988 | 0 | 2814236 | +140080 |
| B | 582086 | +6872 | 24988 | 0 | 2814236 | +140080 |

Both role link maps use 712048 bytes of the FLASH region, 287840 bytes of
internal DRAM, and 1984096 bytes of external DRAM. Relative to the baseline,
those regions increased by 66248, 9008, and 65536 bytes respectively. Internal
DRAM is the tightest region at 87.98% used. Stack sizes are unchanged from the
baseline. Runtime high-water marks require the physical procedure below.

## Intact cases

For every case, require CFDP terminal `status=OK`, exact 115232-byte image
validation, exactly one destination commit, zero ingress overflow, zero route/
peer/UDP terminal errors, and no stale image-slot ownership.

1. Press SEND on EYE-1 and validate reception on EYE-2.
2. Press SEND on EYE-2 and validate reception on EYE-1.
3. Press REQUEST on EYE-1; validate EYE-2 capture and return.
4. Press REQUEST on EYE-2; validate EYE-1 capture and return.
5. Alternate directions for at least ten full transfers. This crosses many
   COP-1 windows and must not increase queue overflow or retain image slots.

Record for each role: peak ingress use, peak outstanding-window use, submitted
backpressure, CFDP NAK/retransmission counts, COP-1 retransmission/timeouts, and
terminal checksum/image status. Confirm that SHOW changes only after a valid
receive and that a deliberately failed transfer preserves the previous image.

## One lost packet-bearing frame

Add `CONFIG_EYE_DEMO_FAULT_DROP_DATA=y` only to the transmitting board's ignored
configuration, rebuild, flash both roles, and run SEND in that direction.

Require `injected=1`, COP-1 retransmissions greater than zero, one exact image
commit, no duplicate packet dispatch, and CFDP NAK/retransmission counts of
zero. Repeat with the roles reversed after moving the selection.

## One lost feedback-only frame

Add `CONFIG_EYE_DEMO_FAULT_DROP_FEEDBACK=y` only to the receiving board's
ignored configuration, rebuild, flash, and SEND toward that receiver.

Require `injected=1`; a later repeated CLCW must advance acknowledgements and
reopen progress. The transfer must validate once without ingress overflow.
Repeat in the opposite direction.

## Window closure

Add `CONFIG_EYE_DEMO_FAULT_WITHHOLD_FEEDBACK=y` only to the receiving board's
ignored configuration. The default discards 12 feedback-only frames and then
releases feedback.

Require the sender's peak outstanding value to equal K=4, with no fifth new
Type-AD frame before an accepted CLCW advances the window. The repeated CLCW
must then reopen the window and the image must validate once.

## Constrained-reception and pacing gate

Run repeated alternating transfers while introducing reproducible receiver
load appropriate to the lab setup. At 8 ms pacing, record:

| Role | Peak ingress / 16 | Peak outstanding / 4 | Overflow | COP-1 retx | CFDP NAK | Result |
|---|---:|---:|---:|---:|---:|---|
| EYE-1 | pending | pending | pending | pending | pending | pending |
| EYE-2 | pending | pending | pending | pending | pending | pending |

Accept 8 ms only when repeated full-image SEND and REQUEST cases in both
directions have zero overflow, stable peaks, bounded retransmission, and exact
single-commit validation. Otherwise change only
`CONFIG_EYE_DEMO_MINIMUM_TRANSMIT_INTERVAL_MS`, rebuild, and repeat.

## Completion decision

Do not mark milestone 4 complete in the module USLP plan until all physical
cases above pass and the measured table replaces every `pending` entry.
