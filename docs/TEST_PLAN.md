# Test Plan

> **Note:** This project uses manual testing during development. The test cases 
> below document expected behavior for reference and future regression testing.
> Formal unit tests and automated integration tests are not currently implemented.

## Manual Test Cases

### Turnout Manager Behavior
- Adding a turnout assigns correct event IDs and initial UNKNOWN state
- State updates via event ID correctly resolve to the right turnout
- Stale detection marks turnouts with no update after timeout
- Pending flag is set when command sent, cleared when state feedback received
- Thread safety: concurrent state updates from LCC and UI reads don't corrupt state
- Renaming a turnout persists across reboots
- Deleting a turnout removes it from storage and unregisters its LCC events
- Swap operation correctly reorders turnouts in the array

### Turnout Storage
- JSON parsing handles valid turnouts.json correctly
- Missing or corrupt turnouts.json starts with empty turnout list
- Turnout additions persist across reboots
- Event IDs round-trip correctly through dotted-hex serialization
- JMRI roster.xml import parses OlcbTurnoutManager turnout elements
- JMRI import skips turnouts whose event IDs already exist
- JMRI import respects inverted attribute (swaps Normal/Reverse)
- JMRI import uses userName when available, falls back to systemName
- Missing or invalid roster.xml is silently ignored (no crash)

### Configuration
- Valid nodeid.txt is parsed correctly
- Invalid node ID format falls back to default
- LCC configuration changes persist via CDI

---

## Functional Verification Checklist

### Turnouts Tab
- [ ] Turnout tiles display with correct name and state text (CLOSED/THROWN/UNKNOWN/STALE)
- [ ] Color coding: Green=Closed, Yellow=Thrown, Grey=Unknown, Red=Stale
- [ ] Tapping a tile sends the correct toggle event
- [ ] Pending indicator (blue border) appears after tap
- [ ] Pending indicator clears when state feedback arrives
- [ ] Tiles update in real-time when events received from bus
- [ ] Stale tiles turn red after timeout with no state updates
- [ ] Grid layout wraps correctly at different turnout counts
- [ ] Edit icon opens rename modal with current name pre-filled
- [ ] Rename modal: Save updates tile label and persists to SD card
- [ ] Rename modal: Cancel closes without changes
- [ ] Trash icon opens delete confirmation dialog
- [ ] Delete dialog shows turnout name and warning message
- [ ] Delete confirm: removes turnout, unregisters LCC events, refreshes grid
- [ ] Delete cancel: closes dialog without changes
- [ ] Tiles show 3-row layout: Name / State / Edit+Delete buttons (150×110px)

### Add Turnout Tab
- [ ] Name, Normal Event ID, and Reverse Event ID fields accept input
- [ ] On-screen keyboard works for text entry
- [ ] Add button creates turnout and saves to SD card
- [ ] New turnout appears on Turnouts tab immediately
- [ ] Discovery mode toggle enables/disables event capture
- [ ] Discovered events appear in scrollable list
- [ ] Invalid event ID format is rejected with feedback

### Boot Behavior
- [ ] Splash screen displays on startup
- [ ] Turnout positions queried after LCC initialization
- [ ] Tiles update as query responses arrive
- [ ] Only ProducerIdentified with VALID state updates tiles (INVALID ignored)
- [ ] Missing SD card shows error screen
- [ ] Missing nodeid.txt uses default node ID
- [ ] Empty turnouts.json shows empty grid (no crash)
- [ ] JMRI roster.xml on SD card imports new turnouts automatically
- [ ] JMRI import skips duplicate event IDs
- [ ] JMRI import handles inverted turnouts correctly
- [ ] Missing roster.xml does not cause errors

### Stale Detection
- [ ] Turnouts transition to STALE after configured timeout
- [ ] Stale timeout of 0 disables stale detection
- [ ] Receiving any event for a turnout resets its stale timer
- [ ] Stale tile can still be tapped to send toggle command

### Power Saving
- [ ] Screen dims after configured timeout
- [ ] Touch wakes screen with fade-in animation
- [ ] Timeout of 0 keeps screen always on

### LCC Integration
- [ ] Node appears on LCC network with configured ID
- [ ] Panel settings configurable via CDI tools (JMRI)
- [ ] IdentifyConsumer sent for each registered event at startup
- [ ] EventReport consumption updates correct turnout tile
- [ ] ProducerIdentified consumption updates correct turnout tile
- [ ] Event production sends correct event on tile tap
- [ ] OTA firmware update works via JMRI

---

