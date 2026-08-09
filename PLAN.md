# CCSDS EYE Demo Plan

## Status

Proposed. Implementation has not started.

## Goal

Build a standalone Zephyr application for two ESP32-S3-EYE boards that makes
the current `zephyr-ccsds` CFDP implementation tangible and usable:

- button A captures a still image and transfers it to the peer with CFDP;
- button B sends a CCSDS TC capture request and receives the peer's image by
  CFDP;
- both displays show a small spacecraft-themed UI with real transfer progress;
- one outbound and one inbound CFDP transaction may operate concurrently,
  subject to hardware performance validation.

Usability is established before the complete camera and file-transfer paths.
Every protocol milestone drives the same on-device UI rather than postponing
the UI until integration is complete.

## Repository Boundary

The demo is a separate Zephyr application under `ccsds-eye-demo`. It consumes
`zephyr-ccsds` as a module but does not place demo code, board policy, UI,
camera handling, Wi-Fi credentials, or mission commands in that module.

One change is expected in `zephyr-ccsds`: extend the generic CFDP event
callback contract to report transfer progress. That change must remain
transport-neutral, board-neutral, and covered by module tests.

## Initial Technical Baseline

- Target: `esp32s3_eye/esp32s3/procpu` on the workspace's Zephyr 4.3 tree.
- Display: 240 x 240 ST7789 using LVGL.
- Camera: OV2640, initially 240 x 240 RGB565.
- Image payload: a small versioned header followed by 115,200 RGB565 bytes.
- Image storage: separate PSRAM-backed transmit and receive buffers.
- Photo delivery: acknowledged CFDP in CCSDS Space Packets.
- Initial bearer: one complete Space Packet per bounded UDP datagram over
  Wi-Fi, sized to avoid IP fragmentation.
- Remote capture request: a mission-specific CCSDS TC Space Packet on a
  dedicated APID, not a CFDP request file.
- Transfer frames and COP-1 are an explicit later decision gate. They are not
  silently implied by the initial packet-over-UDP demo.

## Demo Configuration

Keep one source tree and two non-secret role configurations:

| Setting | EYE A | EYE B |
| --- | --- | --- |
| Callsign | `EYE-1` | `EYE-2` |
| CFDP local entity | 1 | 2 |
| CFDP peer entity | 2 | 1 |
| Local UDP endpoint | role A address/port | role B address/port |
| Peer UDP endpoint | role B address/port | role A address/port |
| UI placement | left | right |

Wi-Fi credentials and site-specific addresses must be supplied through
git-ignored local configuration. Both screens must display their callsign,
entity ID, acquired IP address, and peer state so configuration errors are
visible without a debugger.

## UI Baseline

Keep the presentation deliberately small:

- main region: last captured or last verified received image;
- top strip: two small spacecraft icons, callsigns, and persistent `CFDP`;
- transfer activity: animated packet blocks moving in the real direction;
- bottom strip: physical-button action labels and one or two thin progress
  indicators;
- TC capture request: one amber `TC` block toward the peer;
- CFDP image data: cyan packet activity returning or leaving;
- terminal state: checksum result and a check or error icon.

The UI state model must cover boot, peer unavailable, idle, capturing, TC
request, transmitting, receiving, duplex transfer, verifying, complete,
busy, timeout, and failed. Animation may visualize sampled activity, but byte
progress and protocol state must come from real callbacks.

## Milestone 1: On-Device Usability Proof

Create the standalone application and make the UI usable before integrating
camera or CFDP data transfer.

### Work

- Add the application scaffold, base Kconfig, and reproducible build commands.
- Add separate EYE A and EYE B role configuration fragments.
- Bring up LVGL on the real 240 x 240 display.
- Implement the final UI layout and state model using scripted demo events.
- Show single-direction and duplex packet animations using synthetic progress.
- Add an input diagnostic view that displays every raw Zephyr input event.
- Determine which BOOT/ADC keys are reliably detected and map two suitable
  physical buttons to capture/send and remote capture.
- Show role, CFDP entity, IP configuration state, and peer state on screen.

### Acceptance

- Both role builds compile and boot on their intended boards.
- All intended buttons produce stable press and release events over at least
  50 presses per button on each board, or unreliable buttons are rejected.
- Both primary actions are understandable without a serial console.
- Scripted capture, TC, TX, RX, duplex, completion, and failure states are
  legible on the physical display with no overlaps.
- UI interaction remains responsive while both synthetic progress lanes run.

## Milestone 2: Two-Board Control And Configuration Slice

Establish real peer communication before adding camera memory pressure.

### Work

- Connect both roles to the same Wi-Fi network with reciprocal UDP endpoints.
- Add a bounded peer-presence/status exchange and drive the UI peer state.
- Register a dedicated demo command APID.
- Define a versioned `CAPTURE_AND_RETURN` TC payload containing a request ID
  and requesting CFDP entity ID.
- Send the TC from button B and queue it safely on the peer.
- Return a small command acceptance/status Space Packet so the requester can
  distinguish accepted, busy, invalid, and timed-out commands.
- Run all router callbacks without camera, LVGL, or synchronous CFDP reentry.

### Acceptance

- Swapping or duplicating role configuration is visibly diagnosed.
- Button B produces a visible TC animation, peer acceptance, and completion
  on both boards.
- An absent peer reaches a bounded timeout and returns the UI to a usable
  state.
- Repeated commands do not leak request state or trigger duplicate work.

## Milestone 3: Generic CFDP Progress Callback

Add the reusable monitoring needed by the real UI to `zephyr-ccsds`.

### Contract

Extend the existing CFDP event callback rather than adding a demo-specific
observer. The event data must provide, where applicable:

- transaction ID and sender/receiver direction;
- protocol phase;
- total file size;
- unique bytes sent or received;
- current segment offset and length;
- terminal status;
- explicit retransmission/NAK activity.

Append new event kinds rather than renumbering existing values. Document that
callbacks are synchronous, event storage is transient, callbacks must not
block or reenter the entity, and applications should copy events into their
own queue.

### Progress Semantics

- Progress is always within `0..file_size`.
- Initial sender progress increases after a File Data PDU is accepted by the
  Unitdata Transfer callback.
- Receiver progress is unique covered file data, not cumulative received
  bytes; duplicates and retransmissions cannot increase it twice.
- Retransmission activity is reported separately and cannot push progress
  above 100 percent.
- Completion remains distinct from 100 percent because EOF processing,
  checksum verification, Finished, and acknowledgement may remain.

### Acceptance

- Focused entity tests cover outgoing and incoming progress, out-of-order
  segments, duplicates, NAK recovery, retransmission, zero-length files,
  failure, and terminal ordering.
- Existing CFDP unit and two-peer UDP integration tests remain green.
- The demo can coalesce copied progress events to a fixed UI update cadence
  without reading internal entity slots.
- No Zephyr display, camera, network, or EYE-specific type enters the module
  API.

## Milestone 4: Camera And Image Presentation Slice

### Work

- Start from Zephyr's supported ESP32-S3-EYE video configuration.
- Allocate camera buffers in PSRAM and capture one 240 x 240 RGB565 still.
- Define buffer ownership across camera capture, LVGL display, CFDP sender
  retention, and retransmission.
- Display the captured still through the real UI.
- Exercise repeated capture from button A without networking transfer.
- On accepted button-B commands, capture on the peer and report capture status
  without yet returning the image.

### Acceptance

- Fifty sequential captures succeed on each board without memory growth,
  corrupt frames, or stale LVGL pointers.
- Wi-Fi peer presence remains stable while capture and display are active.
- Capture failure is visible and returns both actions to a valid state.

## Milestone 5: CFDP Image Transfer Slice

### Work

- Implement bounded PSRAM-backed CFDP source and destination filestore
  adapters in the demo.
- Transfer a known test image before connecting live camera output.
- Select the largest CFDP segment that keeps the encoded Space Packet inside
  one non-fragmented UDP datagram.
- Feed real progress events into the existing UI state model.
- Retain the previous image until size and checksum verification succeeds,
  then swap the displayed receive buffer atomically.
- Record data duration, acknowledged completion duration, UDP drops, NAKs,
  retransmitted bytes, and UI update latency.

### Acceptance

- Images match byte-for-byte and appear correctly on the peer.
- Progress is monotonic, reaches 100 percent exactly, and remains distinct
  from final verification.
- Packet loss is recovered within configured CFDP limits.
- A failed or corrupted transaction never replaces the last verified image.

## Milestone 6: Complete User Workflows

Connect the proven slices:

- button A: capture, preview, CFDP transmit, verified peer display;
- button B: TC request, peer capture, CFDP return, verified local display.

Queue camera and CFDP work outside network and UI callbacks. Define bounded
busy behavior for repeated local actions and duplicate TC request IDs.

### Acceptance

- A new user can execute both workflows using only the two displays and
  physical buttons.
- Both boards show the correct direction, actual progress, verification, and
  error recovery.
- Twenty alternating operations complete without reboot or manual cleanup.

## Milestone 7: Bidirectional Performance And Policy

Do not prohibit simultaneous transfer by assumption. Measure it using the
core's supported one-sender plus one-receiver concurrency on each device.

- Establish a single-direction baseline.
- Start equal-size image transfers in both directions simultaneously.
- Compare completion time, packet loss, NAKs, retransmission, PSRAM use,
  camera availability, and UI responsiveness.
- Repeat while injecting bounded UDP loss.
- Decide from results whether the released demo allows duplex image transfer,
  queues the second image, or applies bounded sender pacing.

Record the chosen policy and make the UI behavior deterministic. CFDP control
traffic and TC status must remain receivable even if large image transfers are
serialized.

## Milestone 8: Transfer-Frame Decision Gate

The usable packet-over-UDP demo is the baseline. After measuring it, decide
whether a second profile should carry commands and CFDP through TC/TM transfer
frames and COP-1 behavior.

Before approving that expansion, specify:

- whether the two boards have fixed ground/spacecraft roles or symmetric link
  instances;
- TC encoding and transmitting FOP responsibilities;
- TM parsing and CLCW return handling;
- how bidirectional peer behavior maps to TC uplink and TM downlink semantics;
- the additional performance and interoperability result the framed profile
  must demonstrate.

Track full asymmetric link work against the module's existing CFDP full-link
validation plan. Do not mix it into the initial usability milestones.

## Verification Strategy

- Host tests for the UI state reducer, TC payload codec, request deduplication,
  image header, and memory filestore.
- `zephyr-ccsds` unit tests for the progress callback contract.
- Native or simulated UI runs with scripted state sequences where practical.
- Compile verification for both EYE role configurations.
- On-target button, display, camera, Wi-Fi, single-transfer, duplex-transfer,
  loss-recovery, and soak checklists.
- Serial logs and counters support diagnosis but are not required to operate
  the demo.

## Non-Goals For The Initial Demo

- Adding demo UI, camera, board, or mission-command policy to `zephyr-ccsds`.
- Claiming RF, transfer-frame, COP-1, SDLS, or channel-coding behavior when the
  selected bearer is packet-over-UDP.
- Continuous camera preview or video streaming.
- JPEG decoding, persistent photo archives, discovery protocols, or dynamic
  multi-peer routing.
- More than one active CFDP sender and one active receiver per device.
