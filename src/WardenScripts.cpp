// Moonlit Blessings: C++ spell scripts for the Warden Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/warden.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
// Warden rework 2026-10-22: Bedrock Slam left the kit; every Favor has a SIGNATURE ABILITY (Stormstrike for the Titan's Maul, Lava Burst for the Stormward, Spectral Fang for the Spirit Wolf, see SignatureSpell);
// the Boons that name it work for all three. Triggers on a block became "dodge, block or parry" at 50% (the proc rows decide that, the scripts below only run when it fires).
//
//   spell_blessings_earthen_reprisal      avoidance proc: Nature damage to the attacker, 25% of attack power plus 25% of spell power
//   spell_blessings_stoneclaw_guard       unlimited absorb: a hit that leaves you below 50% health starts a 10 sec stone ward (20% of maximum health, that hit included), once per 30 sec
//   spell_blessings_avalanche             every 3 sec at 10+ Bedrock stacks: Nature damage pulse (15% attack power plus 15% spell power) around you
//   spell_blessings_living_stone          every second: heal 0.1% of maximum health per Bedrock stack
//   spell_blessings_granite_fists         damage done +1% per Bedrock stack (the amount is refreshed every second)
//   spell_blessings_spirit_of_granite     unlimited absorb: a hit that leaves you below 30% health starts a 6 sec damage reduction (once per 90 sec)
//   spell_blessings_ancestral_rebound     avoidance proc: heals the lowest-health ally within 40 yards for 3% of your maximum health
//   spell_blessings_salt_of_the_earth     avoidance proc: heals you for 1% of your maximum health
//   spell_blessings_warden_signature      bound to Stormstrike, Lava Burst (both ranks) and Spectral Fang: Soul of the Mountain heals 4%, Earthquake Step slows the enemies around you
//   spell_blessings_stormbound_reprisal   avoidance proc: the signature ability is ready again
//   spell_blessings_grounding_bulwark     unlimited absorb: direct-damage magic hits are absorbed completely with a chance equal to your dodge chance
//   spell_blessings_stone_resolve         every second: Bedrock cannot fade while Bedrock Barrier is up
//   spell_blessings_bedrock_stacks        bound to the Bedrock buff: 3% a stack instead of 2% while Stone Resolve is known
//   spell_blessings_tectonic_pull         keeps bosses out of the pull
//   spell_blessings_avatar_of_stone       the castable keystone: Bedrock is held at 15 stacks and cannot fade while it lasts
//   spell_blessings_storm_aegis           avoidance proc (every one counts): every third calls a chain lightning bolt at the attacker
//   spell_blessings_spirit_anchor         every 0.5 sec: an ally below 20% health gets an absorb of 30% of your maximum health (once per 2 min)
//   spell_blessings_bedrock_soul          bound to Bedrock Soul (the tank baseline passive, no shield needed): every hit taken or dealt calls the Bedrock buff (a stack, up to 15); a 1 sec tick keeps the shield armour helper on
//   spell_blessings_warden_teacher        the three Favors (1840290, 1840311, 1840320): teach their spells (and take the custom ones away again)
//   spell_blessings_molten_blast          Titan's Maul: puts the weaker Flame Shock on every enemy the cone hits
//   spell_blessings_stormward_block       Stormward: an avoidance gives a Charge, Shield Mastery, 8% mana (1.5 sec cooldown) and a shorter Fire Nova Totem cooldown
//   spell_blessings_stormward_charge      Stormward: the Charge ending takes the instant-and-free Surge with it
//   spell_blessings_shield_mastery        Stormward: armour equal to 30% of the shield's armour per stack
//   spell_blessings_wolf_form_toggle      Spirit Wolf: the form spell as a toggle
//   spell_blessings_wolf_form             Spirit Wolf: keeps the form's helpers (armour, resistances, speed, weapon attack power) on
//   spell_blessings_wolf_link             Spirit Wolf: Spirit Link picks the partner and links both
//   (Retired 2026-10-22 with their Boons: cairn_guard, living_rock, ancestral_fortitude, granite_skin, bedrock_keystone, unbroken, and the Titan's Reprisal reset of the Favor.)
//
// A passive's helper spell id (the buff or bolt a script casts) is read from the TriggerSpell column of one of its dummy effects, never hard-coded, except where another script has to find a passive (the ids below).
// ~Moonlit Nights Team

#include "BlessingsHelpers.h"
#include "Creature.h"
#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"

#include <vector>

namespace
{
    // Kit spells and markers (tools/talents/blessing_spells.py and the Warden recipe). Cross-check these against runtime/blessing_recipes.json when the recipe changes.
    enum WardenSpells : uint32
    {
        SPELL_BEDROCK                = 1840061,   // the stacking buff of Bedrock Soul
        SPELL_BEDROCK_BARRIER        = 1840040,   // the self absorb (kit ability)
        SPELL_BEDROCK_ARMOUR         = 1840062,   // hidden helper of Bedrock Soul: +50% base armour while a shield is worn
        SPELL_STORMSTRIKE            = 17364,
        SPELL_LAVA_BURST             = 51505,     // rank 1 (stock level 75: the realm cap is 60, so the Stormward Favor teaches it)
        SPELL_MOLTEN_BLAST           = 1840300,
        SPELL_FAVOR_TITANS_MAUL      = 1840290,   // the Favors' own passives: which one is on decides the signature ability
        SPELL_FAVOR_STORMWARD        = 1840310,
        SPELL_FAVOR_SPIRIT_WOLF      = 1840320,
        SPELL_SPIRIT_WOLF_FORM       = 1840321,   // the Ghost Wolf form of Favor of the Spirit Wolf
        SPELL_WOLF_HELPER            = 1840322,   // hidden: armour, spectral resistances and speed of the form
        SPELL_WOLF_ATTACK_POWER      = 1840323,   // hidden: attack power from the main hand weapon (set by the form script)
        SPELL_WOLF_LINK_AURA         = 1840325,   // the split-damage aura of Spirit Link
        SPELL_SPECTRAL_FANG          = 1840328,   // the Spirit Wolf's signature ability
        SPELL_STONE_RESOLVE          = 1840230,   // Boons that other scripts look for
        SPELL_SOUL_OF_THE_MOUNTAIN   = 1840370,
        SPELL_EARTHQUAKE_STEP        = 1840400    // its slow helper is the next id
    };

    constexpr uint32 BEDROCK_FULL = 15;
    constexpr uint32 AVALANCHE_STACKS = 10;
    constexpr int32 DAMAGE_PCT_OF_ATTACK_POWER = 25;        // Earthen Reprisal; Storm Aegis uses twice that
    constexpr int32 DAMAGE_PCT_OF_SPELL_POWER = 25;
    constexpr int32 AVALANCHE_PCT = 15;                     // percent of attack power and of spell power
    constexpr int32 STORM_AEGIS_PCT = 50;
    constexpr uint32 STORM_AEGIS_EVERY = 3;                 // every third avoidance
    constexpr int32 STONECLAW_HEALTH_PCT = 50;
    constexpr int32 STONECLAW_WARD_PCT = 20;                // percent of maximum health absorbed
    constexpr uint32 STONECLAW_COOLDOWN_MS = 30 * IN_MILLISECONDS;
    constexpr int32 SPIRIT_OF_GRANITE_HEALTH_PCT = 30;
    constexpr int32 SPIRIT_OF_GRANITE_REDUCTION_PCT = 25;
    constexpr uint32 SPIRIT_OF_GRANITE_COOLDOWN_MS = 90 * IN_MILLISECONDS;
    constexpr int32 SOUL_OF_THE_MOUNTAIN_HEAL_PCT = 4;
    constexpr int32 ANCESTRAL_REBOUND_HEAL_PCT = 3;
    constexpr int32 SALT_OF_THE_EARTH_HEAL_PCT = 1;
    constexpr float ALLY_RANGE = 40.0f;
    constexpr int32 SPIRIT_ANCHOR_HEALTH_PCT = 20;
    constexpr int32 SPIRIT_ANCHOR_SHIELD_PCT = 30;          // percent of the WARDEN's maximum health
    constexpr uint32 SPIRIT_ANCHOR_COOLDOWN_MS = 2 * MINUTE * IN_MILLISECONDS;
    constexpr int32 STONE_RESOLVE_PCT_PER_STACK = 3;        // Bedrock is 2% a stack without Stone Resolve
    constexpr int32 STORMWARD_MANA_PCT = 8;                 // percent of maximum mana per avoidance
    constexpr uint32 STORMWARD_MANA_COOLDOWN_MS = 1500;     // the mana part alone has this internal cooldown (the other parts do not)
    constexpr int32 STORMWARD_FIRE_NOVA_MS = 2000;          // taken off the Fire Nova Totem cooldown per avoidance
    constexpr int32 SHIELD_MASTERY_PCT_OF_SHIELD = 30;      // armour per stack, percent of the shield's armour
    constexpr float WOLF_LINK_RANGE = 40.0f;

    uint32 BedrockStacks(Unit const* unit)
    {
        Aura const* bedrock = unit->GetAura(SPELL_BEDROCK);
        return bedrock ? bedrock->GetStackAmount() : 0;
    }

    // The signature ability of the Warden's Favor (0 without one): Stormstrike for the Titan's Maul, Lava Burst (rank 1, taught by the Favor) for the Stormward, Spectral Fang for the Spirit Wolf.
    uint32 SignatureSpell(Unit const* unit)
    {
        if (unit->HasAura(SPELL_FAVOR_TITANS_MAUL))
            return SPELL_STORMSTRIKE;

        if (unit->HasAura(SPELL_FAVOR_STORMWARD))
            return SPELL_LAVA_BURST;

        if (unit->HasAura(SPELL_FAVOR_SPIRIT_WOLF))
            return SPELL_SPECTRAL_FANG;

        return 0;
    }

    // A share of the unit's attack power plus a share of its Nature spell power (the damage spells that use it ignore the caster's own bonuses, so nothing is counted twice).
    int32 PowerDamage(Unit* unit, int32 attackPct, int32 spellPct)
    {
        float const attackPower = unit->GetTotalAttackPowerValue(BASE_ATTACK);
        float const spellPower = float(std::max<int32>(unit->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_NATURE), 0));
        return std::max<int32>(1, int32(attackPower * float(attackPct) / 100.0f + spellPower * float(spellPct) / 100.0f));
    }

    // The group members (the Warden first) that are alive, in the Warden's map and within `range` yards.
    std::vector<Player*> NearbyAllies(Player* warden, float range)
    {
        std::vector<Player*> allies;
        allies.push_back(warden);
        if (Group* group = warden->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (member && member != warden && member->IsInWorld() && member->IsAlive() && member->IsInMap(warden) && warden->IsWithinDist(member, range))
                    allies.push_back(member);
            }
        }

        return allies;
    }

    // The armour value of the shield in the off hand (0 when none, or when it is broken).
    int32 ShieldArmour(Player const* player)
    {
        Item const* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
        if (!item || item->IsBroken())
            return 0;

        ItemTemplate const* proto = item->GetTemplate();
        if (proto->Class != ITEM_CLASS_ARMOR || proto->SubClass != ITEM_SUBCLASS_ARMOR_SHIELD)
            return 0;

        return int32(proto->Armor);
    }

    // A shield in the off hand (and not broken).
    bool HasShield(Player const* player)
    {
        Item const* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
        if (!item || item->IsBroken())
            return false;

        ItemTemplate const* proto = item->GetTemplate();
        return proto->Class == ITEM_CLASS_ARMOR && proto->SubClass == ITEM_SUBCLASS_ARMOR_SHIELD;
    }

    // What each Favor teaches (the recipe, tools/talents/recipes/warden.py, asserts these ids). `forget` is what is taken away when the Favor is dropped; `form` is the form aura that is then ended.
    struct TeachDef
    {
        uint32 aura;
        std::vector<uint32> learn;
        std::vector<uint32> forget;
        uint32 form;
    };

    TeachDef const* TeachOf(uint32 aura)
    {
        static std::vector<TeachDef> const defs =
        {
            { 1840290, { SPELL_STORMSTRIKE, SPELL_MOLTEN_BLAST }, { SPELL_MOLTEN_BLAST }, 0 },
            { 1840311, { SPELL_LAVA_BURST }, { }, 0 },
            { 1840320, { SPELL_SPIRIT_WOLF_FORM, 1840324, 1840326, 1840327, SPELL_SPECTRAL_FANG }, { SPELL_SPIRIT_WOLF_FORM, 1840324, 1840326, 1840327, SPELL_SPECTRAL_FANG }, SPELL_SPIRIT_WOLF_FORM }
        };

        for (TeachDef const& def : defs)
            if (def.aura == aura)
                return &def;

        return nullptr;
    }

    // The Fire Nova Totem spells (all ranks): an avoidance takes 2 sec off whichever of them is on cooldown.
    constexpr uint32 FIRE_NOVA_TOTEMS[] = { 1535, 8498, 8499, 11314, 11315, 25546, 25547, 61649, 61657 };

    // The attack power a feral druid gets from a weapon, for the weapons a druid can wield: Item::getFeralBonus, (DPS - 54.8) * 14.
    int32 FeralWeaponAttackPower(Player const* player)
    {
        Item const* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        if (!item || item->IsBroken())
            return 0;

        ItemTemplate const* proto = item->GetTemplate();
        if (proto->Class != ITEM_CLASS_WEAPON)
            return 0;

        switch (proto->SubClass)
        {
            case ITEM_SUBCLASS_WEAPON_MACE:
            case ITEM_SUBCLASS_WEAPON_MACE2:
            case ITEM_SUBCLASS_WEAPON_POLEARM:
            case ITEM_SUBCLASS_WEAPON_STAFF:
            case ITEM_SUBCLASS_WEAPON_FIST:
            case ITEM_SUBCLASS_WEAPON_DAGGER:
                return std::max<int32>(0, proto->getFeralBonus());
            default:
                return 0;
        }
    }

    // The Spirit Link partner: the explicit target if it is a group member within 40 yards, else the most injured other group member within 40 yards.
    Unit* FindLinkPartner(Unit* caster, Unit* explicitTarget)
    {
        Player* player = caster->ToPlayer();
        Group* group = player ? player->GetGroup() : nullptr;
        if (!group)
            return nullptr;

        auto usable = [&](Player* member)
        {
            return member && member != player && member->IsInWorld() && member->IsAlive() && member->IsInMap(player) && player->IsWithinDist(member, WOLF_LINK_RANGE);
        };

        if (explicitTarget && explicitTarget->IsPlayer() && usable(explicitTarget->ToPlayer()) && group->IsMember(explicitTarget->GetGUID()))
            return explicitTarget;

        Player* best = nullptr;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (usable(member) && (!best || member->GetHealthPct() < best->GetHealthPct()))
                best = member;
        }

        return best;
    }

    // An unlimited absorb aura only exists to run a script: the amount is -1 (the core hands the decision to the script's absorbAmount).
    void MakeUnlimited(int32& amount, bool& canBeRecalculated)
    {
        amount = -1;
        canBeRecalculated = false;
    }
}

// Earthen Reprisal: effect 0 is a dummy that names the Nature bolt; the proc row (a dodge, block or parry at 50%) decides when this runs.
class spell_blessings_earthen_reprisal : public AuraScript
{
    PrepareAuraScript(spell_blessings_earthen_reprisal);

    void HandleProc(ProcEventInfo& eventInfo)
    {
        Unit* warden = GetTarget();
        Unit* attacker = eventInfo.GetActor();
        if (!attacker || attacker == warden || !attacker->IsAlive())
            return;

        int32 damage = PowerDamage(warden, DAMAGE_PCT_OF_ATTACK_POWER, DAMAGE_PCT_OF_SPELL_POWER);
        warden->CastCustomSpell(attacker, GetSpellInfo()->Effects[EFFECT_0].TriggerSpell, &damage, nullptr, nullptr, true);
    }

    void Register() override
    {
        OnProc += AuraProcFn(spell_blessings_earthen_reprisal::HandleProc);
    }
};

// Stoneclaw Guard: effect 0 is an unlimited absorb, effect 1 a dummy that names the 10 sec ward. The cooldown is kept on the passive's own spell id. The hit that opens the ward is absorbed by it too: the script
// takes that share off by hand (the ward is not in force yet for this hit) and gives the new ward only what is left.
class spell_blessings_stoneclaw_guard : public AuraScript
{
    PrepareAuraScript(spell_blessings_stoneclaw_guard);

    void CalculateAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
    {
        MakeUnlimited(amount, canBeRecalculated);
    }

    void Absorb(AuraEffect* /*aurEff*/, DamageInfo& dmgInfo, uint32& absorbAmount)
    {
        Player* warden = GetTarget()->ToPlayer();
        uint32 const damage = dmgInfo.GetDamage();
        uint32 const ward = GetSpellInfo()->Effects[EFFECT_1].TriggerSpell;
        if (!warden || !damage || warden->HasAura(ward) || warden->HasSpellCooldown(GetSpellInfo()->Id))
            return;

        if (!warden->HealthBelowPctDamaged(STONECLAW_HEALTH_PCT, damage))
            return;

        warden->AddSpellCooldown(GetSpellInfo()->Id, 0, STONECLAW_COOLDOWN_MS);
        uint32 const shield = warden->CountPctFromMaxHealth(STONECLAW_WARD_PCT);
        absorbAmount = std::min(damage, shield);
        int32 remaining = int32(shield) - int32(absorbAmount);
        if (remaining > 0)
            warden->CastCustomSpell(warden, ward, &remaining, nullptr, nullptr, true);
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_blessings_stoneclaw_guard::CalculateAmount, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
        OnEffectAbsorb += AuraEffectAbsorbFn(spell_blessings_stoneclaw_guard::Absorb, EFFECT_0);
    }
};

// Avalanche: effect 0 ticks every 3 sec, effect 1 is a dummy that names the pulse spell (Nature damage only: its threat comes from the damage).
class spell_blessings_avalanche : public AuraScript
{
    PrepareAuraScript(spell_blessings_avalanche);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* warden = GetTarget();
        if (!warden->IsInCombat() || BedrockStacks(warden) < AVALANCHE_STACKS)
            return;

        int32 damage = PowerDamage(warden, AVALANCHE_PCT, AVALANCHE_PCT);
        warden->CastCustomSpell(warden, GetSpellInfo()->Effects[EFFECT_1].TriggerSpell, &damage, nullptr, nullptr, true);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_avalanche::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Living Stone: effect 0 ticks every second.
class spell_blessings_living_stone : public AuraScript
{
    PrepareAuraScript(spell_blessings_living_stone);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* warden = GetTarget();
        uint32 const stacks = BedrockStacks(warden);
        if (!warden->IsAlive() || !stacks)
            return;

        // 0.1% of maximum health per stack (CountPctFromMaxHealth only takes whole percents)
        BlessingsHelpers::HealUnit(warden, warden, std::max<uint32>(1, uint32(uint64(warden->GetMaxHealth()) * stacks / 1000)), GetSpellInfo()->Id);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_living_stone::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Granite Fists: effect 0 is the damage-done aura whose amount is the Bedrock stack count (1% each), effect 1 ticks every second to refresh it.
class spell_blessings_granite_fists : public AuraScript
{
    PrepareAuraScript(spell_blessings_granite_fists);

    void CalculateAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
    {
        canBeRecalculated = true;
        Unit* owner = GetUnitOwner();
        amount = owner ? int32(std::min(BedrockStacks(owner), BEDROCK_FULL)) : 0;
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        GetAura()->RecalculateAmountOfEffects();
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_blessings_granite_fists::CalculateAmount, EFFECT_0, SPELL_AURA_MOD_DAMAGE_PERCENT_DONE);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_granite_fists::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Ancestral Rebound: the proc row (a dodge, block or parry at 50%) decides when this runs. The ally with the lowest health percentage within 40 yards (the Warden counts) is healed for 3% of the WARDEN's
// maximum health; nobody is healed when that ally is at full health.
class spell_blessings_ancestral_rebound : public AuraScript
{
    PrepareAuraScript(spell_blessings_ancestral_rebound);

    void HandleProc(ProcEventInfo& /*eventInfo*/)
    {
        Player* warden = GetTarget()->ToPlayer();
        if (!warden || !warden->IsAlive())
            return;

        Player* lowest = nullptr;
        for (Player* ally : NearbyAllies(warden, ALLY_RANGE))
            if (!lowest || ally->GetHealthPct() < lowest->GetHealthPct())
                lowest = ally;

        if (!lowest || lowest->GetHealth() >= lowest->GetMaxHealth())
            return;

        BlessingsHelpers::HealUnit(warden, lowest, warden->CountPctFromMaxHealth(ANCESTRAL_REBOUND_HEAL_PCT), GetSpellInfo()->Id);
    }

    void Register() override
    {
        OnProc += AuraProcFn(spell_blessings_ancestral_rebound::HandleProc);
    }
};

// Salt of the Earth: the proc row (a dodge, block or parry at 50%, taken-only, phase 0) decides when this runs. The Warden is healed for 1% of his maximum health (nothing at full health).
class spell_blessings_salt_of_the_earth : public AuraScript
{
    PrepareAuraScript(spell_blessings_salt_of_the_earth);

    void HandleProc(ProcEventInfo& /*eventInfo*/)
    {
        Unit* warden = GetTarget();
        if (!warden || !warden->IsAlive() || warden->GetHealth() >= warden->GetMaxHealth())
            return;

        BlessingsHelpers::HealUnit(warden, warden, warden->CountPctFromMaxHealth(SALT_OF_THE_EARTH_HEAL_PCT), GetSpellInfo()->Id);
    }

    void Register() override
    {
        OnProc += AuraProcFn(spell_blessings_salt_of_the_earth::HandleProc);
    }
};

// The signature abilities (Stormstrike, Lava Burst ranks 1 and 2, Spectral Fang): this SpellScript does nothing for a caster without the Boons. Soul of the Mountain heals the caster for 4% of maximum health
// and Earthquake Step casts its area slow (the helper is the Boon's next spell id). It runs once for each target hit.
class spell_blessings_warden_signature : public SpellScript
{
    PrepareSpellScript(spell_blessings_warden_signature);

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->IsAlive())
            return;

        if (caster->HasAura(SPELL_SOUL_OF_THE_MOUNTAIN))
            BlessingsHelpers::HealUnit(caster, caster, caster->CountPctFromMaxHealth(SOUL_OF_THE_MOUNTAIN_HEAL_PCT), SPELL_SOUL_OF_THE_MOUNTAIN);

        if (caster->HasAura(SPELL_EARTHQUAKE_STEP))
            caster->CastSpell(caster, SPELL_EARTHQUAKE_STEP + 1, true);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_blessings_warden_signature::HandleAfterHit);
    }
};

// Stormbound Reprisal: the proc row (a dodge, block or parry at 50%) decides when this runs; the signature ability of the Favor is ready again (as the WoW Forever talent Improved Stormstrike does for a dodge
// or parry). The cooldown of a Lava Burst is stored under its own spell id, so removing it is enough.
class spell_blessings_stormbound_reprisal : public AuraScript
{
    PrepareAuraScript(spell_blessings_stormbound_reprisal);

    void HandleProc(ProcEventInfo& /*eventInfo*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player || !player->IsAlive())
            return;

        uint32 const signature = SignatureSpell(player);
        if (signature && player->HasSpellCooldown(signature))
            player->RemoveSpellCooldown(signature, true);
    }

    void Register() override
    {
        OnProc += AuraProcFn(spell_blessings_stormbound_reprisal::HandleProc);
    }
};

// Grounding Bulwark: effect 0 is an unlimited absorb. Only direct damage from magic-class spells counts (damage over time ticks and melee are left alone); the chance is the Warden's chance to dodge.
class spell_blessings_grounding_bulwark : public AuraScript
{
    PrepareAuraScript(spell_blessings_grounding_bulwark);

    void CalculateAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
    {
        MakeUnlimited(amount, canBeRecalculated);
    }

    void Absorb(AuraEffect* /*aurEff*/, DamageInfo& dmgInfo, uint32& absorbAmount)
    {
        SpellInfo const* spell = dmgInfo.GetSpellInfo();
        if (!spell || spell->DmgClass != SPELL_DAMAGE_CLASS_MAGIC || dmgInfo.GetDamageType() != SPELL_DIRECT_DAMAGE)
            return;

        Player const* player = GetTarget()->ToPlayer();
        if (player && roll_chance_f(player->GetFloatValue(PLAYER_DODGE_PERCENTAGE)))
            absorbAmount = dmgInfo.GetDamage();
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_blessings_grounding_bulwark::CalculateAmount, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
        OnEffectAbsorb += AuraEffectAbsorbFn(spell_blessings_grounding_bulwark::Absorb, EFFECT_0);
    }
};

// Stone Resolve: effect 0 ticks every second. While Bedrock Barrier is up the Bedrock buff (if the Warden has it) gets its full duration back, so the stacks never fade.
class spell_blessings_stone_resolve : public AuraScript
{
    PrepareAuraScript(spell_blessings_stone_resolve);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* warden = GetTarget();
        if (!warden->IsAlive() || !warden->HasAura(SPELL_BEDROCK_BARRIER))
            return;

        if (Aura* bedrock = warden->GetAura(SPELL_BEDROCK))
            bedrock->RefreshDuration();
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_stone_resolve::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// The Bedrock buff itself (bound to its spell id): effect 0 is the damage-taken aura, 2% for each stack (the stack count multiplies the amount). A Warden with Stone Resolve gets 3%.
class spell_blessings_bedrock_stacks : public AuraScript
{
    PrepareAuraScript(spell_blessings_bedrock_stacks);

    void CalculateAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        Unit* owner = GetUnitOwner();
        if (owner && owner->HasAura(SPELL_STONE_RESOLVE))
            amount = -STONE_RESOLVE_PCT_PER_STACK;
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_blessings_bedrock_stacks::CalculateAmount, EFFECT_0, SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN);
    }
};

// Spirit of Granite: effect 0 is an unlimited absorb, effect 1 a dummy that names the 6 sec damage-reduction buff. The cooldown is kept on the passive's own spell id.
class spell_blessings_spirit_of_granite : public AuraScript
{
    PrepareAuraScript(spell_blessings_spirit_of_granite);

    bool Load() override
    {
        return GetUnitOwner()->IsPlayer();
    }

    void CalculateAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
    {
        MakeUnlimited(amount, canBeRecalculated);
    }

    void Absorb(AuraEffect* /*aurEff*/, DamageInfo& dmgInfo, uint32& absorbAmount)
    {
        Player* warden = GetTarget()->ToPlayer();
        uint32 const damage = dmgInfo.GetDamage();
        uint32 const buff = GetSpellInfo()->Effects[EFFECT_1].TriggerSpell;
        if (!warden || !damage || warden->HasAura(buff) || warden->HasSpellCooldown(GetSpellInfo()->Id))
            return;

        // does this hit leave the Warden below the health line?
        if (!warden->HealthBelowPctDamaged(SPIRIT_OF_GRANITE_HEALTH_PCT, damage))
            return;

        warden->AddSpellCooldown(GetSpellInfo()->Id, 0, SPIRIT_OF_GRANITE_COOLDOWN_MS);
        warden->CastSpell(warden, buff, true);
        absorbAmount = CalculatePct(damage, SPIRIT_OF_GRANITE_REDUCTION_PCT);   // the buff is not in force yet for this hit, so take the share off by hand
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_blessings_spirit_of_granite::CalculateAmount, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
        OnEffectAbsorb += AuraEffectAbsorbFn(spell_blessings_spirit_of_granite::Absorb, EFFECT_0);
    }
};

// Tectonic Pull: effect 0 pulls every enemy in 15 yards; bosses are taken out of the target list.
class spell_blessings_tectonic_pull : public SpellScript
{
    PrepareSpellScript(spell_blessings_tectonic_pull);

    void FilterTargets(std::list<WorldObject*>& targets)
    {
        targets.remove_if([](WorldObject* object)
        {
            Creature const* creature = object->ToCreature();
            return creature && (creature->IsDungeonBoss() || creature->isWorldBoss() || creature->GetCreatureTemplate()->rank == CREATURE_ELITE_WORLDBOSS);
        });
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_blessings_tectonic_pull::FilterTargets, EFFECT_0, TARGET_UNIT_SRC_AREA_ENEMY);
    }
};

// Bedrock Soul: the one Warden ability. No shield is needed (the Spirit Wolf Form cannot use one). Effects 0 and 1 are plain auras (critical immunity; the +60% threat aura whose TriggerSpell column names the
// Bedrock buff, and the spell_proc row, which lets only effect 1 proc, makes this script call the buff on every hit taken or dealt: each call adds a stack, 15 at most, and refreshes the 15 sec duration).
// Effect 2 is a 1 second tick that keeps the hidden armour helper (+50% base armour, TriggerSpell column) on while a shield is worn and the Spirit Wolf Form is not on, the way Righteous Fury keeps its own.
class spell_blessings_bedrock_soul : public AuraScript
{
    PrepareAuraScript(spell_blessings_bedrock_soul);

    void HandleProc(ProcEventInfo& /*eventInfo*/)
    {
        Unit* warden = GetTarget();
        if (!warden->IsAlive())
            return;

        warden->CastSpell(warden, GetSpellInfo()->Effects[EFFECT_1].TriggerSpell, true);
    }

    void KeepArmour()
    {
        Unit* warden = GetTarget();
        Player const* player = warden->ToPlayer();
        uint32 const helper = GetSpellInfo()->Effects[EFFECT_2].TriggerSpell;
        bool const want = player && warden->IsInWorld() && warden->IsAlive() && !warden->HasAura(SPELL_SPIRIT_WOLF_FORM) && HasShield(player);
        if (!want)
            warden->RemoveAurasDueToSpell(helper);
        else if (!warden->HasAura(helper))
            warden->CastSpell(warden, helper, true);
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        KeepArmour();
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        KeepArmour();
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(GetSpellInfo()->Effects[EFFECT_2].TriggerSpell);
    }

    void Register() override
    {
        OnProc += AuraProcFn(spell_blessings_bedrock_soul::HandleProc);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_bedrock_soul::HandlePeriodic, EFFECT_2, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_bedrock_soul::HandleApply, EFFECT_2, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_bedrock_soul::HandleRemove, EFFECT_2, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// The teacher of the three Favors (Titan's Maul 1840290, Stormward 1840311, Spirit Wolf 1840320): effect 0 is a one second tick. Applying the Favor learns its spells; the tick learns them again if the Lua
// system took a stock one away (Stormstrike belongs to the Enhancement kit and is removed from other Blessings whenever the Lua system runs); dropping the Favor takes the custom spells (and the form) away again
// (not when the player merely logs out). Stormstrike and Lava Burst are stock spells that other Blessings use: they are learned, never taken away here.
class spell_blessings_warden_teacher : public AuraScript
{
    PrepareAuraScript(spell_blessings_warden_teacher);

    void Learn(Player* player) const
    {
        TeachDef const* def = TeachOf(GetId());
        if (!def || !player->IsInWorld())
            return;

        for (uint32 const spellId : def->learn)
            if (spellId && !player->HasSpell(spellId))
                player->learnSpell(spellId);
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Player* player = GetTarget()->ToPlayer())
            Learn(player);
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        if (Player* player = GetTarget()->ToPlayer())
            Learn(player);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        TeachDef const* def = TeachOf(GetId());
        if (!player || !def || !player->GetSession() || player->GetSession()->PlayerLogout())
            return;

        if (def->form)
            player->RemoveAurasDueToSpell(def->form);

        for (uint32 const spellId : def->forget)
            if (spellId)
                player->removeSpell(spellId, SPEC_MASK_ALL, false);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_warden_teacher::HandleApply, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_warden_teacher::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_warden_teacher::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Molten Blast (1840300): the cone itself is data; effect 2 is a dummy that names the weaker Flame Shock, which this script puts on every enemy the cone hits.
class spell_blessings_molten_blast : public SpellScript
{
    PrepareSpellScript(spell_blessings_molten_blast);

    void HandleHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || !target->IsAlive())
            return;

        caster->CastSpell(target, GetSpellInfo()->Effects[EFFECT_2].TriggerSpell, true);
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_blessings_molten_blast::HandleHit);
    }
};

// Stormward Block (1840311, proc on a dodge, block or parry at 50%): effect 1 names the Charge, effect 2 the Shield Mastery buff. An avoidance adds a Charge stack (and the hidden Surge that makes the casts
// instant and free), adds a Shield Mastery stack, restores 8% of the maximum mana (at most once every 1.5 sec: the cooldown is kept on the passive's own id and covers the mana only) and takes 2 sec off the
// Fire Nova Totem cooldown.
class spell_blessings_stormward_block : public AuraScript
{
    PrepareAuraScript(spell_blessings_stormward_block);

    void HandleProc(ProcEventInfo& /*eventInfo*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player || !player->IsAlive())
            return;

        uint32 const chargeId = GetSpellInfo()->Effects[EFFECT_1].TriggerSpell;
        uint32 const masteryId = GetSpellInfo()->Effects[EFFECT_2].TriggerSpell;
        player->CastSpell(player, chargeId, true);
        if (SpellInfo const* charge = sSpellMgr->GetSpellInfo(chargeId))
        {
            uint32 const surge = charge->Effects[EFFECT_0].TriggerSpell;
            if (surge && !player->HasAura(surge))
                player->CastSpell(player, surge, true);
        }

        player->CastSpell(player, masteryId, true);

        if (player->GetMaxPower(POWER_MANA) && !player->HasSpellCooldown(GetId()))
        {
            player->AddSpellCooldown(GetId(), 0, STORMWARD_MANA_COOLDOWN_MS);
            player->EnergizeBySpell(player, GetId(), CalculatePct(player->GetMaxPower(POWER_MANA), STORMWARD_MANA_PCT), POWER_MANA);
        }

        for (uint32 const rank : FIRE_NOVA_TOTEMS)
            if (player->HasSpellCooldown(rank))
                player->ModifySpellCooldown(rank, -STORMWARD_FIRE_NOVA_MS);
    }

    void Register() override
    {
        OnProc += AuraProcFn(spell_blessings_stormward_block::HandleProc);
    }
};

// Stormward Charge (the stacking aura; a proc on the three casts takes one stack per cast): effect 0 names the Surge. When the last stack goes (a cast, time, a dispel) the Surge goes with it.
class spell_blessings_stormward_charge : public AuraScript
{
    PrepareAuraScript(spell_blessings_stormward_charge);

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(GetSpellInfo()->Effects[EFFECT_0].TriggerSpell);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_stormward_charge::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Shield Mastery: effect 0 is flat armour whose amount is 30% of the shield's armour (the stacks multiply it), effect 1 ticks every 2 sec to refresh it after a gear change.
class spell_blessings_shield_mastery : public AuraScript
{
    PrepareAuraScript(spell_blessings_shield_mastery);

    void CalculateArmour(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
    {
        canBeRecalculated = true;
        Unit* owner = GetUnitOwner();
        Player const* player = owner ? owner->ToPlayer() : nullptr;
        amount = player ? int32(CalculatePct(ShieldArmour(player), SHIELD_MASTERY_PCT_OF_SHIELD)) : 0;
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        GetAura()->RecalculateAmountOfEffects();
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_blessings_shield_mastery::CalculateArmour, EFFECT_0, SPELL_AURA_MOD_RESISTANCE);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_shield_mastery::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Spirit Wolf Form as a CAST: a toggle. Casting it while its aura is up ends the form and fails the cast without a message; casting it otherwise puts the form on.
class spell_blessings_wolf_form_toggle : public SpellScript
{
    PrepareSpellScript(spell_blessings_wolf_form_toggle);

    SpellCastResult CheckToggle()
    {
        Unit* caster = GetCaster();
        uint32 const form = GetSpellInfo()->Id;
        if (!caster || !caster->HasAura(form))
            return SPELL_CAST_OK;

        caster->RemoveAurasDueToSpell(form);
        return SPELL_FAILED_DONT_REPORT;
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_blessings_wolf_form_toggle::CheckToggle);
    }
};

// The form aura (effect 0 is the Ghost Wolf shapeshift, effect 1 a one second tick, effect 2 the 15% less damage taken). The helpers are passive, so they are not saved at logout: this script puts them on at
// once and again every second (which also repairs a login). The attack power helper's amount is the feral bonus of the main hand weapon: a staff, mace, polearm, dagger or fist weapon (the weapons a feral druid
// can wield), (DPS - 54.8) * 14, the formula the core gives a druid. The Bedrock Soul armour helper is taken off while the form is on.
class spell_blessings_wolf_form : public AuraScript
{
    PrepareAuraScript(spell_blessings_wolf_form);

    void Refresh(Player* player)
    {
        if (!player->IsInWorld() || !player->IsAlive())
            return;

        if (!player->HasAura(SPELL_WOLF_HELPER))
            player->CastSpell(player, SPELL_WOLF_HELPER, true);

        int32 const attackPower = FeralWeaponAttackPower(player);
        if (AuraEffect* effect = player->GetAuraEffect(SPELL_WOLF_ATTACK_POWER, EFFECT_0))
        {
            if (effect->GetAmount() != attackPower)
                effect->ChangeAmount(attackPower);
        }
        else
            player->CastCustomSpell(player, SPELL_WOLF_ATTACK_POWER, &attackPower, nullptr, nullptr, true);

        player->RemoveAurasDueToSpell(SPELL_BEDROCK_ARMOUR);
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Player* player = GetTarget()->ToPlayer())
            Refresh(player);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        target->RemoveAurasDueToSpell(SPELL_WOLF_HELPER);
        target->RemoveAurasDueToSpell(SPELL_WOLF_ATTACK_POWER);
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        PreventDefaultAction();
        if (Player* player = GetTarget()->ToPlayer())
            Refresh(player);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_wolf_form::HandleApply, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_wolf_form::HandleRemove, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_wolf_form::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Spirit Link (1840324): the partner is the targeted group member within 40 yards, else the most injured one. Both get the split-damage aura 1840325 with the OTHER as its caster, so half of the damage
// each takes is dealt to the other (the core's Split Damage aura sends the share to the caster of the aura).
class spell_blessings_wolf_link : public SpellScript
{
    PrepareSpellScript(spell_blessings_wolf_link);

    SpellCastResult CheckPartner()
    {
        Unit* caster = GetCaster();
        return caster && FindLinkPartner(caster, GetExplTargetUnit()) ? SPELL_CAST_OK : SPELL_FAILED_BAD_TARGETS;
    }

    void HandleDummy(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Unit* partner = caster ? FindLinkPartner(caster, GetExplTargetUnit()) : nullptr;
        if (!partner)
            return;

        partner->CastSpell(caster, SPELL_WOLF_LINK_AURA, true);
        caster->CastSpell(partner, SPELL_WOLF_LINK_AURA, true);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_blessings_wolf_link::CheckPartner);
        OnEffectHitTarget += SpellEffectFn(spell_blessings_wolf_link::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// Avatar of Stone (the castable keystone, an aura of 15 sec): effect 0 ticks twice a second. While it lasts the Bedrock buff is held at 15 stacks with a fresh duration, so it is full and cannot fade; when the
// Avatar ends the stacks run out as usual (15 sec after the last refresh, or they are kept by the next hit). The cooldown modifier on the signature abilities is data (effect 1).
class spell_blessings_avatar_of_stone : public AuraScript
{
    PrepareAuraScript(spell_blessings_avatar_of_stone);

    void FillBedrock(Unit* warden)
    {
        if (!warden->IsAlive())
            return;

        Aura* bedrock = warden->GetAura(SPELL_BEDROCK);
        if (!bedrock)
        {
            warden->CastSpell(warden, SPELL_BEDROCK, true);
            bedrock = warden->GetAura(SPELL_BEDROCK);
        }

        if (!bedrock)
            return;

        if (bedrock->GetStackAmount() < BEDROCK_FULL)
            bedrock->SetStackAmount(BEDROCK_FULL);

        bedrock->RefreshDuration();
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        FillBedrock(GetTarget());
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        FillBedrock(GetTarget());
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_avatar_of_stone::HandleApply, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_avatar_of_stone::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Storm Aegis: the proc row fires on every dodge, block and parry (100%); every third one calls the chain lightning bolt named by effect 0 at the attacker (50% of attack power plus 50% of spell power). The count
// lives in this aura instance.
class spell_blessings_storm_aegis : public AuraScript
{
    PrepareAuraScript(spell_blessings_storm_aegis);

    void HandleProc(ProcEventInfo& eventInfo)
    {
        if (++_avoided % STORM_AEGIS_EVERY != 0)
            return;

        Unit* warden = GetTarget();
        Unit* attacker = eventInfo.GetActor();
        if (!attacker || attacker == warden || !attacker->IsAlive() || !warden->IsAlive())
            return;

        int32 damage = PowerDamage(warden, STORM_AEGIS_PCT, STORM_AEGIS_PCT);
        warden->CastCustomSpell(attacker, GetSpellInfo()->Effects[EFFECT_0].TriggerSpell, &damage, nullptr, nullptr, true);
    }

    void Register() override
    {
        OnProc += AuraProcFn(spell_blessings_storm_aegis::HandleProc);
    }

private:
    uint32 _avoided = 0;
};

// Spirit Anchor: effect 0 ticks twice a second, effect 1 names the absorb. The first ally (not the Warden) within 40 yards below 20% health, the lowest first, is wrapped in an absorb worth 30% of the WARDEN's
// maximum health. The cooldown (2 min) is kept on the passive's own spell id.
class spell_blessings_spirit_anchor : public AuraScript
{
    PrepareAuraScript(spell_blessings_spirit_anchor);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Player* warden = GetTarget()->ToPlayer();
        if (!warden || !warden->IsAlive() || !warden->IsInWorld() || warden->HasSpellCooldown(GetSpellInfo()->Id))
            return;

        Player* weakest = nullptr;
        for (Player* ally : NearbyAllies(warden, ALLY_RANGE))
            if (ally != warden && ally->GetHealthPct() < float(SPIRIT_ANCHOR_HEALTH_PCT) && (!weakest || ally->GetHealthPct() < weakest->GetHealthPct()))
                weakest = ally;

        if (!weakest)
            return;

        warden->AddSpellCooldown(GetSpellInfo()->Id, 0, SPIRIT_ANCHOR_COOLDOWN_MS);
        int32 shield = int32(warden->CountPctFromMaxHealth(SPIRIT_ANCHOR_SHIELD_PCT));
        warden->CastCustomSpell(weakest, GetSpellInfo()->Effects[EFFECT_1].TriggerSpell, &shield, nullptr, nullptr, true);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_spirit_anchor::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

void AddMoonlitBlessingsWardenScripts()
{
    RegisterSpellScript(spell_blessings_earthen_reprisal);
    RegisterSpellScript(spell_blessings_stoneclaw_guard);
    RegisterSpellScript(spell_blessings_avalanche);
    RegisterSpellScript(spell_blessings_living_stone);
    RegisterSpellScript(spell_blessings_granite_fists);
    RegisterSpellScript(spell_blessings_spirit_of_granite);
    RegisterSpellScript(spell_blessings_ancestral_rebound);
    RegisterSpellScript(spell_blessings_salt_of_the_earth);
    RegisterSpellScript(spell_blessings_warden_signature);
    RegisterSpellScript(spell_blessings_stormbound_reprisal);
    RegisterSpellScript(spell_blessings_grounding_bulwark);
    RegisterSpellScript(spell_blessings_stone_resolve);
    RegisterSpellScript(spell_blessings_bedrock_stacks);
    RegisterSpellScript(spell_blessings_tectonic_pull);
    RegisterSpellScript(spell_blessings_avatar_of_stone);
    RegisterSpellScript(spell_blessings_storm_aegis);
    RegisterSpellScript(spell_blessings_spirit_anchor);
    RegisterSpellScript(spell_blessings_bedrock_soul);
    RegisterSpellScript(spell_blessings_warden_teacher);
    RegisterSpellScript(spell_blessings_molten_blast);
    RegisterSpellScript(spell_blessings_stormward_block);
    RegisterSpellScript(spell_blessings_stormward_charge);
    RegisterSpellScript(spell_blessings_shield_mastery);
    RegisterSpellScript(spell_blessings_wolf_form_toggle);
    RegisterSpellScript(spell_blessings_wolf_form);
    RegisterSpellScript(spell_blessings_wolf_link);
}
