#pragma once

#include <map>
#include <string>

#include "base/fixed_string.hpp"

namespace dfdh {

/**
 * Multiplayer lobby: the pre-game phase. The server owner chooses the level,
 * the game mode and the groups, clients choose their own group and get ready.
 * The game is not simulated while the lobby is active (see game_state::lobby_start)
 */
struct lobby_state {
    /* Groups to choose from: -1 (no group) .. max_group */
    static constexpr int max_group = 7;

    bool        active = false;
    std::string level; /* chosen lvl_ section (in game: the played one) */
    std::string mode;  /* chosen gm_ section */

    /* Ready flags of the client players (the server owner's players are always ready) */
    std::map<player_name_t, bool> ready;

    [[nodiscard]]
    bool is_ready(const player_name_t& name) const {
        auto found = ready.find(name);
        return found != ready.end() && found->second;
    }
};

}
