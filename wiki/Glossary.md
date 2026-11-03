# Glossary

The words you'll meet in Saitama and MeshCore, in plain English.

| Word | What it means |
|---|---|
| **Mesh** | A network made of everyone's radios, passing messages to each other with no phone mast or internet. |
| **MeshCore** | The free, open-source system that Saitama uses to run the mesh: how messages are routed, encrypted and passed on. |
| **LoRa** | "Long Range": the type of radio used. Slow, but it goes a long way on very little battery. |
| **Node** | Any device on the mesh: your handheld, a friend's, or a repeater. |
| **Companion** | A device a person carries and chats on, like yours. Shown as **C** in Heard. |
| **Repeater** | An always-on radio, usually somewhere high, that passes messages on to extend the mesh. Shown as **R** in Heard. |
| **Hop** | One step from one radio to the next. A message that went through two repeaters took 3 hops. |
| **Advert** | A short "I'm here" announcement your device sends out. People can only message you once they've heard it. |
| **Flood** | Sending a message out to everyone, with every repeater passing it on, to find someone when the route isn't known yet. |
| **Path / route** | The list of repeaters a message goes through to reach someone. Once known, messages go that way instead of flooding. |
| **Channel** | A group chat. Everyone with the channel's key can read and post. |
| **Public** | The channel every MeshCore device has. Anyone can read it. |
| **Hashtag channel** | A channel whose name starts with `#`. Its key is made from the name, so anyone who types the same name joins it. |
| **DM (direct message)** | A private message to one person. Only the two of you can read it. |
| **Verified / green padlock** | A DM that's been checked as really coming from that contact. It can't be faked without their secret key. Channel messages can't be verified. |
| **Key / PSK** | The secret that locks a channel. Share it only with people you want in the channel. |
| **Identity** | Your device's unique key pair: its address on the mesh, and its proof that messages are really from you. |
| **Region / scope** | A tag such as `#london` on a message. Some repeaters only pass on messages with their region's tag. |
| **RSSI** | How strong a signal was. Closer to 0 is stronger: −60 is very strong, −120 is very weak. |
| **SNR** | How clear a signal was above the background noise. Higher is better. |
| **Noise floor** | How much background radio noise there is. Lower is better. |
| **Airtime** | How long your radio has spent transmitting. In Europe the law limits how much of the time you may transmit. |
| **Preset** | A ready-made set of radio settings for a country or mesh. Everyone nearby must use the same one. |
| **Firmware** | The software that runs on the device itself. Saitama is firmware. |
| **Flash / flashing** | Installing firmware onto the device from a computer. |
| **SD card** | The memory card that stores your backups, message history and maps. |
| **Storage password** | The password that locks the secrets saved on your SD card. Change it from the default. |
