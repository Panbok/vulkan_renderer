#include "vkr_ltc_lut.h"

/*
 * LUT values are adapted from selfshadow/ltc_code, fit/results/ltc.js,
 * commit 31e5e96b54f98f33098f8503003119ba2231a1c6, source SHA-256
 * 21071160163defd419b8f754ab604a8e60c44fcf539bfff9916e0e8e82316c1d.
 * They fit correlated-Smith isotropic GGX with alpha=roughness^2.
 *
 * Copyright (c) 2017, Eric Heitz, Jonathan Dupuy, Stephen Hill and David
 * Neubelt. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * If you use (or adapt) the source code in your own work, please include a
 *   reference to the paper: Real-Time Polygonal-Light Shading with Linearly
 *   Transformed Cosines. Eric Heitz, Jonathan Dupuy, Stephen Hill and David
 *   Neubelt. ACM Transactions on Graphics (Proceedings of ACM SIGGRAPH 2016)
 *   35(4), 2016. Project page: https://eheitzresearch.wordpress.com/415-2/
 *
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
const uint16_t vkr_ltc_lut_pixels
    [VKR_LTC_LUT_TABLE_COUNT]
    [VKR_LTC_LUT_TABLE_TEXEL_COUNT * VKR_LTC_LUT_CHANNEL_COUNT] = {
#include "vkr_ltc_lut_data.inc"
};

_Static_assert(sizeof(vkr_ltc_lut_pixels) ==
                   VKR_LTC_LUT_TABLE_COUNT * VKR_LTC_LUT_TABLE_BYTE_SIZE,
               "LTC LUT must remain two 64x64 RGBA16F tables");
