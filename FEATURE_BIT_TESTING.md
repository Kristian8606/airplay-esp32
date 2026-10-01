# AirPlay feature-bit A/B testing

Open:

    idf.py menuconfig

Then go to:

    AirPlay feature bits (A/B testing)

Every bit advertised by this build is independently selectable. Defaults match
v4.1.87 (`features=0x4A7FCA00,0x38356BD0`) with bit 58 disabled.

Suggested order for the current RemoteControl/type-130 investigation:

1. Keep defaults: bit 58 OFF. Build/flash and test Apple Music.
2. If type-130 still appears, keep bit 58 OFF and turn bit 38
   `SupportsUnifiedMediaControl` OFF.
3. Continue one bit at a time and record the boot `features=` / `fex=` lines.

Bits 40 (BufferedAudio), 41 (PTP), 59 (AudioStreamConnectionSetup) and 60
(AudioMediaDataControl) affect the transport selected by the sender, so leave
those for later unless intentionally testing a transport change.

If upgrading an existing checkout whose `sdkconfig` already contains old custom
feature-bit values, open menuconfig and verify the checkboxes once. A fresh
configuration uses the defaults above.
