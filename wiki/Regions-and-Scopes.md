# Regions and scopes

Some meshes set their repeaters to forward only traffic tagged with a **region
scope**, such as `#london`. A message without the right tag isn't repeated, so it
only reaches people in direct radio range. If your messages don't seem to get out,
ask locally whether your mesh uses scopes.

## Your default scope

**Settings > Region Scope** has 10 slots:

- Slot 1 is your **default scope**, used for every flood you send (adverts, channel
  messages, DMs that flood). `*` means no scope.
- Slots 2 to 10 are **saved regions** you can pick from elsewhere.

Saitama never adds a region on its own: you choose them.

## Per-channel scope

A channel can use a different scope from the default. In Chat, press the **≡**
button on the channel's row and choose **Configure Scope**.

## The Regions app

**Regions** asks the repeaters you can hear directly (zero hops) which regions they
serve, and lists the answers. Select a region to save it to one of your slots. Your
saved regions are shown as chips under the list.

On the T-Deck, Regions is on the second app page.

## From the Terminal

- `scope` shows the current default scope.
- `scope #<region>` sets it, for example `scope #london`.
- `scope clear` removes it.
