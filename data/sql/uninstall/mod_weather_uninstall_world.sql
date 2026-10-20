-- mod-weather: undo the module's world database changes. Run it by hand on the world database
-- after removing the module. It isn't applied automatically: AzerothCore only runs the module's
-- db-world folder.
--
-- Drops the module's tables (your own rows in them included). Removes the 43 zones the module
-- added to `game_weather` (stock AzerothCore has no rows for them; a row another script has
-- claimed since is left alone) and takes the module's script off the zones AzerothCore already
-- had, which then roll their stock weather again. It also forgets that the module's updates were
-- applied, so installing the module again applies them again. Idempotent: safe to run again.

DELETE FROM `game_weather` WHERE `ScriptName` = 'mod_weather' AND `zone` IN (
    4, 8, 40, 46, 51, 130, 1519, 3430, 3433, 3487,
    14, 16, 17, 331, 361, 400, 406, 493, 1637, 1638, 1657, 3478,
    3483, 3518, 3519, 3520, 3522, 3523, 3524, 3525, 3703,
    65, 66, 67, 210, 394, 495, 2817, 3537, 3711, 4197, 4395, 4742);

UPDATE `game_weather` SET `ScriptName` = '' WHERE `ScriptName` = 'mod_weather';

DROP TABLE IF EXISTS `mod_weather_zone`;
DROP TABLE IF EXISTS `mod_weather_region`;
DROP TABLE IF EXISTS `mod_weather_overcast`;

DELETE FROM `updates` WHERE `name` LIKE 'mod!_weather!_%' ESCAPE '!';
