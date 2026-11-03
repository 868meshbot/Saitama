# Troubleshooting

## I can't hear anyone / nobody hears me

- **Radio preset:** everyone on your mesh must use the same preset. Check
  **Settings > Radio** (or `get radio` in the Terminal) against what your local mesh
  uses.
- **Antenna:** check it's fitted and screwed in.
- **Region scope:** if local repeaters only forward scoped traffic, set your scope.
  See [Regions and scopes](Regions-and-Scopes).
- **Finder:** shows which nodes hear you directly. If it finds none, move somewhere
  higher or more open.
- **Signal:** if the packets-received count isn't rising, the radio is hearing
  nothing on this preset.

## I can't send someone a DM

You need their advert first, and they need yours. Send an advert (T-Deck:
**Advertise** app; Cardputer: `advert` in the Terminal), ask them to do the same, and
check they appear in **Heard**. Then save them as a contact.

If DMs to a known contact keep failing, the saved route may be out of date: in
Contacts choose **Reset Path** and try again.

## Messages show as "?" or empty

The device was short of memory when the message arrived. This was much improved in
1.6; if you still see it, reboot, and report it with the serial log.

## The Cardputer says memory is low

The Cardputer has no PSRAM, so its memory is tight. When it runs low, Saitama
closes the screens you aren't using and shows a notice. Go back to the launcher and
reopen the app. Type `mem` in the Terminal to see the free memory.

## The device restarted by itself

Connect a USB serial monitor (115200 baud) and look at the first lines after it
restarts: `Last reset:` gives the reason (for example a brownout, which points to a
weak battery or a very loud speaker volume).

## The touchscreen taps in the wrong place (T-Deck)

Type `touch-cali` in the Terminal and tap each crosshair.

## The boot screen asks for a password

Your identity backup on the SD card is locked with a storage password that's
different from the one on this device (common after a full reflash). Type the
storage password you set before. See [Backup and security](Backup-and-Security).

## The SD card isn't found

Use a FAT32-formatted card. Type `sd` in the Terminal to see its status, and
`sd mount` to retry.

## Getting help

Open an issue on [GitHub](https://github.com/868meshbot/Saitama/issues) with your
device, the Saitama version (shown on the splash screen and in **Settings > Firmware
Update**), what you did and what happened. A USB serial log helps a lot.
