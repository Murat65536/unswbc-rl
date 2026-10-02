#include "engine/pearls.h"
#include "engine/helpers.h"

static int DrawRespawnGap(Tile const& tile, std::mt19937& rng)
{
    auto const span = static_cast<uint32_t>(tile.mMaxRespawnGap - tile.mMinRespawnGap + 1);
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
    emit(EventPearlCountdown{bed, gap});
}

static void TrySpawnPearl(GameState& state, Point bed, EventSink const& emit)
{
    Tile& tile = state.mTiles.At(bed);
    if (tile.mHasPearl || AliveDragonOccupying(state, bed) != nullptr)
    {
        return;
    }

    tile.mHasPearl = true;
    emit(EventTileChange{bed, true});
}

void InitPearlCountdowns(GameState& state, std::mt19937& rng, EventSink const& emit)
{
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

            int const gap = DrawRespawnGap(state.mTiles.At(bed), rng);
            SetCountdown(state, bed, gap, emit);
            if (mirror != bed)
            {
                SetCountdown(state, mirror, gap, emit);
            }
        }
    }
}

void PearlTick(GameState& state, std::mt19937& rng, EventSink const& emit)
{
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
}
