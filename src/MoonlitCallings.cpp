// Moonlit Callings (testing stage): the C++ half of the Callings talent system.
//
// For the classes listed in MoonlitCallings.Classes the stock ways of gaining talents are closed so that the Callings system (Lua, mod-ale) is the only route:
//   * learning a talent from the talent window is refused;
//   * trainers show no spells and teach nothing (class spells arrive through Learn Spells, everything else through the Callings);
//   * on login the stock talents are reset once and free talent points are set to 0.
// Spells granted by the Lua system go through Player::learnSpell and are NOT blocked: this module only uses the talent veto and the trainer hooks.
// ~Moonlit Nights Team

#include "Config.h"
#include "Creature.h"
#include "Log.h"
#include "NPCPackets.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Trainer.h"

#include <set>

namespace
{
    bool IsCallingsClass(Player const* player)
    {
        if (!player || !sConfigMgr->GetOption<bool>("MoonlitCallings.Enable", false))
            return false;

        std::string const classes = sConfigMgr->GetOption<std::string>("MoonlitCallings.Classes", "5,7");
        std::string token;
        for (size_t i = 0; i <= classes.size(); ++i)
        {
            if (i == classes.size() || classes[i] == ',')
            {
                if (!token.empty() && static_cast<uint8>(std::stoul(token)) == player->getClass())
                    return true;
                token.clear();
            }
            else if (classes[i] != ' ')
                token += classes[i];
        }
        return false;
    }
}

class MoonlitCallingsPlayer : public PlayerScript
{
public:
    MoonlitCallingsPlayer() : PlayerScript("MoonlitCallingsPlayer", {
        PLAYERHOOK_CAN_LEARN_TALENT,
        PLAYERHOOK_ON_GET_TRAINER_SPELL_STATE,
        PLAYERHOOK_ON_BEFORE_RECEIVE_SPELL_LIST_FROM_TRAINER,
        PLAYERHOOK_ON_LOGIN
    }) { }

    bool OnPlayerCanLearnTalent(Player* player, TalentEntry const* /*talent*/, uint32 /*rank*/) override
    {
        return !IsCallingsClass(player);
    }

    void OnPlayerGetTrainerSpellState(Player const* player, uint32 /*trainerId*/, uint32 /*spellId*/, Trainer::SpellState& state) override
    {
        if (IsCallingsClass(player))
            state = Trainer::SpellState::Unavailable;
    }

    void OnPlayerBeforeReceiveSpellListFromTrainer(Player* player, Creature* /*trainer*/, WorldPackets::NPC::TrainerList& list) override
    {
        if (IsCallingsClass(player))
            list.Spells.clear();
    }

    void OnPlayerLogin(Player* player) override
    {
        if (!IsCallingsClass(player))
            return;

        // The stock talent system is closed for this class: take back anything learned before, once (a no-op when nothing is learned)
        if (player->GetFreeTalentPoints() > 0 || player->GetLevel() >= 10)
        {
            player->resetTalents(true);
            player->SetFreeTalentPoints(0);
        }
    }
};

void AddSC_moonlit_callings()
{
    new MoonlitCallingsPlayer();
}
