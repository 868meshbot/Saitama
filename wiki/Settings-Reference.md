# Settings reference

Rows are listed in the order they appear in **Settings**. A row's value is shown on
the right; green means on, red means off or needing attention. Rows marked T-Deck or
Cardputer only appear on that device.

## Identity and radio

| Setting | What it does |
|---|---|
| **Device Name** | The name others see. Default `OMS-0001`. |
| **Share My Contact** | Shows your contact as a QR code for another device or the MeshCore app to scan. |
| **Generate Identity** | Makes a new key pair, so you become a new node on the mesh. Your contacts would need to add you again. Rarely needed. |
| **Channels** | Add, edit or remove channels and their keys (PSK). Empty key on a `#name` channel = derived from the name. |
| **Radio** | The radio preset (see [First setup](First-Setup)) or Custom frequency / SF / BW / CR. |
| **Power** | Transmit power. |
| **Radio Mode** | **Continuous RX** (always listening), **LoRa Duty Cycle** (the radio sleeps 250 ms in every 500 ms when idle: saves power, adds a little delay), or **FHSS** (frequency hopping, only on 915 MHz presets; see `docs/FHSS.md`). Also holds **Routing**: 1, 2 or 3-byte path hashes. |
| **CPU Governor** | Processor speed against battery life. |

## Display and sound

| Setting | What it does |
|---|---|
| **Brightness** | Screen backlight. |
| **Theme** | Colour scheme: Default, Green, Dracula, Tokyo Night, Catppuccin, Synthwave 84 and more. |
| **Font** | The font used for messages and names. |
| **Speaker** | Speaker on/off and volume. |
| **Night Mode** | Start and end times. During Night Mode, sounds are muted and a moon shows by the clock. |
| **Map Style** (T-Deck) | Day, Night or Auto. See [Map and GPS](Map-and-GPS). |
| **Launcher** (Cardputer) | Carousel or Menu. |
| **Screen Timeout** | Seconds of no use before the screensaver starts (default 10). |
| **Screen Off** | Turns the backlight off after the screensaver has run this long. |
| **Screensaver** | T-Deck: Clock or Saitama. Cardputer: Clock or Mesh rain. |
| **Notifications** | New-message popup on or off. |
| **Notification Sound** | Default, Pluck, Clear or Whoosh. |
| **Language** | English, Italiano, Français, Deutsch, Español. |

## Mesh behaviour

| Setting | What it does |
|---|---|
| **Bluetooth** | Turns the Bluetooth link for the MeshCore phone app on or off. |
| **Bluetooth PIN** | The pairing PIN, random for each device. Changing it unpairs every phone. |
| **Advert** (Cardputer) | How often an advert is sent automatically: Off, 3, 6, 12 (default) or 24 hours. On the T-Deck this is in the Advertise app. |
| **Telemetry** | Whether others may ask for your battery, temperature and (optionally) position. |
| **GPS** | Off, Intermittent or On. |
| **Auto Add Contacts** | Save nodes to Contacts / Repeaters automatically as their adverts arrive. |
| **DM Retries** | How many times an unconfirmed DM is resent along its route, then as a flood. Default 5 / 3. |
| **Region Scope** | Your default scope and 9 saved regions. See [Regions and scopes](Regions-and-Scopes). |
| **Location Sharing** | Put your position in your adverts. |

## Device

| Setting | What it does |
|---|---|
| **Keyboard Light** (T-Deck) | Brightness, Off, or Auto (lit between set times and during Night Mode). |
| **Keyboard Layout** | English, French AZERTY or German QWERTZ. |
| **Serial Output** | The USB serial log on or off. |
| **Date / Time** | Set the clock by hand (it is set from GPS when there's a fix). |
| **Timezone** | Hours from UTC. |
| **Firmware Update** | Shows the installed version. Updates are flashed over USB (see [Installing Saitama](Installing-Saitama)). |
| **Save Messages** | Save conversations to the SD card (default on). |
| **Show Hops** / **Show RSSI** | Show hop count and signal strength on received messages. |
| **Backup & Restore** | Write contacts, repeaters and settings to the SD card, or read them back. |
| **Storage Password** | The password that encrypts secrets on the SD card. Shows **Default** in red until changed. See [Backup and security](Backup-and-Security). |
| **Return to Launcher** | Back to the launcher. |
