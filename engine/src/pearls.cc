#include "engine/pearls.h"
#include "engine/helpers.h"

// 1.2.3 draws from the 64-bit generator and reduces modulo the span in 64
// bits (not with std::uniform_int_distribution), so any conforming
// std::mt19937_64 reproduces the judge's pearls exactly.
static int DrawRespawnGap(Tile const& tile, PearlRng& rng)
{
    auto const span = static_cast<uint64_t>(static_cast<uint32_t>(tile.mMaxRespawnGap - tile.mMinRespawnGap + 1));
    return tile.mMinRespawnGap + static_cast<int>(rng() % span);
}

static int TileIndex(GameState const& state, Point p)
{
    return p.y * state.mWidth + p.x;
}

static bool OwnsSharedCountdown(GameState const& state, Point bed, Point mirror)
{
    return TileIndex(state, mirror) >= TileIndex(state, bed);
}

static void SetCountdown(GameState& state, Point bed, int gap, EventSink const& emit)
{
    state.mTiles.At(bed).mNextPearl = gap;
    EMIT(emit, EventPearlCountdown{bed, gap});
}

static void TrySpawnPearl(GameState& state, Point bed, EventSink const& emit)
{
    Tile& tile = state.mTiles.At(bed);
    if (tile.mHasPearl || AliveDragonOccupying(state, bed) != nullptr)
    {
        return;
    }

    tile.mHasPearl = true;
    EMIT(emit, EventTileChange{bed, true});
}

void InitPearlCountdowns(GameState& state, PearlRng& rng, EventSink const& emit)
{
    state.mPearlBeds.clear();
    for (int y = 0; y < state.mHeight; y++)
    {
        for (int x = 0; x < state.mWidth; x++)
        {
            Point const bed{x, y};
            Point const mirror = MirrorTile(state, bed);
            if (!OwnsSharedCountdown(state, bed, mirror) || !state.mTiles.At(bed).mSpawnsPearls)
            {
                continue;
            }
            state.mPearlBeds.push_back({bed, mirror});

            int const gap = DrawRespawnGap(state.mTiles.At(bed), rng);
            SetCountdown(state, bed, gap, emit);
            if (mirror != bed)
            {
                SetCountdown(state, mirror, gap, emit);
            }
        }
    }
}

void PearlTick(GameState& state, PearlRng& rng, EventSink const& emit)
{
    // The beds InitPearlCountdowns listed, in the same top-to-bottom,
    // left-to-right order, so the draws come out the same.
    for (auto const& [bed, mirror] : state.mPearlBeds)
    {
        int const remaining = state.mTiles.At(bed).mNextPearl - 1;
        state.mTiles.At(bed).mNextPearl = remaining;
        state.mTiles.At(mirror).mNextPearl = remaining;
        if (remaining > 0)
        {
            continue;
        }

        TrySpawnPearl(state, bed, emit);
        if (mirror != bed)
        {
            TrySpawnPearl(state, mirror, emit);
        }

        int const gap = DrawRespawnGap(state.mTiles.At(bed), rng);
        SetCountdown(state, bed, gap, emit);
        if (mirror != bed)
        {
            SetCountdown(state, mirror, gap, emit);
        }
    }
}
