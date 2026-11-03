# First setup

Everything below is in **Settings**. Do these once after the first install.

## 1. Pick your radio preset

**Settings > Radio.** Every node on a mesh must use the same frequency, spreading
factor, bandwidth and coding rate, or they can't hear each other. Choose the preset
your local mesh uses:

| Preset | Settings | Frequency |
|---|---|---|
| Australia | SF10 BW250 CR5 | 915.800 MHz |
| Australia Vic. | SF7 BW62 CR8 | 916.575 MHz |
| **EU/UK Narrow (Rec)** (default) | SF8 BW62 CR8 | 869.618 MHz |
| EU/UK Long Range | SF11 BW250 CR5 | 869.525 MHz |
| EU/UK Medium | SF10 BW250 CR5 | 869.525 MHz |
| Czech Narrow | SF7 BW62 CR5 | 869.525 MHz |
| EU 433 Long Range | SF11 BW250 CR5 | 433.650 MHz |
| New Zealand | SF11 BW250 CR5 | 917.375 MHz |
| NZ Narrow | SF7 BW62 CR5 | 917.375 MHz |
| Portugal 433 | SF9 BW62 CR6 | 433.375 MHz |
| Portugal 868 | SF7 BW62 CR6 | 869.618 MHz |
| Switzerland | SF8 BW62 CR8 | 869.618 MHz |
| USA/Canada | SF7 BW62 CR5 | 910.525 MHz |
| Vietnam | SF11 BW250 CR5 | 920.250 MHz |
| Custom | your own values | |

Only use frequencies that are legal where you are.

## 2. Set your name

**Settings > Device Name.** This is the name other people see on your messages and
adverts. It starts as `OMS-0001`; change it to something people will recognise.

## 3. Change the storage password

**Settings > Storage Password.** The row says **Default** in red until you change it.

Channel keys, remembered repeater passwords and your identity backup are encrypted
on the SD card with this password. The default (`changeme1`) is published, so until
you change it, anyone who takes your SD card can read them. Pick a password you'll
remember: you need it to restore your identity onto a new or reflashed device. See
[Backup and security](Backup-and-Security).

## 4. Set the clock

**Settings > Timezone**, then **Settings > Date / Time**. With a GPS fix the time is
set from GPS automatically. The clock is used for message times and Night Mode.

## 5. Tell the mesh you're here

Other nodes learn about you from your **advert**. Until they've heard one, they
can't send you a direct message.

- **T-Deck:** open the **Advertise** app and send one.
- **Cardputer:** **Settings > Advert** sets how often an advert goes out on its own
  (every 12 hours by default). To send one now, type `advert` in the Terminal.

## Optional

- **Notifications** and **Notification Sound**: popups and sounds for new messages.
- **Night Mode**: quiet hours, with sounds muted.
- **Bluetooth** and **Bluetooth PIN**: to use the MeshCore phone app with the device.
- **Screen Timeout**: how long before the screensaver starts (10 s by default).
- **Theme**, **Font**, **Brightness**, **Language**.
- **Telemetry**: whether others may ask for your battery level and position.

All of these are explained in the [Settings reference](Settings-Reference).
