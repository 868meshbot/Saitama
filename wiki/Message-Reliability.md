# Getting your message through

Radio is messy. A message can be lost because a repeater was busy, someone walked
behind a hill, or two radios talked at the same moment. A good mesh device doesn't
pretend that never happens: it notices, tries again, and tells you what happened.

This is where Saitama puts a lot of its effort.

## You always know if a DM arrived

Every direct message you send ends in one of two ways, shown on your message bubble:

- **✓ Delivered.** The other person's device confirmed it got the message.
- **✕ Not delivered.** Saitama tried everything it could and got no confirmation.

There's no "sent… and who knows?" A message never just quietly disappears.

## It tries again for you, and changes its approach

If a DM isn't confirmed in time, Saitama doesn't simply send the same thing again
and hope. It works through a plan:

1. **Try the known route again.** If it knows a path to the person, it resends along
   that path, up to **5 times**.
2. **Give up on a bad route.** If the route keeps failing, the route is probably
   broken (a repeater is down, or the person has moved). Saitama **forgets it**,
   both in memory and on the SD card, so it won't reuse it after a restart.
3. **Search the whole mesh.** It then sends the message as a **flood**, which
   spreads out through every repeater to find the person wherever they are now, up
   to **3 times**.
4. **Learn the new route.** When the reply comes back, the new path is remembered
   for next time.

You can change the number of tries in **Settings > DM Retries**.

### Clever details that make retries work

- **Repeaters don't ignore the retries.** Repeaters skip messages they've already
  passed on. Saitama makes each retry a slightly different packet, so repeaters
  forward it as new.
- **No double messages.** The person you're messaging still only sees your message
  **once**, however many retries it took.
- **Several at once.** Up to 4 DMs can be on their way at the same time, each with
  its own retries and timer. A slow one doesn't hold up the rest.

## Routes are remembered, and kept fresh

The first DM to someone has to search for them. After that, Saitama remembers the
route so later messages go straight there: faster, and less traffic for everyone.

- **Only proven routes are kept.** A route is saved after a message along it is
  confirmed, with the time it was confirmed. Guesses aren't saved.
- **Routes survive a restart.** They're saved with your contacts and repeaters on
  the SD card, and loaded back when the device starts.
- **Repeaters are ready at once.** Saved repeaters are loaded at start-up, so you
  can log in to one straight away, without waiting to hear from it again.
- **You're in control when you need to be.** **Show Path** on a message shows the
  route it took. In Contacts and Repeaters you can **Set Path** by hand or **Reset
  Path** to start fresh, and on the T-Deck **Trace** shows the signal at every hop.

## See that your channel message got out

Channel messages go to everyone, so nobody sends back a "got it". Instead, Saitama
listens for 30 seconds after you send and counts the repeaters it hears passing your
message on. Your bubble shows **repeated ×3** (for example), so you know it reached
the network and how widely. Select the bubble to see which repeaters picked it up.

## Built for big, busy meshes

- **Region scopes.** On meshes where repeaters only pass on messages for their own
  area, Saitama tags your messages correctly, and the **Regions** app finds out
  which tags your local repeaters use. See [Regions and scopes](Regions-and-Scopes).
- **Fewer mix-ups on large networks.** Routes name each repeater by a short code.
  On a mesh with hundreds of repeaters, two can share a 1-byte code. **Settings >
  Radio Mode > Routing** can use 2- or 3-byte codes instead.
- **New people aren't locked out.** When the device's table of known nodes is full,
  Saitama makes room by dropping the least recently heard non-favourite, rather
  than refusing newcomers (who then couldn't message you).

## A device you can rely on

Getting a message through is no use if the device has crashed or lost your data.

- **Safe saving.** Contacts and repeaters are written to a temporary file first and
  only then swapped in, with the previous copy kept as a backup. A flat battery
  mid-save doesn't wipe your address book.
- **Low-memory protection (Cardputer).** The Cardputer has very little memory. If it
  runs short, Saitama closes the screens you aren't using and carries on, instead of
  crashing.
- **Everything backed up.** Your identity, contacts, routes and settings are on the
  SD card and come back after an update or on a new device.
