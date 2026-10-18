# mod-moonlit-callings

The C++ half of the Moonlit Callings talent system (testing stage; Shaman and Priest). It closes the stock talent window and trainers for the configured classes
and carries the characters-database table. The Lua system is `lua_scripts/moonlit_callings` in the main Moonlit Nights repository; the design is in
`docs/Ruling_Talent_Overhaul_Reference.md`.

Config: `MoonlitCallings.Enable`, `MoonlitCallings.Classes` (class IDs, default 5 and 7).
