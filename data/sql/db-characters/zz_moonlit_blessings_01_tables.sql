-- Moonlit Blessings: per-character, per-talent-spec choices. Idempotent (CREATE IF NOT EXISTS).
CREATE TABLE IF NOT EXISTS `moonlit_blessing` (
  `guid` INT UNSIGNED NOT NULL,
  `spec` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `calling` VARCHAR(24) NOT NULL DEFAULT '',
  `boon1` TINYINT NOT NULL DEFAULT -1,
  `boon2` TINYINT NOT NULL DEFAULT -1,
  `boon3` TINYINT NOT NULL DEFAULT -1,
  `boon4` TINYINT NOT NULL DEFAULT -1,
  `boon5` TINYINT NOT NULL DEFAULT -1,
  `boon6` TINYINT NOT NULL DEFAULT -1,
  `prime` TINYINT NOT NULL DEFAULT -1,
  PRIMARY KEY (`guid`, `spec`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Moonlit Blessings choices (talent overhaul testing stage)';
-- ~Moonlit Nights Team
