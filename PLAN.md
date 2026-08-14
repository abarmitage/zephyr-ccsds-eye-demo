# CCSDS EYE Demo Plan

## Status

Milestone 1's application scaffold and scripted UI are implemented. Remaining
milestones are proposed until their acceptance checks are completed on both
boards.

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
| UI perspective | local left | local left |

Wi-Fi credentials, static IPv4 addresses, netmask, gateway, and UDP ports must
be compiled from git-ignored per-board site configuration. Addresses must be
known unused and excluded or reserved from the LAN's DHCP pool; the demo does
not run a DHCP client or a peer-discovery protocol. Both screens must display
their callsign, entity ID, configured local address, and peer state so
configuration errors are visible without a debugger.

## UI Baseline

Keep the normal protocol presentation deliberately small:

- top strip: local spacecraft on the left, remote spacecraft on the right,
  their callsigns, stable EYE-1 cyan/EYE-2 orange identity colors, and
  persistent `CFDP`; position conveys local/peer perspective without literal
  `ME` or `PEER` labels;
- transfer activity: animated packet blocks moving in the real direction;
- bottom strip: physical-button action labels and one or two thin progress
  indicators;
- TC capture request: one amber `TC` block toward the peer;
- CFDP image data: cyan packet activity returning or leaving;
- terminal state: checksum result and a check or error icon.

A separate image view shows the latest valid local capture or verified
received image full-screen, with no protocol animation, thumbnail, or status
overlay.

The UI state model must cover boot, peer unavailable, idle, capturing, TC
request, transmitting, receiving, duplex transfer, verifying, complete,
busy, timeout, failed, and full-screen image display. Animation may visualize
sampled activity, but byte progress and protocol state must come from real
callbacks. Switching to image view changes presentation only; networking,
CFDP, peer monitoring, command handling, and timeouts continue to run.

## Final Button And Image Interaction

Keep the camera workflow deliberately direct:

- button A captures one fresh still and sends that capture to the configured
  peer with acknowledged CFDP;
- button B sends `CAPTURE_AND_RETURN`; an accepting peer captures one fresh
  still and returns it through the same capture and send operations;
- the reliable right-side button, or both right-side buttons as aliases when
  both are usable, toggles `SHOW`;
- `SHOW` replaces the protocol view with the latest valid image at full-screen;
  pressing `SHOW` again returns to the unchanged protocol view;
- pressing A or B from image view returns to the protocol view before starting
  the requested operation;
- `SHOW` with no valid image reports `NO IMAGE` without leaving the protocol
  view.

There is one logical latest valid image. A successful local capture may become
latest immediately. Incoming data may become latest only after CFDP and image
object verification complete. Separate bounded transmit, receive-staging, and
display ownership may still be required internally so camera capture, LVGL,
and acknowledged retransmission cannot overwrite one another. Failure must
preserve the previous valid image.

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
- Determine which BOOT/ADC keys are reliably detected. Map A to capture/send,
  B to remote capture, and the reliable right-side key (or both as aliases) to
  SHOW.
- Show role, CFDP entity, IP configuration state, and peer state on screen.

### Acceptance

- Both role builds compile and boot on their intended boards.
- All intended buttons produce stable press and release events over at least
  50 presses per button on each board, or unreliable buttons are rejected.
- Both primary actions are understandable without a serial console.
- Scripted capture, TC, TX, RX, duplex, completion, and failure states are
  legible on the physical display with no overlaps.
- UI interaction remains responsive while both synthetic progress lanes run.

## Milestone 2: Symmetric Two-Board CFDP And Control Slice

Establish real peer communication and both user actions before adding camera
memory pressure. EYE A and EYE B are equivalent peers; their role
configurations provide identities and reciprocal endpoints, not different
behavior.

Both triggers converge on one queued operation that sends an object to a
destination CFDP entity. Button A queues it locally. A valid
`CAPTURE_AND_RETURN` TC from button B queues the same operation on the peer,
with request metadata used only for validation, acknowledgement, destination
selection, and UI context. Until camera integration, a fixed versioned test
object is the operation's source.

### Work

- Connect both roles to the same Wi-Fi network using the compiled static IPv4
  settings and reciprocal UDP endpoints. Do not start DHCP or discovery.
- Add a bounded unicast peer-presence/status exchange and drive the UI peer
  state.
- Configure the packet profile, APID router, UDP bearer, and acknowledged CFDP
  service identically on both peers.
- Add bounded in-memory source and destination adapters and a fixed versioned
  test object suitable for byte-for-byte verification.
- Implement one worker-owned `send object to entity` operation. Carry origin,
  request ID, and destination as data rather than branching on EYE role.
- Make button A queue that operation directly for the configured peer entity.
- Register a dedicated demo command APID.
- Define a versioned `CAPTURE_AND_RETURN` TC payload containing a request ID
  and requesting CFDP entity ID.
- Make button B send that TC. After validation and duplicate suppression, make
  the receiving peer queue the same send operation used by its button A.
- Return a small command acceptance/status Space Packet so the requester can
  distinguish accepted, busy, invalid, and timed-out commands.
- Drive the existing UI with real peer, TC, CFDP direction, completion, and
  failure events. Coarse transfer indication is acceptable until Milestone 3
  supplies byte progress.
- Copy work out of UDP, router, input, and CFDP callbacks into bounded queues.
  Do not call LVGL, block, or synchronously reenter CFDP from those callbacks.

### Acceptance

- Swapping or duplicating role configuration is visibly diagnosed.
- Button A on either board transfers the test object by acknowledged CFDP; the
  receiver verifies its version, size, and bytes.
- Button B on either board produces a visible TC animation and peer
  acceptance; the peer then uses the same send operation to return the test
  object by CFDP, and the requester verifies it.
- Both displays show the correct TC and CFDP directions and return to a usable
  state after completion or failure.
- An absent peer reaches a bounded timeout and returns the UI to a usable
  state.
- Repeated request IDs do not leak state or trigger duplicate work, and busy
  behavior is bounded and visible.
- No runtime behavior depends on whether the build is role A or role B beyond
  configured identity and endpoint values.

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
- Do not add a live viewfinder: each acquisition captures exactly one fresh
  frame when requested.
- Define buffer ownership across camera capture, LVGL display, CFDP sender
  retention, and retransmission.
- Add the separate full-screen `SHOW` view. It displays the latest valid image
  and toggles back to the existing protocol UI without pausing protocol work.
- Preserve the previous valid image if a later capture fails.
- Add camera capture as the acquisition phase of the shared operation proven
  in Milestone 2. Validate that phase locally before replacing the CFDP test
  object with the captured image.
- Exercise the same capture phase from local button-A requests and accepted
  button-B TCs; request origin must not select a different capture path.

### Acceptance

- Fifty sequential captures succeed on each board without memory growth,
  corrupt frames, or stale LVGL pointers.
- `SHOW`, a second `SHOW`, and A/B from image view produce the specified view
  transitions without affecting network or protocol state.
- `SHOW` before the first valid image reports `NO IMAGE`.
- Wi-Fi peer presence remains stable while capture and display are active.
- Capture failure is visible and returns both actions to a valid state.

## Milestone 5: CFDP Image Transfer Slice

### Work

- Extend the Milestone 2 in-memory CFDP adapters with bounded PSRAM-backed
  image storage and explicit ownership.
- Transfer a known test image before connecting live camera output.
- Select the largest CFDP segment that keeps the encoded Space Packet inside
  one non-fragmented UDP datagram.
- Feed real progress events into the existing UI state model.
- Retain the previous image until size and checksum verification succeeds,
  then swap the displayed receive buffer atomically.
- Keep the CFDP sender's captured object immutable until its terminal event;
  showing an image must not change sender or receiver buffer ownership.
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

- button A: capture one fresh still, retain it as the latest valid image, and
  transmit that same immutable object;
- button B: TC request, peer capture, CFDP return, verified local display.
- `SHOW`: toggle between the existing protocol UI and the latest valid image;
  no capture, transfer, or confirmation is implied by this action.

Queue camera and CFDP work outside network and UI callbacks. Define bounded
busy behavior for repeated local actions and duplicate TC request IDs. SHOW
remains a UI-only operation and must not synchronously enter camera or CFDP.

### Acceptance

- A new user can execute both workflows using only the two displays and
  physical buttons.
- Either board can inspect its latest local or verified received image and
  return to the protocol UI without changing transfer state.
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
  image header, memory filestore, SHOW toggling, no-image handling, and A/B
  transitions from image view.
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
- Image thumbnails or protocol/status overlays on the full-screen image view.
- JPEG decoding, persistent photo archives, discovery protocols, or dynamic
  multi-peer routing.
- More than one active CFDP sender and one active receiver per device.
