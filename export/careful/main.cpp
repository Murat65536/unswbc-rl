// The careful scripted player as a bot (core/careful.h): the training
// environment's stronger scripted opponent, here to be played in the judge's
// sandbox (python -m export.evaluate --opponent careful). Each dragon runs
// its own copy, with the same noise sequence the environment gives it.

#include "core/careful.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

int main()
{
    static char output[1 << 12];
    std::setvbuf(stdout, output, _IOFBF, sizeof output);

    core::BlockReader reader;
    static char line[1 << 12];
    uint64_t rng = core::kCarefulSeed;
    while (std::fgets(line, sizeof line, stdin))
    {
        if (!reader.Feed(std::string_view(line, std::strlen(line))))
        {
            continue;
        }
        core::View const& view = reader.Current();
        int const action = core::CarefulAction(view, core::CarefulNoise(rng));
        std::string const reply = core::FormatCommand(core::Decode(view, action));
        std::fputs(reply.c_str(), stdout);
        std::fputs("\nENDTURN\n", stdout);
        std::fflush(stdout);
    }
    return 0;
}
