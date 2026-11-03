# Backup and security

## What's on the SD card

Saitama keeps a backup of everything important in the `/ops/` folder on the SD card,
so a reflash or a new device picks up where you left off:

| File | Holds |
|---|---|
| `/ops/settings.json` | Your settings. Channel keys are stored encrypted. |
| `/ops/contacts.json` | Saved contacts. |
| `/ops/repeaters.json` | Saved repeaters, with remembered admin passwords encrypted. |
| `/ops/identity.enc` | Your node identity (its private key), encrypted. |
| `/ops/advert_config.json` | The automatic advert interval. |
| `/ops/msgs/` | Message history, one file per channel or DM. |

These are written automatically as things change. **Settings > Backup & Restore**
writes or reads contacts, repeaters and settings on demand.

The files are the same on the T-Deck and the Cardputer, so you can move a card
between them. The Cardputer holds fewer contacts (100) and repeaters (60) than the
T-Deck (250 each).

## The storage password

Secrets on the card (channel keys, remembered repeater passwords and the identity
backup) are encrypted with AES-256-GCM using your **storage password**.

- It starts as `changeme1`, which is published. **Change it** in **Settings > Storage
  Password**: until you do, the row shows **Default** in red, and anyone with your SD
  card can read those secrets.
- The password itself is kept on the device, never on the card.
- **Remember it.** On a new or fully reflashed device, the boot screen asks for it to
  unlock your identity backup. If you can't give it, the device runs on a temporary
  identity until you do.

## Your identity

Your identity is a key pair. The public half is your address on the mesh; the
private half proves messages are from you. It's kept on the device and backed up,
encrypted, to `/ops/identity.enc`.

- **Keep the backup.** If you lose the identity, you become a new node: your
  contacts must add you again, and DMs to the old one can't be read.
- **Settings > Generate Identity** deliberately makes a new one.
- **Terminal:** `identity show` shows your public key; `identity restore` reloads the
  backup.

## Message security

- All mesh traffic is encrypted by MeshCore.
- **Direct messages are verified.** Only you and the recipient can derive the key,
  so a DM can't be forged in a contact's name. A green padlock (by the sender on
  the T-Deck, in the title bar on the Cardputer) shows a verified DM. See
  [Chat and channels](Chat-and-Channels).
- **Channel messages are not authenticated.** Anyone who has the channel key can post
  under any name. Treat names on channels as a claim, not proof.
- **Public** uses a key every MeshCore node has. Don't post anything private there.

## Bluetooth

The MeshCore phone app can connect over Bluetooth (**Settings > Bluetooth**). The
pairing PIN (**Settings > Bluetooth PIN**) is random for each device. Changing it
unpairs every phone.

## Reporting a security problem

Report it privately to the maintainers, not in a public issue. See
[docs/SECURITY.md](https://github.com/868meshbot/Saitama/blob/main/docs/SECURITY.md).
