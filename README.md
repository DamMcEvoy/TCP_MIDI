# TCP_MIDI

TCP_MIDI is a research-oriented client-server MIDI transport prototype that sends MIDI data over TCP/TLS, timestamps events at the server, and performs time-aware scheduling on the receiving client before output to the local MIDI device.

The project is designed as a proof-of-concept implementation rather than a final commercial release. Its purpose is to explore server-timestamped MIDI transport, receiver-side scheduling, and JR-informed playback stabilisation in controlled 1-to-1 use.

## Overview

The system consists of three main parts:

- a **client** that captures local MIDI input and sends raw MIDI bytes to the server
- a **server** that receives MIDI data, assigns a monotonic timestamp, wraps the event in a custom frame, and rebroadcasts it
- a **receiving client** that reconstructs the timed event, updates timing state, schedules playback, and emits the MIDI message locally

Data flow is bidirectional at the session level: either client can send MIDI and receive rebroadcast MIDI from the server. Timing authority, however, is centralised at the server. The server is the source of frame timestamps and JR clock packets, while each receiving client applies local scheduling based on that server-emitted timing model.

## Current timing model

The current implementation uses a hybrid scheduling path:

1. The server timestamps outgoing events using a monotonic nanosecond clock.
2. The receiving client reconstructs the framed event and identifies raw MIDI, JR clock, and recovery-journal payloads.
3. `TimeSync` maintains either an anchor-based timing model or a fresh-JR extrapolated model.
4. `ReceiveScheduler` computes a local `playAt` time and queues messages for release.
5. `AppController` applies a final pre-output lead before sending the MIDI message to the local device.

This makes the project more than a simple relay. It is a timed MIDI redistribution pipeline with client-side playback scheduling derived from server-issued timing information.

## Features

- Bidirectional MIDI transport between clients through a central server
- TLS-based client-server connection
- Server-side event timestamping and framed packet construction
- Client-side frame parsing into `TimedMidiEvent`
- JR clock packet handling and timing-model updates
- Receive-side scheduling with queue-based timed release
- Transport-state checks using clock and song-position information
- Recovery-journal support in the transport format
- Local MIDI input/output through `libremidi`

## Repository contents

Typical project files include:

- `AppController.cpp` / `.h` — top-level orchestration
- `transportClient.cpp` / `.h` — TLS transport, framed receive path, callback dispatch
- `receiveScheduler.cpp` / `.h` — playback scheduling and queue release
- `timeSync.cpp` / `.h` — anchor and JR-based timing logic
- `clockState.cpp` / `.h` — transport-state tracking
- `midiInputHandler.cpp` / `.h` — MIDI input capture and clock-message routing
- `midiOutputHandler.cpp` / `.h` — MIDI output emission
- `server.cpp` — central relay, timestamping, framing, and JR clock emission
- `recoveryJournal.cpp` / `.h` — recovery-journal support
- `main.cpp` — client entry point

## Requirements

The exact dependency setup depends on platform and how the repository is organised locally, but a standard build will typically require:

- a C++17-capable compiler
- CMake 3.16 or newer
- OpenSSL development libraries
- `libremidi`
- pthread support on Unix-like systems
- MIDI backend support appropriate to the target platform
  - CoreMIDI on macOS
  - ALSA/JACK on Linux depending on backend choice
  - WinMM or equivalent on Windows

## Build dependencies

### macOS

If Homebrew is available, a typical dependency setup may look like this:

```bash
brew install cmake openssl
```

If `libremidi` is not vendored into the project, install or provide it separately according to your local setup.

### Ubuntu / Debian

A typical setup may look like this:

```bash
sudo apt update
sudo apt install build-essential cmake libssl-dev pkg-config
```

If `libremidi` is external rather than vendored, it must also be installed or made available to CMake.

## Clone the repository

```bash
git clone https://github.com/your-username/TCP_MIDI.git
cd TCP_MIDI
```

Replace the repository URL with the actual project URL.

## Build the client

A standard out-of-source CMake build is recommended:

```bash
cmake -S . -B build
cmake --build build
```

If dependencies are installed in non-standard locations, pass them explicitly to CMake. For example:

```bash
cmake -S . -B build \
  -DOPENSSL_ROOT_DIR=/path/to/openssl \
  -DCMAKE_PREFIX_PATH=/path/to/libremidi
cmake --build build
```

If the project defines multiple targets, inspect the generated build output or `CMakeLists.txt` to identify the client executable name.

## Build the server

If the server is built as part of the same CMake project, the same build command may also produce the server executable:

```bash
cmake -S . -B build
cmake --build build
```
## Running the client TCP_MIDI

Run the client executable after the server is active:

```bash
./build/TCP_MIDI_Client
```

Adjust the executable name to match your build output.

At runtime, the client typically:

1. connects to the configured server IP and port
2. enumerates MIDI input and output devices
3. opens the selected ports
4. starts the transport receive loop and scheduler
5. sends local MIDI input to the server and schedules incoming rebroadcast events for output

## Typical workflow

A typical test session looks like this:

1. Start the server.
2. Start Client A and connect it to the server.
3. Start Client B and connect it to the server.
4. Select a MIDI input and output port on each client.
5. Begin streaming from one client.
6. Observe server rebroadcast and timed release on the receiving client.

Because transport is bidirectional, Client A can send to Client B and Client B can send to Client A. In both cases, the server remains the timestamp authority and the receiver performs the local playback scheduling.

## Architecture summary

### Send path

- `MidiInputHandler` captures local MIDI input.
- `AppController` forwards raw MIDI to `TransportClient`.
- `TransportClient` sends raw MIDI bytes over TLS to the server.

### Server path

- `server.cpp` receives raw MIDI bytes from a client.
- The server timestamps the event using a monotonic clock.
- The event is wrapped in a framed packet with sequence and timing metadata.
- The framed event is broadcast to the other connected clients.
- The server also emits periodic JR clock packets.

### Receive path

- `TransportClient::receiveLoop()` reconstructs the framed packet.
- A `TimedMidiEvent` is created from header and payload data.
- JR clock samples are passed into `TimeSync`.
- `ReceiveScheduler` computes a `playAt` time and queues the event.
- `AppController` applies the output lead and sends the MIDI message to the selected output device.

## Limitations

This project is a proof-of-concept research implementation. Important current limitations include:

- the server currently reports a conservative zero drift estimate in JR packets
- the scheduler uses a fixed 80 ms buffer rather than a fully adaptive jitter buffer
- transport-state release decisions are still constrained by coarse 24 PPQN clock resolution and Song Position Pointer state
- the implementation is intended for controlled testing and evaluation, not hardened production deployment
- future refinement is expected in timing estimation, adaptive buffering, and broader interoperability testing

## Troubleshooting

### CMake cannot find OpenSSL

Pass the OpenSSL path explicitly:

```bash
cmake -S . -B build -DOPENSSL_ROOT_DIR=/path/to/openssl
```

### CMake cannot find `libremidi`

If `libremidi` is not bundled with the project, add its install prefix to `CMAKE_PREFIX_PATH`:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/libremidi
```

### Client connects but no MIDI is heard

Check the following:

- the correct local MIDI output port is selected
- the receiving client has started streaming
- the server is running and both clients are connected
- the output device accepts the MIDI message format being sent

## Development notes

If this repository is being used alongside a dissertation or technical report, it is helpful to keep the README aligned with the current architecture language used in the written work. In particular, this project is best described as:

- **bidirectional in transport/data flow**
- **server-authoritative in timing**
- **receiver-side in playback compensation and release scheduling**

That phrasing is consistent with the present implementation and avoids overstating the role of the sender or implying symmetric timing authority.

## Suggested future improvements

- adaptive jitter buffer sizing
- non-zero server drift estimation
- richer recovery-journal recovery logic
- clearer runtime configuration for ports and certificates
- packaging and installer support
- broader multi-client and cross-platform validation

## License

This project is licensed under the [MIT License](LICENSE).