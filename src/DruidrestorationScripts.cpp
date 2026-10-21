// Moonlit Blessings: C++ spell scripts for the Restoration (Druid healer) Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/druidrestoration.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_blessings_druid_ns_cast_only      lets only Nature's Swiftness itself (not Elune's Wrath or Predator's Swiftness, which share its class bit) start the Swift Touch
//                                           and Grove Guardian buffs
//   spell_blessings_wild_growth_reach       Wider Growth, Favor of the Rejuvenation Grove: when a Wild Growth cast finishes, N more injured allies receive it
//   spell_blessings_great_grove             the same, and a Rejuvenation of yours on every ally the Wild Growth reached is refreshed
//   spell_blessings_elder_touch             a critical Healing Touch, Nourish or Regrowth heal refunds a share of the spell's mana cost
//   spell_blessings_verdant_bloom           bound to the STOCK Rejuvenation (-774) and Lifebloom (-33763) auras: when one of yours ends or is dispelled it blooms
//
// Spell ids come from the registry block 1,230,000 (Restoration): unit n of tools/talents/recipes/druidrestoration.py is 1,230,000 + 10 n. The only hard-coded
// ids are below and are asserted at the end of that recipe. ~Moonlit Nights Team

#include "BlessingsHelpers.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include <algorithm>
#include <limits>
#include <vector>

namespace
{
    enum DruidRestorationSpells : uint32
    {
        SPELL_NATURES_SWIFTNESS     = 17116,
        SPELL_VERDANT_BLOOM_MARKER  = 1230061,      // effect 0: the percent of the total heal over time the bloom heals
        SPELL_VERDANT_BLOOM_HEAL    = 1230062       // the spell id the bloom heal is attributed to
    };

    constexpr uint32 REJUVENATION_FAMILY_FLAG = 0x10;  // SpellFamilyFlags word 0 of Rejuvenation (class mask 16,0,0)

    // The caster, the other members of the caster's group and their pets (the units a Wild Growth may reach: the core's own script keeps raid members only).
    void CollectGroupUnits(Unit* caster, std::vector<Unit*>& out)
    {
        out.push_back(caster);
        Player* player = caster->ToPlayer();
        if (!player)
            return;

        if (Pet* pet = player->GetPet())
            out.push_back(pet);

        if (Group* group = player->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == player)
                    continue;

                out.push_back(member);
                if (Pet* pet = member->GetPet())
                    out.push_back(pet);
            }
        }
    }

    // Wild Growth has just finished casting around `center`: give it (same spell id, so the same rank) to the `extra` most injured group units within its radius that
    // it did not reach, then (refresh) refresh the caster's Rejuvenation on every unit that has the fresh Wild Growth. AddAura keeps the core's tick script on the aura.
    void SpreadWildGrowth(Unit* caster, Unit* center, SpellInfo const* wildGrowth, uint32 extra, bool refresh)
    {
        if (!caster || !center || !wildGrowth || !caster->IsInWorld() || !center->IsInWorld())
            return;

        float const radius = wildGrowth->Effects[EFFECT_0].CalcRadius(caster);
        ObjectGuid const casterGuid = caster->GetGUID();

        std::vector<Unit*> friends;
        CollectGroupUnits(caster, friends);

        std::vector<Unit*> nearby;
        for (Unit* unit : friends)
            if (unit->IsInWorld() && unit->IsAlive() && unit->GetMap() == caster->GetMap() && unit->IsWithinDist(center, radius))
                nearby.push_back(unit);

        if (extra)
        {
            std::vector<Unit*> candidates;
            for (Unit* unit : nearby)
                if (!unit->HasAura(wildGrowth->Id, casterGuid) && center->IsWithinLOSInMap(unit))
                    candidates.push_back(unit);

            std::sort(candidates.begin(), candidates.end(), [](Unit const* a, Unit const* b) { return a->GetHealthPct() < b->GetHealthPct(); });
            if (candidates.size() > extra)
                candidates.resize(extra);

            for (Unit* unit : candidates)
                caster->AddAura(wildGrowth->Id, unit);
        }

        if (refresh)
        {
            for (Unit* unit : nearby)
            {
                // only the Wild Growth that has just been applied (full duration), not an older one of the same rank
                Aura const* wild = unit->GetAura(wildGrowth->Id, casterGuid);
                if (!wild || wild->GetDuration() + 2000 < wild->GetMaxDuration())
                    continue;

                if (AuraEffect const* rejuvenation = unit->GetAuraEffect(SPELL_AURA_PERIODIC_HEAL, SPELLFAMILY_DRUID, REJUVENATION_FAMILY_FLAG, 0, 0, casterGuid))
                    rejuvenation->GetBase()->RefreshDuration(true);
            }
        }
    }
}

// Swift Touch and Favor of the Grove Guardian: the passives proc on the cast of "Nature's Swiftness" by class mask, but that bit is shared with Elune's Wrath and
// Predator's Swiftness. Only Nature's Swiftness itself may give the buff.
class spell_blessings_druid_ns_cast_only : public AuraScript
{
    PrepareAuraScript(spell_blessings_druid_ns_cast_only);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        return spellInfo && spellInfo->Id == SPELL_NATURES_SWIFTNESS;
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_blessings_druid_ns_cast_only::CheckProc);
    }
};

// Wider Growth and Favor of the Rejuvenation Grove: effect 0 (dummy) is the number of extra allies. Procs when a Wild Growth cast finishes (phase 4: once per cast).
class spell_blessings_wild_growth_reach : public AuraScript
{
    PrepareAuraScript(spell_blessings_wild_growth_reach);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        Spell const* spell = eventInfo.GetProcSpell();
        Unit* caster = GetTarget();
        if (!spellInfo || !spell || aurEff->GetAmount() <= 0)
            return;

        Unit* center = spell->m_targets.GetUnitTarget();
        SpreadWildGrowth(caster, center ? center : caster, spellInfo, uint32(aurEff->GetAmount()), false);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_wild_growth_reach::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Great Grove: the same as above, plus the Rejuvenation refresh.
class spell_blessings_great_grove : public AuraScript
{
    PrepareAuraScript(spell_blessings_great_grove);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        Spell const* spell = eventInfo.GetProcSpell();
        Unit* caster = GetTarget();
        if (!spellInfo || !spell)
            return;

        Unit* center = spell->m_targets.GetUnitTarget();
        SpreadWildGrowth(caster, center ? center : caster, spellInfo, uint32(std::max(aurEff->GetAmount(), 0)), true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_great_grove::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Elder Touch: effect 0 (dummy) is the percent of the spell's mana cost that a critical heal gives back.
class spell_blessings_elder_touch : public AuraScript
{
    PrepareAuraScript(spell_blessings_elder_touch);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        Spell const* spell = eventInfo.GetProcSpell();
        Unit* caster = GetTarget();
        if (!spell || !caster->IsAlive() || spell->GetSpellInfo()->PowerType != POWER_MANA)
            return;

        int32 const refund = CalculatePct(std::max(spell->GetPowerCost(), 0), aurEff->GetAmount());
        if (refund > 0)
            caster->EnergizeBySpell(caster, GetId(), uint32(refund), POWER_MANA);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_elder_touch::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Verdant Bloom: runs on the STOCK Rejuvenation and Lifebloom auras of every caster, so it first checks that the caster has the Boon. Only when the aura runs out or is
// dispelled (not when it is refreshed, replaced, cancelled or consumed by Swiftmend), the target is healed for the marker's percent of the total heal over time.
class spell_blessings_verdant_bloom : public AuraScript
{
    PrepareAuraScript(spell_blessings_verdant_bloom);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_VERDANT_BLOOM_MARKER, SPELL_VERDANT_BLOOM_HEAL });
    }

    void HandleRemove(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        AuraRemoveMode const mode = GetTargetApplication()->GetRemoveMode();
        if (mode != AURA_REMOVE_BY_EXPIRE && mode != AURA_REMOVE_BY_ENEMY_SPELL)
            return;

        Unit* caster = GetCaster();
        Unit* target = GetTarget();
        if (!caster || !caster->IsAlive() || !target->IsAlive())
            return;

        AuraEffect const* marker = caster->GetAuraEffect(SPELL_VERDANT_BLOOM_MARKER, EFFECT_0);
        if (!marker || aurEff->GetAmount() <= 0 || aurEff->GetTotalTicks() <= 0)
            return;

        // one tick the way the aura heals it (the amount already holds the stacks), times the ticks it would have had in all
        uint32 const stack = GetStackAmount();
        uint32 tick = caster->SpellHealingBonusDone(target, GetSpellInfo(), uint32(aurEff->GetAmount()), DOT, aurEff->GetEffIndex(), 0.0f, stack);
        tick = target->SpellHealingBonusTaken(caster, GetSpellInfo(), tick, DOT, stack);
        uint64 const total = uint64(tick) * uint64(aurEff->GetTotalTicks());
        uint32 const bloom = uint32(std::min<uint64>(CalculatePct(total, uint32(std::max(marker->GetAmount(), 0))), uint64(std::numeric_limits<int32>::max())));

        BlessingsHelpers::HealUnit(caster, target, bloom, SPELL_VERDANT_BLOOM_HEAL);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_verdant_bloom::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_HEAL, AURA_EFFECT_HANDLE_REAL);
    }
};

void AddMoonlitBlessingsDruidrestorationScripts()
{
    RegisterSpellScript(spell_blessings_druid_ns_cast_only);
    RegisterSpellScript(spell_blessings_wild_growth_reach);
    RegisterSpellScript(spell_blessings_great_grove);
    RegisterSpellScript(spell_blessings_elder_touch);
    RegisterSpellScript(spell_blessings_verdant_bloom);
}
