#pragma once

// The actor's forward pass in integers: int8 weights, uint8 activations,
// exact 32-bit accumulation, fixed-point requantisation between layers. It
// matches export/quantize.py IntegerActor bit for bit, natively and in the
// judge's wasm (where WASM SIMD does eight multiply-adds an instruction).
//
// weights.h (written by export/export_bot.py) defines the layer sizes and
// the arrays this reads.

#include "weights.h"

#include <cstdint>

#if defined(__wasm_simd128__)
#include <wasm_simd128.h>
#endif

namespace net {

/// out[j] = sum_i w[j * stride + i] * in[i], for i < stride (inputs past the
/// real width are zero, as are their weights).
inline void MatVec(int8_t const* w, int16_t const* in, int rows, int stride, int32_t* out)
{
#if defined(__wasm_simd128__)
    for (int j = 0; j < rows; j++)
    {
        int8_t const* row = w + j * stride;
        v128_t acc = wasm_i32x4_splat(0);
        for (int i = 0; i < stride; i += 8)
        {
            v128_t const weights = wasm_i16x8_load8x8(row + i);
            v128_t const inputs = wasm_v128_load(in + i);
            acc = wasm_i32x4_add(acc, wasm_i32x4_dot_i16x8(weights, inputs));
        }
        out[j] = wasm_i32x4_extract_lane(acc, 0) + wasm_i32x4_extract_lane(acc, 1) + wasm_i32x4_extract_lane(acc, 2) +
                 wasm_i32x4_extract_lane(acc, 3);
    }
#else
    for (int j = 0; j < rows; j++)
    {
        int8_t const* row = w + j * stride;
        int32_t acc = 0;
        for (int i = 0; i < stride; i++)
        {
            acc += static_cast<int32_t>(row[i]) * static_cast<int32_t>(in[i]);
        }
        out[j] = acc;
    }
#endif
}

/// uint8 codes (kInputs of them) to the int32 logits of the last layer.
inline void Logits(uint8_t const* codes, int32_t* logits)
{
    alignas(16) static int16_t a[kMaxWidth];
    alignas(16) static int32_t acc[kMaxWidth];
    for (int i = 0; i < kStride[0]; i++)
    {
        a[i] = i < kInputs ? codes[i] : 0;
    }
    for (int layer = 0; layer < kLayers; layer++)
    {
        int const rows = kRows[layer];
        MatVec(kWeights[layer], a, rows, kStride[layer], acc);
        int32_t const* bias = kBias[layer];
        if (layer == kLayers - 1)
        {
            for (int j = 0; j < rows; j++)
            {
                logits[j] = acc[j] + bias[j];
            }
            return;
        }
        int64_t const* multiplier = kMultiplier[layer];
        int const next = kStride[layer + 1];
        for (int j = 0; j < next; j++)
        {
            if (j >= rows)
            {
                a[j] = 0;
                continue;
            }
            int64_t const scaled = (static_cast<int64_t>(acc[j] + bias[j]) * multiplier[j] + (int64_t{1} << 31)) >> 32;
            a[j] = static_cast<int16_t>(scaled < 0 ? 0 : scaled > 255 ? 255 : scaled);
        }
    }
}

/// The allowed action with the highest logit (the lowest index on a tie).
inline int Act(uint8_t const* codes, uint8_t const* mask)
{
    int32_t logits[kOutputs];
    Logits(codes, logits);
    int best = -1;
    for (int a = 0; a < kOutputs; a++)
    {
        if (mask[a] && (best < 0 || logits[a] > logits[best]))
        {
            best = a;
        }
    }
    return best < 0 ? 0 : best;
}

} // namespace net
