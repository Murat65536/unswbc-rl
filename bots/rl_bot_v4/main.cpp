// UNSW Battlecode 2026 bot: a policy trained by self-play PPO (unswbc-rl).
//
// It speaks the wire protocol itself (docs/rules/protocol.md): core/view.h
// reads the init and round blocks into a View, core/features.h turns the
// View into the observation and the legal-action mask exactly as training
// did (the same files, compiled into both), net.h runs the integer actor,
// and the chosen action goes back as one MOVE or SPLIT line.

#include "core/features.h"
#include "net.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

static_assert(kInputs == core::kObsSize, "weights.h was exported for another observation");
static_assert(kOutputs == core::kNumActions, "weights.h was exported for another action set");

int main()
{
    // One write per turn: the judge charges 2.5M points a write.
    static char output[1 << 14];
    std::setvbuf(stdout, output, _IOFBF, sizeof output);

    core::BlockReader reader;
    // This dragon's memory of its own body, kept across its turns.
    core::Memory memory;
    static char line[1 << 12];
    alignas(16) static uint8_t obs[core::kObsSize];
    uint8_t mask[core::kNumActions];
    while (std::fgets(line, sizeof line, stdin))
    {
        if (!reader.Feed(std::string_view(line, std::strlen(line))))
        {
            continue;
        }
        core::View const& view = reader.Current();
        memory.Update(view);
        core::MoveSight const sight = core::LookAhead(view, &memory);
        core::Encode(view, sight, obs);
        core::Mask(view, sight, kMaskLevel, mask);
        int const action = net::Act(obs, mask);
#ifdef UNSWBC_GATE
        // The export gate's native build also reports what it saw.
        std::fputs("OBS ", stdout);
        for (int i = 0; i < core::kObsSize; i++)
        {
            std::fprintf(stdout, "%02x", obs[i]);
        }
        std::fputs("\n", stdout);
#endif
        std::string const reply = core::FormatCommand(core::Decode(view, action));
        std::fputs(reply.c_str(), stdout);
        std::fputs("\nENDTURN\n", stdout);
        std::fflush(stdout);
    }
    return 0;
}
