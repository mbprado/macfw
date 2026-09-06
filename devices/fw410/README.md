# M-Audio FireWire 410

The released FW410 implementation currently remains under the historical top-level `fw410/` tree.

This directory is the device-profile anchor for the new multi-device layout. FW410-specific code will move here incrementally only when doing so helps share proven infrastructure with another interface and can be regression-tested against the released FW410 behavior.

Initial macfw release scope for this profile is 44.1 kHz and 48 kHz full-duplex PCM audio. MIDI remains deferred.
