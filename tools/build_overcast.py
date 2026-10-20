#!/usr/bin/env python3
"""The client patch for mod-weather's overcast skies, and the module's list of its rows
(`mod_weather_overcast`) that goes with it.

    python3 tools/build_overcast.py --client /path/to/client --out patch-Z.MPQ \\
        --sql data/sql/db-world/mod_weather_overcast.sql [--patch your-lighting-patch.MPQ ...]

--client is a 3.3.5a client folder (with Data/ and Data/<locale>/). Each of the four lighting
tables (Light, LightParams, LightIntBand, LightFloatBand) is read from the last archive the client
loads that has it: Data/patch-*.MPQ other than patch, patch-2 and patch-3, last name first, then
Data/<locale>/patch-<locale>-*.MPQ, then Data/patch-3, patch-2 and patch.MPQ, then
Data/<locale>/locale-<locale>.MPQ. --patch adds patches your players load after those (give them
in load order), such as mod-night-brightness's patch; leave the built overcast patch itself out
(a build on top of an old overcast patch stops).

Weather never touches the sky in 3.3.5. The client has a blend for it (a light's storm params,
slot 2, mixed in by a global, 0xD38B88 = min(1, 4 x 0xD38B4C) in Wow.exe 12340, read at 0x7EE559),
but nothing ever writes its input: it stays at the 0 it gets when the client starts (0x7F2854).
What the server can do is lay a light of its own over the sky: SMSG_OVERRIDE_LIGHT names a Light
row to replace, the row to use in its place and a fade time.

A swap is blended over everything the client has lit so far, by the same rule as a zone light
(0x7EE5D0): all of it inside the inner radius, fading to nothing at the outer one, and never more
than its own fade in (0x7F1590). A swap is drawn wherever the camera is inside the reach of the
row it replaces. So the overcast is two kinds of rows:

- a palette: the colours of an overcast sky (LightParams with its LightIntBand and LightFloatBand
  rows, and a Light row that uses it). One for each kind of weather: rain, storm (thunder, ash and
  black snow), snow and sand.
- a weight: a Light row whose reach covers the whole world but whose middle is so far away that
  every place in it is the same share of the way out. The client turns a row's place into world
  coordinates (17066.67 - z/36, 17066.67 - x/36, y/36, at 0x7EAF90), so a row at y = 36 x DEPTH
  sits DEPTH yards straight up; with an inner radius of 0 and an outer one of DEPTH / (1 - w), a
  camera anywhere on a map is drawn at w, give or take 0.00015. WEIGHTS of them.

A swap of weight w with a palette is the sky the client would show, w of the way to the palette:
it keeps every zone's own light and its night, and only pulls it toward grey. mod-weather picks
the weight from how heavy the weather is and switches it a step at a time as a front builds and
tapers, with the client's own fade in between.

    sunlight          weak and grey: the sun is behind clouds
    ambient           about what a zone has: shade stays as light as it was
    sky and horizon   grey, a little darker at the top
    fog               the horizon's colour, and closer (125 to 200 yards)
    sun disc          dim, with little glow through the clouds; clouds dense
    water             as clear as the client's typical zone has it

The night. The palettes are colours for the day; by night they get darker on the client's own
clock, so an overcast night is as dark as the night under it. The builder reads that clock from
the client: for each of the 18 colour bands, at every 90 minutes, the median over the continents'
clear-weather lights of the band's brightness as a share of its brightness at noon (never more
than 1). Its lowest share is the band's night. The sky, fog, sun and cloud bands (2-13) go
further, to NIGHT_SKY of that night at the darkest time: a cloud cover is darker at night than the
clear sky it hides, so an overcast night never lifts a black sky to grey. Sunlight, ambient and
water keep the client's night alone, so a storm doesn't make the night harder to see in. A stock
client's night is mild (most of its colours are about 80% of the day at midnight, the sky top
about 25%), and the overcast's follows it.

The rows sit on a map number that doesn't exist (from 9001 up), so the client never lights
anything with them unasked. Their IDs are picked above the highest the tables already have (a
Light, a LightParams and its band rows), and checked to be free. The four tables keep their order
(the client goes through Light.dbc row by row): the new rows go at the end.

The patch holds all four lighting tables, so it has to load after every other patch that has
one, mod-night-brightness's included: name it so it sorts after them. A stock Wow.exe only loads
patches with a one-character name (patch-Z.MPQ sorts after the letters and digits); a longer name
such as patch-z99-overcast.MPQ needs a client that loads those. Take patches the client doesn't
load out of Data/ while building, or the builder reads them. Build it again whenever any of them
changes, and build them first. The patch and the SQL have to come from the same build.

--sql writes `mod_weather_overcast` for your client: the module's
data/sql/db-world/mod_weather_overcast.sql in your server's copy of the module (the one in the
repository was built for another client). It has the rows for night brightness level 3, the
client's own night. With mod-night-brightness, a player at another level gets no overcast.

Needs Python 3 and StormLib (`STORMLIB`, default /usr/local/lib/libstorm.dylib).
"""

import argparse
import ctypes
import glob
import os
import statistics
import struct
import sys
import tempfile

LIGHT = "DBFilesClient\\Light.dbc"
PARAMS = "DBFilesClient\\LightParams.dbc"
INTBAND = "DBFilesClient\\LightIntBand.dbc"
FLOATBAND = "DBFilesClient\\LightFloatBand.dbc"
MAPS = "DBFilesClient\\Map.dbc"
TABLES = (LIGHT, PARAMS, INTBAND, FLOATBAND)
# Light.dbc: ID, map, x, y, z, inner, outer (floats), 8 LightParams slots.
LIGHT_FIELDS, SLOTS = 15, 7
# LightParams.dbc: ID, highlight sky, skybox, cloud type, glow, 4 water alphas (floats).
PARAM_FIELDS, WATER_FIELDS = 9, 5
# LightParams has 18 LightIntBand rows (ID = (param - 1) * 18 + band + 1) and 6 LightFloatBand rows.
INT_BANDS, FLOAT_BANDS = 18, 6
# Both band tables: ID, key count, 16 times, 16 values. Times are in half minutes.
KEYS = 16
BAND_FIELDS = 2 + KEYS * 2
DAY, NOON = 2880, 1440
# The palettes' keys: every 90 minutes, midnight and noon among them.
CLOCK = tuple(range(0, DAY, DAY // KEYS))
# The maps whose clear-weather lights are the typical zone: Eastern Kingdoms, Kalimdor, Outland,
# Northrend.
CONTINENTS = (0, 1, 530, 571)

# The rows sit on the first map from here up that neither Map.dbc nor Light.dbc has.
OVERCAST_MAP = 9001
# The weights a swap can have: 0.05 to 0.90.
WEIGHTS = [round(0.05 * n, 2) for n in range(1, 19)]
# How far up the weight rows sit, in yards. The camera is never more than a few thousand yards
# from the ground or 48,300 from the middle of a map's square, so a weight is off by under
# 0.00015 (3,000 yards up).
DEPTH = 2.0e7
# The param slots filled: clear, clear under water, storm, storm under water. The client only
# reads the first two (0x7EE510) while the storm blend above is dead.
FILLED = 4
# A cloud cover is darker at night than the clear sky it hides: the sky, fog, sun and cloud
# bands (2-13) go down to this share of the client's night.
NIGHT_SKY = 0.5

# mod-weather's palette numbers (mod_weather_overcast.palette).
RAIN, STORM, SNOW, SAND = 1, 2, 3, 4

# The day colours of each LightIntBand band (R, G, B): sunlight, ambient, sky top to horizon (5),
# fog, band 8, sun disc, clouds (lit, emissive, second layer, dark side), ocean close and far,
# river close and far. Made for a zone that has sunlight (199, 162, 136), ambient
# (91, 100, 113), a sky top of (35, 48, 73) and fog of (77, 81, 96) at noon; the builder prints
# your client's.
COLOURS = {
    RAIN: [(112, 114, 120), (104, 108, 116),
           (104, 109, 117), (118, 122, 129), (128, 132, 138), (136, 139, 145), (140, 143, 148),
           (136, 139, 144), (70, 72, 76), (96, 98, 102),
           (122, 125, 130), (88, 91, 96), (52, 54, 58), (84, 88, 95),
           (46, 58, 64), (32, 44, 50), (52, 62, 58), (40, 52, 52)],
    STORM: [(70, 72, 80), (74, 77, 86),
            (56, 60, 68), (66, 70, 78), (76, 80, 87), (84, 88, 94), (90, 93, 99),
            (86, 89, 95), (50, 52, 56), (60, 62, 66),
            (80, 83, 90), (58, 60, 66), (36, 38, 42), (54, 58, 64),
            (36, 46, 52), (24, 34, 40), (40, 48, 46), (30, 40, 40)],
    SNOW: [(150, 154, 162), (140, 146, 158),
           (150, 156, 166), (166, 171, 180), (178, 182, 190), (186, 190, 197), (192, 196, 202),
           (190, 194, 200), (90, 92, 96), (140, 142, 146),
           (180, 184, 190), (130, 134, 142), (80, 82, 88), (120, 126, 136),
           (60, 74, 84), (44, 58, 68), (66, 78, 80), (52, 66, 70)],
    SAND: [(168, 140, 104), (150, 128, 100),
           (150, 124, 92), (166, 138, 102), (178, 150, 112), (186, 158, 118), (192, 164, 124),
           (190, 160, 120), (90, 78, 64), (150, 130, 100),
           (180, 152, 114), (120, 100, 76), (70, 58, 44), (110, 92, 70),
           (70, 70, 60), (52, 54, 48), (76, 74, 60), (62, 62, 52)],
}
# LightFloatBand: fog distance (yards x 36; a typical zone has about 18,750), fog multiplier,
# celestial glow through, cloud density. The last two bands are the client's typical zone's.
FLOATS = {
    RAIN: [7200.0, 0.0, 0.2, 1.0],
    STORM: [6000.0, 0.0, 0.1, 1.0],
    SNOW: [6000.0, 0.0, 0.3, 1.0],
    SAND: [4500.0, 0.0, 0.3, 0.8],
}
# LightParams glow; the water alphas are the client's typical zone's, so the water stays as clear
# as it was.
GLOW = {RAIN: 0.15, STORM: 0.1, SNOW: 0.25, SAND: 0.3}

BASE_PATCHES = ("patch-3.mpq", "patch-2.mpq", "patch.mpq")


# --- MPQ ---------------------------------------------------------------------------------------

H = ctypes.c_void_p
lib = None


def storm():
    """StormLib, loaded on first use."""
    global lib
    if lib is None:
        lib = ctypes.CDLL(os.environ.get("STORMLIB", "/usr/local/lib/libstorm.dylib"))
        for name, args in [
            ("SFileOpenArchive", [ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(H)]),
            ("SFileCreateArchive", [ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(H)]),
            ("SFileAddFileEx", [H, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint]),
            ("SFileExtractFile", [H, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]),
            ("SFileCloseArchive", [H]),
        ]:
            getattr(lib, name).argtypes = args
            getattr(lib, name).restype = ctypes.c_bool
    return lib


def mpqs(folder, pattern):
    return [p for p in glob.glob(os.path.join(folder, "*")) if os.path.basename(p).lower().endswith(".mpq")
            and pattern(os.path.basename(p).lower())]


def same_file(a, b):
    return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))


def search_order(client, locale, extra):
    """The archives that can hold a lighting table, the one the client uses first."""
    data, loc = os.path.join(client, "Data"), locale.lower()
    # Last loaded first: patch-enUS-3 before patch-enUS-2 before patch-enUS.
    by_name = lambda paths: sorted(paths, key=lambda p: os.path.basename(p).lower()[:-4], reverse=True)
    order = list(reversed(extra))
    order += by_name(mpqs(data, lambda n: n.startswith("patch-") and n not in BASE_PATCHES))
    order += by_name(mpqs(os.path.join(data, locale), lambda n: n.startswith("patch-%s" % loc)))
    found = {os.path.basename(p).lower(): p for p in mpqs(data, lambda n: n in BASE_PATCHES)}
    order += [found[n] for n in BASE_PATCHES if n in found]
    order += mpqs(os.path.join(data, locale), lambda n: n == "locale-%s.mpq" % loc)
    return order


def extract(archives, name, dst):
    """The first copy of name in archives; returns the archive it came from."""
    storm()
    for mpq in archives:
        h = H()
        if not lib.SFileOpenArchive(mpq.encode(), 0, 0x100, ctypes.byref(h)):
            sys.exit("can't open " + mpq)
        try:
            if lib.SFileExtractFile(h, name.encode("latin-1"), dst.encode(), 0):
                return mpq
        finally:
            lib.SFileCloseArchive(h)
    sys.exit("no %s in the client" % name)


def pack(out, files):
    """files: [(local path, archive name)]. zlib-compressed, with listfile and attributes."""
    storm()
    if os.path.exists(out):
        os.remove(out)
    h = H()
    if not lib.SFileCreateArchive(out.encode(), 0x00300000, max(64, len(files) * 2), ctypes.byref(h)):
        sys.exit("can't create " + out)
    for src, name in files:
        if not lib.SFileAddFileEx(h, src.encode(), name.encode("latin-1"), 0x80000200, 0x02, 0x02):
            lib.SFileCloseArchive(h)
            os.remove(out)
            sys.exit("can't add " + name)
    if not lib.SFileCloseArchive(h):
        os.remove(out)
        sys.exit("can't finish " + out)


# --- DBC ---------------------------------------------------------------------------------------

class Table:
    """A string-less WDBC file as {ID: list of uint32 fields}, in the file's order, which is kept:
    new rows go at the end, with IDs higher than all."""

    def __init__(self, path, fields):
        data = open(path, "rb").read()
        magic, count, self.fields, size, strsize = struct.unpack_from("<4s4I", data, 0)
        if magic != b"WDBC" or size != self.fields * 4 or strsize > 1:
            sys.exit(path + " isn't a WDBC file without strings")
        if self.fields != fields:
            sys.exit("%s has %d fields, not %d" % (path, self.fields, fields))
        self.rows = {}
        for i in range(count):
            row = list(struct.unpack_from("<%dI" % self.fields, data, 20 + i * size))
            self.rows[row[0]] = row
        if len(self.rows) != count:
            sys.exit(path + " has rows with the same ID")

    def add(self, row):
        if len(row) != self.fields or self.rows and row[0] <= max(self.rows):
            sys.exit("row %d doesn't fit at the end" % row[0])
        self.rows[row[0]] = row

    def write(self, path):
        with open(path, "wb") as out:
            out.write(struct.pack("<4s4I", b"WDBC", len(self.rows), self.fields, self.fields * 4, 1))
            for row in self.rows.values():
                out.write(struct.pack("<%dI" % self.fields, *row))
            out.write(b"\0")


def u32(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def f32(value):
    return struct.unpack("<f", struct.pack("<I", value))[0]


def map_ids(archives):
    """Every map of the client."""
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "Map.dbc")
        extract(archives, MAPS, path)
        data = open(path, "rb").read()
    magic, count, _, size, _ = struct.unpack_from("<4s4I", data, 0)
    if magic != b"WDBC":
        sys.exit("Map.dbc isn't a WDBC file")
    return {struct.unpack_from("<I", data, 20 + i * size)[0] for i in range(count)}


# --- The client's typical zone -----------------------------------------------------------------

def channels(colour):
    """R, G, B."""
    return [(colour >> shift) & 0xFF for shift in (16, 8, 0)]


def colour(rgb):
    r, g, b = rgb
    return (r << 16) | (g << 8) | b


def value_at(times, values, t):
    """The client's own reading of a row: a straight line between keys, around the clock."""
    before = max((i for i in range(len(times)) if times[i] <= t), default=len(times) - 1)
    after = (before + 1) % len(times)
    span = (times[after] - times[before]) % DAY or DAY
    share = ((t - times[before]) % DAY) / span
    return values[before] + (values[after] - values[before]) * share


def keyed(row):
    """A band row's key times and values, or None for one without keys in order."""
    count = row[1]
    times = list(row[2:2 + count])
    if not 1 <= count <= KEYS or times != sorted(set(times)):
        return None
    return times, row[2 + KEYS:2 + KEYS + count]


def typical_params(lights):
    """The LightParams of the continents' lights in clear weather."""
    return sorted({row[SLOTS] for row in lights.values() if row[1] in CONTINENTS and row[SLOTS]})


def night_curve(ints, params):
    """For each band, its brightness at each CLOCK time as a share of noon's: the median over
    params of the rows that change with the time of day, never more than 1."""
    curve = []
    for band in range(INT_BANDS):
        shares = {t: [] for t in CLOCK}
        for p in params:
            row = ints.get((p - 1) * INT_BANDS + band + 1)
            keys = row and keyed(row)
            if not keys or len(keys[0]) < 2:
                continue
            times, colours = keys
            light = lambda t: sum(value_at(times, [channels(c)[i] for c in colours], t) for i in range(3))
            noon = light(NOON)
            if noon > 0:
                for t in CLOCK:
                    shares[t].append(light(t) / noon)
        curve.append([min(1.0, statistics.median(shares[t])) if shares[t] else 1.0 for t in CLOCK])
    return curve


def typical_noon(ints, params, band):
    """The median colour of a band at noon."""
    found = []
    for p in params:
        row = ints.get((p - 1) * INT_BANDS + band + 1)
        keys = row and keyed(row)
        if keys:
            found.append([value_at(keys[0], [channels(c)[i] for c in keys[1]], NOON) for i in range(3)])
    return tuple(round(statistics.median(c[i] for c in found)) for i in range(3)) if found else None


def typical_floats(flts, params):
    """The median noon value of each LightFloatBand band."""
    out = []
    for band in range(FLOAT_BANDS):
        found = []
        for p in params:
            row = flts.get((p - 1) * FLOAT_BANDS + band + 1)
            keys = row and keyed(row)
            if keys:
                found.append(value_at(keys[0], [f32(v) for v in keys[1]], NOON))
        if not found:
            sys.exit("the continents' lights have no LightFloatBand band %d" % band)
        out.append(statistics.median(found))
    return out


# --- The overcast rows -------------------------------------------------------------------------

def band_row(row_id, value):
    """A LightIntBand or LightFloatBand row holding one value all day, as two keys (midnight and
    noon)."""
    return [row_id, 2, 0, NOON] + [0] * 14 + [value, value] + [0] * 14


def colour_row(row_id, rgb, curve, night_share):
    """A LightIntBand row with the colour by day and the client's night: at each CLOCK time the
    curve's share, going further down to night_share of the curve's lowest at the darkest time."""
    low = min(curve)
    if low >= 1:
        return band_row(row_id, colour(rgb))
    scales = [1 - (1 - share) / (1 - low) * (1 - night_share * low) for share in curve]
    keys = [colour(tuple(round(c * scale) for c in rgb)) for scale in scales]
    return [row_id, KEYS] + list(CLOCK) + keys


def old_overcast(lights, maps):
    """A weight row of an overcast patch the tables already have, or None."""
    return next((key for key, row in lights.items() if row[1] not in maps and row[3] == u32(36 * DEPTH)
                 and row[2] == row[4] == row[5] == 0), None)


def free_map(lights, maps):
    used = maps | {row[1] for row in lights.values()}
    return next(m for m in range(OVERCAST_MAP, OVERCAST_MAP + 1000) if m not in used)


def add(tables, maps):
    """Adds the overcast rows. Returns the overcast map, the night curve and
    [(palette, weight number, weight, area light, light)]."""
    lights, params = tables[LIGHT].rows, tables[PARAMS].rows
    ints, flts = tables[INTBAND].rows, tables[FLOATBAND].rows
    old = old_overcast(lights, maps)
    if old is not None:
        sys.exit("the lighting tables already have overcast rows (Light %d): leave the old overcast patch "
                 "out of the client folder and --patch" % old)
    if not (lights and params and ints and flts):
        sys.exit("a lighting table is empty")
    typical = [p for p in typical_params(lights) if p in params]
    if not typical:
        sys.exit("the client has no lights on the continents")
    curve = night_curve(ints, typical)
    alphas = [statistics.median(f32(params[p][i]) for p in typical) for i in range(WATER_FIELDS, PARAM_FIELDS)]
    last_floats = typical_floats(flts, typical)[len(FLOATS[RAIN]):]

    # IDs above every one the tables have: a Light row for each weight, then one for each palette;
    # a LightParams for each palette, whose band rows are above the band tables' too.
    weight_base = max(lights) + 1
    palette_base = weight_base + len(WEIGHTS)
    param_base = max(max(params), (max(ints) - 1) // INT_BANDS + 1, (max(flts) - 1) // FLOAT_BANDS + 1) + 1
    overcast_map = free_map(lights, maps)
    new_lights = list(range(weight_base, palette_base + len(COLOURS)))
    new_params = [param_base + n for n in range(len(COLOURS))]
    taken = [i for i in new_lights if i in lights] + [p for p in new_params if p in params]
    taken += [(p - 1) * INT_BANDS + b + 1 for p in new_params for b in range(INT_BANDS) if (p - 1) * INT_BANDS + b + 1 in ints]
    taken += [(p - 1) * FLOAT_BANDS + b + 1 for p in new_params for b in range(FLOAT_BANDS) if (p - 1) * FLOAT_BANDS + b + 1 in flts]
    if taken or any(row[1] == overcast_map for row in lights.values()):
        sys.exit("the overcast rows' IDs or map are taken: %s" % taken)

    rows = []
    for n, weight in enumerate(WEIGHTS, 1):
        tables[LIGHT].add([weight_base + n - 1, overcast_map, 0, u32(36 * DEPTH), 0,
                           0, u32(36 * DEPTH / (1 - weight))] + [0] * 8)
    for n, palette in enumerate(sorted(COLOURS)):
        p = param_base + n
        tables[PARAMS].add([p, 0, 0, 0, u32(GLOW[palette])] + [u32(a) for a in alphas])
        tables[LIGHT].add([palette_base + n, overcast_map, 0, 0, 0, 0, 0] + [p] * FILLED + [0] * (8 - FILLED))
        for band, rgb in enumerate(COLOURS[palette]):
            share = NIGHT_SKY if 2 <= band <= 13 else 1.0
            tables[INTBAND].add(colour_row((p - 1) * INT_BANDS + band + 1, rgb, curve[band], share))
        for band, value in enumerate(FLOATS[palette] + last_floats):
            tables[FLOATBAND].add(band_row((p - 1) * FLOAT_BANDS + band + 1, u32(value)))
        rows += [(palette, w, weight, weight_base + w - 1, palette_base + n) for w, weight in enumerate(WEIGHTS, 1)]
    return overcast_map, curve, rows


SQL_HEAD = """\
-- mod-weather: the Light.dbc rows of the overcast skies. A swap of `light` (the palette's
-- colours) on `area_light` (a row that reaches everywhere at `weight`) pulls the sky
-- `weight` of the way to the palette. `level` is the night brightness level the rows are
-- for (3 is the client's own night; the others are for mod-night-brightness).
-- Written by the patch builder together with the client patch, which holds the rows named here.
-- Don't edit; build both again.

CREATE TABLE IF NOT EXISTS `mod_weather_overcast` (
    `palette`    TINYINT UNSIGNED NOT NULL COMMENT '1 rain, 2 storm, 3 snow, 4 sand',
    `step`       TINYINT UNSIGNED NOT NULL,
    `level`      TINYINT UNSIGNED NOT NULL,
    `weight`     FLOAT NOT NULL,
    `area_light` INT UNSIGNED NOT NULL,
    `light`      INT UNSIGNED NOT NULL,
    PRIMARY KEY (`palette`, `step`, `level`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

DELETE FROM `mod_weather_overcast`;
INSERT INTO `mod_weather_overcast` (`palette`, `step`, `level`, `weight`, `area_light`, `light`) VALUES
"""
# The night brightness level the rows are for: the client's own night.
LEVEL = 3


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--client", required=True, help="the client folder (has Data/ and Data/<locale>/)")
    ap.add_argument("--locale", default="enUS")
    ap.add_argument("--patch", action="append", default=[],
                    help="a patch your players load after the client's own, in load order (repeatable)")
    ap.add_argument("--out", required=True, help="the patch to write")
    ap.add_argument("--sql", required=True, help="where to write the module's `mod_weather_overcast`")
    args = ap.parse_args()
    for mpq in args.patch:
        if not os.path.isfile(mpq):
            sys.exit("no such patch: " + mpq)
    if any(same_file(args.out, p) for p in args.patch):
        sys.exit("--out is one of the --patch files; build from the patches it loads after")

    archives = [mpq for mpq in search_order(args.client, args.locale, args.patch) if not same_file(mpq, args.out)]
    fields = {LIGHT: LIGHT_FIELDS, PARAMS: PARAM_FIELDS, INTBAND: BAND_FIELDS, FLOATBAND: BAND_FIELDS}
    tables = {}
    with tempfile.TemporaryDirectory() as tmp:
        for name in TABLES:
            path = os.path.join(tmp, name.split("\\")[1])
            source = extract(archives, name, path)
            tables[name] = Table(path, fields[name])
            print("%s from %s" % (name.split("\\")[1], source))
    before = {name: len(tables[name].rows) for name in TABLES}
    typical = typical_params(tables[LIGHT].rows)
    noon = {band: typical_noon(tables[INTBAND].rows, typical, band) for band in (0, 1, 2, 7)}

    overcast_map, curve, rows = add(tables, map_ids(archives))

    for row in tables[LIGHT].rows.values():
        for p in row[SLOTS:SLOTS + 8]:
            if p and p not in tables[PARAMS].rows:
                sys.exit("Light %d uses LightParams %d, which doesn't exist" % (row[0], p))

    # Both are written next to where they go and only then put in place, so a failed run leaves
    # neither or both.
    new_out, new_sql = args.out + ".new", args.sql + ".new"
    try:
        for path in (args.out, args.sql):
            os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        with open(new_sql, "w") as sql:
            sql.write(SQL_HEAD)
            sql.write(",\n".join("(%d, %d, %d, %.2f, %d, %d)" % (palette, n, LEVEL, weight, area, light)
                                 for palette, n, weight, area, light in rows) + ";\n")
        with tempfile.TemporaryDirectory() as tmp:
            out = []
            for name in TABLES:
                path = os.path.join(tmp, name.split("\\")[1])
                tables[name].write(path)
                out.append((path, name))
            pack(new_out, out)
        os.replace(new_out, args.out)
        os.replace(new_sql, args.sql)
    except OSError as error:
        sys.exit("can't write: %s" % error)
    finally:
        for path in (new_out, new_sql):
            if os.path.exists(path):
                os.remove(path)

    print("your client's typical zone at noon: sunlight %s, ambient %s, sky top %s, fog %s"
          % (noon[0], noon[1], noon[2], noon[7]))
    print("its night (the lowest share of noon): sunlight %.2f, ambient %.2f, sky top %.2f, fog %.2f, water %.2f"
          % (min(curve[0]), min(curve[1]), min(curve[2]), min(curve[7]), min(curve[14])))
    print("added %s rows on map %d: Light %d-%d"
          % ("/".join(str(len(tables[name].rows) - before[name]) for name in TABLES), overcast_map,
             rows[0][3], rows[-1][4]))
    print("%d palettes x %d weights (level %d) in %s" % (len(COLOURS), len(WEIGHTS), LEVEL, args.sql))
    print("wrote " + args.out)


if __name__ == "__main__":
    main()
