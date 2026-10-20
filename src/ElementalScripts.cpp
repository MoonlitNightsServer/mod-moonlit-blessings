// Moonlit Blessings: C++ spell scripts for the Elemental Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/elemental.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_blessings_shaman_hybrid_heal     GENERIC (also bound by enhancement.py): a share of the damage that procs the passive heals the most injured ally. Effect 0 is a proc
//                                          trigger whose value is the percent; the trigger spell is the heal helper, whose maximum range is how far the ally may be.
//   spell_blessings_lightning_sovereign    Boon: every Lightning Overload copy restores mana; (a UnitScript below makes the copies hit for 75%)
//   spell_blessings_stormcaller            Boon: a Totem of Wrath that throws a lightning strike every 3 sec
//   spell_blessings_thunderlord            Favor: every third Lightning Bolt calls a free Chain Lightning
//
// Spell ids come from the Elemental registry block 1,810,000 (tools/talents/recipes/elemental.py asserts the units that are named here).
// ~Moonlit Nights Team

#include "BlessingsHelpers.h"
#include "Creature.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "Timer.h"

namespace
{
    enum ElementalSpells : uint32
    {
        SPELL_LIGHTNING_SOVEREIGN          = 1810070,
        SPELL_STORMCALLER_STRIKE           = 1810081,

        // stock spells
        SPELL_LIGHTNING_BOLT_OVERLOAD      = 45284,
        SPELL_CHAIN_LIGHTNING_OVERLOAD     = 45297
    };

    // Totem of Wrath I to IV (the creatures the four ranks summon)
    bool IsTotemOfWrath(uint32 entry)
    {
        return entry == 17539 || (entry >= 30652 && entry <= 30654);
    }

    // Chain Lightning ranks, best first
    constexpr uint32 CHAIN_LIGHTNING_RANKS[] = { 49271, 49270, 25442, 25439, 10605, 2860, 930, 421 };

    // A Lightning Bolt rank 1 hits for about 13 at level 1 and rank 14 for about 790 at level 80: a straight line between them stands in for a bolt at any level.
    int32 StrikeBaseDamage(Unit const* unit)
    {
        float const level = float(std::max<uint8>(unit->GetLevel(), 1));
        float const bolt = 13.0f + (level - 1.0f) * (790.0f - 13.0f) / 79.0f;
        return std::max<int32>(1, int32(bolt * 0.6f));      // the strike is about 60% of a bolt (plus its share of spell power, set on the strike spell)
    }
}

// Hybrid conversion for every Shaman Blessing: the passive procs on the named spells; the damage dealt is turned into a heal on the most injured friend in range.
class spell_blessings_shaman_hybrid_heal : public AuraScript
{
    PrepareAuraScript(spell_blessings_shaman_hybrid_heal);

    bool Validate(SpellInfo const* spellInfo) override
    {
        return ValidateSpellInfo({ spellInfo->Effects[EFFECT_0].TriggerSpell });
    }

    // the spell_proc row can only select spells by family name and mask words; an effect class mask on the trigger narrows that further
    bool CheckProc(ProcEventInfo& eventInfo)
    {
        AuraEffect const* effect = GetEffect(EFFECT_0);
        return !effect || !effect->HasSpellClassMask() || effect->IsAffectedOnSpell(eventInfo.GetSpellInfo());
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        if (!damageInfo || !damageInfo->GetDamage())
            return;

        uint32 const healSpellId = aurEff->GetSpellInfo()->Effects[aurEff->GetEffIndex()].TriggerSpell;
        SpellInfo const* healSpell = sSpellMgr->GetSpellInfo(healSpellId);
        if (!healSpell)
            return;

        Unit* caster = GetTarget();
        Unit* ally = BlessingsHelpers::MostInjuredFriend(caster, caster, healSpell->GetMaxRange(true));
        if (!ally)
            return;

        int32 const heal = CalculatePct(int32(damageInfo->GetDamage()), aurEff->GetAmount());
        if (heal > 0)
            caster->CastCustomSpell(healSpellId, SPELLVALUE_BASE_POINT0, heal, ally, true, nullptr, aurEff);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_blessings_shaman_hybrid_heal::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_blessings_shaman_hybrid_heal::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// Lightning Sovereign: the passive procs on Lightning Bolt and Chain Lightning, but only the Overload copies (45284, 45297) count. Effect 1 holds the mana percent.
class spell_blessings_lightning_sovereign : public AuraScript
{
    PrepareAuraScript(spell_blessings_lightning_sovereign);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        if (!spellInfo || (spellInfo->Id != SPELL_LIGHTNING_BOLT_OVERLOAD && spellInfo->Id != SPELL_CHAIN_LIGHTNING_OVERLOAD))
            return false;

        // a Chain Lightning copy hits several targets in the same instant: one refund per copy
        uint32 const now = getMSTime();
        if (_lastRefund && getMSTimeDiff(_lastRefund, now) < 150)
            return false;

        _lastRefund = now;
        return true;
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
    {
        Unit* target = GetTarget();
        int32 const amount = CalculatePct(int32(target->GetMaxPower(POWER_MANA)), aurEff->GetAmount());
        if (amount > 0)
            target->EnergizeBySpell(target, GetId(), uint32(amount), POWER_MANA);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_blessings_lightning_sovereign::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_blessings_lightning_sovereign::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }

private:
    uint32 _lastRefund = 0;
};

// Lightning Sovereign, the damage half: a stock Overload copy casts for half damage; with the Boon its damage is multiplied so that it deals the percent in effect 2 (75).
class MoonlitBlessingsOverloadScript : public UnitScript
{
public:
    MoonlitBlessingsOverloadScript() : UnitScript("MoonlitBlessingsOverloadScript", true, { UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN }) { }

    void ModifySpellDamageTaken(Unit* /*target*/, Unit* attacker, int32& damage, SpellInfo const* spellInfo) override
    {
        if (!attacker || !spellInfo || damage <= 0)
            return;

        if (spellInfo->Id != SPELL_LIGHTNING_BOLT_OVERLOAD && spellInfo->Id != SPELL_CHAIN_LIGHTNING_OVERLOAD)
            return;

        if (AuraEffect const* percent = attacker->GetAuraEffect(SPELL_LIGHTNING_SOVEREIGN, EFFECT_2))
            damage = int32(int64(damage) * percent->GetAmount() / 50);
    }
};

// Stormcaller: every 3 seconds, while the player's fire totem is a Totem of Wrath and the player fights, the totem throws a lightning strike at a random enemy near it.
class spell_blessings_stormcaller : public AuraScript
{
    PrepareAuraScript(spell_blessings_stormcaller);

    void HandlePeriodic(AuraEffect const* aurEff)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player || !player->IsAlive() || !player->IsInCombat())
            return;

        Creature* totem = ObjectAccessor::GetCreature(*player, player->m_SummonSlot[SUMMON_SLOT_TOTEM_FIRE]);
        if (!totem || !totem->IsAlive() || !IsTotemOfWrath(totem->GetEntry()))
            return;

        Unit* victim = totem->SelectNearbyTarget(nullptr, 30.0f);
        if (!victim || !victim->IsInCombat())
            return;

        totem->CastCustomSpell(SPELL_STORMCALLER_STRIKE, SPELLVALUE_BASE_POINT0, StrikeBaseDamage(player), victim, true, nullptr, aurEff, player->GetGUID());
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_stormcaller::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Favor of the Thunderlord: counts the Lightning Bolts that hit; the third casts a free Chain Lightning (the best rank the player knows) at the same target.
class spell_blessings_thunderlord : public AuraScript
{
    PrepareAuraScript(spell_blessings_thunderlord);

    // the proc row selects by name (which also includes a stray Chain Lightning-flagged "Lightning Bolt" spell); the effect's class mask is exactly Lightning Bolt
    bool CheckProc(ProcEventInfo& eventInfo)
    {
        AuraEffect const* effect = GetEffect(EFFECT_1);
        return effect && effect->IsAffectedOnSpell(eventInfo.GetSpellInfo());
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        if (++_bolts < aurEff->GetAmount())
            return;

        _bolts = 0;
        Player* player = GetTarget()->ToPlayer();
        Unit* victim = eventInfo.GetActionTarget();
        if (!player || !victim || !victim->IsAlive())
            return;

        for (uint32 const rank : CHAIN_LIGHTNING_RANKS)
        {
            if (player->HasSpell(rank))
            {
                player->CastSpell(victim, rank, true, nullptr, aurEff);
                return;
            }
        }
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_blessings_thunderlord::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_blessings_thunderlord::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }

private:
    int32 _bolts = 0;
};

void AddMoonlitBlessingsElementalScripts()
{
    RegisterSpellScript(spell_blessings_shaman_hybrid_heal);
    RegisterSpellScript(spell_blessings_lightning_sovereign);
    RegisterSpellScript(spell_blessings_stormcaller);
    RegisterSpellScript(spell_blessings_thunderlord);
    new MoonlitBlessingsOverloadScript();
}
