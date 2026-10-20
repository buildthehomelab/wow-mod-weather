-- mod-weather: weather regions, and how strongly each zone feels a front.
--
-- The module runs weather as fronts. A region shares one front at a time, so neighbouring zones
-- rain together instead of each rolling its own weather. Each zone turns the front into its own
-- climate (rain, snow or sandstorm, from its `game_weather` chances for the season), so the same
-- front can snow on Dun Morogh and rain on Loch Modan.
--
-- How the `game_weather` chances are read now: once a region's dry spell is over, it rolls for a
-- new front every Weather.FrontInterval (10 minutes), with its wettest zone's total chance for the
-- season. Each zone in the region then joins the front as often as its own chance says, next to
-- that wettest zone: a zone at 10% next to one at 20% joins every other front.
--
-- A zone with no row here is a region of its own.
--
--   region    Zones with the same number share fronts. 1 to 65535.
--   strength  Percent of the front's strength the zone gets (100 = all of it). Cities are lower,
--             so they get drizzle when the land around them gets rain.
--
-- Idempotent: safe to run again.

CREATE TABLE IF NOT EXISTS `mod_weather_region` (
    `zone`     INT UNSIGNED      NOT NULL,
    `region`   SMALLINT UNSIGNED NOT NULL,
    `strength` TINYINT UNSIGNED  NOT NULL DEFAULT 100,
    `comment`  VARCHAR(255)      NOT NULL DEFAULT '',
    PRIMARY KEY (`zone`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-weather: zones that share weather fronts';

DELETE FROM `mod_weather_region`;

INSERT INTO `mod_weather_region` (`zone`, `region`, `strength`, `comment`) VALUES
-- Eastern Kingdoms
    (12,    1, 100, 'Elwynn Forest'),
    (1519,  1,  60, 'Stormwind City'),
    (40,    1, 100, 'Westfall'),
    (10,    1, 100, 'Duskwood'),
    (44,    1, 100, 'Redridge Mountains'),
    (1,     2, 100, 'Dun Morogh'),
    (38,    2, 100, 'Loch Modan'),
    (3,     3, 100, 'Badlands'),
    (51,    3, 100, 'Searing Gorge'),
    (46,    3, 100, 'Burning Steppes'),
    (11,    4, 100, 'Wetlands'),
    (45,    4, 100, 'Arathi Highlands'),
    (267,   4, 100, 'Hillsbrad Foothills'),
    (36,    4, 100, 'Alterac Mountains'),
    (47,    4, 100, 'The Hinterlands'),
    (85,    5, 100, 'Tirisfal Glades'),
    (130,   5, 100, 'Silverpine Forest'),
    (28,    6, 100, 'Western Plaguelands'),
    (139,   6, 100, 'Eastern Plaguelands'),
    (3430,  7, 100, 'Eversong Woods'),
    (3487,  7,  60, 'Silvermoon City'),
    (3433,  7, 100, 'Ghostlands'),
    (8,     8, 100, 'Swamp of Sorrows'),
    (41,    8, 100, 'Deadwind Pass'),
    (4,     8, 100, 'Blasted Lands'),
-- Kalimdor
    (14,    9, 100, 'Durotar'),
    (1637,  9,  60, 'Orgrimmar'),
    (17,    9, 100, 'The Barrens'),
    (215,  10, 100, 'Mulgore'),
    (1638, 10,  60, 'Thunder Bluff'),
    (331,  11, 100, 'Ashenvale'),
    (361,  11, 100, 'Felwood'),
    (406,  11, 100, 'Stonetalon Mountains'),
    (16,   11, 100, 'Azshara'),
    (141,  12, 100, 'Teldrassil'),
    (1657, 12,  60, 'Darnassus'),
    (148,  12, 100, 'Darkshore'),
    (493,  12, 100, 'Moonglade'),
    (440,  13, 100, 'Tanaris'),
    (400,  13, 100, 'Thousand Needles'),
    (1377, 13, 100, 'Silithus'),
    (3478, 13, 100, 'Gates of Ahn''Qiraj'),
-- Outland
    (3524, 14, 100, 'Azuremyst Isle'),
    (3525, 14, 100, 'Bloodmyst Isle'),
    (3519, 15, 100, 'Terokkar Forest'),
    (3703, 15,  60, 'Shattrath City'),
-- Northrend
    (65,   16, 100, 'Dragonblight'),
    (2817, 16, 100, 'Crystalsong Forest'),
    (4395, 16,  60, 'Dalaran'),
    (210,  17, 100, 'Icecrown'),
    (4742, 17, 100, 'Hrothgar''s Landing'),
    (66,   18, 100, 'Zul''Drak'),
    (394,  18, 100, 'Grizzly Hills');
