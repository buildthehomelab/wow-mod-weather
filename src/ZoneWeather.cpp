/*
 * mod-weather
 *
 * Stock AzerothCore rolls each zone's weather every 10 minutes, and the roll can jump from clear
 * skies to a downpour in one step, so weather starts and stops like a switch. This module takes
 * the weather over and runs it as fronts:
 *
 *  - Zones are grouped into regions (`mod_weather_region`). A region shares one front at a time,
 *    so neighbouring zones rain together. A zone in no region is a region of its own.
 *  - A front builds up, peaks with gusts, tapers off and is followed by a dry spell. How often a
 *    region gets one comes from its zones' `game_weather` chances for the season.
 *  - Each zone turns the front into its own climate: rain, snow or sandstorm, picked from its own
 *    chances, scaled by its `strength` (cities get drizzle), and with the zone's rules on top:
 *    thunder at the height of heavy rain, black rain, black snow, fog under clear skies, mist
 *    after rain (`mod_weather_zone`).
 *  - What a zone shows only ever eases toward what it should show, so nothing pops in or out.
 *    Switching between kinds (rain to snow, rain to mist) fades the old one out first.
 *
 * Fronts advance on the world thread. Each zone's weather is sent from that zone's Weather hook,
 * which runs on the thread of the map that owns it, once a second. On continents the weather is
 * sent as the map's zone override (Map::SetZoneWeather), which is also what players who enter the
 * zone are sent, so it can carry light weather the core would send as clear. Zones inside
 * instances go through Weather::SetWeather instead, and get no fog, black snow or faint drizzle.
 *
 * The core's own rolls are switched off while the module runs, by stretching
 * ChangeWeatherInterval. The one zone a core script drives itself, Isle of Quel'Danas (the
 * Sunwell event), keeps the core's rolls on the original interval.
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
#include "World.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    // Grades as the core reads them in Weather::GetWeatherState.
    constexpr float GRADE_VISIBLE = 0.27f; // the core sends anything lighter as clear
    constexpr float GRADE_MEDIUM  = 0.40f;
    constexpr float GRADE_HEAVY   = 0.70f;
    constexpr float GRADE_MAX     = 0.9999f;

    // A zone's weather moves toward its target by at most this much a second. That's 0 to full
    // in about 80 seconds: slower than any pop, faster than a front builds.
    constexpr float GRADE_SLEW = 0.012f;

    // Smaller changes than this aren't worth a packet.
    constexpr float GRADE_STEP = 0.02f;

    // During a front's peak its strength drifts around the peak by up to this much.
    constexpr float GUST_RANGE = 0.08f;

    // How fast the strength follows a gust, in ms (time constant).
    constexpr float GUST_FOLLOW_MS = 30000.0f;

    // Region keys: `mod_weather_region` ids are below this, a zone's own region is this + zone.
    constexpr uint32 OWN_REGION = 0x10000;

    // The Sunwell event sets this zone's weather itself (WorldState.cpp), so it keeps stock rolls.
    constexpr uint32 ZONE_ISLE_OF_QUEL_DANAS = 4080;

    // Long enough that the core never rolls again (a week).
    constexpr uint32 CORE_INTERVAL_OFF = 7 * DAY * IN_MILLISECONDS;

    struct Range
    {
        uint32 min = 0;
        uint32 max = 0;

        uint32 Roll() const { return urand(min, std::max(min, max)); }
    };

    struct Config
    {
        bool enabled = true;
        uint32 frontInterval = 10 * MINUTE * IN_MILLISECONDS;
        Range buildUp { 3 * MINUTE * IN_MILLISECONDS, 6 * MINUTE * IN_MILLISECONDS };
        Range peak    { 10 * MINUTE * IN_MILLISECONDS, 25 * MINUTE * IN_MILLISECONDS };
        Range taper   { 5 * MINUTE * IN_MILLISECONDS, 8 * MINUTE * IN_MILLISECONDS };
        Range dry     { 20 * MINUTE * IN_MILLISECONDS, 60 * MINUTE * IN_MILLISECONDS };
        uint32 mediumChance = 30;
        uint32 heavyChance = 10;
        float faintGrade = 0.10f;
        bool fog = true;
        uint32 fogInterval = 10 * MINUTE * IN_MILLISECONDS;
        bool mistAfterRain = true;
        bool thunderstorms = true;
        bool blackRain = true;
        bool blackSnow = true;

        // ChangeWeatherInterval from worldserver.conf, before the module stretched it.
        uint32 coreInterval = 10 * MINUTE * IN_MILLISECONDS;
    };

    Config config;

    // What falls in a zone. One at a time: the client only draws one weather.
    enum class Kind : uint8
    {
        None,
        Rain,
        Snow,
        Sand,
        Fog,
    };

    enum class Phase : uint8
    {
        Dry,
        BuildUp,
        Peak,
        Taper,
    };

    struct ZoneRule
    {
        uint32 region = 0;
        uint8 strength = 100;
        uint8 fogChance = 0;
        uint8 thunderChance = 0;
        bool blackRain = false;
        bool blackSnow = false;
    };

    struct Region
    {
        std::vector<uint32> zones;
        Phase phase = Phase::Dry;
        uint32 frontId = 0;    // the current front, or the last one while dry; 0 for none yet
        uint32 elapsed = 0;    // ms into this phase
        uint32 duration = 0;   // ms this phase lasts; while dry, until the next roll
        float peak = 0.0f;     // the front's strength at its height
        float strength = 0.0f; // now, 0 to 1
        float from = 0.0f;     // the strength the build-up or taper started at
        float gust = 0.0f;     // where the strength is drifting to during the peak
        uint32 gustTimer = 0;
        bool forced = false;   // started with .weather start: every zone in the region joins
    };

    // What the module last saw and did in a zone.
    struct ZoneState
    {
        bool seen = false;
        bool continent = false;
        uint32 lastTick = 0; // getMSTime() of the last tick. Instances of one zone share this state.

        // The front this zone last rolled for, and what it rolled.
        uint32 frontId = 0;
        bool joined = false;
        Kind frontKind = Kind::None;
        bool thunder = false;

        // Fog or mist under clear skies.
        float fogGrade = 0.0f;
        uint32 fogLeft = 0;
        uint32 fogTimer = 0;

        // What the zone shows, which eases toward what the front and fog want.
        Kind kind = Kind::None;
        float grade = 0.0f;

        // What was last sent.
        WeatherState sentState = WEATHER_STATE_FINE;
        float sentGrade = 0.0f;
        bool overridden = false;

        // Isle of Quel'Danas only: time toward the core's next roll.
        uint32 stockTimer = 0;
    };

    std::mutex lock;
    std::unordered_map<uint32, ZoneRule> rules;  // every `game_weather` zone
    std::unordered_map<uint32, Region> regions;
    std::unordered_map<uint32, ZoneState> zones;
    uint32 lastFrontId = 0;
    uint32 regionStepTimer = 0;

    // Set while this module changes a zone's weather, because Weather::SetWeather calls OnChange
    // again. Maps update on several threads, so it's per thread.
    thread_local bool applying = false;

    float Smooth(float t)
    {
        t = std::clamp(t, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    float Progress(Region const& region)
    {
        return region.duration ? float(region.elapsed) / float(region.duration) : 1.0f;
    }

    // Same season as Weather::ReGenerate.
    uint32 Season()
    {
        return ((Acore::Time::GetDayInYear() - 78 + 365) / 91) % 4;
    }

    uint32 TotalChance(uint32 zone, uint32 season)
    {
        WeatherData const* data = WeatherMgr::GetWeatherData(zone);
        if (!data)
            return 0;

        WeatherSeasonChances const& chances = data->data[season];
        return chances.rainChance + chances.snowChance + chances.stormChance;
    }

    // How likely a dry region is to get a front on each roll: as likely as its wettest zone.
    uint32 RegionChance(Region const& region)
    {
        uint32 const season = Season();
        uint32 chance = 0;
        for (uint32 zone : region.zones)
            chance = std::max(chance, TotalChance(zone, season));
        return std::min<uint32>(chance, 100);
    }

    // Rain, snow or sandstorm, weighted by the zone's chances for the season. With `anySeason`,
    // falls back to the whole year and then to rain, for fronts a GM starts by hand.
    Kind RollKind(uint32 zone, bool anySeason)
    {
        WeatherData const* data = WeatherMgr::GetWeatherData(zone);
        if (!data)
            return anySeason ? Kind::Rain : Kind::None;

        WeatherSeasonChances chances = data->data[Season()];
        if (anySeason && !(chances.rainChance + chances.snowChance + chances.stormChance))
        {
            for (WeatherSeasonChances const& season : data->data)
            {
                chances.rainChance += season.rainChance;
                chances.snowChance += season.snowChance;
                chances.stormChance += season.stormChance;
            }
        }

        uint32 const total = chances.rainChance + chances.snowChance + chances.stormChance;
        if (!total)
            return anySeason ? Kind::Rain : Kind::None;

        uint32 const roll = urand(1, total);
        if (roll <= chances.rainChance)
            return Kind::Rain;
        if (roll <= chances.rainChance + chances.snowChance)
            return Kind::Snow;
        return Kind::Sand;
    }

    // Most fronts stay light, some reach medium, a few become a downpour.
    float RollPeak()
    {
        uint32 const roll = urand(1, 100);
        if (roll <= config.heavyChance)
            return frand(0.75f, 0.95f);
        if (roll <= config.heavyChance + config.mediumChance)
            return frand(0.45f, 0.65f);
        return frand(0.30f, 0.40f);
    }

    void StartFront(Region& region, float peak, bool forced)
    {
        region.phase = Phase::BuildUp;
        region.frontId = ++lastFrontId;
        region.elapsed = 0;
        region.duration = config.buildUp.Roll();
        region.peak = peak;
        region.from = region.strength;
        region.forced = forced;
    }

    void StartTaper(Region& region)
    {
        region.phase = Phase::Taper;
        region.elapsed = 0;
        region.duration = config.taper.Roll();
        region.from = region.strength;
    }

    void AdvanceRegion(Region& region, uint32 diff)
    {
        region.elapsed += diff;

        switch (region.phase)
        {
            case Phase::Dry:
                if (region.elapsed < region.duration)
                    return;

                region.elapsed = 0;
                region.duration = config.frontInterval;
                if (roll_chance_i(RegionChance(region)))
                    StartFront(region, RollPeak(), false);
                return;
            case Phase::BuildUp:
                region.strength = region.from + (region.peak - region.from) * Smooth(Progress(region));
                if (region.elapsed >= region.duration)
                {
                    region.phase = Phase::Peak;
                    region.elapsed = 0;
                    region.duration = config.peak.Roll();
                    region.gust = region.peak;
                    region.gustTimer = 0;
                }
                return;
            case Phase::Peak:
                if (region.gustTimer <= diff)
                {
                    region.gust = std::clamp(region.peak + frand(-GUST_RANGE, GUST_RANGE), 0.05f, GRADE_MAX);
                    region.gustTimer = urand(MINUTE * IN_MILLISECONDS, 2 * MINUTE * IN_MILLISECONDS);
                }
                else
                    region.gustTimer -= diff;

                region.strength += (region.gust - region.strength) * std::min(1.0f, diff / GUST_FOLLOW_MS);
                if (region.elapsed >= region.duration)
                    StartTaper(region);
                return;
            case Phase::Taper:
                region.strength = region.from * (1.0f - Smooth(Progress(region)));
                if (region.elapsed >= region.duration)
                {
                    region.phase = Phase::Dry;
                    region.elapsed = 0;
                    region.duration = config.dry.Roll();
                    region.strength = 0.0f;
                    region.forced = false;
                }
                return;
        }
    }

    ZoneRule const& RuleFor(uint32 zone)
    {
        static ZoneRule const none;
        auto itr = rules.find(zone);
        return itr != rules.end() ? itr->second : none;
    }

    Region* RegionOf(uint32 zone)
    {
        auto itr = regions.find(RuleFor(zone).region);
        return itr != regions.end() ? &itr->second : nullptr;
    }

    // The continent a zone is on, or nullptr for zones inside instances.
    Map* ContinentOf(uint32 zone)
    {
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(zone);
        return area ? sMapMgr->FindBaseNonInstanceMap(area->mapid) : nullptr;
    }

    WeatherState Band(float grade, WeatherState light, WeatherState medium, WeatherState heavy)
    {
        return grade < GRADE_MEDIUM ? light : grade < GRADE_HEAVY ? medium : heavy;
    }

    // The state the client is sent for what a zone shows.
    WeatherState StateFor(ZoneState const& zoneState, ZoneRule const& rule)
    {
        // Instances go through the core, which sends anything under GRADE_VISIBLE as clear.
        float const faint = zoneState.continent ? config.faintGrade : GRADE_VISIBLE;
        if (zoneState.grade < faint)
            return WEATHER_STATE_FINE;

        float const grade = zoneState.grade;
        switch (zoneState.kind)
        {
            case Kind::Rain:
                if (config.blackRain && rule.blackRain)
                    return WEATHER_STATE_BLACKRAIN;
                if (config.thunderstorms && zoneState.thunder && grade >= GRADE_HEAVY)
                    return WEATHER_STATE_THUNDERS;
                return Band(grade, WEATHER_STATE_LIGHT_RAIN, WEATHER_STATE_MEDIUM_RAIN, WEATHER_STATE_HEAVY_RAIN);
            case Kind::Snow:
                if (config.blackSnow && rule.blackSnow && zoneState.continent)
                    return WEATHER_STATE_BLACKSNOW;
                return Band(grade, WEATHER_STATE_LIGHT_SNOW, WEATHER_STATE_MEDIUM_SNOW, WEATHER_STATE_HEAVY_SNOW);
            case Kind::Sand:
                return Band(grade, WEATHER_STATE_LIGHT_SANDSTORM, WEATHER_STATE_MEDIUM_SANDSTORM,
                    WEATHER_STATE_HEAVY_SANDSTORM);
            case Kind::Fog:
                return zoneState.continent ? WEATHER_STATE_FOG : WEATHER_STATE_FINE;
            default:
                return WEATHER_STATE_FINE;
        }
    }

    // The core's weather type for a state, for zones inside instances.
    WeatherType TypeFor(WeatherState state)
    {
        switch (state)
        {
            case WEATHER_STATE_LIGHT_RAIN:
            case WEATHER_STATE_MEDIUM_RAIN:
            case WEATHER_STATE_HEAVY_RAIN:
                return WEATHER_TYPE_RAIN;
            case WEATHER_STATE_LIGHT_SNOW:
            case WEATHER_STATE_MEDIUM_SNOW:
            case WEATHER_STATE_HEAVY_SNOW:
                return WEATHER_TYPE_SNOW;
            case WEATHER_STATE_LIGHT_SANDSTORM:
            case WEATHER_STATE_MEDIUM_SANDSTORM:
            case WEATHER_STATE_HEAVY_SANDSTORM:
                return WEATHER_TYPE_STORM;
            case WEATHER_STATE_THUNDERS:
                return WEATHER_TYPE_THUNDERS;
            case WEATHER_STATE_BLACKRAIN:
                return WEATHER_TYPE_BLACKRAIN;
            default:
                return WEATHER_TYPE_FINE;
        }
    }

    // Sends what a zone on a continent shows. Call with the lock held, on the thread of the
    // zone's map.
    void Send(Weather* weather, Map* map, ZoneState& zoneState, WeatherState state, float grade)
    {
        uint32 const zone = weather->GetZone();
        applying = true;

        if (state != WEATHER_STATE_FINE)
        {
            map->SetZoneWeather(zone, state, grade);
            zoneState.overridden = true;
        }
        else
        {
            if (zoneState.overridden)
                map->SetZoneWeather(zone, WEATHER_STATE_FINE, 0.0f);
            zoneState.overridden = false;

            // With no override, players who enter are sent the core's weather, so keep it clear.
            weather->SetWeather(WEATHER_TYPE_FINE, 0.0f);
        }

        applying = false;
        zoneState.sentState = state;
        zoneState.sentGrade = grade;
    }

    // The same for a zone inside an instance. Every instance has its own Weather, and
    // Weather::SetWeather does nothing when nothing changed, so the grade is rounded to
    // GRADE_STEP and each instance only sends when its own weather changes.
    void SendToInstance(Weather* weather, ZoneState& zoneState, WeatherState state, float grade)
    {
        float const rounded = state != WEATHER_STATE_FINE ? std::round(grade / GRADE_STEP) * GRADE_STEP : 0.0f;

        applying = true;
        weather->SetWeather(TypeFor(state), rounded);
        applying = false;

        zoneState.sentState = state;
        zoneState.sentGrade = rounded;
    }

    // A front just reached this zone (or this zone just woke up during one).
    void RollForFront(uint32 zone, ZoneState& zoneState, ZoneRule const& rule, Region const& region)
    {
        zoneState.frontId = region.frontId;
        zoneState.thunder = rule.thunderChance && roll_chance_i(rule.thunderChance);

        if (region.forced)
        {
            zoneState.joined = true;
            zoneState.frontKind = RollKind(zone, true);
            return;
        }

        // A zone joins as often as its own chances say, next to the region's wettest zone.
        uint32 const regionChance = RegionChance(region);
        uint32 const chance = TotalChance(zone, Season());
        zoneState.joined = regionChance && chance && urand(1, regionChance) <= chance;
        zoneState.frontKind = zoneState.joined ? RollKind(zone, false) : Kind::None;
    }

    // Isle of Quel'Danas: the core's rolls on the original interval, as if the module weren't here.
    void StockTick(Weather* weather, ZoneState& zoneState, uint32 diff)
    {
        zoneState.stockTimer += diff;
        if (zoneState.stockTimer < config.coreInterval)
            return;

        zoneState.stockTimer = 0;
        applying = true;
        if (weather->ReGenerate())
            weather->UpdateWeather();
        applying = false;
    }

    // Once a second per zone, from the zone's Weather on its map's thread. With `resend`, the core
    // just sent weather of its own (a new Weather, .wchange, a script), so send ours again.
    void ZoneTick(Weather* weather, bool resend)
    {
        uint32 const zone = weather->GetZone();

        std::lock_guard<std::mutex> guard(lock);

        // Real time since the last tick, not the hook's diff: several instances of one dungeon
        // each tick the same zone.
        ZoneState& zoneState = zones[zone];
        uint32 const now = getMSTime();
        bool const first = !zoneState.seen;
        uint32 const diff = first ? 0 : getMSTimeDiff(zoneState.lastTick, now);
        zoneState.lastTick = now;

        if (zone == ZONE_ISLE_OF_QUEL_DANAS)
        {
            zoneState.seen = true;
            StockTick(weather, zoneState, diff);
            return;
        }

        ZoneRule const& rule = RuleFor(zone);
        Region const* region = RegionOf(zone);

        Map* map = ContinentOf(zone);
        if (first)
        {
            zoneState.seen = true;
            zoneState.continent = map != nullptr;
        }

        // The front: a new one to roll for, or the end of the one this zone joined.
        float frontGrade = 0.0f;
        if (region && region->phase != Phase::Dry)
        {
            if (zoneState.frontId != region->frontId)
                RollForFront(zone, zoneState, rule, *region);

            if (zoneState.joined)
                frontGrade = std::min(region->strength * rule.strength / 100.0f, GRADE_MAX);
        }
        else if (zoneState.joined)
        {
            zoneState.joined = false;
            if (config.fog && config.mistAfterRain && zoneState.continent && rule.fogChance
                && roll_chance_i(std::min(100, rule.fogChance * 2)))
            {
                zoneState.fogGrade = frand(0.3f, 0.6f);
                zoneState.fogLeft = urand(3 * MINUTE * IN_MILLISECONDS, 8 * MINUTE * IN_MILLISECONDS);
            }
        }

        // Fog under clear skies: a roll every Weather.FogInterval, each success lasting until the next.
        zoneState.fogLeft = zoneState.fogLeft > diff ? zoneState.fogLeft - diff : 0;
        if (config.fog && zoneState.continent && rule.fogChance && !zoneState.joined)
        {
            zoneState.fogTimer += diff;
            if (zoneState.fogTimer >= config.fogInterval || first)
            {
                zoneState.fogTimer = 0;
                if (roll_chance_i(rule.fogChance))
                {
                    zoneState.fogGrade = frand(0.4f, 0.9f);
                    zoneState.fogLeft = std::max(zoneState.fogLeft, config.fogInterval);
                }
            }
        }

        Kind wantKind = Kind::None;
        float want = 0.0f;
        if (frontGrade > 0.0f && zoneState.frontKind != Kind::None)
        {
            wantKind = zoneState.frontKind;
            want = frontGrade;
        }
        else if (zoneState.fogLeft && config.fog)
        {
            wantKind = Kind::Fog;
            want = zoneState.fogGrade;
        }

        // Ease toward it. A different kind fades the old one out first. A zone seen for the first
        // time (the first player in) starts where it should be.
        if (first)
        {
            zoneState.kind = wantKind;
            zoneState.grade = want;
        }
        else
        {
            if (wantKind != zoneState.kind)
            {
                if (zoneState.grade <= 0.0f)
                    zoneState.kind = wantKind;
                else
                    want = 0.0f;
            }

            float const step = GRADE_SLEW * diff / IN_MILLISECONDS;
            zoneState.grade = want > zoneState.grade ? std::min(want, zoneState.grade + step)
                : std::max(want, zoneState.grade - step);
        }

        WeatherState const state = StateFor(zoneState, rule);
        float const grade = state != WEATHER_STATE_FINE ? zoneState.grade : 0.0f;
        if (!map)
        {
            SendToInstance(weather, zoneState, state, grade);
            return;
        }

        if (!first && !resend && state == zoneState.sentState && std::fabs(grade - zoneState.sentGrade) < GRADE_STEP)
            return;

        Send(weather, map, zoneState, state, grade);
    }

    uint32 LoadRules()
    {
        std::unordered_map<uint32, ZoneRule> loaded;

        if (QueryResult result = WorldDatabase.Query("SELECT `zone` FROM `game_weather`"))
        {
            do
            {
                uint32 const zone = result->Fetch()[0].Get<uint32>();
                loaded[zone].region = OWN_REGION + zone;
            } while (result->NextRow());
        }

        if (QueryResult result = WorldDatabase.Query(
            "SELECT `zone`, `fog_chance`, `thunder_chance`, `black_rain`, `black_snow` FROM `mod_weather_zone`"))
        {
            do
            {
                Field* fields = result->Fetch();
                auto itr = loaded.find(fields[0].Get<uint32>());
                if (itr == loaded.end())
                    continue; // no `game_weather` row, so no weather to apply it to

                ZoneRule& rule = itr->second;
                rule.fogChance     = std::min<uint8>(fields[1].Get<uint8>(), 100);
                rule.thunderChance = std::min<uint8>(fields[2].Get<uint8>(), 100);
                rule.blackRain     = fields[3].Get<uint8>() != 0;
                rule.blackSnow     = fields[4].Get<uint8>() != 0;
            } while (result->NextRow());
        }

        if (QueryResult result = WorldDatabase.Query("SELECT `zone`, `region`, `strength` FROM `mod_weather_region`"))
        {
            do
            {
                Field* fields = result->Fetch();
                auto itr = loaded.find(fields[0].Get<uint32>());
                if (itr == loaded.end())
                    continue;

                uint32 const region = fields[1].Get<uint16>();
                if (region && region < OWN_REGION)
                    itr->second.region = region;
                itr->second.strength = std::min<uint8>(fields[2].Get<uint8>(), 100);
            } while (result->NextRow());
        }

        loaded.erase(ZONE_ISLE_OF_QUEL_DANAS);

        std::lock_guard<std::mutex> guard(lock);
        rules.swap(loaded);

        // Regions keep the front they're in. New ones start dry, and roll some time in the next
        // Weather.FrontInterval, so they don't all roll at once.
        std::unordered_map<uint32, Region> rebuilt;
        for (auto const& [zone, rule] : rules)
            rebuilt[rule.region].zones.push_back(zone);

        for (auto& [key, region] : rebuilt)
        {
            std::sort(region.zones.begin(), region.zones.end());

            auto old = regions.find(key);
            if (old != regions.end())
            {
                old->second.zones.swap(region.zones);
                region = std::move(old->second);
            }
            else
                region.duration = urand(0, config.frontInterval);
        }

        regions.swap(rebuilt);
        return rules.size();
    }

    void AdvanceRegions(uint32 diff)
    {
        regionStepTimer += diff;
        if (regionStepTimer < IN_MILLISECONDS)
            return;

        uint32 const step = regionStepTimer;
        regionStepTimer = 0;

        std::lock_guard<std::mutex> guard(lock);
        for (auto& [key, region] : regions)
            AdvanceRegion(region, step);
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

    char const* KindName(Kind kind)
    {
        switch (kind)
        {
            case Kind::Rain: return "rain";
            case Kind::Snow: return "snow";
            case Kind::Sand: return "sandstorm";
            case Kind::Fog:  return "fog";
            default:         return "nothing";
        }
    }

    char const* PhaseName(Phase phase)
    {
        switch (phase)
        {
            case Phase::BuildUp: return "building up";
            case Phase::Peak:    return "at its height";
            case Phase::Taper:   return "tapering off";
            default:             return "dry";
        }
    }

    char const* ZoneName(ChatHandler* handler, uint32 zone)
    {
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(zone);
        return area ? area->area_name[handler->GetSessionDbcLocale()] : "?";
    }
}

class ModWeatherScript : public WeatherScript
{
public:
    ModWeatherScript() : WeatherScript("mod_weather") { }

    void OnChange(Weather* weather, WeatherState /*state*/, float /*grade*/) override
    {
        if (applying || !config.enabled)
            return;

        ZoneTick(weather, true);
    }

    void OnUpdate(Weather* weather, uint32 /*diff*/) override
    {
        if (config.enabled)
            ZoneTick(weather, false);
    }
};

class ModWeatherWorldScript : public WorldScript
{
public:
    ModWeatherWorldScript() : WorldScript("ModWeatherWorldScript") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        auto minutes = [](char const* name, uint32 fallback)
        {
            return sConfigMgr->GetOption<uint32>(name, fallback) * MINUTE * IN_MILLISECONDS;
        };

        config.enabled       = sConfigMgr->GetOption<bool>("Weather.Enable", true);
        config.frontInterval = std::max<uint32>(minutes("Weather.FrontInterval", 10), MINUTE * IN_MILLISECONDS);
        config.buildUp       = { minutes("Weather.BuildUpMin", 3), minutes("Weather.BuildUpMax", 6) };
        config.peak          = { minutes("Weather.PeakMin", 10), minutes("Weather.PeakMax", 25) };
        config.taper         = { minutes("Weather.TaperMin", 5), minutes("Weather.TaperMax", 8) };
        config.dry           = { minutes("Weather.DryMin", 20), minutes("Weather.DryMax", 60) };
        config.mediumChance  = std::min<uint32>(sConfigMgr->GetOption<uint32>("Weather.MediumChance", 30), 100);
        config.heavyChance   = std::min<uint32>(sConfigMgr->GetOption<uint32>("Weather.HeavyChance", 10), 100);
        config.faintGrade    = std::clamp(sConfigMgr->GetOption<float>("Weather.FaintGrade", 0.10f), 0.01f, GRADE_VISIBLE);
        config.fog           = sConfigMgr->GetOption<bool>("Weather.Fog", true);
        config.fogInterval   = sConfigMgr->GetOption<uint32>("Weather.FogInterval", 600) * IN_MILLISECONDS;
        config.mistAfterRain = sConfigMgr->GetOption<bool>("Weather.MistAfterRain", true);
        config.thunderstorms = sConfigMgr->GetOption<bool>("Weather.Thunderstorms", true);
        config.blackRain     = sConfigMgr->GetOption<bool>("Weather.BlackRain", true);
        config.blackSnow     = sConfigMgr->GetOption<bool>("Weather.BlackSnow", true);

        if (config.fogInterval < MINUTE * IN_MILLISECONDS)
            config.fogInterval = MINUTE * IN_MILLISECONDS;

        // The core has just read ChangeWeatherInterval. Keep it for Quel'Danas, then stretch it so
        // the core stops rolling. Weathers already running keep their interval, so turning the
        // module on or off needs a restart.
        config.coreInterval = sWorld->getIntConfig(CONFIG_INTERVAL_CHANGEWEATHER);
        if (config.enabled)
            sWorld->setIntConfig(CONFIG_INTERVAL_CHANGEWEATHER, CORE_INTERVAL_OFF);
    }

    void OnStartup() override
    {
        uint32 const zoneCount = LoadRules();
        LOG_INFO("module", "mod-weather: {} weather zones in {} regions{}.", zoneCount, regions.size(),
            config.enabled ? "; the core's own weather rolls are off" : "; module off");

        if (!WeatherMgr::GetWeatherData(3537)) // Borean Tundra, one of the zones the SQL adds
            LOG_ERROR("module", "mod-weather: `game_weather` has no row for Borean Tundra. Apply the module's SQL.");
    }

    void OnUpdate(uint32 diff) override
    {
        if (config.enabled)
            AdvanceRegions(diff);
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
            { "start",  HandleStartCommand,  SEC_GAMEMASTER,    Console::No  },
            { "stop",   HandleStopCommand,   SEC_GAMEMASTER,    Console::No  },
            { "show",   HandleShowCommand,   SEC_GAMEMASTER,    Console::No  },
            { "reload", HandleReloadCommand, SEC_ADMINISTRATOR, Console::Yes },
        };

        static ChatCommandTable commandTable =
        {
            { "weather", subCommands },
        };

        return commandTable;
    }

    // The region, its front, and what the zone you're standing in shows.
    static bool HandleInfoCommand(ChatHandler* handler)
    {
        uint32 const zone = handler->GetPlayer()->GetZoneId();
        char const* name = ZoneName(handler, zone);

        if (!WeatherMgr::GetWeatherData(zone))
        {
            handler->PSendSysMessage("{} (zone {}) has no weather: no `game_weather` row.", name, zone);
            return true;
        }

        if (zone == ZONE_ISLE_OF_QUEL_DANAS)
        {
            handler->PSendSysMessage("{} (zone {}) is left to the core, because the Sunwell event sets its weather.",
                name, zone);
            return true;
        }

        std::lock_guard<std::mutex> guard(lock);

        ZoneRule const& rule = RuleFor(zone);
        Region const* region = RegionOf(zone);
        if (!region)
        {
            handler->PSendSysMessage("{} (zone {}) isn't loaded yet. Try .weather reload.", name, zone);
            return true;
        }

        std::string members;
        for (uint32 member : region->zones)
        {
            if (!members.empty())
                members += ", ";
            members += ZoneName(handler, member);
        }

        if (rule.region >= OWN_REGION)
            handler->PSendSysMessage("{} (zone {}) is a region of its own.", name, zone);
        else
            handler->PSendSysMessage("{} (zone {}) is in region {}: {}.", name, zone, rule.region, members);

        uint32 const minutesLeft = (region->duration - std::min(region->elapsed, region->duration)) / (MINUTE * IN_MILLISECONDS);
        if (region->phase == Phase::Dry)
            handler->PSendSysMessage("The region is dry. Next roll in {} min, {}% chance of a front.", minutesLeft,
                RegionChance(*region));
        else
            handler->PSendSysMessage("Front {} is {} (peak {:.2f}), strength now {:.2f}, {} min left in this phase{}.",
                region->frontId, PhaseName(region->phase), region->peak, region->strength, minutesLeft,
                region->forced ? ", started by a GM" : "");

        handler->PSendSysMessage("Rules: strength {}%, fog {}%, thunderstorms {}%, black rain {}, black snow {}.",
            rule.strength, rule.fogChance, rule.thunderChance, rule.blackRain ? "yes" : "no", rule.blackSnow ? "yes" : "no");

        auto zoneItr = zones.find(zone);
        if (zoneItr == zones.end() || !zoneItr->second.seen)
        {
            handler->SendSysMessage("This zone hasn't had its weather sent yet.");
            return true;
        }

        ZoneState const& zoneState = zoneItr->second;
        if (region->phase != Phase::Dry && zoneState.frontId == region->frontId)
        {
            if (zoneState.joined)
                handler->PSendSysMessage("This zone gets {} from the front{}.", KindName(zoneState.frontKind),
                    zoneState.thunder ? ", with thunder if it gets heavy" : "");
            else
                handler->SendSysMessage("This zone sits this front out.");
        }

        handler->PSendSysMessage("Showing {} at {:.2f} (sent: {} at {:.2f}){}.", KindName(zoneState.kind), zoneState.grade,
            StateName(zoneState.sentState), zoneState.sentGrade, zoneState.continent ? "" : ", inside an instance");
        return true;
    }

    // Starts a front in your region now, which every zone in it joins.
    static bool HandleStartCommand(ChatHandler* handler, Optional<std::string> strength)
    {
        float peak = 0.0f;
        std::string const which = strength.value_or("");
        if (which.empty())
            peak = RollPeak();
        else if (which == "light")
            peak = frand(0.30f, 0.40f);
        else if (which == "medium")
            peak = frand(0.45f, 0.65f);
        else if (which == "heavy")
            peak = frand(0.75f, 0.95f);
        else
        {
            handler->SendSysMessage("Usage: .weather start [light|medium|heavy]");
            return true;
        }

        std::lock_guard<std::mutex> guard(lock);

        Region* region = RegionOf(handler->GetPlayer()->GetZoneId());
        if (!region)
        {
            handler->SendSysMessage("This zone has no weather.");
            return true;
        }

        StartFront(*region, peak, true);
        handler->PSendSysMessage("Front {} building up to {:.2f} over {} min.", region->frontId, peak,
            region->duration / (MINUTE * IN_MILLISECONDS));
        return true;
    }

    // Starts the taper of your region's front now.
    static bool HandleStopCommand(ChatHandler* handler)
    {
        std::lock_guard<std::mutex> guard(lock);

        Region* region = RegionOf(handler->GetPlayer()->GetZoneId());
        if (!region || region->phase == Phase::Dry || region->phase == Phase::Taper)
        {
            handler->SendSysMessage("No front here to stop.");
            return true;
        }

        StartTaper(*region);
        handler->PSendSysMessage("Front {} tapering off over {} min.", region->frontId,
            region->duration / (MINUTE * IN_MILLISECONDS));
        return true;
    }

    // Sends a weather state to you alone, to see how the client draws it. Any state the client
    // knows works, including fog (1) and black snow (106), which .wchange can't set. It lasts until
    // the zone's weather next changes or you change zones.
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
        uint32 const zoneCount = LoadRules();
        handler->PSendSysMessage("mod-weather: reloaded {} weather zones. Fronts already running carry on. "
            "New `game_weather` rows need a restart.", zoneCount);
        return true;
    }
};

void AddModWeatherScripts()
{
    new ModWeatherScript();
    new ModWeatherWorldScript();
    new ModWeatherCommandScript();
}
