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
- **Overcast skies** (with a client patch, `Weather.Overcast`). The 3.3.5a client never changes its sky for
  weather. With the patch, the sky turns grey as a front builds over a zone: softer sun, denser
  clouds, closer grey fog, and back again as it tapers. Heavy rain is darker than drizzle, and
  thunderstorms, black rain and black snow are darkest. Snow brings a white-grey sky and
  sandstorms a dusty one.

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
- A WoW 3.3.5a (12340) client. No client patch or addon is needed, except for overcast skies
  (below).

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

## Overcast skies

Weather in 3.3.5a is only what falls: the sky, the sun and the fog stay as they are. (The client
has code to blend in a light's "storm" colours, but nothing in it ever turns that on.) What the
server can do is lay a light of its own over a player's lighting: `SMSG_OVERRIDE_LIGHT` names a
`Light.dbc` row to replace, the row to draw there and a fade time. The client keeps a list of
these swaps and blends each one over everything it has lit so far, by the same falloff as a zone
light. That makes a see-through layer possible:

- a **palette** is a `Light.dbc` row with overcast colours (its `LightParams`, `LightIntBand` and
  `LightFloatBand` rows): grey sky and horizon, fog of the horizon's colour and closer, weak
  sunlight, a dim sun, dense clouds, and a darker sky at night. There are four: rain, storm
  (thunder, black rain, black snow), snow and sand.
- a **weight** is a `Light.dbc` row whose reach covers the whole map but whose middle is
  millions of yards straight up, so every place on the map is the same share of the way out. A
  palette swapped onto it is drawn at that share everywhere. The patch has weights from 0.05 to
  0.90.

A palette at a weight is the player's own sky (their zone's lighting, its time of day and its
night) pulled that far toward the palette. The module picks the weight from how heavy the
zone's weather is (`Weather.OvercastLight` at the faintest, `Weather.OvercastHeavy` at the
heaviest), moves it a step at a time, and lets the client fade each step over about as long as
the weather took to get there (3 to 20 seconds). A new kind of weather or another zone fades over
10 seconds; on a loading screen it is there at once. Only continents get it.

The rows have to be in the players' client, so overcast skies need a **client patch** with the
four lighting tables plus these rows, and the table `mod_weather_overcast`
(`data/sql/db-world/mod_weather_overcast.sql`) has to name the same rows: **the patch and that
file come from the same build.** The rows in this repository were built for one client with
lighting patches of its own; a client without that patch ignores the swaps and sees no
overcast, but a client with other custom lighting rows under the same IDs would get them, so
`Weather.Overcast` is off by default: turn it on when your players have the patch the rows came
from. A builder for other clients is not part of this repository yet. With `Weather.Overcast` on
and no rows in the table, the worldserver logs a warning and the rest of the module runs as
before.

**With [mod-night-brightness](https://github.com/buildthehomelab/wow-mod-night-brightness).** That
module sends light swaps too, and the client keeps one list for both, in which order matters.
When both modules are in the server (GCC or Clang), the overcast goes through mod-night-brightness,
which keeps the list: the overcast lies above the player's night brightness level, on rows made
for that level, so an overcast night is as bright as the player picked. `mod_weather_overcast`
has rows for each night level (`level`; 3 is the realm's own night). The two modules find each
other only in the same binary, the default static module build: on MSVC or with
`MODULES=dynamic` they don't, so use one of them there. A `.night` change asked for while the sky
is changing goes before the next overcast step.

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
| `.weather sky rain\|storm\|snow\|sand [weight]` | Shows an overcast to you only, at the step nearest the weight (0 to 1, default 0.6), until you change zones. `.weather sky off` ends it. Needs the client patch. |
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
- Overcast skies are continents only, like fog. Isle of Quel'Danas keeps stock weather and gets
  no overcast. The stock star dome is drawn the same through an overcast night.
- Players in a zone change overcast together once a second at most; the client fades between
  steps, and waits for one change to finish before the next. Heavy rain and a thunderstorm
  trade palettes only after the new one has lasted 30 seconds.

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
- **No overcast**: check the startup log for "overcast skies" (the number of steps loaded, or a
  warning that `mod_weather_overcast` is empty), that the client has the patch the rows came
  from, and `.weather info` in the zone ("Overcast: ..."). `.weather sky rain 0.9` shows one
  regardless of the weather.

## Credits

Author: [buildthehomelab](https://github.com/buildthehomelab)

## License

MIT, see [LICENSE](LICENSE).
