# Stored Secrets

Saitama can remember repeater admin passwords so you do not retype them on
every login. This document says exactly what that storage protects against,
because "encrypted" on its own does not tell you.

## What is stored, and where

| Where | Contents |
|---|---|
| `/ops/repeaters.json` (SD card) | `"pwEnc"` — a 92-byte sealed blob, hex encoded. Ciphertext only. |
| NVS namespace `opscrypt` (internal flash) | 16-byte random salt, and the **storage password** (default `changeme1`). |

The storage password deliberately does **not** live in `ops::Config`.
`config::save()` mirrors every setting to `/ops/settings.json`, which would
write the key material onto the very card the encryption is meant to protect.

## Threat model

**Protected:** someone takes the SD card. `/ops/` is a documented,
reflash-proof backup and every other file in it is readable JSON. Admin
passwords in that file would otherwise be plaintext. With this, a lifted card
yields ciphertext and nothing more.

**Not protected:** someone has the device, or a dump of its internal flash.
They have the salt and the storage password, so they have the key. There is no
way around this while the device must decrypt unattended at boot — and being
able to do so is the entire point of the feature. Prompting for a passphrase
on every boot would protect against this, and would also be the thing the
feature exists to avoid.

Do not treat a remembered repeater password as safe against a stolen T-Deck.
Treat it as safe against a stolen SD card.

## Construction

```
key  = PBKDF2-HMAC-SHA256(storage password, salt, 20 000 iterations, 32 bytes)
blob = IV(12) || AES-256-GCM(key, IV, plaintext, AAD = pubKeyPrefix[4]) (64) || tag(16)
```

- **Fixed 64-byte plaintext slot**, zero padded. Every blob is 92 bytes, so the
  ciphertext length says nothing about the password length. Secrets of 64
  characters or more are rejected rather than truncated.
- **Fresh random IV per seal** (`esp_fill_random`). A repeated IV under one key
  breaks GCM completely.
- **AAD is the repeater's 4-byte key prefix.** A blob copied from one entry to
  another in the JSON file will not authenticate, so a remembered password
  cannot be transplanted onto a different repeater by editing the card.
- **GCM tag is checked**, so a wrong storage password or an edited file fails
  cleanly and visibly instead of returning garbage that gets transmitted.
- The PBKDF2 iteration count buys nothing against an attacker holding the flash
  (they have the password too). It is there for the case the guard actually
  covers: a lifted card plus a guessable password.

AES-256-GCM and PBKDF2 come from mbedtls, which ships with the ESP32 Arduino
core — no new dependency. On the ESP32-S3 mbedtls routes AES through the
hardware accelerator.

## Changing the storage password

Settings > Storage Password. The list row reads **Default** until you change
it, because `changeme1` is published in the source, in this file, and in every
release — it is a placeholder, not a secret.

Changing it re-encrypts every stored secret in a single pass:

1. The new password is verified against the old one and **persisted first**.
   That is the step that can fail, so it is done first and checked.
2. `seal()` then uses the new key while `unseal()` still uses the old one, so
   one decrypt-then-encrypt pass over the records is enough.
3. Any record that will not decrypt is **forgotten rather than left
   unreadable**, and the dialog reports how many.

If the device loses power mid-pass, the worst case is losing remembered
passwords, which you can re-enter — not a device that cannot read its own
storage password.

## Using it

In Repeaters, tap a repeater > Admin Login. Tick **Remember password** and log
in; the password is sealed and saved. Next time the box is pre-filled and
ticked, so logging in is one keypress.

Untick the box and log in to forget the stored password.

If the box says *Remember (unavailable)*, `crypto::init()` could not open NVS
and nothing can be stored this session. If the hint reads *(stored one
unreadable)*, a blob exists but will not decrypt — the storage password was
changed while that record was not re-keyed, or the card was edited.
