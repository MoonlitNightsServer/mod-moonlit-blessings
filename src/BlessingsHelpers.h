// Moonlit Blessings: small helpers shared by the C++ spell scripts of the Blessings (one source file per Blessing, see src/*Scripts.cpp).
// ~Moonlit Nights Team
#ifndef MOONLIT_BLESSINGS_HELPERS_H
#define MOONLIT_BLESSINGS_HELPERS_H

#include "Group.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Unit.h"

namespace BlessingsHelpers
{
    // The group member (or the caster, or a pet) within `range` yards of `center` that is missing the most health. Null if nobody is hurt.
    inline Unit* MostInjuredFriend(Unit* caster, Unit* center, float range)
    {
        if (!caster || !center)
            return nullptr;

        Unit* best = nullptr;
        uint32 bestMissing = 0;
        auto consider = [&](Unit* u)
        {
            if (!u || !u->IsAlive() || !u->IsWithinDist(center, range))
                return;

            uint32 const missing = u->GetMaxHealth() - u->GetHealth();
            if (missing > bestMissing)
            {
                bestMissing = missing;
                best = u;
            }
        };

        consider(caster);
        if (Player* player = caster->ToPlayer())
        {
            if (Group* group = player->GetGroup())
            {
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                {
                    Player* member = ref->GetSource();
                    consider(member);
                    if (member)
                        consider(member->GetPet());
                }
            }

            consider(player->GetPet());
        }

        return best;
    }

    // Heal `target` for `amount` as `caster` (no cast needed). `spellId` is the spell the heal is attributed to; it may be 0.
    inline void HealUnit(Unit* caster, Unit* target, uint32 amount, uint32 spellId = 0)
    {
        if (!caster || !target || !amount)
            return;

        SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
        HealInfo healInfo(caster, target, amount, info, info ? info->GetSchoolMask() : SPELL_SCHOOL_MASK_HOLY);
        caster->HealBySpell(healInfo, false);
    }
}

#endif
