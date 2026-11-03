# Terminal commands

The **Terminal** app takes typed commands. The same commands work over the USB
serial port (115200 baud), where the prompt is `OPS> `. Type `help` for the list.

Received messages also appear in the Terminal, as do replies from repeaters.

## Device

| Command | What it does |
|---|---|
| `help` | List the commands. |
| `get radio` | Show the radio settings. |
| `set name <name>` | Set the device name. |
| `set lat <deg>` / `set lon <deg>` | Set your position by hand. |
| `set freq`, `sf`, `bw`, `tx`, `cr`, `af` + value | Set a radio parameter. |
| `clock [analog]` | Show the time and start the clock screensaver. |
| `time <epoch-seconds>` | Set the clock. |
| `memory` | Free memory. |
| `battery` | Battery voltage and level. |
| `reboot` | Restart. |
| `shutdown` | Power off (T-Deck: click the trackball to turn it back on). |
| `gps [on\|off\|intermittent]` | Set the GPS mode, or show its status. |
| `sd` | SD card status. Also `sd mount`, `sd unmount`, `sd restore`, `sd ls [path]`. |
| `identity show` | Show your node's public key. |
| `identity restore` | Restore the identity from the SD card backup. |
| `screenshot [name]` | Save a screenshot to the SD card. |
| `touch-cali` | Calibrate the touchscreen (T-Deck). `touch-cali reset` undoes it. |
| `saitama-scr` | Start the Saitama screensaver (T-Deck). |

## Messages and contacts

| Command | What it does |
|---|---|
| `card` | Print your business card, to share. |
| `import <card>` | Add a contact from someone's business card. |
| `list [n]` | List saved contacts (the last n). |
| `messages <n\|all>` / `clearmessages` | Show or clear the message log. |
| `find <id>` | Look up a contact by ID. |
| `to <name\|prefix>` then `send <text>` | Send a DM. |
| `public <text>` | Post on the Public channel. |
| `ch3 <text>` … `ch5 <text>` | Post on channel slot 3, 4 or 5. |
| `channels` | List the channels. |
| `channel <1-10>` | Show one channel, including its key in base64. |
| `setchannel <1-10> #<name> [psk]` | Set up a channel. |
| `clearcontacts` / `contacts delete <prefix>` | Remove contacts. |
| `advert [flood]` | Send an advert now. |
| `adverts` | Turn on or off showing adverts in the Terminal as they arrive. |
| `quiet` | Turn new-message popups off or on. |

## Routes and scopes

| Command | What it does |
|---|---|
| `trace <path>` | Trace a route (T-Deck). |
| `getpath <hex>` / `setpath <hex> <path>` | Show or set the route to a node. |
| `resetpath <hex>` / `reset path` | Forget a route. |
| `scope` / `scope #<region>` / `scope clear` | Show, set or clear the default region scope. |
| `pathsize [1\|2\|3]` | Routing hash size in bytes. |

## Repeaters

| Command | What it does |
|---|---|
| `repeaters [secs]` | Ask all repeaters for their status. |
| `replist` / `clearrep` | List or clear saved repeaters. |
| `repeateradmin <hex> [pass]` | Log in to a repeater as admin. |
| `repadmin [<hex>] <cmd>` | Send an admin command (log in first). Replies appear in the Terminal. |

`<hex>` is the start of the node's public key, as shown in Contacts or Repeaters.
