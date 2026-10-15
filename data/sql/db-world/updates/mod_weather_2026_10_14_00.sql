-- mod-weather: much less rain in Stormwind City.
--
-- A rain chance is the chance per weather roll (every ChangeWeatherInterval) that clear skies
-- turn to rain, not the share of time it rains. Once it's raining, the core's rolls keep it going
-- for about 70-80 minutes on average, so the old 10-20% chances meant rain about half the time
-- (54% in fall). At 1-2% it rains about 4-9% of the time: one or two showers a day.

UPDATE `game_weather` SET
    `spring_rain_chance` = 2,
    `summer_rain_chance` = 1,
    `fall_rain_chance`   = 2,
    `winter_rain_chance` = 2
WHERE `zone` = 1519; -- Stormwind City
