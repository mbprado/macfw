# Initial Binary Analysis

**Status:** Initial static analysis

**Target:** `M-AudioFireWireBeBoB.kext`

## Source artifact

The analysis started from the vendor kext supplied for this project.

Bundle:

```text
M-AudioFireWireBeBoB.kext
```

Executable:

```text
Contents/MacOS/M-AudioFireWireBeBoB
```

## Mach-O architecture

The executable identifies as:

```text
Mach-O 64-bit x86_64 kext bundle
```

This is an important finding: the project is **not** a simple 32-bit-to-64-bit port.

The original executable is already Intel 64-bit. The compatibility problem is primarily its dependency on the legacy macOS kernel/FireWire/audio architecture.

## Bundle metadata

From `Contents/Info.plist`:

| Property | Value |
|---|---|
| Bundle identifier | `com.m-audio.driver.firewire` |
| Executable | `M-AudioFireWireBeBoB` |
| Package type | `KEXT` |
| Version | `1.10.6 "Sierra mod"` |
| FireWire product | `FW 410` |
| IOKit class | `com_m_audio_FW410Device` |
| Provider class | `com_m_audio_FWMetaNub` |
| User client | `com_m_audio_FWUserClient` |

The active IOKit personality is specifically for the FW410. The plist also contains commented-out personalities for several other M-Audio FireWire products, suggesting that the original codebase was designed as a broader device family driver.

## Legacy dependencies

The kext declares dependencies on:

```text
com.apple.iokit.IOAudioFamily
com.apple.iokit.IOFireWireAVC
com.apple.iokit.IOFireWireFamily
com.apple.kpi.iokit
com.apple.kpi.libkern
com.apple.kpi.mach
```

The first three are particularly important for the modernization effort.

The original architecture is therefore approximately:

```text
FW410
  │
  ▼
IOFireWireFamily / IOFireWireAVC
  │
  ▼
M-Audio FireWire / BeBoB classes
  │
  ▼
IOAudioFamily
  │
  ▼
CoreAudio
```

The modern implementation should not assume these legacy kernel interfaces are available on Sonoma or newer macOS.

## Important classes and symbols

The binary contains symbols for substantial portions of the original implementation.

### FW410-specific

```text
com_m_audio_FW410Device
FW410
```

Observed FW410-specific methods include functionality for:

- audio-engine setup
- input level
- input pan
- input routing
- software return routing
- headphone source
- AC3 activation
- HSCI data
- control packet sizing

### Audio framework

```text
m_audio_b_FWAudioDevice
m_audio_b_FWBaseEngine
FWAudioDevice
FWAudioEngine
FWAudioStream
```

The binary contains methods dealing with:

- input/output formats
- stream formats
- MIDI input/output ports
- MIDI port properties
- DCL program management

### FireWire isochronous transport

```text
m_audio_b_FWIsochChannel
m_audio_b_FWDCLBuilder
m_audio_b_FWDCLProgram
m_audio_b_FWDCLInputProgram
m_audio_b_FWDCLOutputProgram
```

Notable functions include:

```text
SetPacketSize
SetPacketParameters
BuildPacketDCLSegment
CreateIsochChannelAndPorts
SetupCIP
CalcIsochPacketHeaders
Start
Stop
ForceStop
ResetPort
```

These are strong indicators that the original driver implements the FireWire isochronous audio transport itself rather than relying only on a high-level audio abstraction.

### Connection management

Observed classes include:

```text
FWP2PConnection
FWConnectionManager
FWAVCConnectionManager
```

These should be mapped before attempting to reproduce stream startup.

## Firmware implementation

The binary contains explicit firmware-management functionality.

Observed symbols and strings include:

```text
MAFirmwareInfo
MAFirmwareUpgradeImage
MAFirmwareStart
MAFirmwareHalt
```

Diagnostic strings include messages for:

```text
FirmUp - upload start successful
FirmUp - Uploaded %lu bytes
FirmUp - Successful upload
FirmUp - Upload failed
FirmUp - Starting code upload
FirmUp - Starting config upload
FirmUp - Unknown firmware base address
FirmUp - status
FirmUp - Error during firmware update
```

This means firmware behavior must be treated as a first-class protocol area rather than an optional implementation detail.

## Embedded source-path evidence

The executable contains compiler/debug diagnostic strings referencing the original source tree, including:

```text
/Volumes/MacBuild_Source/FireWireBeBob/01.10.005/Source/KernelExtension/Source/
```

Observed source filenames include:

```text
FWDCLBuilder.h
FWIsochChannel.cpp
FWDCLProgram.cpp
TPacketIterator.cpp
```

Some assertions retain source line numbers. These strings can help locate function boundaries and reconstruct relationships in the decompiler.

## Preliminary architecture map

```text
com_m_audio_FW410Device
          │
          ├── FWAudioDevice
          │      │
          │      ├── FWBaseEngine
          │      │      ├── input format
          │      │      ├── output format
          │      │      ├── stream format
          │      │      └── MIDI
          │      │
          │      └── FWAudioStream
          │
          ├── FWConnectionManager
          │      └── FWAVCConnectionManager
          │
          ├── FWIsochChannel
          │
          ├── FWDCLProgram
          │      ├── FWDCLInputProgram
          │      └── FWDCLOutputProgram
          │
          └── Firmware
                 ├── MAFirmwareInfo
                 ├── MAFirmwareUpgradeImage
                 ├── MAFirmwareStart
                 └── MAFirmwareHalt
```

This is a **working map**, not yet a complete class hierarchy.

## Next reverse-engineering targets

The next analysis pass should concentrate on the following functions/classes in order:

1. `com_m_audio_FW410Device` initialization and startup path
2. `FWMetaNub` device matching and provider relationship
3. Firmware detection and loading
4. `FWConnectionManager` / `FWAVCConnectionManager`
5. `FWIsochChannel`
6. `FWDCLProgram::SetupCIP`
7. `FWDCLProgram::SetPacketParameters`
8. Input/output DCL program construction
9. `FWBaseEngine` stream startup
10. FW410-specific control packets

## Important conclusion

The old driver is valuable as a **protocol and architecture reference**, but a direct kext port should not be the default strategy.

The binary already gives us evidence of three separable layers:

```text
FireWire transport
       │
       ▼
BeBoB / M-Audio device protocol
       │
       ▼
Audio / MIDI / controls
```

The modernization effort should attempt to preserve this conceptual separation and replace the obsolete macOS transport/audio integration independently.

## Evidence classification

### Confirmed

- x86_64 Mach-O kext
- FW410-specific IOKit personality
- Legacy IOAudioFamily dependency
- Legacy IOFireWire dependencies
- FW410-specific classes
- FireWire isochronous/DCL implementation
- Firmware-management implementation

### Observed

- Source-path strings and assertion locations
- FW410-specific control methods
- Multiple commented device personalities

### Inferred

- The FireWire transport, device protocol and CoreAudio integration can potentially be treated as separate layers.

### Unknown

- Exact Sonoma FireWire transport strategy
- Exact firmware image format
- Complete FW410 command protocol
- Exact stream channel/packet layout
- Which portions of the old transport can be reused through modern macOS APIs
