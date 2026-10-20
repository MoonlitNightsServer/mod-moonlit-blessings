// Moonlit Blessings: C++ spell scripts for the Restoration Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/restoration.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_blessings_mana_tide_cast          lets the Mana Tide Totem cast (and no other totem that shares its class mask bit) start the Mana Tide Mastery and Spirit Well auras
//   spell_blessings_ancestral_tide_spread   Riptide also lands on a second, injured ally
//   spell_blessings_ancestral_tide_consume  Chain Heal on a Riptide target also heals the Riptide's remaining heal over time at once
//   spell_blessings_earthliving_heart       every 3 sec heals the most injured ally for 20% of the healing done in those 3 sec
//   spell_blessings_spirit_link             casts the Spirit Link on the target and its partner (each is the caster of the other's link)
//   spell_blessings_spirit_link_aura        half of every heal either linked ally receives is also given to the other
//   spell_blessings_spirit_well_tide        the Favor of the Spirit Well's extra mana pulse on the party
//
// The damage half of the Spirit Link is the core's own Split Damage aura (it sends damage to the CASTER of the aura, hence the crossed casters).
// Spell ids come from the registry block 1,830,000 (Restoration): unit n of tools/talents/recipes/restoration.py is 1,830,000 + 10 n.
// ~Moonlit Nights Team

#include "BlessingsHelpers.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include <algorithm>
#include <vector>

namespace
{
    enum RestorationSpells : uint32
    {
        SPELL_MANA_TIDE_TOTEM        = 16190,
        SPELL_ANCESTRAL_TIDE_HEAL    = 1830032,
        SPELL_EARTHLIVING_HEART_HEAL = 1830041,
        SPELL_SPIRIT_LINK_AURA       = 1830051
    };

    constexpr float ALLY_SEARCH_RANGE = 40.0f;       // yards from the caster: where a second Riptide, an Earthliving Heart heal or a Spirit Link partner may be

    // The caster, the other members of the caster's group and their pets.
    void CollectFriends(Unit* caster, std::vector<Unit*>& out)
    {
        out.push_back(caster);
        if (Player* player = caster->ToPlayer())
        {
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
    }

    // The living friend within ALLY_SEARCH_RANGE of `caster` that is missing the most health, `exclude` (may be null) and the full-health left out. Null if nobody qualifies.
    Unit* MostInjuredOther(Unit* caster, Unit const* exclude)
    {
        std::vector<Unit*> friends;
        CollectFriends(caster, friends);

        Unit* best = nullptr;
        uint32 bestMissing = 0;
        for (Unit* unit : friends)
        {
            if (unit == exclude || !unit->IsAlive() || !unit->IsWithinDist(caster, ALLY_SEARCH_RANGE))
                continue;

            uint32 const missing = unit->GetMaxHealth() - unit->GetHealth();
            if (missing > bestMissing)
            {
                bestMissing = missing;
                best = unit;
            }
        }

        return best;
    }
}

// Mana Tide Mastery and Favor of the Spirit Well: the passives proc on the cast of "Mana Tide Totem" by class mask, but that bit is shared with most other long totems
// (Stoneskin, Strength of Earth, Windfury ...). Only the Mana Tide Totem itself may start the aura.
class spell_blessings_mana_tide_cast : public AuraScript
{
    PrepareAuraScript(spell_blessings_mana_tide_cast);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        return spellInfo && spellInfo->Id == SPELL_MANA_TIDE_TOTEM;
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_blessings_mana_tide_cast::CheckProc);
    }
};

// Ancestral Tide, part 1: casting Riptide also casts the same Riptide (same rank) on the most injured other ally. The cast is triggered, so it does not proc this again.
class spell_blessings_ancestral_tide_spread : public AuraScript
{
    PrepareAuraScript(spell_blessings_ancestral_tide_spread);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        Unit* first = eventInfo.GetActionTarget();
        if (!spellInfo || !first)
            return;

        Unit* caster = GetTarget();
        if (Unit* second = MostInjuredOther(caster, first))
            caster->CastSpell(second, spellInfo->Id, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_ancestral_tide_spread::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Ancestral Tide, part 2: the Chain Heal is cast (proc on the cast, before the heal lands and the core's Chain Heal script consumes the Riptide): if the
// target has the caster's Riptide, the Riptide's remaining ticks are healed at once. The core still adds its own 25% and removes the Riptide.
class spell_blessings_ancestral_tide_consume : public AuraScript
{
    PrepareAuraScript(spell_blessings_ancestral_tide_consume);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        Spell const* spell = eventInfo.GetProcSpell();
        Unit* caster = GetTarget();
        Unit* target = spell ? spell->m_targets.GetUnitTarget() : nullptr;
        if (!target || !target->IsAlive())
            return;

        // The same lookup the core's Chain Heal script uses for Riptide (Shaman, class mask word 3 bit 0x10, cast by us).
        AuraEffect const* riptide = target->GetAuraEffect(SPELL_AURA_PERIODIC_HEAL, SPELLFAMILY_SHAMAN, 0, 0, 0x10, caster->GetGUID());
        if (!riptide)
            return;

        int32 const ticksLeft = riptide->GetTotalTicks() - int32(riptide->GetTickNumber());
        if (ticksLeft <= 0 || riptide->GetAmount() <= 0)
            return;

        BlessingsHelpers::HealUnit(caster, target, uint32(riptide->GetAmount()) * uint32(ticksLeft), SPELL_ANCESTRAL_TIDE_HEAL);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_ancestral_tide_consume::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Earthliving Heart: effect 0 (dummy) counts the healing you do, effect 1 (periodic dummy, every 3 sec, value 20) turns 20% of it into a heal on the most injured ally.
class spell_blessings_earthliving_heart : public AuraScript
{
    PrepareAuraScript(spell_blessings_earthliving_heart);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        if (HealInfo* healInfo = eventInfo.GetHealInfo())
            _healed += healInfo->GetHeal();
    }

    void HandlePeriodic(AuraEffect const* aurEff)
    {
        uint32 const amount = CalculatePct(_healed, aurEff->GetAmount());
        _healed = 0;

        Unit* caster = GetTarget();
        if (!amount || !caster->IsAlive())
            return;

        if (Unit* ally = BlessingsHelpers::MostInjuredFriend(caster, caster, ALLY_SEARCH_RANGE))
            BlessingsHelpers::HealUnit(caster, ally, amount, SPELL_EARTHLIVING_HEART_HEAL);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_earthliving_heart::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_earthliving_heart::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
    }

    uint32 _healed = 0;
};

// Spirit Link (the castable ability): the target and the most injured other ally each get the link aura, cast by the other one.
class spell_blessings_spirit_link : public SpellScript
{
    PrepareSpellScript(spell_blessings_spirit_link);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_SPIRIT_LINK_AURA });
    }

    void HandleDummy(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Unit* first = GetHitUnit();
        if (!caster || !first)
            return;

        Unit* partner = MostInjuredOther(caster, first);
        if (!partner)
            return;

        partner->CastSpell(first, SPELL_SPIRIT_LINK_AURA, true);
        first->CastSpell(partner, SPELL_SPIRIT_LINK_AURA, true);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_blessings_spirit_link::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// Spirit Link (the aura; its caster is the partner): effect 1 is a dummy (value 50) that runs on every heal the linked ally receives and gives the partner half of it.
class spell_blessings_spirit_link_aura : public AuraScript
{
    PrepareAuraScript(spell_blessings_spirit_link_aura);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        HealInfo* healInfo = eventInfo.GetHealInfo();
        Unit* partner = GetCaster();
        Unit* target = GetTarget();
        if (!healInfo || !partner || partner == target || !partner->IsAlive())
            return;

        uint32 const amount = CalculatePct(healInfo->GetHeal(), aurEff->GetAmount());
        Unit* healer = eventInfo.GetActor() ? eventInfo.GetActor() : target;
        BlessingsHelpers::HealUnit(healer, partner, amount, SPELL_SPIRIT_LINK_AURA);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_spirit_link_aura::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

// Favor of the Spirit Well: the party area aura pulses every 3 sec; each pulse restores 2.4% (value 24, in tenths of a percent) of the member's maximum mana.
class spell_blessings_spirit_well_tide : public AuraScript
{
    PrepareAuraScript(spell_blessings_spirit_well_tide);

    void HandlePeriodic(AuraEffect const* aurEff)
    {
        Unit* target = GetTarget();
        if (!target->IsAlive() || !target->HasActivePowerType(POWER_MANA))
            return;

        uint32 const amount = uint32(uint64(target->GetMaxPower(POWER_MANA)) * uint32(std::max<int32>(aurEff->GetAmount(), 0)) / 1000);
        if (!amount)
            return;

        Unit* caster = GetCaster();
        (caster ? caster : target)->EnergizeBySpell(target, GetId(), amount, POWER_MANA);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_spirit_well_tide::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

void AddMoonlitBlessingsRestorationScripts()
{
    RegisterSpellScript(spell_blessings_mana_tide_cast);
    RegisterSpellScript(spell_blessings_ancestral_tide_spread);
    RegisterSpellScript(spell_blessings_ancestral_tide_consume);
    RegisterSpellScript(spell_blessings_earthliving_heart);
    RegisterSpellScript(spell_blessings_spirit_link);
    RegisterSpellScript(spell_blessings_spirit_link_aura);
    RegisterSpellScript(spell_blessings_spirit_well_tide);
}
