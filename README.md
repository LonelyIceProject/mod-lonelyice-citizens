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
      -DAC_PLUGIN_ABI=lonelyice-ac-1 "-DAC_PLUGIN_SOURCE_DIRS=<path>/mod-playerbots;<path>/mod-lonelyice-citizens"
cmake --build build --config RelWithDebInfo
```

The plugin is laid out in `bin/<config>/plugins/lonelyice.citizens/`. Copy that folder into the server's `plugins` folder
(`PluginsDir` in worldserver.conf); [LonelyIce](https://github.com/LonelyIceProject/lonelyice) does this for you.

**As a classic static module**: clone into `modules/mod-lonelyice-citizens` of the core and rebuild.
## Configuration

`conf/mod_lonelyice_citizens.conf.dist`: `Citizens.Enable`, citizens per city (`Citizens.Count.<City>`),
home race share, minimum levels, release delay and activity weights.
## License

GNU General Public License v2.0 or later, see [LICENSE](LICENSE). Part of the
[LonelyIce](https://github.com/LonelyIceProject/lonelyice) single-player project.
