// Moonlit Blessings: C++ spell scripts for the Vicar Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/vicar.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_blessings_aegis_of_light   helper aura on a shielded ally: watches your Power Word: Shield and pulses a heal when it runs out
//   spell_blessings_radiant_grace    Greater Heal / Flash Heal critical strikes refund 15% of the cost they were cast for
//   spell_blessings_weaver_spread    casting Renew also casts the same Renew on the most injured other ally
//
// Spell ids come from the registry block 1,620,000 (Vicar). The scripts use GetId() and no hard-coded ids.
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
    constexpr float ALLY_SEARCH_RANGE = 40.0f;       // yards from the caster: where a second Renew may land
    constexpr float PULSE_RANGE = 15.0f;             // yards around the shielded ally
    constexpr uint32 PULSE_MAX_TARGETS = 5;
    constexpr int32 PULSE_SHIELD_SHARE_PCT = 40;     // percent of the shield's unused absorb that each ally is healed for
    constexpr int32 SHIELD_EXPIRY_WINDOW_MS = 1500;  // the helper samples the shield once a second: seen with at most this much time left means it ran out
    constexpr int32 RADIANT_GRACE_MAX_PCT = 100;

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

    inline uint32 MissingHealth(Unit const* unit)
    {
        return unit->GetMaxHealth() - unit->GetHealth();
    }
}

// Aegis of Light: the helper aura (put on the shielded ally by the passive when your Power Word: Shield lands) ticks every second. While your shield is on the ally it
// remembers how long the shield had left and how much it still absorbed; when the shield is gone and was last seen about to run out (not broken, not dispelled), the
// unused absorb is pulsed as healing to up to 5 injured allies around the ally.
class spell_blessings_aegis_of_light : public AuraScript
{
    PrepareAuraScript(spell_blessings_aegis_of_light);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* target = GetTarget();
        Unit* caster = GetCaster();
        if (!caster)
            return;

        // Power Word: Shield (class mask word 1 bit 0x1), cast by the Vicar
        AuraEffect const* shield = target->GetAuraEffect(SPELL_AURA_SCHOOL_ABSORB, SPELLFAMILY_PRIEST, 0x1, 0, 0, caster->GetGUID());
        if (shield)
        {
            _armed = true;
            _lastDuration = shield->GetBase()->GetDuration();
            _lastAbsorb = shield->GetAmount();
            return;
        }

        if (!_armed)
            return;

        _armed = false;
        if (_lastDuration > SHIELD_EXPIRY_WINDOW_MS || _lastAbsorb <= 0)
            return;

        uint32 const amount = CalculatePct(uint32(_lastAbsorb), PULSE_SHIELD_SHARE_PCT);
        std::vector<Unit*> friends;
        CollectFriends(caster, friends);

        std::vector<Unit*> hurt;
        for (Unit* unit : friends)
            if (unit->IsAlive() && unit->IsWithinDist(target, PULSE_RANGE) && MissingHealth(unit) > 0)
                hurt.push_back(unit);

        std::sort(hurt.begin(), hurt.end(), [](Unit const* a, Unit const* b) { return MissingHealth(a) > MissingHealth(b); });
        if (hurt.size() > PULSE_MAX_TARGETS)
            hurt.resize(PULSE_MAX_TARGETS);

        for (Unit* unit : hurt)
            BlessingsHelpers::HealUnit(caster, unit, amount, GetId());
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_aegis_of_light::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }

    bool _armed = false;
    int32 _lastDuration = 0;
    int32 _lastAbsorb = 0;
};

// Radiant Grace: a critical Greater Heal or Flash Heal gives back part (the effect value, 15) of the mana the cast cost. Free casts (Inner Focus ...) refund nothing.
class spell_blessings_radiant_grace : public AuraScript
{
    PrepareAuraScript(spell_blessings_radiant_grace);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        Spell const* spell = eventInfo.GetProcSpell();
        if (!spell)
            return;

        int32 const cost = spell->GetPowerCost();
        if (cost <= 0)
            return;

        Unit* caster = GetTarget();
        int32 const refund = CalculatePct(cost, std::min<int32>(aurEff->GetAmount(), RADIANT_GRACE_MAX_PCT));
        if (refund > 0)
            caster->EnergizeBySpell(caster, GetId(), uint32(refund), POWER_MANA);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_radiant_grace::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Weaver of Renewals, part 2: casting Renew also casts the same Renew (same rank) on the most injured other ally within 40 yards. The cast is triggered, so it
// cannot spread again.
class spell_blessings_weaver_spread : public AuraScript
{
    PrepareAuraScript(spell_blessings_weaver_spread);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        Unit const* first = eventInfo.GetActionTarget();
        if (!spellInfo || !first)
            return;

        Unit* caster = GetTarget();
        std::vector<Unit*> friends;
        CollectFriends(caster, friends);

        Unit* best = nullptr;
        uint32 bestMissing = 0;
        for (Unit* unit : friends)
        {
            if (unit == first || !unit->IsAlive() || !unit->IsWithinDist(caster, ALLY_SEARCH_RANGE))
                continue;

            uint32 const missing = MissingHealth(unit);
            if (missing > bestMissing)
            {
                bestMissing = missing;
                best = unit;
            }
        }

        if (best)
            caster->CastSpell(best, spellInfo->Id, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_weaver_spread::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

void AddMoonlitBlessingsVicarScripts()
{
    RegisterSpellScript(spell_blessings_aegis_of_light);
    RegisterSpellScript(spell_blessings_radiant_grace);
    RegisterSpellScript(spell_blessings_weaver_spread);
}
