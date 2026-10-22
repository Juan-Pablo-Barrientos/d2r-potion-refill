# Potion Refill

A [D2RLoader](https://d2rloader.net) plugin for Diablo II: Resurrected. When you
drink a potion from your belt, it moves a matching potion from your inventory
into the same belt column.

- Healing refills healing, mana refills mana, rejuvenation refills
  rejuvenation. Stamina, antidote and thawing potions refill their own kind.
- The same potion is used first. Otherwise the closest size is used, and on a
  tie the larger one.
- It works in single player and when you join a TCP/IP game.

The plugin moves the potion the same way you would by hand: inventory to
cursor, then cursor to the belt slot. The game checks each move, so the plugin
can't create or duplicate items.

## Install

1. Install [D2RLoader](https://d2rloader.net). Tested with 1.3.1-beta.
2. Download `d2rl-potion-refill.dll` from the
   [latest release](../../releases/latest).
3. Copy it into `<game folder>\d2rloader\plugins\`.
4. Start the game with D2RLoader.

To remove it, delete the DLL.

### With D2RMM

If you manage mods with [D2RMM](https://github.com/olegbl/d2rmm), use
[Potion Refill for D2RMM](https://github.com/Juan-Pablo-Barrientos/d2r-potion-refill-d2rmm)
instead. It lets you change the settings below in D2RMM and includes the DLL.

## Settings

The first launch creates `<game folder>\d2rloader\config\potion-refill.toml`:

```toml
[potion-refill]
enabled = true

# "family": any healing potion refills healing, and so on.
# "exact":  only the same potion, such as Super Healing for Super Healing.
match = "family"

# Write every refill to d2rloader\logs\potion-refill.log.
log_refills = false
```

A mod can override these settings by shipping
`data\d2rloader\potion-refill\settings.toml` with the same keys. The
[D2RMM mod](https://github.com/Juan-Pablo-Barrientos/d2r-potion-refill-d2rmm)
writes this file. When the plugin finds it, the log says
`Using settings from the active mod`.

## When it does nothing

- **No matching potion:** only the normal inventory is searched. The stash
  and the Horadric Cube are not.
- **Holding an item:** if an item is on your cursor, the plugin waits up to
  five seconds, then skips the refill.
- **Several potions gone at once:** if more than one belt potion disappears
  at the same time (death, removing the belt), it does nothing.
- **Rejected placement:** if the game rejects the belt slot, the potion goes
  back to where it was in your inventory.

## Compatibility

The plugin sends the moves through a D2RLoader function that is not part of
the public plugin SDK, so a D2RLoader update can change it. On startup the
plugin checks that the function still matches what it expects. If it doesn't,
the plugin stays off and writes the reason to
`d2rloader\logs\potion-refill.log`. A new build of the plugin is then needed.

## Build from source

You need Visual Studio 2022 with the C++ desktop tools and a Windows 10/11 SDK.

```powershell
.\build.ps1                 # builds build\d2rl-potion-refill.dll
.\build.ps1 -Install        # also copies it into the game's plugin folder
.\build.ps1 -Install -GameDir "E:\Games\Diablo II Resurrected"
```

The D2RLoader plugin SDK headers are included in `third_party/PluginSDK`
(MIT license).

### Packet notes

These layouts were checked against the D2Game packet handlers in the D2R build
current in October 2026. Packet 0x19 is larger than in classic Diablo II.

| Packet | Action | Layout | Size |
| --- | --- | --- | --- |
| 0x18 | cursor to grid cell | `[id][itemId u32][x u32][y u32][page u32]` | 17 |
| 0x19 | grid cell to cursor | `[id][itemId u32][x u32][y u32][page u32]` | 17 |
| 0x23 | cursor to belt slot | `[id][itemId u32][slot u32]` | 9 |

## License

MIT. See [LICENSE](LICENSE).
