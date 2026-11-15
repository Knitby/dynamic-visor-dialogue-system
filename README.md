# Dynamic Visor Dialog system (DVDs) 1.0

by Knitby

## What it does

A KCSE-dependent mod that dynamically keeps your visor state persistent during dialogue.
Your visor stays lowered during dialogue, and it only does so if you lowered the visor before starting that dialogue.

## Requirements

- Kingdom Come: Deliverance II version 1.5.6. On any other version the mod does nothing.
- Kingdom Come Script Extender (KCSE)

## Install

Copy the `dvds` folder into your game's `Mods` folder:

```
...\KingdomComeDeliverance2\Mods\dvds
```

The result should contain `Mods\dvds\mod.manifest` and `Mods\dvds\KCSE\Plugins\DVDS.dll`.

If you use a `mod_order.txt`, add `dvds` (without quotation marks) as a new line in it. Mod order doesn't matter, since this is not a traditional .pak mod.

## Uninstall

Delete the `dvds` folder from `Mods`. The mod is completely safe to remove mid-playthrough.

## Using it

The game resets your visor to raised when you load a save. After loading, press the visor key once to lower it. From then on, dialogues keep it lowered until you raise it again.

## Source code

Source: <https://github.com/Knitby/dynamic-visor-dialogue-system>

DVDS is built against libKCD2 (see its repository for build steps): <https://github.com/JerryYOJ/libKCD2>

## License

DVDS is free software, released under the GNU General Public License version 3. See the [LICENSE](LICENSE) file.
It comes with no warranty. It links against libKCD2 (GPLv3) and uses MinHook. Their notices are in [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt).

## Credits

KCSE and libKCD2 and their authors. The plugin also uses MinHook as a hooking library.
