/*
 * mod-weather
 *
 * The core's weather (Weather.cpp) only ever rolls rain, snow or sandstorm, and most of what it
 * rolls is too light for the client to draw: a grade under 0.27 is sent as clear skies. This
 * module hooks every zone's weather through the `mod_weather` ScriptName on `game_weather` and:
 *
 *  - Makes light weather visible. Weather the core rolled but would send as clear gets a grade
 *    the client draws (light).
 *  - Turns heavy rain into a thunderstorm in the zones that ask for it (`thunder_chance`), and
 *    back into rain once it eases.
 *  - Turns all rain into black rain (`black_rain`) and all snow into black snow (`black_snow`).
 *  - Rolls fog under clear skies (`fog_chance`) every Weather.FogInterval.
 *
 * Thunderstorms and black rain are core weather types, so they go through Weather::SetWeather
 * and the core carries them on from there. Fog and black snow have no weather type, only a state
 * the client understands, so they're set as the map's zone override (Map::SetZoneWeather), the
 * same thing the Sunwell event uses for Quel'Danas. The map sends an override to everyone who
 * enters the zone, in place of the core's weather. Overrides only work on continents: a zone
 * inside an instance gets thunderstorms and black rain but no fog or black snow.
 *
 * Each map updates on its own thread, and every Weather belongs to one map, so the hooks for one
 * zone always run on the thread that owns it. The shared tables are locked anyway, because
 * different maps update at the same time.
 *
 * Released under GNU GPL v2 or (at your option) any later version.
 */

#include "Chat.h"
#include "Config.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "MiscPackets.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "Timer.h"
#include "Weather.h"
#include "WeatherMgr.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

using namespace Acore::ChatCommands;

namespace
{
    // Grades as the core reads them in Weather::GetWeatherState.
    constexpr float GRADE_VISIBLE = 0.27f; // below this the client is told it's clear
    constexpr float GRADE_MEDIUM  = 0.40f;
    constexpr float GRADE_HEAVY   = 0.70f;

    // Where hidden weather lands once it's made visible: the light band, 0.28 to 0.39.
    constexpr float GRADE_LIGHT_MIN = 0.28f;
    constexpr float GRADE_LIGHT_MAX = 0.39f;

    // Clear skies are sent with grade 0, or 0.0001 after the core clamps a negative grade. Above
    // this the core rolled real weather that's just too light to show.
    constexpr float GRADE_HIDDEN_MIN = 0.001f;

    struct Config
    {
        bool enabled = true;
        bool showLightWeather = true;
        bool fog = true;
        uint32 fogInterval = 10 * MINUTE * IN_MILLISECONDS;
        bool thunderstorms = true;
        bool blackRain = true;
        bool blackSnow = true;
    };

    Config config;

    struct ZoneRule
    {
        uint8 fogChance = 0;
        uint8 thunderChance = 0;
        bool blackRain = false;
        bool blackSnow = false;
    };

    // What the module last saw and did in a zone on a continent.
    struct ZoneState
    {
        WeatherState state = WEATHER_STATE_FINE;    // the core's weather, after this module's changes
        float grade = 0.0f;
        WeatherState override = WEATHER_STATE_FINE; // fog or black snow this module set, FINE for none
        uint32 fogTimer = 0;
    };

    std::mutex lock;
    std::unordered_map<uint32, ZoneRule> rules;
    std::unordered_map<uint32, ZoneState> zones;

    // Set while this module changes a zone's weather, because Weather::SetWeather calls OnChange
    // again. Maps update on several threads, so it's per thread.
    thread_local bool applying = false;

    // The same mapping as Weather::GetWeatherState, which is private.
    WeatherState StateFor(WeatherType type, float grade)
    {
        if (grade < GRADE_VISIBLE)
            return WEATHER_STATE_FINE;

        switch (type)
        {
            case WEATHER_TYPE_RAIN:
                return grade < GRADE_MEDIUM ? WEATHER_STATE_LIGHT_RAIN
                    : grade < GRADE_HEAVY ? WEATHER_STATE_MEDIUM_RAIN : WEATHER_STATE_HEAVY_RAIN;
            case WEATHER_TYPE_SNOW:
                return grade < GRADE_MEDIUM ? WEATHER_STATE_LIGHT_SNOW
                    : grade < GRADE_HEAVY ? WEATHER_STATE_MEDIUM_SNOW : WEATHER_STATE_HEAVY_SNOW;
            case WEATHER_TYPE_STORM:
                return grade < GRADE_MEDIUM ? WEATHER_STATE_LIGHT_SANDSTORM
                    : grade < GRADE_HEAVY ? WEATHER_STATE_MEDIUM_SANDSTORM : WEATHER_STATE_HEAVY_SANDSTORM;
            case WEATHER_TYPE_THUNDERS:
                return WEATHER_STATE_THUNDERS;
            case WEATHER_TYPE_BLACKRAIN:
                return WEATHER_STATE_BLACKRAIN;
            default:
                return WEATHER_STATE_FINE;
        }
    }

    bool IsRain(WeatherState state)
    {
        return state == WEATHER_STATE_LIGHT_RAIN || state == WEATHER_STATE_MEDIUM_RAIN
            || state == WEATHER_STATE_HEAVY_RAIN;
    }

    bool IsSnow(WeatherState state)
    {
        return state == WEATHER_STATE_LIGHT_SNOW || state == WEATHER_STATE_MEDIUM_SNOW
            || state == WEATHER_STATE_HEAVY_SNOW;
    }

    // The core doesn't say which weather it rolled when it sends clear skies, so pick one the way
    // the core does: from the zone's chances for this season. Most zones have one kind per season,
    // so this is usually exactly what was rolled.
    WeatherType GuessRolledType(uint32 zone)
    {
        WeatherData const* data = WeatherMgr::GetWeatherData(zone);
        if (!data)
            return WEATHER_TYPE_FINE;

        // Same season as Weather::ReGenerate.
        uint32 season = ((Acore::Time::GetDayInYear() - 78 + 365) / 91) % 4;
        WeatherSeasonChances const& chances = data->data[season];

        uint32 total = chances.rainChance + chances.snowChance + chances.stormChance;
        if (!total)
            return WEATHER_TYPE_FINE;

        uint32 roll = urand(1, total);
        if (roll <= chances.rainChance)
            return WEATHER_TYPE_RAIN;
        if (roll <= chances.rainChance + chances.snowChance)
            return WEATHER_TYPE_SNOW;
        return WEATHER_TYPE_STORM;
    }

    void SetWeather(Weather* weather, WeatherType type, float grade, WeatherState& state)
    {
        weather->SetWeather(type, grade);
        state = StateFor(type, grade);
    }

    // The continent a zone is on, or nullptr for zones inside instances.
    Map* ContinentOf(uint32 zone)
    {
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(zone);
        return area ? sMapMgr->FindBaseNonInstanceMap(area->mapid) : nullptr;
    }

    // Sets or clears the fog or black snow on a zone. Call with the lock held.
    void SetOverride(uint32 zone, ZoneState& zoneState, WeatherState wanted, float grade)
    {
        if (wanted == zoneState.override)
            return;

        Map* map = ContinentOf(zone);
        if (!map)
            return;

        if (wanted != WEATHER_STATE_FINE)
            map->SetZoneWeather(zone, wanted, grade);
        else
        {
            // Clearing the override sends clear skies, so send the core's weather after it.
            map->SetZoneWeather(zone, WEATHER_STATE_FINE, 0.0f);
            map->SendZoneMessage(zone, WorldPackets::Misc::Weather(zoneState.state, zoneState.grade).Write());
        }

        zoneState.override = wanted;
    }

    WeatherState RollFog(ZoneRule const& rule, float& grade)
    {
        if (!config.enabled || !config.fog || !rule.fogChance || !roll_chance_i(rule.fogChance))
            return WEATHER_STATE_FINE;

        grade = frand(0.4f, 0.9f);
        return WEATHER_STATE_FOG;
    }

    void OnWeatherChanged(Weather* weather, WeatherState state, float grade)
    {
        uint32 const zone = weather->GetZone();

        std::lock_guard<std::mutex> guard(lock);

        auto ruleItr = rules.find(zone);
        ZoneRule const rule = ruleItr != rules.end() ? ruleItr->second : ZoneRule();

        if (config.enabled)
        {
            if (config.showLightWeather && state == WEATHER_STATE_FINE && grade > GRADE_HIDDEN_MIN)
            {
                WeatherType type = GuessRolledType(zone);
                if (type != WEATHER_TYPE_FINE)
                {
                    grade = GRADE_LIGHT_MIN + std::min(grade / GRADE_VISIBLE, 1.0f) * (GRADE_LIGHT_MAX - GRADE_LIGHT_MIN);
                    SetWeather(weather, type, grade, state);
                }
            }

            if (IsRain(state))
            {
                if (config.blackRain && rule.blackRain)
                    SetWeather(weather, WEATHER_TYPE_BLACKRAIN, grade, state);
                else if (config.thunderstorms && state == WEATHER_STATE_HEAVY_RAIN && rule.thunderChance
                    && roll_chance_i(rule.thunderChance))
                    SetWeather(weather, WEATHER_TYPE_THUNDERS, grade, state);
            }
            else if (state == WEATHER_STATE_THUNDERS && grade < GRADE_HEAVY)
                SetWeather(weather, WEATHER_TYPE_RAIN, grade, state); // the storm has eased
        }

        ZoneState& zoneState = zones[zone];
        zoneState.state = state;
        zoneState.grade = grade;
        zoneState.fogTimer = 0;

        WeatherState wanted = WEATHER_STATE_FINE;
        float wantedGrade = 0.0f;
        if (config.enabled && config.blackSnow && rule.blackSnow && IsSnow(state))
        {
            wanted = WEATHER_STATE_BLACKSNOW;
            wantedGrade = grade;
        }
        else if (state == WEATHER_STATE_FINE)
            wanted = RollFog(rule, wantedGrade);

        SetOverride(zone, zoneState, wanted, wantedGrade);
    }

    // Fog comes and goes while the sky stays clear, which the core doesn't count as a change.
    void OnWeatherTick(Weather* weather, uint32 diff)
    {
        uint32 const zone = weather->GetZone();

        std::lock_guard<std::mutex> guard(lock);

        auto ruleItr = rules.find(zone);
        if (ruleItr == rules.end() || !ruleItr->second.fogChance)
            return;

        auto zoneItr = zones.find(zone);
        if (zoneItr == zones.end() || zoneItr->second.state != WEATHER_STATE_FINE)
            return;

        ZoneState& zoneState = zoneItr->second;
        zoneState.fogTimer += diff;
        if (zoneState.fogTimer < config.fogInterval)
            return;

        zoneState.fogTimer = 0;

        float grade = 0.0f;
        WeatherState wanted = RollFog(ruleItr->second, grade);
        SetOverride(zone, zoneState, wanted, grade);
    }

    uint32 LoadRules()
    {
        std::unordered_map<uint32, ZoneRule> loaded;

        if (QueryResult result = WorldDatabase.Query(
            "SELECT `zone`, `fog_chance`, `thunder_chance`, `black_rain`, `black_snow` FROM `mod_weather_zone`"))
        {
            do
            {
                Field* fields = result->Fetch();
                ZoneRule& rule = loaded[fields[0].Get<uint32>()];
                rule.fogChance     = std::min<uint8>(fields[1].Get<uint8>(), 100);
                rule.thunderChance = std::min<uint8>(fields[2].Get<uint8>(), 100);
                rule.blackRain     = fields[3].Get<uint8>() != 0;
                rule.blackSnow     = fields[4].Get<uint8>() != 0;
            } while (result->NextRow());
        }

        std::lock_guard<std::mutex> guard(lock);
        rules.swap(loaded);
        return rules.size();
    }

    char const* StateName(WeatherState state)
    {
        switch (state)
        {
            case WEATHER_STATE_FINE:             return "clear";
            case WEATHER_STATE_FOG:              return "fog";
            case WEATHER_STATE_LIGHT_RAIN:       return "light rain";
            case WEATHER_STATE_MEDIUM_RAIN:      return "rain";
            case WEATHER_STATE_HEAVY_RAIN:       return "heavy rain";
            case WEATHER_STATE_LIGHT_SNOW:       return "light snow";
            case WEATHER_STATE_MEDIUM_SNOW:      return "snow";
            case WEATHER_STATE_HEAVY_SNOW:       return "heavy snow";
            case WEATHER_STATE_LIGHT_SANDSTORM:  return "light sandstorm";
            case WEATHER_STATE_MEDIUM_SANDSTORM: return "sandstorm";
            case WEATHER_STATE_HEAVY_SANDSTORM:  return "heavy sandstorm";
            case WEATHER_STATE_THUNDERS:         return "thunderstorm";
            case WEATHER_STATE_BLACKRAIN:        return "black rain";
            case WEATHER_STATE_BLACKSNOW:        return "black snow";
            default:                             return "unknown";
        }
    }
}

class ModWeatherScript : public WeatherScript
{
public:
    ModWeatherScript() : WeatherScript("mod_weather") { }

    void OnChange(Weather* weather, WeatherState state, float grade) override
    {
        if (applying)
            return;

        applying = true;
        OnWeatherChanged(weather, state, grade);
        applying = false;
    }

    void OnUpdate(Weather* weather, uint32 diff) override
    {
        if (config.enabled && config.fog)
            OnWeatherTick(weather, diff);
    }
};

class ModWeatherWorldScript : public WorldScript
{
public:
    ModWeatherWorldScript() : WorldScript("ModWeatherWorldScript") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        config.enabled          = sConfigMgr->GetOption<bool>("Weather.Enable", true);
        config.showLightWeather = sConfigMgr->GetOption<bool>("Weather.ShowLightWeather", true);
        config.fog              = sConfigMgr->GetOption<bool>("Weather.Fog", true);
        config.fogInterval      = sConfigMgr->GetOption<uint32>("Weather.FogInterval", 600) * IN_MILLISECONDS;
        config.thunderstorms    = sConfigMgr->GetOption<bool>("Weather.Thunderstorms", true);
        config.blackRain        = sConfigMgr->GetOption<bool>("Weather.BlackRain", true);
        config.blackSnow        = sConfigMgr->GetOption<bool>("Weather.BlackSnow", true);

        if (config.fogInterval < MINUTE * IN_MILLISECONDS)
            config.fogInterval = MINUTE * IN_MILLISECONDS;
    }

    void OnStartup() override
    {
        LOG_INFO("module", "mod-weather: loaded {} zone rules.", LoadRules());

        if (!WeatherMgr::GetWeatherData(3537)) // Borean Tundra, one of the zones the SQL adds
            LOG_ERROR("module", "mod-weather: `game_weather` has no row for Borean Tundra. Apply the module's SQL.");
    }
};

class ModWeatherCommandScript : public CommandScript
{
public:
    ModWeatherCommandScript() : CommandScript("ModWeatherCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable subCommands =
        {
            { "info",   HandleInfoCommand,   SEC_GAMEMASTER,    Console::No  },
            { "show",   HandleShowCommand,   SEC_GAMEMASTER,    Console::No  },
            { "reload", HandleReloadCommand, SEC_ADMINISTRATOR, Console::Yes },
        };

        static ChatCommandTable commandTable =
        {
            { "weather", subCommands },
        };

        return commandTable;
    }

    // What the module knows about the zone you're standing in.
    static bool HandleInfoCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        uint32 const zone = player->GetZoneId();

        AreaTableEntry const* area = sAreaTableStore.LookupEntry(zone);
        char const* name = area ? area->area_name[handler->GetSessionDbcLocale()] : "?";

        if (!WeatherMgr::GetWeatherData(zone))
        {
            handler->PSendSysMessage("{} (zone {}) has no weather: no `game_weather` row.", name, zone);
            return true;
        }

        std::lock_guard<std::mutex> guard(lock);

        auto ruleItr = rules.find(zone);
        ZoneRule const rule = ruleItr != rules.end() ? ruleItr->second : ZoneRule();
        handler->PSendSysMessage("{} (zone {}): fog {}%, thunderstorms {}%, black rain {}, black snow {}.",
            name, zone, rule.fogChance, rule.thunderChance, rule.blackRain ? "yes" : "no",
            rule.blackSnow ? "yes" : "no");

        auto zoneItr = zones.find(zone);
        if (zoneItr == zones.end())
        {
            handler->SendSysMessage("No weather change seen here since the server started.");
            return true;
        }

        ZoneState const& zoneState = zoneItr->second;
        handler->PSendSysMessage("Weather: {} (grade {:.2f}){}{}.", StateName(zoneState.state), zoneState.grade,
            zoneState.override != WEATHER_STATE_FINE ? ", shown as " : "",
            zoneState.override != WEATHER_STATE_FINE ? StateName(zoneState.override) : "");
        return true;
    }

    // Sends a weather state to you alone, to see how the client draws it. Any state the client
    // knows works, including fog (1) and black snow (106), which .wchange can't set. It lasts until
    // the zone's weather changes or you change zones.
    static bool HandleShowCommand(ChatHandler* handler, uint32 state, Optional<float> grade)
    {
        float const intensity = std::clamp(grade.value_or(0.9f), 0.0f, 1.0f);
        handler->GetPlayer()->SendDirectMessage(WorldPackets::Misc::Weather(WeatherState(state), intensity).Write());
        handler->PSendSysMessage("Showing weather state {} ({}) at grade {:.2f} to you only.", state,
            StateName(WeatherState(state)), intensity);
        return true;
    }

    static bool HandleReloadCommand(ChatHandler* handler)
    {
        handler->PSendSysMessage("mod-weather: reloaded {} zone rules. They apply from each zone's next weather "
            "change. New `game_weather` rows need a restart.", LoadRules());
        return true;
    }
};

void AddModWeatherScripts()
{
    new ModWeatherScript();
    new ModWeatherWorldScript();
    new ModWeatherCommandScript();
}
