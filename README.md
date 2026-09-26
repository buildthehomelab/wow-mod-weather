# Weather

An [AzerothCore](https://www.azerothcore.org/) (WotLK 3.3.5a) module that gives the world more
weather: rain, snow and sandstorms in the zones that never had any, plus fog, thunderstorms,
black rain and black snow.

- **More zones.** Stock AzerothCore only has weather in 35 zones. This adds 43 more, including
  all of Northrend, most of Outland, the Barrens, Durotar, Ashenvale, Westfall, Silverpine,
  Felwood and the blood elf and draenei starting zones. Each zone gets chances that fit its
  climate for every season. The Storm Peaks snows most of the year, Sholazar rains, and Durotar
  gets sandstorms.
- **Weather you can see.** The core sends most of the light weather it rolls to the client as
  clear skies, so a zone can be "raining" on the server with nothing on screen. The module draws
  that weather as light rain, snow or sandstorm.
- **Thunderstorms.** In zones like Stranglethorn, Swamp of Sorrows, Grizzly Hills, Sholazar
  and Netherstorm, heavy rain can turn into a thunderstorm. It goes back to rain as it eases.
- **Fog.** Duskwood, Tirisfal, Silverpine, Howling Fjord, the swamps and a few others get
  fog under clear skies.
- **Black rain and black snow.** Rain falls black in the Blasted Lands, Burning Steppes,
  Searing Gorge, Deadwind Pass, Eastern Plaguelands, Felwood, Hellfire Peninsula and Shadowmoon
  Valley. Snow falls black in Icecrown.

Seasons follow the real calendar, so winter (from about December 20) brings more snow to
the zones that get it.

## How it works

The core picks each zone's weather from `game_weather`, and none of its random rolls ever
produce fog, thunderstorms, black rain or black snow. This module attaches a weather script
(`mod_weather`) to every `game_weather` row and changes the core's weather as it happens:

- **Thunderstorms and black rain** are core weather types, so the module switches the zone's
  weather to them and the core carries them on from there.
- **Fog and black snow** have no core weather type, only a state the client understands. The
  module sets them as the map's zone override, the same thing the Sunwell event uses for Isle of
  Quel'Danas, so players who enter the zone later see them too. That only works on continents:
  zones inside instances (Zul'Gurub, Stratholme, battlegrounds) get thunderstorms and black rain
  but never fog or black snow.

The rules live in the world table `mod_weather_zone`: `fog_chance`, `thunder_chance`,
`black_rain` and `black_snow` per zone. Edit it and run `.weather reload` to apply changes
without a restart. New `game_weather` rows need a restart, because the core only loads that
table at startup.

## Install

```bash
cd azerothcore-wotlk/modules
git clone https://github.com/buildthehomelab/wow-mod-weather.git mod-weather
```

Clone into `mod-weather` exactly, because AzerothCore derives the loader symbol from the folder
name. Then re-run CMake, rebuild, and copy `conf/mod_weather.conf.dist` to your config folder
as `mod_weather.conf`.

The SQL in `data/sql/db-world/updates` adds the zones and the rules, and the worldserver's
updater applies it. If the zones are missing at startup, the worldserver logs an error saying
so.

Make sure `ActivateWeather = 1` in `worldserver.conf` (it's the default).

## Config

See `conf/mod_weather.conf.dist`. You can turn each part off on its own: light weather, fog
(and how often it rolls), thunderstorms, black rain and black snow.

## GM commands

| Command | What it does |
|---|---|
| `.weather info` | Shows the rules for the zone you're in and its current weather. |
| `.weather show <state> [grade]` | Shows a weather state to you only, to check how the client draws it. The grade (0 to 1, default 0.9) is the strength. Lasts until the zone's weather changes or you leave. |
| `.weather reload` | Reloads `mod_weather_zone`. |

States for `.weather show`: `0` clear, `1` fog, `3`/`4`/`5` light/medium/heavy rain,
`6`/`7`/`8` light/medium/heavy snow, `22`/`41`/`42` light/medium/heavy sandstorm, `86`
thunderstorm, `90` black rain, `106` black snow.

The core's `.wchange <type> <grade>` still works and sets the zone's weather for everyone
(`0` clear, `1` rain, `2` snow, `3` sandstorm, `86` thunderstorm, `90` black rain). It goes
through the module's rules, so in a black rain zone `.wchange 1 1` gives black rain, and a
thunderstorm below grade 0.7 turns back into rain.

## Notes

- Weather indoors, in caves and under roofs is hidden by the client, not the server.
- The core unloads a zone's weather when nobody's in it. Fog or black snow that was showing at
  that point can still be there for the next player to arrive, until the zone's weather next
  changes.
- Weather is visual only. Nothing in the game changes because of it.
