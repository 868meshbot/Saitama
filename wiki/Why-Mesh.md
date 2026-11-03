# Why a mesh? Why MeshCore?

## Messages without the phone network

Your phone needs a phone mast or Wi-Fi to send anything. Usually that's fine. But
there are plenty of times when it isn't:

- **Out in the hills, at sea, or deep in the countryside**, where there's no signal.
- **At a festival, a big match or a protest**, where thousands of phones jam the
  local mast and nothing gets through.
- **In a power cut, storm or flood**, when masts go down or run out of battery.
- **When you'd simply rather not** put every message through a phone company, an
  app store and somebody's servers.

A **mesh** is a way round all of that. Each device is a small radio. It talks
directly to other devices nearby, with no mast, no internet and no monthly bill.

## How a mesh works (the short version)

Imagine passing a note across a crowded room. You can't reach the far side, so you
hand it to someone near you, who hands it on, and so on until it arrives.

A mesh does the same with radio messages:

1. You type a message. Your device sends it out by radio.
2. Any device in range hears it. Some of them are **repeaters**: small, always-on
   radios, often on a roof or a hill, whose job is to pass messages on.
3. Each repeater sends it on again, one **hop** further, until it reaches the
   person it's for.

The more people and repeaters there are, the further messages travel. Nobody owns the
network: it's made of everyone's devices together.

## Why LoRa?

The radios use **LoRa** ("long range"). It sends small amounts of data a long way
on very little power:

- **Distance:** a few kilometres between handhelds in town is normal. From a hill or
  a high repeater it can be tens of kilometres.
- **Battery:** the radio sips power, so a device lasts for days, not hours.
- **No licence needed:** LoRa uses frequency bands that anyone may use without a
  radio licence (868 MHz in the UK and Europe, 915 MHz in the US and Australia).

The catch: LoRa is slow. It's made for text messages, not photos or video. That is
exactly what you want in an emergency: short messages that get through.

## Why MeshCore?

There's more than one way to run a LoRa mesh. **MeshCore** is the one Saitama uses,
because it's good at the things that matter:

- **It finds a route, then uses it.** The first message to someone spreads out to
  find them (a **flood**). After that, MeshCore remembers the path, so later
  messages go straight there instead of filling the airwaves. That keeps the
  network quiet, so it works with many more people.
- **Repeaters are separate.** Your handheld doesn't relay everyone else's traffic,
  which saves your battery. Dedicated repeaters do that job.
- **It's encrypted.** Direct messages can only be read by you and the person you
  sent them to. Private channels can only be read by people who have the key.
- **It's open source and free**, with a growing community and repeaters already
  running in many areas.

## Why messaging matters

When things go wrong, the first thing people need is to say "I'm OK", "where are
you?" or "we need help here". A text message is the smallest, most reliable way to do
that. A mesh that's already set up and already in your pocket means you have it
ready before you need it.

And on a normal day it's simply fun: chatting with people nearby, seeing who's
around, and being part of a network that you and your neighbours run yourselves.
