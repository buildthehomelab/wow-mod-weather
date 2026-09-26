-- mod-weather: weather for the zones that have none, and the rules for fog, thunderstorms,
-- black rain and black snow.
--
-- Stock AzerothCore only has `game_weather` rows for 35 zones. Every other zone is always clear,
-- including all of Northrend, most of Outland and a lot of Kalimdor.
--
-- How a `game_weather` row works: every ChangeWeatherInterval (10 minutes by default), a zone
-- with clear skies rolls for new weather. The chance columns are the percent chance of rain,
-- snow or sandstorm ("storm") for that roll, per season. Seasons follow the real calendar:
-- spring starts about March 20, summer June 20, fall September 20, winter December 20. The
-- chances in one season must add up to 100 or less. Once it's raining, the weather gets
-- lighter, heavier or clears on later rolls.
--
-- Idempotent: safe to run again. It only replaces the rows it adds, and only claims a
-- ScriptName that's empty.

-- ---------------------------------------------------------------------------------------------
-- New zones
-- ---------------------------------------------------------------------------------------------

DELETE FROM `game_weather` WHERE `zone` IN (
    4, 8, 40, 46, 51, 130, 1519, 3430, 3433, 3487,
    14, 16, 17, 331, 361, 400, 406, 493, 1637, 1638, 1657, 3478,
    3483, 3518, 3519, 3520, 3522, 3523, 3524, 3525, 3703,
    65, 66, 67, 210, 394, 495, 2817, 3537, 3711, 4197, 4395, 4742);

INSERT INTO `game_weather`
    (`zone`,
     `spring_rain_chance`, `spring_snow_chance`, `spring_storm_chance`,
     `summer_rain_chance`, `summer_snow_chance`, `summer_storm_chance`,
     `fall_rain_chance`,   `fall_snow_chance`,   `fall_storm_chance`,
     `winter_rain_chance`, `winter_snow_chance`, `winter_storm_chance`,
     `ScriptName`)
VALUES
-- Eastern Kingdoms and Quel'Thalas     spring       summer       fall         winter
    (4,    /* Blasted Lands */           10, 0, 10,   5, 0, 15,   10, 0, 10,   10, 0, 10, 'mod_weather'),
    (8,    /* Swamp of Sorrows */        25, 0,  0,  20, 0,  0,   25, 0,  0,   25, 0,  0, 'mod_weather'),
    (40,   /* Westfall */                10, 0,  0,   5, 0,  0,   20, 0,  0,   10, 0,  0, 'mod_weather'),
    (46,   /* Burning Steppes */         15, 0,  0,  15, 0,  0,   15, 0,  0,   15, 0,  0, 'mod_weather'),
    (51,   /* Searing Gorge */           10, 0, 10,  10, 0, 15,   10, 0, 10,   10, 0, 10, 'mod_weather'),
    (130,  /* Silverpine Forest */       20, 0,  0,  15, 0,  0,   25, 0,  0,   20, 0,  0, 'mod_weather'),
    (1519, /* Stormwind City */          15, 0,  0,  10, 0,  0,   20, 0,  0,   15, 0,  0, 'mod_weather'),
    (3430, /* Eversong Woods */           5, 0,  0,   5, 0,  0,   10, 0,  0,    5, 0,  0, 'mod_weather'),
    (3433, /* Ghostlands */              15, 0,  0,  10, 0,  0,   20, 0,  0,   15, 0,  0, 'mod_weather'),
    (3487, /* Silvermoon City */          5, 0,  0,   5, 0,  0,   10, 0,  0,    5, 0,  0, 'mod_weather'),
-- Kalimdor
    (14,   /* Durotar */                  0, 0, 15,   0, 0, 20,    5, 0, 15,    5, 0, 10, 'mod_weather'),
    (16,   /* Azshara */                 20, 0,  0,  15, 0,  0,   25, 0,  0,   20, 0,  0, 'mod_weather'),
    (17,   /* The Barrens */              5, 0, 10,   0, 0, 15,    5, 0, 10,    5, 0,  5, 'mod_weather'),
    (331,  /* Ashenvale */               20, 0,  0,  15, 0,  0,   25, 0,  0,   20, 0,  0, 'mod_weather'),
    (361,  /* Felwood */                 20, 0,  0,  15, 0,  0,   20, 0,  0,   20, 0,  0, 'mod_weather'),
    (400,  /* Thousand Needles */         0, 0, 10,   0, 0, 15,    0, 0, 10,    0, 0, 10, 'mod_weather'),
    (406,  /* Stonetalon Mountains */    10, 0,  5,   5, 0,  5,   15, 0,  5,   10, 0,  5, 'mod_weather'),
    (493,  /* Moonglade */               15, 0,  0,  10, 0,  0,   15, 0,  0,    0, 15, 0, 'mod_weather'),
    (1637, /* Orgrimmar */                0, 0,  5,   0, 0, 10,    0, 0,  5,    0, 0,  5, 'mod_weather'),
    (1638, /* Thunder Bluff */           10, 0,  0,  10, 0,  0,   15, 0,  0,   10, 0,  0, 'mod_weather'),
    (1657, /* Darnassus */               15, 0,  0,  10, 0,  0,   20, 0,  0,   15, 0,  0, 'mod_weather'),
    (3478, /* Gates of Ahn'Qiraj */       0, 0, 20,   0, 0, 20,    0, 0, 20,    0, 0, 20, 'mod_weather'),
-- Outland
    (3483, /* Hellfire Peninsula */       5, 0, 15,   5, 0, 15,    5, 0, 15,    5, 0, 15, 'mod_weather'),
    (3518, /* Nagrand */                 15, 0,  0,  15, 0,  0,   20, 0,  0,   15, 0,  0, 'mod_weather'),
    (3519, /* Terokkar Forest */         10, 0,  0,   5, 0,  0,   15, 0,  0,   10, 0,  0, 'mod_weather'),
    (3520, /* Shadowmoon Valley */       15, 0,  5,  15, 0,  5,   15, 0,  5,   15, 0,  5, 'mod_weather'),
    (3522, /* Blade's Edge Mountains */   0, 0, 10,   0, 0, 10,    0, 0, 10,    0, 0, 10, 'mod_weather'),
    (3523, /* Netherstorm */             10, 0,  0,  10, 0,  0,   10, 0,  0,   10, 0,  0, 'mod_weather'),
    (3524, /* Azuremyst Isle */          15, 0,  0,  10, 0,  0,   20, 0,  0,   15, 0,  0, 'mod_weather'),
    (3525, /* Bloodmyst Isle */          10, 0,  0,  10, 0,  0,   15, 0,  0,   10, 0,  0, 'mod_weather'),
    (3703, /* Shattrath City */           5, 0,  0,   5, 0,  0,   10, 0,  0,    5, 0,  0, 'mod_weather'),
-- Northrend
    (65,   /* Dragonblight */             0, 25, 0,   0, 15, 0,    0, 25, 0,    0, 30, 0, 'mod_weather'),
    (66,   /* Zul'Drak */                 0, 15, 0,   0, 10, 0,    0, 15, 0,    0, 20, 0, 'mod_weather'),
    (67,   /* The Storm Peaks */          0, 30, 0,   0, 20, 0,    0, 30, 0,    0, 35, 0, 'mod_weather'),
    (210,  /* Icecrown */                 0, 25, 0,   0, 20, 0,    0, 25, 0,    0, 30, 0, 'mod_weather'),
    (394,  /* Grizzly Hills */           20,  5, 0,  25,  0, 0,   25,  5, 0,   10, 20, 0, 'mod_weather'),
    (495,  /* Howling Fjord */           25,  0, 0,  20,  0, 0,   25,  0, 0,   15, 10, 0, 'mod_weather'),
    (2817, /* Crystalsong Forest */       0, 10, 0,   0,  5, 0,    0, 10, 0,    0, 20, 0, 'mod_weather'),
    (3537, /* Borean Tundra */            5, 20, 0,  10, 10, 0,    5, 20, 0,    0, 30, 0, 'mod_weather'),
    (3711, /* Sholazar Basin */          25,  0, 0,  25,  0, 0,   25,  0, 0,   25,  0, 0, 'mod_weather'),
    (4197, /* Wintergrasp */              0, 20, 0,   0, 10, 0,    0, 20, 0,    0, 30, 0, 'mod_weather'),
    (4395, /* Dalaran */                  0,  5, 0,   0,  0, 0,    0,  5, 0,    0, 15, 0, 'mod_weather'),
    (4742, /* Hrothgar's Landing */       0, 20, 0,   0, 15, 0,    0, 20, 0,    0, 25, 0, 'mod_weather');

-- The zones AzerothCore already had keep their chances, and get the module's script so the rules
-- below and the light-weather fix apply to them too. A zone another script already claims (none
-- do in stock AzerothCore) is left alone.
UPDATE `game_weather` SET `ScriptName` = 'mod_weather' WHERE `ScriptName` = '';

-- ---------------------------------------------------------------------------------------------
-- Zone rules
--
--   fog_chance      Percent chance of fog each time a clear zone rolls for it (every
--                   Weather.FogInterval). Fog only rolls under clear skies, and lasts until the
--                   next roll or the next weather change.
--   thunder_chance  Percent chance that heavy rain becomes a thunderstorm. It turns back into
--                   rain once it eases below heavy.
--   black_rain      1 = all rain in the zone falls as black rain (ash, fel, plague).
--   black_snow      1 = all snow in the zone falls as black snow.
--
-- A zone needs a `game_weather` row for its rules to do anything. Each feature can also be
-- switched off server-wide in mod_weather.conf.
-- ---------------------------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS `mod_weather_zone` (
    `zone`           INT UNSIGNED     NOT NULL,
    `fog_chance`     TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `thunder_chance` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `black_rain`     TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `black_snow`     TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `comment`        VARCHAR(255)     NOT NULL DEFAULT '',
    PRIMARY KEY (`zone`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-weather: fog, thunderstorms, black rain and snow';

DELETE FROM `mod_weather_zone`;

INSERT INTO `mod_weather_zone` (`zone`, `fog_chance`, `thunder_chance`, `black_rain`, `black_snow`, `comment`) VALUES
-- Fog
    (10,   30,  0, 0, 0, 'Duskwood'),
    (85,   25,  0, 0, 0, 'Tirisfal Glades'),
    (130,  25,  0, 0, 0, 'Silverpine Forest'),
    (267,  10,  0, 0, 0, 'Hillsbrad Foothills'),
    (28,   15,  0, 0, 0, 'Western Plaguelands'),
    (331,  15,  0, 0, 0, 'Ashenvale'),
    (3433, 25,  0, 0, 0, 'Ghostlands'),
    (3519, 10,  0, 0, 0, 'Terokkar Forest'),
-- Thunderstorms (some foggy too)
    (33,    0, 50, 0, 0, 'Stranglethorn Vale'),
    (8,    25, 50, 0, 0, 'Swamp of Sorrows'),
    (15,   25, 50, 0, 0, 'Dustwallow Marsh'),
    (11,   15, 30, 0, 0, 'Wetlands'),
    (44,    0, 20, 0, 0, 'Redridge Mountains'),
    (45,    0, 20, 0, 0, 'Arathi Highlands'),
    (47,    0, 20, 0, 0, 'The Hinterlands'),
    (16,    0, 30, 0, 0, 'Azshara'),
    (3521, 20, 40, 0, 0, 'Zangarmarsh'),
    (3518,  0, 25, 0, 0, 'Nagrand'),
    (3523,  0, 60, 0, 0, 'Netherstorm'),
    (394,  20, 40, 0, 0, 'Grizzly Hills'),
    (495,  25, 30, 0, 0, 'Howling Fjord'),
    (3711, 15, 50, 0, 0, 'Sholazar Basin'),
-- Black rain
    (4,     0,  0, 1, 0, 'Blasted Lands'),
    (41,   30,  0, 1, 0, 'Deadwind Pass'),
    (46,    0,  0, 1, 0, 'Burning Steppes'),
    (51,    0,  0, 1, 0, 'Searing Gorge'),
    (139,  20,  0, 1, 0, 'Eastern Plaguelands'),
    (361,  20,  0, 1, 0, 'Felwood'),
    (3483,  0,  0, 1, 0, 'Hellfire Peninsula'),
    (3520,  0,  0, 1, 0, 'Shadowmoon Valley'),
-- Black snow
    (210,   0,  0, 0, 1, 'Icecrown');
