-- Moonlit Blessings Stage 2: bind the C++ spell scripts. Idempotent. ~Moonlit Nights Team
DELETE FROM `spell_script_names` WHERE `spell_id` IN (1640010, 1640011, 1640012, 1640030);
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(1640010, 'spell_blessings_eternal_aegis'),
(1640011, 'spell_blessings_eternal_aegis_shield'),
(1640012, 'spell_blessings_martyrs_mantle'),
(1640030, 'spell_blessings_sanctified_blow');
