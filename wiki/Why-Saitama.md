# Why Saitama?

Most MeshCore radios are **a radio with a phone attached**. The radio does the
talking, but you read and type everything in an app on your phone, connected by
Bluetooth. If your phone is flat, lost or left at home, the radio is useless.

**Saitama is the whole thing in one device.** It runs on handhelds that already have
a screen and a keyboard, so you can read, type and send messages with nothing else.

## Full touchscreen on the T-Deck

**On the LilyGo T-Deck, Saitama works completely by touch, just like a phone.**

Many firmwares for the T-Deck ignore its touchscreen, or only use it on a screen or
two. You end up rolling a tiny trackball for everything. Saitama supports touch
**everywhere**:

- **Tap** apps, buttons, list rows, contacts and messages.
- **Swipe** between the launcher pages, and scroll through chats and lists with
  your finger.
- **Drag** sliders and pick from drop-down lists by touch.
- Pop-up menus use **big two-column buttons** sized for fingers, not fiddly little
  links.
- The touch controller is found automatically at start-up, and if taps ever land in
  the wrong place there's a quick calibration (`touch-cali` in the Terminal).

The keyboard and trackball still work everywhere too, so use whichever you like:
type with your thumbs, tap to choose, or never take your hands off the keys.

## Verified direct messages: know who you're talking to

**Every direct message in Saitama is checked to make sure it really came from the
person whose name is on it. You can see that it's been checked: it has a green
padlock 🔒.**

Why this matters: on a radio network, anyone can transmit anything. On a group
channel, somebody can type any name they like and pretend to be someone else. A
fake "This is Dad, meet me at the bridge" is exactly the kind of message you don't
want to fall for.

A direct message is different. It's locked with a key that only the two of you
share: your device and your contact's device each work it out from their own
secret key and the other's public key. Nobody else has it. So:

- **It can't be read** by anyone else, and
- **it can't be faked.** Someone without your contact's secret key can't make a
  message that your device will accept as coming from them.

Saitama shows you the result:

- **T-Deck:** a **green padlock** beside the sender's name on each verified
  message.
- **Cardputer:** a **green padlock** in the title bar of the conversation.

**No padlock = not verified.** Channel messages never have one, because everyone in
a channel shares the same key. Treat a name on a channel as "probably them", and use
a DM when it matters.

The small print: your device trusts a contact's key from the first advert it saves
for them, so add contacts you know are genuine. The check is MeshCore's standard
message check, which stops casual forgery and tampering, not a full digital
signature.

## Built to get your message through

Sending a message is easy. Making sure it **arrives** is the hard part, and it's
where Saitama does a lot of work you never see:

- **✓ Delivered or ✕ Not delivered.** Every DM ends with a clear answer. Nothing
  vanishes silently.
- **Smart retries.** It resends along the known route; if that route has broken it
  forgets it and searches the whole mesh for a new one, then remembers what works.
  The other person still sees your message only once.
- **Routes remembered.** Proven routes are saved to the SD card, so after a restart
  your messages go straight to people instead of searching again.
- **"repeated ×3".** Channel messages show how many repeaters passed them on, so you
  know they got out.
- **Ready for big meshes.** Region tags, longer route codes for very large
  networks, and room made for newcomers when the device's node table fills up.

The full story is on [Getting your message through](Message-Reliability).

## What makes it different

**No phone needed.** Chat, contacts, maps, settings: everything is on the device.
(You can still connect the MeshCore phone app if you want to.)

**Easy to use.** Big buttons, clear lists, and the same layout everywhere. On the
T-Deck you tap the screen like a phone (see above). On the Cardputer everything
works from the keyboard.

**Free, with nothing locked away.** No subscription, no "pro" version, no features
held back for paying users. All of it is open source (GPL), so anyone can check what
it does, fix it or improve it.

**Not just messages.** Saitama includes tools that usually need a laptop or a second
device: see who's around, check your signal, look after repeaters, find a route,
view offline maps. See [The apps explained](Apps-Explained).

**Your data stays yours.**

- Messages are encrypted from end to end by MeshCore.
- **Direct messages are verified**, with a green padlock to prove it (see above).
- Keys and passwords saved to the SD card are locked with your own password.

**Survives a reflash.** Your identity, contacts, repeaters and settings are backed up
to the SD card. Update the firmware or swap to another device, and everything comes
back.

**Works with everyone else.** Saitama speaks standard MeshCore, so you can message
people using any other MeshCore device or app.

## Saitama compared

How Saitama compares with the usual way of using MeshCore: a radio paired with
the phone app.

| | Typical MeshCore radio + phone app | Saitama |
|---|---|---|
| **Works without a phone** | No: the radio has no screen or keyboard of its own | **Yes: everything is on the device** |
| **Touchscreen** | On the phone | **Full touch on the T-Deck itself** |
| **Phone battery** | Drained by the app and Bluetooth | Not needed at all |
| **DM resends** | Up to the phone app; if the phone is off, nothing happens | **Built into the device: route first, then a fresh search** |
| **Repeater admin** | In the phone app | **On the device, with a remembered password** |
| **Offline maps** | On the phone, if you've downloaded them | **On the device, from the SD card (T-Deck)** |
| **Cost** | Free | **Free and open source, nothing locked behind a paywall** |

You can still pair the MeshCore phone app with Saitama over Bluetooth if you'd
like to.

## Which device should I get?

| | LilyGo T-Deck Plus | M5Stack Cardputer ADV |
|---|---|---|
| **In short** | The full experience, with touch | Small, cheap and pocketable |
| **Screen** | 320 × 240, touch | 240 × 135, no touch |
| **Touchscreen** | **Yes: full touch support on every screen** | No |
| **Typing** | Keyboard + trackball | Small keyboard |
| **GPS** | Built in | On the LoRa module |
| **Apps** | All of them, including maps, radio tools, music and games | The essentials: chat, contacts, repeaters, finder, signal, GPS |
| **Status** | Main device, well tested | Beta, tested |

If you want a touchscreen that works like a phone, maps or the radio tools, get the
T-Deck. If you want something
pocket-sized for messaging and checking the mesh, the Cardputer does the job.
