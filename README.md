# mod-moonlit-blessings

The C++ half of the Moonlit Blessings talent system (testing stage; Paladin, Priest and Shaman). It closes the stock talent window and trainers for the configured classes
and carries the characters-database table. The Lua system is `lua_scripts/moonlit_blessings` in the main Moonlit Nights repository; the design is in
`docs/Ruling_Talent_Overhaul_Reference.md`.

Config: `MoonlitBlessings.Enable`, `MoonlitBlessings.Classes` (class IDs, default 2, 5 and 7).
