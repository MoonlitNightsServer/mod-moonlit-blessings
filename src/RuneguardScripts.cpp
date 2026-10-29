// Moonlit Blessings: C++ spell scripts for the Runeguard (Death Knight tank) kit, Boons and Favors. Registered by AddMoonlitBlessingsRuneguardScripts() (the owner adds the call to MoonlitBlessings.cpp).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/runeguard.py (the baseline in tools/phase2/gen_blessings_spells.py), then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   Tank baseline (the kit item Frost Presence; the gate of every script below that acts on a stock spell is "the character knows the spell 1140060"):
//   spell_blessings_runeguard_baseline     passive 1140060: a 1 sec driver that keeps the helper 1140061 (melee crit immunity, +60% threat) on only while Frost Presence is up, and keeps both stock threat bonuses at 0
//   spell_runeguard_presence               a second script on Frost Presence (48263): the +45% threat of effect 2 is 0 for a Runeguard; the helper goes on and off with the presence at once
//   spell_runeguard_tank_threat            a script on the linked "Tank Class Passive Threat" (57340, +43% from Frost Presence): 0 for a Runeguard
//   Boons:
//   spell_runeguard_runic_corruption       avoidance-free proc on Rune Strike / Death Coil / Frost Strike: starts the 3 sec helper
//   spell_runeguard_runic_corruption_tick  helper: every 0.1 sec takes 0.1 sec off every recovering rune
//   spell_runeguard_death_grip             a script on the stock Death Grip (49576): Death's Advance gives 4 sec of snare immunity
//   spell_runeguard_army_of_the_dead       the copy of Army of the Dead: the channel's damage reduction is dodge plus parry
//   spell_runeguard_ghoul_taunt            a script on the stock Ghoul Taunt (43263): the ghouls of a Runeguard do not taunt
//   spell_runeguard_unholy_ground          half-second check: a helper is on while you stand in your own Death and Decay
//   spell_runeguard_necropolis_sovereign   unlimited absorb extending the stock Will of the Necropolis
//   spell_runeguard_iron_sovereign         half-second check: a helper is on while your own Unbreakable Armor is up
//   MoonlitRuneguardUnitScript             Purgatory (damage hook, after absorbs) and the damage history of the Bloodforged Aegis
//   Favors:
//   spell_runeguard_aegis_favor            Bloodforged Aegis: clears its damage history when the Favor goes
//   spell_runeguard_death_strike_heal      a script on the heal of the stock Death Strike (45470): no heal, the over-shield instead, for the Favor
//   spell_runeguard_frozen_bastion         Frozen Bastion: Rune Strike enabled at will, Frozen Plate stacks with one timer each
//   spell_runeguard_bones_teacher          Unbroken Bones: teaches Bone Shield and Scourge Strike
//   spell_runeguard_bones_return           Unbroken Bones: Plague Strike and Scourge Strike return a bone
//   spell_runeguard_bone_shield_cast       the castable Bone Shield: gives 3 bones
//
// A passive's helper spell id (the buff a script casts) is read from the TriggerSpell column of one of its effects, never hard-coded. The hard-coded ids are the kit spells and stock spells below; the recipe
// (tools/talents/recipes/runeguard.py) asserts the units. ~Moonlit Nights Team

#include "BlessingsHelpers.h"
#include "DynamicObject.h"
#include "GameTime.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "UnitScript.h"

#include <algorithm>
#include <deque>
#include <list>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    enum RuneguardSpells : uint32
    {
        SPELL_BASELINE            = 1140060,   // passive: marker and driver of the tank baseline
        SPELL_PRESENCE_HELPER     = 1140061,   // hidden helper: melee crit immunity and +60% threat
        SPELL_FROST_PRESENCE      = 48263,
        SPELL_TANK_THREAT_LINK    = 57340,     // linked to Frost Presence in spell_linked_spell
        SPELL_PURGATORY           = 1140170,   // effect 0: health left in percent, effect 1: cooldown in seconds, effect 2 names the helper
        SPELL_FAVOR_AEGIS         = 1140190,
        SPELL_AEGIS_SHIELD        = 1140191,
        SPELL_FAVOR_BASTION       = 1140200,
        SPELL_GLYPH_DEATH_STRIKE  = 1140260,   // Glyph of the Bloodforged Bulwark: Improved Death Strike rewrite (2 ranks): +10% per rank on the over-shield
        SPELL_GLYPH_TOUGHNESS     = 1140280,   // Glyph of the Frozen Bastion: Toughness rewrite (5 ranks): +1% armour per stack for each rank
        SPELL_GLYPH_ANTICIPATION  = 1140290,   // Glyph of the Frozen Bastion: Anticipation rewrite (5 ranks): dodge from the stacks
        SPELL_FROZEN_REFLEXES     = 1140300,   // the dodge helper (amount set by the script)
        SPELL_GLYPH_BONE_SHIELD   = 1140240,   // the Bone Shield rewrite of the Glyph of the Unbroken Bones (a clone of the stock talent spell 49222 without its damage bonus)
        SPELL_BONE_SHIELD_CAST    = 1140212,   // the castable Bone Shield of the Favor of the Unbroken Bones
        SPELL_BONE_SHIELD_AURA    = 1140213,   // its aura: charges are the bones
        SPELL_DEATHS_ADVANCE      = 1140110,
        SPELL_RUNE_STRIKE_ENABLE  = 56817,     // the aura the stock Rune Strike needs (CasterAuraSpell)
        SPELL_UNBREAKABLE_ARMOR   = 51271,
        SPELL_SCOURGE_STRIKE_R1   = 55090
    };

    // stock spells that must not count twice: the off-hand strikes of Threat of Thassarian and the triggered Scourge Strike
    constexpr uint32 SPELL_RUNE_STRIKE_OFF_HAND = 66217;
    constexpr uint32 SPELL_PLAGUE_STRIKE_OFF_HAND = 66216;
    constexpr uint32 SPELL_SCOURGE_STRIKE_TRIGGERED = 70890;

    constexpr uint32 DEATH_AND_DECAY_RANKS[] = { 43265, 49936, 49937, 49938 };

    constexpr int32 NECROPOLIS_BAND_PCT = 50;               // Necropolis Sovereign: the health line of the extended Will of the Necropolis
    constexpr int32 NECROPOLIS_STOCK_BAND_PCT = 35;         // the stock line
    constexpr int32 NECROPOLIS_EXTRA_PCT = 15;              // between the lines: as much as the stock talent takes
    constexpr int32 NECROPOLIS_BELOW_PCT = 10;              // below the stock line: a further share

    constexpr uint32 AEGIS_WINDOW_MS = 5000;
    constexpr int32 AEGIS_SHARE_PCT = 30;
    constexpr int32 AEGIS_MIN_PCT = 7;
    constexpr int32 AEGIS_CAP_PCT = 35;

    constexpr uint32 PLATE_DURATION_MS = 8000;
    constexpr uint32 PLATE_MAX_STACKS = 8;

    constexpr int32 BONES_PER_CAST = 3;

    uint32 NowMs()
    {
        return uint32(GameTime::GetGameTimeMS().count());
    }

    // The character is a Runeguard when it has learned the baseline driver (the kit item Frost Presence teaches it).
    bool IsRuneguard(Unit const* unit)
    {
        Player const* player = unit ? unit->ToPlayer() : nullptr;
        return player && player->HasSpell(SPELL_BASELINE);
    }

    void SetHelper(Unit* unit, uint32 helper, bool wanted)
    {
        if (!helper)
            return;

        if (wanted)
        {
            if (!unit->HasAura(helper))
                unit->CastSpell(unit, helper, true);
        }
        else
            unit->RemoveAurasDueToSpell(helper);
    }

    // The two stock threat bonuses of Frost Presence take their amount again (0 for a Runeguard, the stock value for everyone else).
    void RecalculateStockThreat(Unit* unit, bool onlyIfNotZero)
    {
        if (AuraEffect* effect = unit->GetAuraEffect(SPELL_FROST_PRESENCE, EFFECT_1))
            if (!onlyIfNotZero || effect->GetAmount() != 0)
                effect->RecalculateAmount();

        if (AuraEffect* effect = unit->GetAuraEffect(SPELL_TANK_THREAT_LINK, EFFECT_0))
            if (!onlyIfNotZero || effect->GetAmount() != 0)
                effect->RecalculateAmount();
    }

    // The tank baseline: the helper 1140061 is on exactly while Frost Presence is up.
    void SyncBaseline(Unit* unit)
    {
        if (!unit || !unit->IsInWorld())
            return;

        SetHelper(unit, SPELL_PRESENCE_HELPER, IsRuneguard(unit) && unit->IsAlive() && unit->HasAura(SPELL_FROST_PRESENCE));
        if (IsRuneguard(unit))
            RecalculateStockThreat(unit, true);
    }

    void CalcUnlimited(int32& amount, bool& canBeRecalculated)
    {
        amount = -1;
        canBeRecalculated = false;
    }

    // ------------------------------------------------------------------------------------------------------------------------------ Bone Shield
    // Raise `count` bones: a new aura starts with exactly `count`, an existing one gets `count` more (never above its 6) and its full 30 sec back.
    void GiveBones(Player* player, int32 count)
    {
        if (!player || !player->IsAlive())
            return;

        Aura* bones = player->GetAura(SPELL_BONE_SHIELD_AURA);
        if (!bones)
        {
            player->CastSpell(player, SPELL_BONE_SHIELD_AURA, true);
            bones = player->GetAura(SPELL_BONE_SHIELD_AURA);
            if (bones)
                bones->SetCharges(uint8(count));
        }
        else
        {
            bones->ModCharges(count);
            bones->RefreshDuration();
        }
    }

    // ------------------------------------------------------------------------------------------------------------------------------ Bloodforged Aegis
    // The damage a character with the Favor took in the last 5 sec (real damage to health, after absorbs). The map is reached from several map threads: a mutex guards it.
    std::mutex g_aegisLock;
    std::unordered_map<uint64, std::deque<std::pair<uint32, uint32>>> g_aegisHistory;

    void AegisRecord(Unit* victim, uint32 damage)
    {
        uint32 const now = NowMs();
        std::lock_guard<std::mutex> guard(g_aegisLock);
        auto& history = g_aegisHistory[victim->GetGUID().GetRawValue()];
        history.emplace_back(now, damage);
        while (!history.empty() && now - history.front().first > AEGIS_WINDOW_MS)
            history.pop_front();
    }

    uint32 AegisDamageTaken(Unit* victim)
    {
        uint32 const now = NowMs();
        uint32 total = 0;
        std::lock_guard<std::mutex> guard(g_aegisLock);
        auto it = g_aegisHistory.find(victim->GetGUID().GetRawValue());
        if (it == g_aegisHistory.end())
            return 0;

        while (!it->second.empty() && now - it->second.front().first > AEGIS_WINDOW_MS)
            it->second.pop_front();
        for (auto const& entry : it->second)
            total += entry.second;
        return total;
    }

    void AegisForget(Unit* victim)
    {
        std::lock_guard<std::mutex> guard(g_aegisLock);
        g_aegisHistory.erase(victim->GetGUID().GetRawValue());
    }

    // The rank (0 = none) of a rewritten talent the player has: the rewrite spells run on by one per rank.
    uint32 VariantRank(Unit const* owner, uint32 firstRank, uint32 ranks)
    {
        for (uint32 rank = ranks; rank > 0; --rank)
            if (owner->HasAura(firstRank + rank - 1))
                return rank;

        return 0;
    }

    // Forge the over-shield: 30% of the damage of the last 5 sec, at least 7% and at most 35% of maximum health, added to the shield that is still there, up to 35% of maximum health, for a fresh 10 sec.
    void ForgeAegis(Unit* player)
    {
        uint32 const maxCap = player->CountPctFromMaxHealth(AEGIS_CAP_PCT);
        uint32 shield = CalculatePct(AegisDamageTaken(player), AEGIS_SHARE_PCT);
        if (uint32 const rank = VariantRank(player, SPELL_GLYPH_DEATH_STRIKE, 2))                    // Glyph of the Bloodforged Bulwark: Improved Death Strike enlarges the shield
            shield += CalculatePct(shield, 10 * rank);
        shield = std::max<uint32>(shield, player->CountPctFromMaxHealth(AEGIS_MIN_PCT));
        shield = std::min<uint32>(shield, maxCap);

        if (AuraEffect* existing = player->GetAuraEffect(SPELL_AEGIS_SHIELD, EFFECT_0))
        {
            existing->SetAmount(int32(std::min<uint32>(uint32(std::max(existing->GetAmount(), 0)) + shield, maxCap)));
            existing->GetBase()->RefreshDuration();
        }
        else
        {
            player->CastCustomSpell(SPELL_AEGIS_SHIELD, SPELLVALUE_BASE_POINT0, int32(shield), player, true);
            if (AuraEffect* fresh = player->GetAuraEffect(SPELL_AEGIS_SHIELD, EFFECT_0))
                fresh->SetAmount(int32(shield));
        }
    }

    // Is the unit standing in a Death and Decay of its own?
    bool InOwnDeathAndDecay(Unit* unit)
    {
        for (uint32 const rank : DEATH_AND_DECAY_RANKS)
            if (DynamicObject* ground = unit->GetDynObject(rank))
                if (unit->GetExactDist2d(ground) <= ground->GetRadius())
                    return true;
        return false;
    }
}

// ================================================================================================================================ tank baseline
// Frost Presence Resolve (1140060): effect 0 ticks every second, effect 1 names the helper. The helper (crit immunity and the +60% threat aura) is on exactly while Frost Presence is up; when the passive goes
// (the Blessing is dropped) the helper goes and the stock threat bonuses of the presence take their stock amounts back.
class spell_blessings_runeguard_baseline : public AuraScript
{
    PrepareAuraScript(spell_blessings_runeguard_baseline);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        SyncBaseline(GetTarget());
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        SyncBaseline(GetTarget());
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        target->RemoveAurasDueToSpell(SPELL_PRESENCE_HELPER);
        RecalculateStockThreat(target, false);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_runeguard_baseline::HandleApply, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_runeguard_baseline::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_runeguard_baseline::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// A second script on Frost Presence (the stock spell_dk_presence stays): effect 1 is the stock +45% threat, 0 for a Runeguard; the helper goes on and off with the presence.
class spell_runeguard_presence : public AuraScript
{
    PrepareAuraScript(spell_runeguard_presence);

    void CalculateThreat(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        if (IsRuneguard(GetCaster()))
            amount = 0;
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        SyncBaseline(GetTarget());
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(SPELL_PRESENCE_HELPER);
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_runeguard_presence::CalculateThreat, EFFECT_1, SPELL_AURA_MOD_THREAT);
        AfterEffectApply += AuraEffectApplyFn(spell_runeguard_presence::HandleApply, EFFECT_0, SPELL_AURA_MOD_BASE_RESISTANCE_PCT, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_runeguard_presence::HandleRemove, EFFECT_0, SPELL_AURA_MOD_BASE_RESISTANCE_PCT, AURA_EFFECT_HANDLE_REAL);
    }
};

// The linked "Tank Class Passive Threat" (57340, cast on the Death Knight when Frost Presence goes on, +43% threat): 0 for a Runeguard.
class spell_runeguard_tank_threat : public AuraScript
{
    PrepareAuraScript(spell_runeguard_tank_threat);

    void CalculateThreat(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        if (IsRuneguard(GetCaster()))
            amount = 0;
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_runeguard_tank_threat::CalculateThreat, EFFECT_0, SPELL_AURA_MOD_THREAT);
    }
};

// ================================================================================================================================ Boons
// Runic Corruption: effect 0 names the helper; the proc row (Rune Strike, Death Coil, Frost Strike, 20%) decides when this runs.
class spell_runeguard_runic_corruption : public AuraScript
{
    PrepareAuraScript(spell_runeguard_runic_corruption);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        Unit* knight = GetTarget();
        if (uint32 const helper = GetSpellInfo()->Effects[EFFECT_0].TriggerSpell)
            knight->CastSpell(knight, helper, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_runeguard_runic_corruption::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// The Runic Corruption helper: its 0.1 sec tick takes another 0.1 sec off every rune that is recovering, so the runes recover twice as fast while it lasts (3 sec).
class spell_runeguard_runic_corruption_tick : public AuraScript
{
    PrepareAuraScript(spell_runeguard_runic_corruption_tick);

    void HandlePeriodic(AuraEffect const* aurEff)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player || player->getClass() != CLASS_DEATH_KNIGHT)
            return;

        uint32 const step = uint32(std::max(aurEff->GetAmplitude(), 1));
        bool readied = false;
        for (uint8 i = 0; i < MAX_RUNES; ++i)
            if (uint32 const cooldown = player->GetRuneCooldown(i))
            {
                uint32 const next = cooldown > step ? cooldown - step : 0;
                player->SetRuneCooldown(i, next);
                readied |= (next == 0);
            }

        // the client keeps its own rune timers: the engine's early refunds (Blood Tap, Empower Rune Weapon) cast the visual spell 47804 so the client sees the rune ready
        if (readied)
            player->CastSpell(player, 47804, true);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_runeguard_runic_corruption_tick::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Death's Advance: bound to the stock Death Grip (49576). Effect 1 of the passive names the 4 sec snare immunity.
class spell_runeguard_death_grip : public SpellScript
{
    PrepareSpellScript(spell_runeguard_death_grip);

    void HandleAfterCast()
    {
        Unit* knight = GetCaster();
        if (!knight)
            return;

        if (AuraEffect const* advance = knight->GetAuraEffect(SPELL_DEATHS_ADVANCE, EFFECT_1))
            if (uint32 const helper = advance->GetSpellInfo()->Effects[EFFECT_1].TriggerSpell)
                knight->CastSpell(knight, helper, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_runeguard_death_grip::HandleAfterCast);
    }
};

// Army of the Dead (the copy, bound to its own id): effect 1 is the damage taken aura of the channel; its amount is the caster's dodge plus parry chance (the stock spell has a flat 50%).
class spell_runeguard_army_of_the_dead : public AuraScript
{
    PrepareAuraScript(spell_runeguard_army_of_the_dead);

    void CalculateReduction(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        Unit* knight = GetCaster();
        if (!knight)
            return;

        float const avoidance = knight->GetUnitDodgeChance() + knight->GetUnitParryChance();
        amount = -std::min<int32>(100, int32(avoidance));
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_runeguard_army_of_the_dead::CalculateReduction, EFFECT_1, SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN);
    }
};

// Bound to the stock Ghoul Taunt (43263, cast by the Army of the Dead ghouls): the ghouls of a Runeguard find no target to taunt. The stock script (spell_dk_aotd_taunt) leaves the bosses out of the list.
class spell_runeguard_ghoul_taunt : public SpellScript
{
    PrepareSpellScript(spell_runeguard_ghoul_taunt);

    void FilterTargets(std::list<WorldObject*>& targets)
    {
        Unit* ghoul = GetCaster();
        if (ghoul && IsRuneguard(ghoul->GetCharmerOrOwnerOrSelf()))
            targets.clear();
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_runeguard_ghoul_taunt::FilterTargets, EFFECT_ALL, TARGET_UNIT_SRC_AREA_ENEMY);
    }
};

// Unholy Ground: effect 0 ticks twice a second and names the helper (-8% damage taken, +5% dodge), which is on while the knight stands in a Death and Decay he cast.
class spell_runeguard_unholy_ground : public AuraScript
{
    PrepareAuraScript(spell_runeguard_unholy_ground);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* knight = GetTarget();
        SetHelper(knight, GetSpellInfo()->Effects[EFFECT_0].TriggerSpell, knight->IsAlive() && InOwnDeathAndDecay(knight));
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(GetSpellInfo()->Effects[EFFECT_0].TriggerSpell);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_runeguard_unholy_ground::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_runeguard_unholy_ground::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Necropolis Sovereign: effect 0 is an unlimited absorb. The stock Will of the Necropolis (a Mastery fold) takes 15% off a blow that would leave the knight below 35% health; this absorb takes 15% off in the
// band from 35% to 50% and a further 10% below 35% (the absorbs apply one after the other).
class spell_runeguard_necropolis_sovereign : public AuraScript
{
    PrepareAuraScript(spell_runeguard_necropolis_sovereign);

    void CalculateAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
    {
        CalcUnlimited(amount, canBeRecalculated);
    }

    void Absorb(AuraEffect* /*aurEff*/, DamageInfo& dmgInfo, uint32& absorbAmount)
    {
        Unit* knight = GetTarget();
        uint32 const damage = dmgInfo.GetDamage();
        if (!damage)
            return;

        int64 const remaining = int64(knight->GetHealth()) - int64(damage);
        int32 share = 0;
        if (remaining < int64(knight->CountPctFromMaxHealth(NECROPOLIS_STOCK_BAND_PCT)))
            share = NECROPOLIS_BELOW_PCT;
        else if (remaining < int64(knight->CountPctFromMaxHealth(NECROPOLIS_BAND_PCT)))
            share = NECROPOLIS_EXTRA_PCT;

        if (share)
            absorbAmount = CalculatePct(damage, share);
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_runeguard_necropolis_sovereign::CalculateAmount, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
        OnEffectAbsorb += AuraEffectAbsorbFn(spell_runeguard_necropolis_sovereign::Absorb, EFFECT_0);
    }
};

// Iron Sovereign: effect 0 ticks twice a second and names the -10% damage helper, which is on while the knight's own Unbreakable Armor is up (effects 1 and 2 are the duration and cooldown modifiers).
class spell_runeguard_iron_sovereign : public AuraScript
{
    PrepareAuraScript(spell_runeguard_iron_sovereign);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* knight = GetTarget();
        SetHelper(knight, GetSpellInfo()->Effects[EFFECT_0].TriggerSpell, knight->IsAlive() && knight->HasAura(SPELL_UNBREAKABLE_ARMOR));
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(GetSpellInfo()->Effects[EFFECT_0].TriggerSpell);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_runeguard_iron_sovereign::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_runeguard_iron_sovereign::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// ================================================================================================================================ Favors
// Favor of the Bloodforged Aegis: when the Favor goes, the damage history and the shield go too.
class spell_runeguard_aegis_favor : public AuraScript
{
    PrepareAuraScript(spell_runeguard_aegis_favor);

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* knight = GetTarget();
        AegisForget(knight);
        knight->RemoveAurasDueToSpell(SPELL_AEGIS_SHIELD);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_runeguard_aegis_favor::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Bound to the heal of the stock Death Strike (45470): the stock script always casts it. For a knight with the Favor it heals nothing and forges the over-shield instead.
class spell_runeguard_death_strike_heal : public SpellScript
{
    PrepareSpellScript(spell_runeguard_death_strike_heal);

    void HandleHeal(SpellEffIndex /*effIndex*/)
    {
        Unit* knight = GetCaster();
        if (!knight || !knight->IsPlayer() || !knight->HasAura(SPELL_FAVOR_AEGIS))
            return;

        SetHitHeal(0);
        ForgeAegis(knight);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_runeguard_death_strike_heal::HandleHeal, EFFECT_0, SPELL_EFFECT_HEAL);
    }
};

// Favor of the Frozen Bastion: effect 0 ticks every 0.25 sec and names the Frozen Plate aura; effect 1 is the proc on a Rune Strike cast (the proc row leaves effect 0 out).
// The tick (1) keeps the aura the stock Rune Strike needs (56817) on the knight, so that Rune Strike needs no dodge or parry, and (2) ends every stack whose own 8 sec timer is over.
// The timers are kept here, one per stack (like retail Ironfur): a new stack never touches the older ones; the visible aura's stack count is the number of timers running and its own duration
// is that of the newest stack. A character that logs in with a Frozen Plate aura still on (its timers are lost) starts clean: the aura is removed when this passive is applied.
class spell_runeguard_frozen_bastion : public AuraScript
{
    PrepareAuraScript(spell_runeguard_frozen_bastion);

    uint32 PlateSpell() const
    {
        return GetSpellInfo()->Effects[EFFECT_0].TriggerSpell;
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        _expiry.clear();
        GetTarget()->RemoveAurasDueToSpell(PlateSpell());
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        _expiry.clear();
        GetTarget()->RemoveAurasDueToSpell(PlateSpell());
        GetTarget()->RemoveAurasDueToSpell(SPELL_FROZEN_REFLEXES);
    }

    // Glyph of the Frozen Bastion: Toughness makes every stack stronger (10% plus 1% per rank), Anticipation turns the stacks into dodge. Without the rewrites nothing changes.
    void SyncRewrites(Player* knight, Aura* plate) const
    {
        uint32 const stacks = plate ? plate->GetStackAmount() : 0;
        if (plate)
            if (AuraEffect* armor = plate->GetEffect(EFFECT_0))
                armor->ChangeAmount(10 + int32(VariantRank(knight, SPELL_GLYPH_TOUGHNESS, 5)));

        int32 const dodge = int32((stacks * VariantRank(knight, SPELL_GLYPH_ANTICIPATION, 5) + 3) / 4);
        if (stacks && dodge > 0)
        {
            if (!knight->HasAura(SPELL_FROZEN_REFLEXES))
                knight->CastSpell(knight, SPELL_FROZEN_REFLEXES, true);

            if (Aura* reflexes = knight->GetAura(SPELL_FROZEN_REFLEXES))
            {
                if (AuraEffect* effect = reflexes->GetEffect(EFFECT_0))
                    effect->ChangeAmount(dodge);

                reflexes->SetDuration(plate->GetDuration());
            }
        }
        else
            knight->RemoveAurasDueToSpell(SPELL_FROZEN_REFLEXES);
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Player* knight = GetTarget()->ToPlayer();
        if (!knight)
            return;

        if (!knight->IsAlive())
        {
            _expiry.clear();
            return;
        }

        if (!knight->HasAura(SPELL_RUNE_STRIKE_ENABLE))
            knight->CastSpell(knight, SPELL_RUNE_STRIKE_ENABLE, true);

        uint32 const now = NowMs();
        _expiry.erase(std::remove_if(_expiry.begin(), _expiry.end(), [now](uint32 end) { return int32(end - now) <= 0; }), _expiry.end());

        Aura* plate = knight->GetAura(PlateSpell());
        if (!plate)
            _expiry.clear();                                           // dispelled or removed by something else: the timers mean nothing
        else if (_expiry.empty())
            plate->Remove();
        else if (plate->GetStackAmount() != _expiry.size())
            plate->SetStackAmount(uint8(_expiry.size()));

        SyncRewrites(knight, knight->GetAura(PlateSpell()));
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        Player* knight = GetTarget()->ToPlayer();
        if (!knight || !knight->IsAlive())
            return;

        // the off-hand Rune Strike of Threat of Thassarian is not a second cast
        if (SpellInfo const* spellInfo = eventInfo.GetSpellInfo())
            if (spellInfo->Id == SPELL_RUNE_STRIKE_OFF_HAND)
                return;

        if (_expiry.size() >= PLATE_MAX_STACKS)
            return;                                                    // full: a new cast adds nothing and refreshes nothing

        _expiry.push_back(NowMs() + PLATE_DURATION_MS);

        Aura* plate = knight->GetAura(PlateSpell());
        if (!plate)
        {
            knight->CastSpell(knight, PlateSpell(), true);
            plate = knight->GetAura(PlateSpell());
        }

        if (!plate)
        {
            _expiry.clear();
            return;
        }

        plate->SetStackAmount(uint8(_expiry.size()));
        plate->SetDuration(plate->GetMaxDuration());                   // the aura's own clock follows the newest stack
        SyncRewrites(knight, plate);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_runeguard_frozen_bastion::HandleApply, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_runeguard_frozen_bastion::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_runeguard_frozen_bastion::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectProc += AuraEffectProcFn(spell_runeguard_frozen_bastion::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }

private:
    std::vector<uint32> _expiry;                                       // end time (game time, ms) of every running stack, oldest first
};

// Favor of the Unbroken Bones (teacher): effect 0 ticks every second. Applying the Favor learns Bone Shield and Scourge Strike (Scourge Strike belongs to the Unholy kit, which the Lua system removes from
// other Blessings, so every tick learns it again); dropping the Favor takes Bone Shield and its aura away (not when the player merely logs out).
class spell_runeguard_bones_teacher : public AuraScript
{
    PrepareAuraScript(spell_runeguard_bones_teacher);

    void Learn(Player* player) const
    {
        if (!player->IsInWorld())
            return;

        for (uint32 const spellId : { uint32(SPELL_BONE_SHIELD_CAST), uint32(SPELL_SCOURGE_STRIKE_R1) })
            if (!player->HasSpell(spellId))
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
        if (!player || !player->GetSession() || player->GetSession()->PlayerLogout())
            return;

        player->RemoveAurasDueToSpell(SPELL_BONE_SHIELD_AURA);
        player->removeSpell(SPELL_BONE_SHIELD_CAST, SPEC_MASK_ALL, false);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_runeguard_bones_teacher::HandleApply, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_runeguard_bones_teacher::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_runeguard_bones_teacher::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Favor of the Unbroken Bones (the proc passive): a Plague Strike or Scourge Strike hit returns one bone to an active Bone Shield (never above 6) and gives it its full 30 sec back. The proc row has a 0.1 sec
// cooldown so that the off-hand strike and the triggered Scourge Strike do not count twice.
class spell_runeguard_bones_return : public AuraScript
{
    PrepareAuraScript(spell_runeguard_bones_return);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        if (SpellInfo const* spellInfo = eventInfo.GetSpellInfo())
            if (sSpellMgr->GetFirstSpellInChain(spellInfo->Id) == SPELL_PLAGUE_STRIKE_OFF_HAND || spellInfo->Id == SPELL_SCOURGE_STRIKE_TRIGGERED)
                return;

        if (Aura* bones = GetTarget()->GetAura(SPELL_BONE_SHIELD_AURA))
        {
            bones->ModCharges(1);
            bones->RefreshDuration();
        }
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_runeguard_bones_return::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Glyph of the Unbroken Bones (the proc passive that is the glyph): a Plague Strike or Scourge Strike hit adds one bone to the Bone Shield rewrite, never above the charges the
// spell starts with. The proc row has a 0.1 sec cooldown so that the off-hand strike and the triggered Scourge Strike do not count twice.
class spell_runeguard_glyph_bones_return : public AuraScript
{
    PrepareAuraScript(spell_runeguard_glyph_bones_return);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        if (SpellInfo const* spellInfo = eventInfo.GetSpellInfo())
            if (sSpellMgr->GetFirstSpellInChain(spellInfo->Id) == SPELL_PLAGUE_STRIKE_OFF_HAND || spellInfo->Id == SPELL_SCOURGE_STRIKE_TRIGGERED)
                return;

        Aura* bones = GetTarget()->GetAura(SPELL_GLYPH_BONE_SHIELD);
        if (!bones)
            return;

        uint32 const most = std::max<uint32>(bones->GetSpellInfo()->ProcCharges, 1);
        if (bones->GetCharges() < most)
            bones->ModCharges(1);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_runeguard_glyph_bones_return::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// The castable Bone Shield: an instant self cast whose dummy effect gives the bones.
class spell_runeguard_bone_shield_cast : public SpellScript
{
    PrepareSpellScript(spell_runeguard_bone_shield_cast);

    void HandleDummy(SpellEffIndex /*effIndex*/)
    {
        if (Unit* caster = GetCaster())
            GiveBones(caster->ToPlayer(), BONES_PER_CAST);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_runeguard_bone_shield_cast::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// ================================================================================================================================ damage hook
// UnitScript::OnDamage runs inside Unit::DealDamage after every absorb has been taken, so `damage` is what will really come off the player's health.
//  * Purgatory: a killing blow leaves the knight at effect 0 percent of maximum health and starts the 4 sec helper (the passive's effect 2 names it); once per effect 1 seconds (kept as a cooldown on the passive).
//  * Bloodforged Aegis: the damage taken by a knight with the Favor is written to the history the over-shield is worked out from.
class MoonlitRuneguardUnitScript : public UnitScript
{
public:
    MoonlitRuneguardUnitScript() : UnitScript("MoonlitRuneguardUnitScript", true, { UNITHOOK_ON_DAMAGE }) { }

    void OnDamage(Unit* /*attacker*/, Unit* victim, uint32& damage) override
    {
        if (!victim || !victim->IsPlayer() || !damage)
            return;

        if (victim->HasAura(SPELL_FAVOR_AEGIS))
            AegisRecord(victim, damage);

        if (!victim->IsAlive())
            return;

        Player* player = victim->ToPlayer();
        AuraEffect const* purgatory = player->GetAuraEffect(SPELL_PURGATORY, EFFECT_0);
        uint32 const health = player->GetHealth();
        if (!purgatory || player->HasSpellCooldown(SPELL_PURGATORY) || damage < health)
            return;

        uint32 const left = player->CountPctFromMaxHealth(std::max(purgatory->GetAmount(), 1));
        if (health > left)
            damage = health - left;
        else
        {
            damage = 0;
            player->SetHealth(left);
        }

        int32 const cooldown = player->GetAuraEffect(SPELL_PURGATORY, EFFECT_1) ? player->GetAuraEffect(SPELL_PURGATORY, EFFECT_1)->GetAmount() : 240;
        player->AddSpellCooldown(SPELL_PURGATORY, 0, uint32(cooldown) * IN_MILLISECONDS);
        if (uint32 const helper = purgatory->GetSpellInfo()->Effects[EFFECT_2].TriggerSpell)
            player->CastSpell(player, helper, true);
    }
};

void AddMoonlitBlessingsRuneguardScripts()
{
    RegisterSpellScript(spell_blessings_runeguard_baseline);
    RegisterSpellScript(spell_runeguard_presence);
    RegisterSpellScript(spell_runeguard_tank_threat);
    RegisterSpellScript(spell_runeguard_runic_corruption);
    RegisterSpellScript(spell_runeguard_runic_corruption_tick);
    RegisterSpellScript(spell_runeguard_death_grip);
    RegisterSpellScript(spell_runeguard_army_of_the_dead);
    RegisterSpellScript(spell_runeguard_ghoul_taunt);
    RegisterSpellScript(spell_runeguard_unholy_ground);
    RegisterSpellScript(spell_runeguard_necropolis_sovereign);
    RegisterSpellScript(spell_runeguard_iron_sovereign);
    RegisterSpellScript(spell_runeguard_aegis_favor);
    RegisterSpellScript(spell_runeguard_death_strike_heal);
    RegisterSpellScript(spell_runeguard_frozen_bastion);
    RegisterSpellScript(spell_runeguard_bones_teacher);
    RegisterSpellScript(spell_runeguard_bones_return);
    RegisterSpellScript(spell_runeguard_glyph_bones_return);
    RegisterSpellScript(spell_runeguard_bone_shield_cast);
    new MoonlitRuneguardUnitScript();
}
