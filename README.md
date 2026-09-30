<p align="center"><img src="logo.png" width="128" alt="logo"></p>

# mod-lonelyice-citizens

Living capitals. While you are in a capital, a few random bots of that city's faction live there: they
stroll between points of interest, sit, go AFK, talk to each other and greet you. When you leave, they go
back to their normal random-bot life.

## Features

- Citizens per city (Stormwind, Orgrimmar, Ironforge, Undercity, Thunder Bluff, Darnassus, Exodar,
  Silvermoon, Shattrath, Dalaran), mostly of the city's home race, with level limits per city.
- Bots already in the city are used first; the rest arrive out of sight.
- Weighted activities (stroll, afk, sit, social, fool) and emote cooldowns.
- Nothing is stored in the database; bots are released a while after the last real player left.
- `.citizen` GM command for inspecting and testing.

## Requirements

[LonelyIceProject/mod-playerbots](https://github.com/LonelyIceProject/mod-playerbots): uses its external hooks
to pin bots to a city.
## Install

This module is written for [LonelyIceProject/azerothcore-wotlk](https://github.com/LonelyIceProject/azerothcore-wotlk),
a fork of AzerothCore with runtime plugins, and builds in two ways.

**As a plugin** (the core built with `-DWITH_DYNAMIC_LINKING=ON`):

```
cmake -S azerothcore-wotlk -B build -DWITH_DYNAMIC_LINKING=ON -DWITH_PLAYERBOTS_HOOKS=ON ^
      -DAC_PLUGIN_ABI=lonelyice-ac-2 "-DAC_PLUGIN_SOURCE_DIRS=<path>/mod-playerbots;<path>/mod-lonelyice-citizens"
cmake --build build --config RelWithDebInfo
```

The plugin is laid out in `bin/<config>/plugins/lonelyice.citizens/`. Copy that folder into the server's `plugins` folder
(`PluginsDir` in worldserver.conf); [LonelyIce](https://github.com/LonelyIceProject/lonelyice) does this for you.

**As a classic static module**: clone into `modules/mod-lonelyice-citizens` of the core and rebuild.
## Configuration

`conf/mod_lonelyice_citizens.conf.dist`: `Citizens.Enable`, citizens per city (`Citizens.Count.<City>`),
home race share, minimum levels, release delay and activity weights.
## Support

LonelyIce is free, with no ads and no paid features. If it is useful to you, you can
[buy me a coffee](https://buymeacoffee.com/darthgelum): it pays for the server, code signing and development time.

<a href="https://buymeacoffee.com/darthgelum"><img src=".github/buy-me-a-coffee.png" alt="Buy me a coffee" width="303"></a>

## License

GNU General Public License v2.0 or later, see [LICENSE](LICENSE). Part of the
[LonelyIce](https://github.com/LonelyIceProject/lonelyice) single-player project.

### Blizzard Entertainment

World of Warcraft®, Warcraft®, Wrath of the Lich King® and Blizzard Entertainment® are trademarks or registered
trademarks of Blizzard Entertainment, Inc. in the U.S. and/or other countries.

The game and everything in it belong to Blizzard Entertainment, Inc.: the game client and its program files, data
files and archives, maps and terrain, models, textures, art, animations, interface, music, sounds, voices, texts,
names, lore, characters, creatures, spells, items, quests and every other part of the game. All of it remains
Blizzard's property wherever it appears, including the data a server extracts from your client on your own computer
(game tables, maps, collision and navigation data).

LonelyIce is an unofficial, non-commercial fan project. It is not affiliated with, endorsed, sponsored, approved or
supported by Blizzard Entertainment, Inc. Blizzard's names are used only to say which game client the project works
with.

This repository contains no files from the game client, and LonelyIce neither distributes nor downloads any. It works
only with a copy of the game you already own. Keep the data extracted from your client to yourself: it is Blizzard's
property and is not ours or yours to share.

The license above covers only the code and files of this project and the works it is based on. It grants no rights
to anything that belongs to Blizzard Entertainment, Inc. All other trademarks belong to their respective owners.
