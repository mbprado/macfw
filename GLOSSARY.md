# macfw FireWire / BeBoB glossary

This glossary collects the main terms and abbreviations used while reverse-engineering and bringing up the M-Audio FireWire 410 on modern macOS.

## 1394 / IEEE 1394

**IEEE 1394** is the serial bus standard commonly known as **FireWire** (Apple), **i.LINK** (Sony), or DV/1394. The FW410 communicates with the host over IEEE 1394.

## AV/C

**Audio/Video Control**. A command protocol used over IEEE 1394 for controlling audio/video devices. The FW410 responds to AV/C commands for plug information, signal format, and BridgeCo extended stream discovery.

## AMDTP

**Audio and Music Data Transmission Protocol**. The IEC 61883 protocol used to carry real-time audio and MIDI data in FireWire isochronous packets. The FW410's audio streams are AMDTP streams.

An AMDTP stream contains data positions/slots. A slot may contain PCM audio or MIDI-related data; therefore an 11-position stream does not necessarily mean 11 audio channels.

## BeBoB

Common name for the **BridgeCo BeBoB** FireWire audio platform/protocol family. Many FireWire audio interfaces were built around BridgeCo technology and expose similar AV/C and streaming behavior. Linux supports these devices through `snd-bebob`.

The FW410 behaves as a BeBoB-family device, although it also has M-Audio-specific boot behavior.

## BridgeCo

Company/platform behind the FireWire audio technology used in many BeBoB devices. The FW410's information registers identify the manufacturer as `bridgeCo`, and its extended AV/C commands expose stream topology and channel information.

## CMP

**Connection Management Procedures**. The IEC 61883 mechanism used to establish and tear down isochronous connections between FireWire nodes.

CMP uses plug-control registers (PCRs) and master plug registers (MPRs). Before AMDTP packets can flow, the appropriate device plugs normally need a CMP connection and FireWire isochronous resources such as a channel and bandwidth.

## CSR

**Control and Status Register** architecture/register space. IEEE 1394 nodes expose standardized registers in the CSR address space. Important examples for this project include the CMP registers and FCP command/response registers.

Common base address:

`0xfffff0000000`

## FCP

**Function Control Protocol**. The IEEE 1394 mechanism used by AV/C to send commands and receive responses.

Standard CSR locations used by this project:

- FCP command: `0xfffff0000b00`
- FCP response: `0xfffff0000d00`

## PCR

**Plug Control Register**. CMP register describing and controlling an isochronous plug. PCR fields include whether the plug is online, broadcast/point-to-point connection state, isochronous channel, and—on output PCRs—speed/overhead information.

Two forms are important:

- **oPCR** — Output Plug Control Register: controls a stream leaving the FireWire device.
- **iPCR** — Input Plug Control Register: controls a stream entering the FireWire device.

For the FW410, direction is **device-relative**:

- device `OUTPUT` / oPCR → FW410 to Mac → host **capture/input**
- device `INPUT` / iPCR → Mac to FW410 → host **playback/output**

## MPR

**Master Plug Register**. CMP register describing global capabilities for a group of plugs, including the number of plugs and supported speed information.

Two forms are used:

- **oMPR** — Output Master Plug Register
- **iMPR** — Input Master Plug Register

Standard addresses:

- oMPR: `0xfffff0000900`
- oPCR0: `0xfffff0000904`
- iMPR: `0xfffff0000980`
- iPCR0: `0xfffff0000984`

## oPCR / iPCR

See **PCR**. The leading `o` and `i` mean output and input from the **FireWire device's perspective**, not the computer's perspective. This distinction is particularly important when translating BeBoB topology into CoreAudio channels.

## oMPR / iMPR

See **MPR**. Output and input master plug registers, again named from the FireWire device's perspective.

## P2P

**Point-to-point** connection. A CMP connection between a specific transmitting plug and receiving endpoint. The PCR contains a point-to-point connection count. A non-zero count means the plug is already participating in one or more P2P connections.

## Isochronous / ISO

FireWire transfer mode designed for real-time data with guaranteed bus resources. Audio streaming uses isochronous transfers rather than ordinary asynchronous reads/writes.

A stream generally needs an allocated **isochronous channel** and sufficient **bandwidth** before streaming begins.

## Isochronous channel

A numbered FireWire bus channel used to identify an isochronous packet stream. CMP associates a plug with the allocated channel.

This is a FireWire transport channel and should not be confused with an audio channel such as Line 1 or S/PDIF Left.

## Bandwidth

FireWire reserves bus capacity for isochronous streams. Establishing a real audio stream therefore involves both choosing an isochronous channel and reserving enough bandwidth for the stream's packet payload at the selected sample rate.

## Plug

An AV/C/CMP logical endpoint. A plug can represent an isochronous input or output stream. The FW410 reports multiple plugs through its MPRs, while our current audio investigation is focused on plug 0 in each direction.

## Stream position / slot

Position inside the multiplexed AMDTP data block. BridgeCo extended discovery tells us which logical channel occupies each stream position.

For example, at 48 kHz the FW410 device INPUT stream has 11 positions: 10 PCM audio positions plus one MIDI position.

## Cluster

A grouping in the AV/C extended stream-format description. A cluster describes one or more related data channels and their format/type. The FW410 stream-format responses contain clusters for PCM audio groups and MIDI.

## PCM

**Pulse-Code Modulation**. The normal uncompressed digital representation of audio samples. In our stream-format summaries, `PCM=10` means ten audio channels, independently of any additional MIDI slot.

## MIDI

**Musical Instrument Digital Interface**. The FW410 carries MIDI data alongside audio in its FireWire/AMDTP streams. BridgeCo reports MIDI as a separate conformant data section/cluster rather than as a PCM audio channel.

## S/PDIF

**Sony/Philips Digital Interface**. Two-channel digital audio interface. The FW410's discovered topology contains a two-channel S/PDIF section in both stream directions.

## SFC

**Sampling Frequency Code**. Encoded value used in AV/C/AM824 signal-format information to identify a sample rate. For example, our probes decoded SFC `0x02` as 48,000 Hz.

## AM824

IEC 61883-6 audio/music data format used inside AMDTP streams. It defines how audio/MIDI-related data is represented in FireWire isochronous packets.

## Configuration ROM

Standard IEEE 1394 ROM data that identifies a node and its units/capabilities. We use it to identify the FW410, read fields such as `Unit_Spec_ID`, `Unit_SW_Version`, model information, and distinguish `FW Bootloader` from operational `FW 410` mode.

## GUID

**Globally Unique Identifier**. A 64-bit identifier associated with a FireWire node/device. The FW410 GUID lets software recognize the same physical unit across bus resets and node-ID changes.

## Node ID

FireWire bus address assigned to a node for the current bus generation. Unlike the GUID, the node ID can change after a bus reset.

## Bus generation

Counter identifying the current FireWire bus topology generation. A bus reset increments/changes the generation. Transactions tied to an old generation must not simply be assumed valid after re-enumeration.

## Bus reset

FireWire event that causes nodes to re-enumerate and the topology/generation to change. Booting the FW410 from its bootloader into operational firmware causes re-enumeration and was observed to change the bus generation.

## Bootloader

Minimal firmware mode presented by the FW410 before the main application firmware is running. In this state the device identifies itself as `FW Bootloader` and its power LED blinks.

The project successfully issues the M-Audio boot-from-flash cue, after which the device re-enumerates as `FW 410` and the power LED remains steadily lit.

## Boot-from-flash cue

M-Audio/BridgeCo boot command used by this project to tell the FW410 bootloader to start the application image already stored in device flash. `fwboot` performs this operation after guarded prerequisite checks.

## CoreAudio

Apple's audio subsystem/API. The eventual goal is to expose the FW410's FireWire streams to macOS as usable CoreAudio input and output channels.

Be careful with direction terminology: CoreAudio describes direction relative to the **host**, whereas AV/C/BridgeCo commonly describes it relative to the **device**.

## IOFireWireFamily

Apple's FireWire driver/framework family. Even on the modern Intel macOS system used for this project, enough of the legacy FireWire user-space interfaces remain available for direct FireWire transactions and probing.

## IOFireWireLib

User-space FireWire interface exposed through IOKit. The project's tools use `IOFireWireLibDeviceRef` and related interfaces to inspect the FW410 and issue asynchronous FireWire transactions.

## `snd-bebob`

Linux ALSA FireWire driver for BeBoB-family devices. Its implementation is an important behavioral reference for understanding FW410 stream discovery, CMP setup, AMDTP configuration, and M-Audio-specific behavior.

## Host capture / input

Audio traveling **FW410 → Mac**. In BridgeCo/CMP terminology this comes from the device's **OUTPUT** plug.

Confirmed FW410 formats:

- 44.1/48/88.2/96 kHz: 4 PCM inputs + MIDI
- 176.4/192 kHz: 2 PCM inputs + MIDI

## Host playback / output

Audio traveling **Mac → FW410**. In BridgeCo/CMP terminology this goes to the device's **INPUT** plug.

Confirmed FW410 formats:

- 44.1/48/88.2/96 kHz: 10 PCM outputs + MIDI
- 176.4/192 kHz: 8 PCM outputs + MIDI
