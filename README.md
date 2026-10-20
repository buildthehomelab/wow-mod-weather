# Weather

An [AzerothCore](https://www.azerothcore.org/) (WotLK 3.3.5a) module that makes the world's
weather feel real: it comes in fronts that build up, peak and fade, neighbouring zones share
them, and many more zones get weather at all.

- **Fronts, not dice rolls.** Stock AzerothCore re-rolls each zone every 10 minutes and can jump
  from clear skies to a downpour in one step. Here, a shower starts as a faint drizzle, builds
  over a few minutes, drifts up and down like gusts at its height, tapers off and leaves a dry
  spell behind. Most showers stay light, some reach medium, and only about one in ten becomes a
  real downpour.
- **Regions.** Neighbouring zones share one front, so a storm over Elwynn is also over Westfall
  and Redridge, and you don't ride out of the rain at a zone line. Each zone turns the front into
  its own climate: the same front snows on Dun Morogh and rains on Loch Modan.
- **Milder cities.** Stormwind, Orgrimmar, Darnassus and the other capitals feel fronts at 60%,
  so they get drizzle when the land around them gets rain.
- **Nothing pops.** A zone's weather only ever eases toward what it should be. Rain turning to
  snow, or rain giving way to mist, fades one out before the other comes in.
- **More zones.** Stock AzerothCore only has weather in 35 zones. This adds 43 more, including
  all of Northrend, most of Outland, the Barrens, Durotar, Ashenvale, Westfall, Silverpine,
  Felwood and the blood elf and draenei starting zones, each with chances that fit its climate
  for every season.
- **Thunderstorms.** In zones like Stranglethorn, Swamp of Sorrows, Grizzly Hills, Sholazar
  and Netherstorm, a front that gets heavy can bring thunder at its height.
- **Fog and mist.** Duskwood, Tirisfal, Silverpine, Howling Fjord, the swamps and a few others
  get fog under clear skies, and mist can settle there for a few minutes after rain.
- **Black rain and black snow.** Rain falls black in the Blasted Lands, Burning Steppes,
  Searing Gorge, Deadwind Pass, Eastern Plaguelands, Felwood, Hellfire Peninsula and Shadowmoon
  Valley. Snow falls black in Icecrown.

Seasons follow the real calendar, so winter (from about December 20) brings more snow to
the zones that get it.

## How it works

The module takes each zone's weather over from the core. It attaches a weather script
(`mod_weather`) to every `game_weather` row and stretches `ChangeWeatherInterval` so the core
stops rolling on its own.

- **Regions** (`mod_weather_region`) group zones that share fronts. A zone in no region is a
  region of its own.
- **Fronts.** Once a region's dry spell is over, it rolls for a front every 10 minutes, with its
  wettest zone's `game_weather` chance for the season. Each zone then joins the front as often as
  its own chance says, next to that wettest zone, and picks rain, snow or sandstorm from its own
  chances. A zone's `strength` scales how hard the front hits it.
- **What a zone shows** eases toward the front at a capped rate, and only changes kind (rain,
  snow, sandstorm, fog) after the old kind has faded out.
- **On continents** the weather is sent as the map's zone override, the same thing the Sunwell
  event uses for Isle of Quel'Danas, so players who enter a zone see exactly what's there,
  including drizzle lighter than the core can send. **Zones inside instances** (Zul'Gurub,
  Stratholme, battlegrounds) go through the core's weather instead, so they get rain, snow,
  thunderstorms and black rain, but no fog, black snow or faint drizzle.
- **Isle of Quel'Danas** keeps stock weather, because the Sunwell event sets it.

With the shipped chances, a zone at 20% is wet about a quarter of the time (stock AzerothCore:
about half), with a downpour under 2% of the time. Raise or lower a zone's `game_weather`
chances to change that.

The zone rules live in the world table `mod_weather_zone`: `fog_chance`, `thunder_chance`,
`black_rain` and `black_snow` per zone. Regions and strength live in `mod_weather_region`. Edit
either and run `.weather reload` to apply changes without a restart; fronts already running
carry on. New `game_weather` rows need a restart, because the core only loads that table at
startup.

## Requirements

- An [AzerothCore](https://github.com/azerothcore/azerothcore-wotlk) WotLK (master) server. The
  module has no other module or core-fork dependencies.
- `ActivateWeather = 1` in `worldserver.conf` (the default).
- A WoW 3.3.5a (12340) client. No client patch or addon is needed.

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

See `conf/mod_weather.conf.dist`. You can set how long each part of a front lasts, how often
fronts reach medium or heavy, how often dry regions roll, and turn fog, mist after rain,
thunderstorms, black rain and black snow off on their own.

## GM commands

| Command | What it does |
|---|---|
| `.weather info` | Shows your zone's region, the region's front (phase, strength, time left), the zone's rules and what it shows. |
| `.weather start [light\|medium\|heavy]` | Starts a front in your region now. Every zone in the region joins it. |
| `.weather stop` | Makes your region's front taper off now. |
| `.weather show <state> [grade]` | Shows a weather state to you only, to check how the client draws it. The grade (0 to 1, default 0.9) is the strength. Lasts until the zone's weather next changes (every few seconds during a front) or you leave. |
| `.weather reload` | Reloads `mod_weather_zone` and `mod_weather_region`. |

States for `.weather show`: `0` clear, `1` fog, `3`/`4`/`5` light/medium/heavy rain,
`6`/`7`/`8` light/medium/heavy snow, `22`/`41`/`42` light/medium/heavy sandstorm, `86`
thunderstorm, `90` black rain, `106` black snow.

The core's `.wchange` no longer sticks: the module puts the zone's own weather straight back.
Use `.weather start` and `.weather stop` instead.

## Notes

- Weather indoors, in caves and under roofs is hidden by the client, not the server.
- Fronts keep moving while nobody's in a zone, so you arrive in whatever weather is there.
- Fronts aren't saved: after a restart every region starts dry and rolls within 10 minutes.
- Weather a core script sets on a zone that has `game_weather` (the Scourge Invasion event's
  storms) is replaced by the zone's own weather.
- Weather is visual only. Nothing in the game changes because of it.

## Troubleshooting

- **No new weather and the worldserver logs that `game_weather` has no row for Borean Tundra**:
  the module's SQL in `data/sql/db-world/updates` wasn't applied. Apply it, or let the
  worldserver's updater do it.
- **The module builds but nothing changes**: the folder isn't named `mod-weather`, so
  AzerothCore never calls the loader. Clone into `mod-weather` exactly.
- **Edits to `mod_weather_zone` or `mod_weather_region` don't show**: run `.weather reload`. New
  `game_weather` rows need a worldserver restart.
- **Fronts start and end with a pop instead of a drizzle**: the client may not draw weather as
  faint as `Weather.FaintGrade`. Check with `.weather show 3 0.1` under clear skies, and raise
  `Weather.FaintGrade` to the faintest grade you can see.
- **Fog or black snow never appears in a dungeon or battleground**: expected. Those need a
  continent, so instance zones only get thunderstorms and black rain.

## Credits

Author: [buildthehomelab](https://github.com/buildthehomelab)

## License

MIT, see [LICENSE](LICENSE).
