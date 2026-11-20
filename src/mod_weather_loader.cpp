/*
 * mod-weather loader.
 *
 * AzerothCore calls a loader whose name it derives from the module's folder name: folder
 * "mod-weather" calls "Addmod_weatherScripts", and folder "wow-mod-weather" (what a plain
 * `git clone` of this repository creates) calls "Addwow_mod_weatherScripts". Both are defined
 * here, so the module builds and loads under either name; the generated loader only calls the
 * one for the folder it found. For any other folder name, add a function named after it.
 *
 * Released under GNU GPL v2 or (at your option) any later version.
 */

void AddModWeatherScripts();

void Addmod_weatherScripts()
{
    AddModWeatherScripts();
}

void Addwow_mod_weatherScripts()
{
    AddModWeatherScripts();
}
