/*******************************************************************************
 *
 * MIT License
 *
 * Copyright (c) 2020 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *******************************************************************************/

#include <miopen/solver.hpp>
#include <miopen/conv/invokers/impl_gemm.hpp>
#include <miopen/conv/wrw_invoke_params.hpp>
#include <miopen/handle.hpp>
#include <miopen/generic_search.hpp>
#include <miopen/hip_build_utils.hpp>
#include <miopen/solver/implicitgemm_util.hpp>
#include <miopen/stringutils.hpp>
#include <miopen/tensor_ops.hpp>
#include <miopen/implicitgemm_params.hpp>

/// Fatal compiler errors with ROCm 3.7 on some BF16 configs.
#define WORKAROUND_MI100_BF16_FATAL_COMPILER_ERRORS \
    (HIP_PACKAGE_VERSION_FLAT >= 3007000000 && HIP_PACKAGE_VERSION_FLAT <= 3007999999)

MIOPEN_DECLARE_ENV_VAR(MIOPEN_DEBUG_CONV_IMPLICIT_GEMM_HIP_WRW_V4R4_PADDED_GEMM_XDLOPS)

namespace miopen {
namespace solver {

PerformanceImplicitGemmWrwV4R4Xdlops_Padded_Gemm::PerformanceImplicitGemmWrwV4R4Xdlops_Padded_Gemm()
    : PerformanceImplicitGemmWrwV4R4Xdlops_Padded_Gemm::
          PerformanceImplicitGemmWrwV4R4Xdlops_Padded_Gemm(
              4, 4, 1, 4, 4, 1, 16, 64, 16, false, false)
{ // GemmMFactor GemmNFactor, GemmKTotalFactor are fixed value at this moment
}

PerformanceImplicitGemmWrwV4R4Xdlops_Padded_Gemm::PerformanceImplicitGemmWrwV4R4Xdlops_Padded_Gemm(
    int GemmMPerBlock_,
    int GemmNPerBlock_,
    int GemmKPerBlock_,
    int GemmMPerWave_,
    int GemmNPerWave_,
    int GemmKPack_,
    int GemmMFactor_,
    int GemmNFactor_,
    int GemmKTotalFactor_,
    bool GemmAThreadCopyMoreGemmK_,
    bool GemmBThreadCopyMoreGemmK_)
    : GemmMPerBlock(GemmMPerBlock_),
      GemmNPerBlock(GemmNPerBlock_),
      GemmKPerBlock(GemmKPerBlock_),
      GemmMPerWave(GemmMPerWave_),
      GemmNPerWave(GemmNPerWave_),
      GemmKPack(GemmKPack_),
      GemmMFactor(GemmMFactor_),
      GemmNFactor(GemmNFactor_),
      GemmKTotalFactor(GemmKTotalFactor_),
      GemmAThreadCopyMoreGemmK(GemmAThreadCopyMoreGemmK_),
      GemmBThreadCopyMoreGemmK(GemmBThreadCopyMoreGemmK_)
{
}

bool PerformanceImplicitGemmWrwV4R4Xdlops_Padded_Gemm::operator==(
    const PerformanceImplicitGemmWrwV4R4Xdlops_Padded_Gemm& other) const
{
    // clang-format off
    return GemmMPerBlock == other.GemmMPerBlock
        && GemmNPerBlock == other.GemmNPerBlock
        && GemmKPerBlock == other.GemmKPerBlock
        && GemmMPerWave == other.GemmMPerWave
        && GemmNPerWave == other.GemmNPerWave
        && GemmKPack == other.GemmKPack
        && GemmMFactor == other.GemmMFactor
        && GemmNFactor == other.GemmNFactor
        && GemmKTotalFactor == other.GemmKTotalFactor
        && GemmAThreadCopyMoreGemmK  == other.GemmAThreadCopyMoreGemmK
        && GemmBThreadCopyMoreGemmK  == other.GemmBThreadCopyMoreGemmK;
    // clang-format on
}

bool PerformanceImplicitGemmWrwV4R4Xdlops_Padded_Gemm::SetNextValue(
    const ConvolutionContext& /*config*/)
{
    do
    {
        // list performance parameters in reverse order, in order for tuning to iterate over the
        // range in normal order
        if(!NextFlag<false, true>(GemmBThreadCopyMoreGemmK))
            break;
        if(!NextFlag<false, false>(GemmAThreadCopyMoreGemmK))
            break;
        if(!NextTwoPower<1, 8>(GemmKPack))
            break;
        if(!NextTwoPower<4, 128>(GemmNPerWave))
            break;
        if(!NextTwoPower<4, 128>(GemmMPerWave))
            break;
        if(!NextTwoPower<1, 8>(GemmKPerBlock))
            break;
        if(!NextTwoPower<4, 256>(GemmNPerBlock))
            break;
        if(!NextTwoPower<4, 256>(GemmMPerBlock))
            break;
        return false;
    } while(false);

    return true;
}

void PerformanceImplicitGemmWrwV4R4Xdlops_Padded_Gemm::HeuristicInit(const ConvolutionContext& ctx)
{
    PerformanceImplicitGemmWrwV4R4Xdlops_Padded_Gemm tmp;
    // GemmMFactor GemmNFactor, GemmKTotalFactor are fixed value at this moment.
    // loop over certain ranges of tuning parameter
    auto get_euristic_config = [&](auto is_valid_func) {
        if(ctx.IsFp32())
        {
            tmp = {256, 256, 8, 128, 128, 4, 16, 64, 16, false, true};

            bool all_visited = false;
            do
            {
                do
                {
                    // list in reverse order of importance,
                    // and favor large GEMM
                    if(!PreviousTwoPower<1, 8>(tmp.GemmKPerBlock))
                        break;
                    if(!PreviousTwoPower<1, 4>(tmp.GemmKPack))
                        break;
                    if(!PreviousTwoPower<4, 128>(tmp.GemmNPerWave))
                        break;
                    if(!PreviousTwoPower<4, 128>(tmp.GemmMPerWave))
                        break;
                    if(!PreviousTwoPower<4, 256>(tmp.GemmNPerBlock))
                        break;
                    if(!PreviousTwoPower<4, 256>(tmp.GemmMPerBlock))
                        break;

                    all_visited = true;
                } while(false);

                if(is_valid_func(tmp, ctx))
                    break;
            } while(!all_visited);
        }
        else if(ctx.IsFp16())
        {
            tmp              = {256, 256, 8, 128, 128, 8, 16, 64, 16, false, true};
            bool all_visited = false;
            do
            {
                do
                {
                    // list in reverse order of importance,
                    // and favor large GEMM
                    if(!PreviousTwoPower<1, 8>(tmp.GemmKPerBlock))
                        break;
                    if(!PreviousTwoPower<4, 8>(tmp.GemmKPack))
                        break;
                    if(!PreviousTwoPower<4, 128>(tmp.GemmNPerWave))
                        break;
                    if(!PreviousTwoPower<4, 128>(tmp.GemmMPerWave))
                        break;
                    if(!PreviousTwoPower<4, 256>(tmp.GemmNPerBlock))
                        break;
                    if(!PreviousTwoPower<4, 256>(tmp.GemmMPerBlock))
                        break;

                    all_visited = true;
                } while(false);

                if(is_valid_func(tmp, ctx))
                    break;
            } while(!all_visited);
        }
        else if(ctx.IsBfp16())
        {
            tmp = {256, 256, 8, 128, 128, 8, 16, 64, 16, false, true};

            bool all_visited = false;
            do
            {
                do
                {
                    // list in reverse order of importance,
                    // and favor large GEMM
                    if(!PreviousTwoPower<1, 8>(tmp.GemmKPerBlock))
                        break;
                    if(!PreviousTwoPower<2, 8>(tmp.GemmKPack))
                        break;
                    if(!PreviousTwoPower<4, 128>(tmp.GemmNPerWave))
                        break;
                    if(!PreviousTwoPower<4, 128>(tmp.GemmMPerWave))
                        break;
                    if(!PreviousTwoPower<4, 256>(tmp.GemmNPerBlock))
                        break;
                    if(!PreviousTwoPower<4, 256>(tmp.GemmMPerBlock))
                        break;

                    all_visited = true;
                } while(false);

                if(is_valid_func(tmp, ctx))
                    break;
            } while(!all_visited);
        }
        else
        {
            MIOPEN_LOG_E("Only fp32, fp16, and bfp16 are supported");
            assert(false);
        }
    };

    // first round: really valid and fast
    get_euristic_config([](auto config, auto conv_context) {
        return config.IsReallyValid(conv_context) && config.IsFastToBeUsedForTuning(conv_context);
    });

    // second round: really valid
    if(!tmp.IsReallyValid(ctx))
    {
        get_euristic_config(
            [](auto config, auto conv_context) { return config.IsReallyValid(conv_context); });
    }

    // final check
    if(!tmp.IsReallyValid(ctx))
    {
        MIOPEN_LOG_I("All attempts unsuccessful");
    }
    *this = tmp;
    MIOPEN_LOG_I(ToString());
}

std::string PerformanceImplicitGemmWrwV4R4Xdlops_Padded_Gemm::ToString() const
{
    std::ostringstream ss;
    Serialize(ss);
    return ss.str();
}

std::tuple<int, bool> PerformanceImplicitGemmWrwV4R4Xdlops_Padded_Gemm::CalculateBlockSize() const
{
    int block_size = 0;

    try
    {
        if(!(GemmMPerBlock % GemmMPerWave == 0 && GemmNPerBlock % GemmNPerWave == 0))
            MIOPEN_THROW("invalid performance parameter");

        const auto WaveSize = 64;
        block_size = (GemmNPerBlock * GemmMPerBlock) / (GemmMPerWave * GemmNPerWave) * WaveSize;
    }
    catch(...)
    {
        return std::make_tuple(-1, false);
    }

    return std::make_tuple(block_size, true);
}

std::tuple<int, bool> PerformanceImplicitGemmWrwV4R4Xdlops_Padded_Gemm::CalculateGridSize(
    const ConvolutionContext& ctx) const
{
    int GridSize = 0;

    try
    {
        bool valid = false;

        int gemm_g = -1;
        int gemm_m = -1;
        int gemm_n = -1;

        std::tie(gemm_g,
                 gemm_m,
                 gemm_n,
                 std::ignore,
                 std::ignore,
                 std::ignore,
                 std::ignore,
                 std::ignore,
                 valid) = CalculateGemmSizeAndGemmKBlock(ctx);

        if(!valid)
            MIOPEN_THROW("invalid performance parameter");

        if(!(gemm_m % GemmMPerBlock == 0 && gemm_n % GemmNPerBlock == 0))
            MIOPEN_THROW("invalid performance parameter");

        GridSize = gemm_g * (gemm_m / GemmMPerBlock) * (gemm_n / GemmNPerBlock);
    }
    catch(...)
    {
        return std::make_tuple(-1, false);
    }

    return std::make_tuple(GridSize, true);
}

std::tuple<int, int, int, int, int, bool>
PerformanceImplicitGemmWrwV4R4Xdlops_Padded_Gemm::CalculateGemmABlockCopyPerformanceParameters(
    const ConvolutionContext& ctx) const
{
    // A tensor shape [GemmG, GemmK, GemmM, GemmKPack]
    int ClusterLengths_GemmK     = -1;
    int ClusterLengths_GemmM     = -1;
    int ClusterLengths_GemmKPack = -1;
    int SrcDataPerRead_GemmKPack = ctx.IsFp32() ? amd_buffer_load_max_length<float>()
                                                : amd_buffer_load_max_length<half_float::half>();
    int DstDataPerWrite_GemmKPack = ctx.IsFp32() ? amd_lds_write_max_length<float>()
                                                 : amd_lds_write_max_length<half_float::half>();
    try
    {
        bool valid = false;

        int block_size = -1;

        std::tie(block_size, valid) = CalculateBlockSize();

        if(!valid)
            MIOPEN_THROW("invalid performance parameter");

        // GemmKPack is src vector read dimension, bounded by GemmKPack
        SrcDataPerRead_GemmKPack = gcd(SrcDataPerRead_GemmKPack, GemmKPack);

        // GemmPack bounded by ho*wo
        const auto ho            = ConvolutionContextInterpreter::GetOutputHeightHo(ctx);
        const auto wo            = ConvolutionContextInterpreter::GetOutputWidthWo(ctx);
        SrcDataPerRead_GemmKPack = gcd(SrcDataPerRead_GemmKPack, ho * wo);

        // calculate threadwise copy size
        auto data_per_thread_copy =
            std::max(1, (GemmKPerBlock * GemmMPerBlock * GemmKPack) / block_size);

        // make sure a thread can do a full vector load, at the cost that some threads
        // may not do threadwise copy at all
        data_per_thread_copy = lcm(data_per_thread_copy, SrcDataPerRead_GemmKPack);

        const auto data_per_thread_copy_gemmkpack = SrcDataPerRead_GemmKPack;
        const auto tmp = data_per_thread_copy / data_per_thread_copy_gemmkpack;

        if(tmp == 0)
            MIOPEN_THROW("invalid performance parameter");

        int data_per_thread_copy_gemmk = -1;
        int data_per_thread_copy_gemmm = -1;

        if(GemmAThreadCopyMoreGemmK)
        {
            data_per_thread_copy_gemmk = gcd(GemmKPerBlock, tmp);
            data_per_thread_copy_gemmm = tmp / data_per_thread_copy_gemmk;
        }
        else
        {
            data_per_thread_copy_gemmm = gcd(GemmMPerBlock, tmp);
            data_per_thread_copy_gemmk = tmp / data_per_thread_copy_gemmm;
        }

        if(data_per_thread_copy_gemmk <= 0 || data_per_thread_copy_gemmm <= 0 ||
           data_per_thread_copy_gemmkpack <= 0)
            MIOPEN_THROW("invalid performance parameter");

        // vector write into LDS
        DstDataPerWrite_GemmKPack = gcd(DstDataPerWrite_GemmKPack, data_per_thread_copy_gemmkpack);

        if(!(GemmKPerBlock % data_per_thread_copy_gemmk == 0 &&
             GemmMPerBlock % data_per_thread_copy_gemmm == 0 &&
             GemmKPack % data_per_thread_copy_gemmkpack == 0))
            MIOPEN_THROW("invalid performance parameter");

        ClusterLengths_GemmK     = GemmKPerBlock / data_per_thread_copy_gemmk;
        ClusterLengths_GemmM     = GemmMPerBlock / data_per_thread_copy_gemmm;
        ClusterLengths_GemmKPack = GemmKPack / data_per_thread_copy_gemmkpack;

        if(ClusterLengths_GemmK < 0 || ClusterLengths_GemmM < 0 || ClusterLengths_GemmKPack < 0)
            MIOPEN_THROW("invalid performance parameter");
        // blockwise-copy support that block_size is larger than thread cluster size, which means
        // some threads may not do threadwise copy
        if(block_size < ClusterLengths_GemmK * ClusterLengths_GemmM * ClusterLengths_GemmKPack)
            MIOPEN_THROW("invalid performance parameter");
    }
    catch(...)
    {
        return std::make_tuple(-1, -1, -1, -1, -1, false);
    }

    return std::make_tuple(ClusterLengths_GemmK,
                           ClusterLengths_GemmM,
                           ClusterLengths_GemmKPack,
                           SrcDataPerRead_GemmKPack,
                           DstDataPerWrite_GemmKPack,
                           true);
}

std::tuple<int, int, int, int, int, bool>
PerformanceImplicitGemmWrwV4R4Xdlops_Padded_Gemm::CalculateGemmBBlockCopyPerformanceParameters(
    const ConvolutionContext& ctx) const
{
    // B tensor shape [GemmG, GemmK, GemmN, GemmKPack]
    // vector load should GemmKPack or GemmK
    int ClusterLengths_GemmK     = -1;
    int ClusterLengths_GemmN     = -1;
    int ClusterLengths_GemmKPack = -1;

    int SrcDataPerRead_GemmKPack = ctx.IsFp32() ? amd_buffer_load_max_length<float>()
                                                : amd_buffer_load_max_length<half_float::half>();
    int DstDataPerWrite_GemmKPack = ctx.IsFp32() ? amd_lds_write_max_length<float>()
                                                 : amd_lds_write_max_length<half_float::half>();

    try
    {
        bool valid = false;

        int block_size = -1;

        std::tie(block_size, valid) = CalculateBlockSize();

        if(!valid)
            MIOPEN_THROW("invalid performance parameter");

        // GemmN is src vector read dimension
        // calculate vector length on gemmn dimension based on global tensor layout
        const auto y  = ConvolutionContextInterpreter::GetFilterHeightY(ctx);
        const auto x  = ConvolutionContextInterpreter::GetFilterWidthX(ctx);
        const auto ho = ConvolutionContextInterpreter::GetOutputHeightHo(ctx);
        const auto wo = ConvolutionContextInterpreter::GetOutputWidthWo(ctx);
        const auto conv_stride_h =
            ConvolutionContextInterpreter::GetAdjustedConvolutionStrideH(ctx);
        const auto conv_stride_w =
            ConvolutionContextInterpreter::GetAdjustedConvolutionStrideW(ctx);
        const auto conv_dilation_w =
            ConvolutionContextInterpreter::GetAdjustedConvolutionDilationW(ctx);
        const auto in_left_pad_h  = ConvolutionContextInterpreter::GetInputLeftPadH(ctx);
        const auto in_left_pad_w  = ConvolutionContextInterpreter::GetInputLeftPadW(ctx);
        const auto in_right_pad_h = ConvolutionContextInterpreter::GetAdjustedInputRightPadH(ctx);
        const auto in_right_pad_w = ConvolutionContextInterpreter::GetAdjustedInputRightPadW(ctx);

        // GemmKPack is src vector read dimension, bounded by input tensor global memory layout
        // TODO this logic need to be more aggresive
        if(y == 1 && x == 1 && conv_stride_h == 1 && conv_stride_w == 1 && in_left_pad_h == 0 &&
           in_left_pad_w == 0 && in_right_pad_h == 0 && in_right_pad_w == 0)
        {
            SrcDataPerRead_GemmKPack = gcd(SrcDataPerRead_GemmKPack, ho * wo);
        }
        else if(conv_stride_w == 1 && in_left_pad_w == 0 && in_right_pad_w == 0)
        {
            SrcDataPerRead_GemmKPack = gcd(SrcDataPerRead_GemmKPack, wo);
        }
        else if(conv_stride_w == 1)
        {
            SrcDataPerRead_GemmKPack =
                gcd(SrcDataPerRead_GemmKPack, wo, in_left_pad_w, in_right_pad_w, conv_dilation_w);
        }
        else
        {
            SrcDataPerRead_GemmKPack = 1;
        }

        // SrcDataPerRead_GemmKPack also bounded by GemmKPack
        SrcDataPerRead_GemmKPack = gcd(SrcDataPerRead_GemmKPack, GemmKPack);

        // calculate threadwise copy size
        auto data_per_thread_copy =
            std::max(1, (GemmKPerBlock * GemmNPerBlock * GemmKPack) / block_size);

        // make sure a thread can do a full vector load, at the cost that some threads
        // may not do threadwise copy at all
        data_per_thread_copy = lcm(data_per_thread_copy, SrcDataPerRead_GemmKPack);

        const auto data_per_thread_copy_gemmkpack = SrcDataPerRead_GemmKPack;
        const auto tmp = data_per_thread_copy / data_per_thread_copy_gemmkpack;

        int data_per_thread_copy_gemmn = -1;
        int data_per_thread_copy_gemmk = -1;

        if(GemmBThreadCopyMoreGemmK)
        {
            data_per_thread_copy_gemmk = gcd(GemmNPerBlock, tmp);
            data_per_thread_copy_gemmn = tmp / data_per_thread_copy_gemmk;
        }
        else
        {
            data_per_thread_copy_gemmn = gcd(GemmKPerBlock, tmp);
            data_per_thread_copy_gemmk = tmp / data_per_thread_copy_gemmn;
        }

        if(data_per_thread_copy_gemmk <= 0 || data_per_thread_copy_gemmn <= 0 ||
           data_per_thread_copy_gemmkpack <= 0)
            MIOPEN_THROW("invalid performance parameter");

        // vector write into LDS
        DstDataPerWrite_GemmKPack = gcd(DstDataPerWrite_GemmKPack, data_per_thread_copy_gemmkpack);

        if(!(GemmKPerBlock % data_per_thread_copy_gemmk == 0 &&
             GemmNPerBlock % data_per_thread_copy_gemmn == 0 &&
             GemmKPack % data_per_thread_copy_gemmkpack == 0))
            MIOPEN_THROW("invalid performance parameter");

        ClusterLengths_GemmK     = GemmKPerBlock / data_per_thread_copy_gemmk;
        ClusterLengths_GemmN     = GemmNPerBlock / data_per_thread_copy_gemmn;
        ClusterLengths_GemmKPack = GemmKPack / data_per_thread_copy_gemmkpack;

        if(ClusterLengths_GemmK < 0 || ClusterLengths_GemmN < 0 || ClusterLengths_GemmKPack < 0)
            MIOPEN_THROW("invalid performance parameter");

        // blockwise-copy support that block_size is larger than thread cluster size, which means
        // some threads may not do threadwise copy
        if(block_size < ClusterLengths_GemmK * ClusterLengths_GemmN * ClusterLengths_GemmKPack)
            MIOPEN_THROW("invalid performance parameter");
    }
    catch(...)
    {
        return std::make_tuple(-1, -1, -1, -1, -1, false);
    }

    return std::make_tuple(ClusterLengths_GemmK,
                           ClusterLengths_GemmN,
                           ClusterLengths_GemmKPack,
                           SrcDataPerRead_GemmKPack,
                           DstDataPerWrite_GemmKPack,
                           true);
}

std::tuple<std::size_t, bool>
PerformanceImplicitGemmWrwV4R4Xdlops_Padded_Gemm::CalculateLdsNumberOfByte(
    const ConvolutionContext& ctx) const
{
    const auto a_block_space = GemmKPerBlock * GemmMPerBlock * GemmKPack;
    const auto b_block_space = GemmKPerBlock * GemmNPerBlock * GemmKPack;

    std::size_t lds_size =
        (a_block_space + b_block_space) * (ctx.IsFp32() ? sizeof(float) : sizeof(half_float::half));

    return std::make_tuple(lds_size, true);
}

// Used by IsReallyValid()
bool PerformanceImplicitGemmWrwV4R4Xdlops_Padded_Gemm::IsValidValue() const
{
    // clang-format off
    return IsTwoPower<4, 256>(GemmMPerBlock)
        && IsTwoPower<4, 256>(GemmNPerBlock)
        && IsTwoPower<1, 8>(GemmKPerBlock)
        && IsTwoPower<4, 128>(GemmMPerWave)
        && IsTwoPower<4, 128>(GemmNPerWave)
        && IsTwoPower<1, 8>(GemmKPack);
    // clang-format on
}

// Used by HeuristicInit() and GenericSearch
// Only return false if a performance config will violate requirements given by kernel algorithm
bool PerformanceImplicitGemmWrwV4R4Xdlops_Padded_Gemm::IsReallyValid(
    const ConvolutionContext& ctx) const
{
    if(!IsValidValue())
        return false;

    if(!IsValidBlockwiseGemmXdlops(
           ctx, GemmMPerBlock, GemmNPerBlock, GemmKPerBlock, GemmMPerWave, GemmNPerWave, GemmKPack))
        return false;

    bool valid = false;
    // check blockwise GEMM size
    {
        int gemm_m       = -1;
        int gemm_n       = -1;
        int gemm_k_total = -1;

        std::tie(std::ignore,
                 gemm_m,
                 gemm_n,
                 gemm_k_total,
                 std::ignore,
                 std::ignore,
                 std::ignore,
                 std::ignore,
                 valid) = CalculateGemmSizeAndGemmKBlock(ctx);

        if(!valid)
            return false;

        if(gemm_k_total % GemmKPack != 0)
            return false;

        const auto gemm_k = gemm_k_total / GemmKPack;

        if(!(gemm_m % GemmMPerBlock == 0 && gemm_n % GemmNPerBlock == 0 &&
             gemm_k % GemmKPerBlock == 0))
            return false;
    }

    // check blockwise copy of A matrix
    {
        std::tie(std::ignore, std::ignore, std::ignore, std::ignore, std::ignore, valid) =
            CalculateGemmABlockCopyPerformanceParameters(ctx);

        if(!valid)
            return false;
    }

    // check blockwise copy of B matrix
    {
        std::tie(std::ignore, std::ignore, std::ignore, std::ignore, std::ignore, valid) =
            CalculateGemmBBlockCopyPerformanceParameters(ctx);

        if(!valid)
            return false;
    }

    // check LDS allocation
    std::size_t lds_size      = 0;
    std::tie(lds_size, valid) = CalculateLdsNumberOfByte(ctx);

    return (valid and lds_size <= get_lds_max_number_of_byte());
}

// Used by GenericSearch, not used by HeuristicInit
// Return false if a performance config is known to be sub-optimal, comparing to other performance
// config inside tuning range
bool PerformanceImplicitGemmWrwV4R4Xdlops_Padded_Gemm::IsFastToBeUsedForTuning(
    const ConvolutionContext& ctx) const
{
    // somehow, 128x128 wave-wise GEMM tend to spill register
    // TODO revisit this when 128x128 wave-wise GEMM become efficient
    {
        if(GemmMPerWave * GemmNPerWave > 64 * 128)
            return false;
    }

    // don't need too many blocks
    {
        int gemm_m = 0;
        int gemm_n = 0;

        std::tie(std::ignore,
                 gemm_m,
                 gemm_n,
                 std::ignore,
                 std::ignore,
                 std::ignore,
                 std::ignore,
                 std::ignore,
                 std::ignore) = CalculateGemmSizeAndGemmKBlock(ctx);

        // this is grid size using current blockwise-GEMM
        const int grid_size = (gemm_m * gemm_n) / (GemmMPerBlock * GemmNPerBlock);

        // this the the biggest blockwise-GEMM you can do
        int max_blockwise_gemm_size =
            std::max(gcd(256, gemm_m) * gcd(128, gemm_n), gcd(128, gemm_m) * gcd(256, gemm_n));

        // this is the grid size using the biggest blockwise-GEMM
        auto grid_size_max_blockwise_gemm =
            (std::size_t(gemm_m) * gemm_n) / max_blockwise_gemm_size;

        const float ratio = float(grid_size) / grid_size_max_blockwise_gemm;

        const auto num_cu = ctx.GetStream().GetMaxComputeUnits();

        // heuristic to exclude performance paramater that result in very large number of blocks
        if(grid_size_max_blockwise_gemm > 5 * num_cu)
        {
            if(ratio > 2.81)
                return false;
        }
        else if(grid_size_max_blockwise_gemm > 4 * num_cu)
        {
            if(ratio > 3.61)
                return false;
        }
        else if(grid_size_max_blockwise_gemm > 3 * num_cu)
        {
            if(ratio > 4.41)
                return false;
        }
        else if(grid_size_max_blockwise_gemm > 2 * num_cu)
        {
            if(ratio > 6.41)
                return false;
        }
        else if(grid_size_max_blockwise_gemm > num_cu)
        {
            if(ratio > 12.41)
                return false;
        }
    }

    // don't need too many waves per block
    {
        const int wave_per_block = (GemmMPerBlock / GemmMPerWave) * (GemmNPerBlock / GemmNPerWave);

        if(!(wave_per_block > 1 && wave_per_block <= 4))
        {
            return false;
        }
    }

    // avoid skinny blockwise GEMM whenever possible
    {
        int gemm_m = 0;
        int gemm_n = 0;

        std::tie(std::ignore,
                 gemm_m,
                 gemm_n,
                 std::ignore,
                 std::ignore,
                 std::ignore,
                 std::ignore,
                 std::ignore,
                 std::ignore) = CalculateGemmSizeAndGemmKBlock(ctx);

        if(GemmMPerBlock > 2 * GemmNPerBlock)
        {
            if(gemm_n % (2 * GemmNPerBlock) == 0)
                return false;
        }

        if(GemmNPerBlock > 2 * GemmMPerBlock)
        {
            if(gemm_m % (2 * GemmMPerBlock) == 0)
                return false;
        }
    }

    // avoid skinny wavewise GEMM whenever possible
    {
        if(GemmMPerWave > 2 * GemmNPerWave)
        {
            if(GemmNPerBlock % (2 * GemmNPerWave) == 0)
                return false;
        }

        if(GemmNPerWave > 2 * GemmMPerWave)
        {
            if(GemmMPerBlock % (2 * GemmMPerWave) == 0)
                return false;
        }
    }

    // each thread should not too much data
    {
        const int block_size = (GemmMPerBlock / GemmMPerWave) * (GemmNPerBlock / GemmNPerWave) * 64;

        const int a_data_per_thread_copy = (GemmKPerBlock * GemmMPerBlock * GemmKPack) / block_size;
        const int b_data_per_thread_copy = (GemmKPerBlock * GemmNPerBlock * GemmKPack) / block_size;
        // << "block_size: " << block_size << std::endl;
        if(ctx.IsFp32())
        {
            if(a_data_per_thread_copy > 16 || b_data_per_thread_copy > 16)
                return false;
        }
        else if(ctx.IsFp16() || ctx.IsBfp16())
        {
            if(a_data_per_thread_copy > 32 || b_data_per_thread_copy > 32)
                return false;
        }
    }

    // GemmKPerBlock*GemmKPack should not be too small, otherwise read performance of A matrix would
    // be bad
    {
        if(ctx.IsFp32())
        {
            if(GemmKPack > 4)
                return false;

            if(GemmKPerBlock * GemmKPack < 8)
                return false;
        }
        else if(ctx.IsFp16() || ctx.IsBfp16())
        {
            if(GemmKPerBlock * GemmKPack < 16)
                return false;
        }
    }

    return true;
}

// Used by GenericSearch, not used by HeuristicInit
// Return false, if you don't want to this to be included in tuning range used by generic search
// A performance config may still be valid w.r.t algorithm correctness, even when IsValid() return
// false
bool PerformanceImplicitGemmWrwV4R4Xdlops_Padded_Gemm::IsValid(const ConvolutionContext& ctx) const
{
    return IsReallyValid(ctx) && IsFastToBeUsedForTuning(ctx);
}

// Used by GenericSearch, not used by HeuristicInit
bool ConvHipImplicitGemmWrwV4R4Xdlops_Padded_Gemm::IsValidPerformanceConfig(
    const ConvolutionContext& ctx, const PerformanceImplicitGemmWrwV4R4Xdlops_Padded_Gemm& c) const
{
    return c.IsReallyValid(ctx);
}

std::tuple<int, int, int, int, int, int, int, int, bool>
PerformanceImplicitGemmWrwV4R4Xdlops_Padded_Gemm::CalculateGemmSizeAndGemmKBlock(
    const ConvolutionContext& ctx) const
{
    int gemm_g           = -1;
    int gemm_m           = -1;
    int gemm_n           = -1;
    int gemm_k_total     = -1;
    int gemm_k_block     = -1;
    int gemm_m_pad       = -1;
    int gemm_n_pad       = -1;
    int gemm_k_total_pad = -1;

    try
    {
        const auto g  = ConvolutionContextInterpreter::GetGroupCountG(ctx);
        const auto n  = ConvolutionContextInterpreter::GetBatchN(ctx);
        const auto k  = ConvolutionContextInterpreter::GetOutputChannelK(ctx);
        const auto c  = ConvolutionContextInterpreter::GetInputChannelC(ctx);
        const auto ho = ConvolutionContextInterpreter::GetOutputHeightHo(ctx);
        const auto wo = ConvolutionContextInterpreter::GetOutputWidthWo(ctx);
        const auto y  = ConvolutionContextInterpreter::GetFilterHeightY(ctx);
        const auto x  = ConvolutionContextInterpreter::GetFilterWidthX(ctx);

        const auto k_per_group = k / g;
        const auto c_per_group = c / g;

        const auto gemm_m_no_pad = k_per_group;
        const auto gemm_n_no_pad = c_per_group * y * x;

        // pad gemm_m and gemm_n
        gemm_m = ((gemm_m_no_pad - 1) / GemmMFactor + 1) * GemmMFactor;
        gemm_n = ((gemm_n_no_pad - 1) / GemmNFactor + 1) * GemmNFactor;

        gemm_m_pad = gemm_m - gemm_m_no_pad;
        gemm_n_pad = gemm_n - gemm_n_no_pad;

        if(!(gemm_m % GemmMPerBlock == 0 && gemm_n % GemmNPerBlock == 0))
            MIOPEN_THROW("invalid performance parameter");

        int grid_size_without_split_gemmk = g * (gemm_m / GemmMPerBlock) * (gemm_n / GemmNPerBlock);

        const int max_grid_size = 20 * static_cast<int>(ctx.GetStream().GetMaxComputeUnits());

        // calculate gemm_k_block
        gemm_k_block = std::max(max_grid_size / grid_size_without_split_gemmk, 1);
        gemm_k_block = std::min(gemm_k_block, n);

        for(; gemm_k_block > 1; gemm_k_block--)
        {
            if(n % gemm_k_block != 0)
                continue;

            const auto n_sub = n / gemm_k_block;

            const auto gemm_k_total_no_pad = n_sub * ho * wo;

            // pad gemm_k_total
            gemm_k_total = ((gemm_k_total_no_pad - 1) / GemmKTotalFactor + 1) * GemmKTotalFactor;

            if(!(gemm_k_total % (GemmKPerBlock * GemmKPack) == 0))
                continue;

            break;
        }

        // in case gemm_k_block <= 1, do calculate again
        gemm_k_block = std::max(1, gemm_k_block);

        const auto n_sub = n / gemm_k_block;

        const auto gemm_k_total_no_pad = n_sub * ho * wo;

        gemm_k_total = ((gemm_k_total_no_pad - 1) / GemmKTotalFactor + 1) * GemmKTotalFactor;

        // gemm_k_total_pad
        gemm_k_total_pad = gemm_k_total - gemm_k_total_no_pad;

        // gemm_g
        gemm_g = g * gemm_k_block;
    }
    catch(...)
    {
        return std::make_tuple(-1, -1, -1, -1, -1, -1, -1, -1, false);
    }

    return std::make_tuple(gemm_g,
                           gemm_m,
                           gemm_n,
                           gemm_k_total,
                           gemm_k_block,
                           gemm_m_pad,
                           gemm_n_pad,
                           gemm_k_total_pad,
                           true);
}

PerformanceImplicitGemmWrwV4R4Xdlops_Padded_Gemm
ConvHipImplicitGemmWrwV4R4Xdlops_Padded_Gemm::GetPerformanceConfig(
    const ConvolutionContext& ctx) const
{
    PerformanceImplicitGemmWrwV4R4Xdlops_Padded_Gemm config;
    config.HeuristicInit(ctx);
    MIOPEN_LOG_I(config.ToString());
    return config;
}

ConvSolution ConvHipImplicitGemmWrwV4R4Xdlops_Padded_Gemm::GetSolution(
    const ConvolutionContext& ctx,
    const PerformanceImplicitGemmWrwV4R4Xdlops_Padded_Gemm& config,
    bool) const
{
    ConvSolution result;

    if(!config.IsReallyValid(ctx))
    {
        MIOPEN_LOG_E("invalid performance parameter");
        assert(false);
    }

    KernelInfo construction_parameters;

    // clang-format off
    construction_parameters.kernel_file =
        "static_kernel_gridwise_convolution_backward_weights_implicit_gemm_v4r4_xdlops_nchw_kcyx_nkhw_padded_gemm.cpp";

    construction_parameters.kernel_name =
        "gridwise_convolution_backward_weights_implicit_gemm_v4r4_xdlops_nchw_kcyx_nkhw_padded_gemm";
    // clang-format on

    int grid_size                     = 0;
    int block_size                    = 0;
    std::tie(grid_size, std::ignore)  = config.CalculateGridSize(ctx);
    std::tie(block_size, std::ignore) = config.CalculateBlockSize();

    construction_parameters.l_wk.push_back(block_size);
    construction_parameters.l_wk.push_back(1);
    construction_parameters.l_wk.push_back(1);

    construction_parameters.g_wk.push_back(block_size * grid_size);
    construction_parameters.g_wk.push_back(1);
    construction_parameters.g_wk.push_back(1);

    int GemmKBlock    = 0;
    int GemmMPad      = 0;
    int GemmNPad      = 0;
    int GemmKTotalPad = 0;

    std::tie(std::ignore,
             std::ignore,
             std::ignore,
             std::ignore,
             GemmKBlock,
             GemmMPad,
             GemmNPad,
             GemmKTotalPad,
             std::ignore) = config.CalculateGemmSizeAndGemmKBlock(ctx);

    int GemmABlockCopyClusterLengths_GemmK      = -1;
    int GemmABlockCopyClusterLengths_GemmM      = -1;
    int GemmABlockCopyClusterLengths_GemmKPack  = -1;
    int GemmABlockCopySrcDataPerRead_GemmKPack  = -1;
    int GemmABlockCopyDstDataPerWrite_GemmKPack = -1;

    int GemmBBlockCopyClusterLengths_GemmK      = -1;
    int GemmBBlockCopyClusterLengths_GemmN      = -1;
    int GemmBBlockCopyClusterLengths_GemmKPack  = -1;
    int GemmBBlockCopySrcDataPerRead_GemmKPack  = -1;
    int GemmBBlockCopyDstDataPerWrite_GemmKPack = -1;

    std::tie(GemmABlockCopyClusterLengths_GemmK,
             GemmABlockCopyClusterLengths_GemmM,
             GemmABlockCopyClusterLengths_GemmKPack,
             GemmABlockCopySrcDataPerRead_GemmKPack,
             GemmABlockCopyDstDataPerWrite_GemmKPack,
             std::ignore) = config.CalculateGemmABlockCopyPerformanceParameters(ctx);

    std::tie(GemmBBlockCopyClusterLengths_GemmK,
             GemmBBlockCopyClusterLengths_GemmN,
             GemmBBlockCopyClusterLengths_GemmKPack,
             GemmBBlockCopySrcDataPerRead_GemmKPack,
             GemmBBlockCopyDstDataPerWrite_GemmKPack,
             std::ignore) = config.CalculateGemmBBlockCopyPerformanceParameters(ctx);

    // clang-format off
    construction_parameters.comp_options =
        std::string(" -DCK_PARAM_PROBLEM_G=") + std::to_string(ConvolutionContextInterpreter::GetGroupCountG(ctx)) +
        std::string(" -DCK_PARAM_PROBLEM_N=") + std::to_string(ConvolutionContextInterpreter::GetBatchN(ctx)) +
        std::string(" -DCK_PARAM_PROBLEM_K=") + std::to_string(ConvolutionContextInterpreter::GetOutputChannelK(ctx)) +
        std::string(" -DCK_PARAM_PROBLEM_C=") + std::to_string(ConvolutionContextInterpreter::GetInputChannelC(ctx)) +
        std::string(" -DCK_PARAM_PROBLEM_HI=") + std::to_string(ConvolutionContextInterpreter::GetInputHeightHi(ctx)) +
        std::string(" -DCK_PARAM_PROBLEM_WI=") + std::to_string(ConvolutionContextInterpreter::GetInputWidthWi(ctx)) +
        std::string(" -DCK_PARAM_PROBLEM_HO=") + std::to_string(ConvolutionContextInterpreter::GetOutputHeightHo(ctx)) +
        std::string(" -DCK_PARAM_PROBLEM_WO=") + std::to_string(ConvolutionContextInterpreter::GetOutputWidthWo(ctx)) +
        std::string(" -DCK_PARAM_PROBLEM_Y=") + std::to_string(ConvolutionContextInterpreter::GetFilterHeightY(ctx)) +
        std::string(" -DCK_PARAM_PROBLEM_X=") + std::to_string(ConvolutionContextInterpreter::GetFilterWidthX(ctx)) +
        std::string(" -DCK_PARAM_PROBLEM_CONV_STRIDE_H=") + std::to_string(ConvolutionContextInterpreter::GetAdjustedConvolutionStrideH(ctx)) +
        std::string(" -DCK_PARAM_PROBLEM_CONV_STRIDE_W=") + std::to_string(ConvolutionContextInterpreter::GetAdjustedConvolutionStrideW(ctx)) +
        std::string(" -DCK_PARAM_PROBLEM_CONV_DILATION_H=") + std::to_string(ConvolutionContextInterpreter::GetAdjustedConvolutionDilationH(ctx)) +
        std::string(" -DCK_PARAM_PROBLEM_CONV_DILATION_W=") + std::to_string(ConvolutionContextInterpreter::GetAdjustedConvolutionDilationW(ctx)) +
        std::string(" -DCK_PARAM_PROBLEM_IN_LEFT_PAD_H=") + std::to_string(ConvolutionContextInterpreter::GetInputLeftPadH(ctx)) +
        std::string(" -DCK_PARAM_PROBLEM_IN_LEFT_PAD_W=") + std::to_string(ConvolutionContextInterpreter::GetInputLeftPadW(ctx)) +
        std::string(" -DCK_PARAM_PROBLEM_IN_RIGHT_PAD_H=") + std::to_string(ConvolutionContextInterpreter::GetAdjustedInputRightPadH(ctx)) +
        std::string(" -DCK_PARAM_PROBLEM_IN_RIGHT_PAD_W=") + std::to_string(ConvolutionContextInterpreter::GetAdjustedInputRightPadW(ctx)) +
        std::string(" -DCK_PARAM_TUNABLE_GEMM_M_PER_BLOCK=") + std::to_string(config.GemmMPerBlock) +
        std::string(" -DCK_PARAM_TUNABLE_GEMM_N_PER_BLOCK=") + std::to_string(config.GemmNPerBlock) +
        std::string(" -DCK_PARAM_TUNABLE_GEMM_K_PER_BLOCK=") + std::to_string(config.GemmKPerBlock) +
        std::string(" -DCK_PARAM_TUNABLE_GEMM_M_PER_WAVE=") + std::to_string(config.GemmMPerWave) +
        std::string(" -DCK_PARAM_TUNABLE_GEMM_N_PER_WAVE=") + std::to_string(config.GemmNPerWave) +
        std::string(" -DCK_PARAM_TUNABLE_GEMM_KPACK=") + std::to_string(config.GemmKPack) +
        std::string(" -DCK_PARAM_GEMM_K_BLOCK=") + std::to_string(GemmKBlock) +
        std::string(" -DCK_PARAM_DEPENDENT_BLOCK_SIZE=") + std::to_string(block_size) +
        std::string(" -DCK_PARAM_DEPENDENT_GRID_SIZE=") + std::to_string(grid_size) +
        std::string(" -DCK_PARAM_DEPENDENT_GEMM_A_BLOCK_COPY_CLUSTER_LENGTHS_GEMM_K=") + std::to_string(GemmABlockCopyClusterLengths_GemmK) +
        std::string(" -DCK_PARAM_DEPENDENT_GEMM_A_BLOCK_COPY_CLUSTER_LENGTHS_GEMM_M=") + std::to_string(GemmABlockCopyClusterLengths_GemmM) +
        std::string(" -DCK_PARAM_DEPENDENT_GEMM_A_BLOCK_COPY_CLUSTER_LENGTHS_GEMM_KPACK=") + std::to_string(GemmABlockCopyClusterLengths_GemmKPack) +
        std::string(" -DCK_PARAM_DEPENDENT_GEMM_A_BLOCK_COPY_SRC_DATA_PER_READ_GEMM_KPACK=") + std::to_string(GemmABlockCopySrcDataPerRead_GemmKPack) +
        std::string(" -DCK_PARAM_DEPENDENT_GEMM_A_BLOCK_COPY_DST_DATA_PER_WRITE_GEMM_KPACK=") + std::to_string(GemmABlockCopyDstDataPerWrite_GemmKPack) +
        std::string(" -DCK_PARAM_DEPENDENT_GEMM_B_BLOCK_COPY_CLUSTER_LENGTHS_GEMM_K=") + std::to_string(GemmBBlockCopyClusterLengths_GemmK) +
        std::string(" -DCK_PARAM_DEPENDENT_GEMM_B_BLOCK_COPY_CLUSTER_LENGTHS_GEMM_N=") + std::to_string(GemmBBlockCopyClusterLengths_GemmN) +
        std::string(" -DCK_PARAM_DEPENDENT_GEMM_B_BLOCK_COPY_CLUSTER_LENGTHS_GEMM_KPACK=") + std::to_string(GemmBBlockCopyClusterLengths_GemmKPack) +
        std::string(" -DCK_PARAM_DEPENDENT_GEMM_B_BLOCK_COPY_SRC_DATA_PER_READ_GEMM_KPACK=") + std::to_string(GemmBBlockCopySrcDataPerRead_GemmKPack) +
        std::string(" -DCK_PARAM_DEPENDENT_GEMM_B_BLOCK_COPY_DST_DATA_PER_WRITE_GEMM_KPACK=") + std::to_string(GemmBBlockCopyDstDataPerWrite_GemmKPack) +
        std::string(" -DCK_GEMM_M_PAD=") + std::to_string(GemmMPad) +
        std::string(" -DCK_GEMM_N_PAD=") + std::to_string(GemmNPad) +
        std::string(" -DCK_GEMM_K_TOTAL_PAD=") + std::to_string(GemmKTotalPad) +
        std::string(" -DCK_USE_AMD_XDLOPS=") + std::to_string(IsXdlopsSupport(ctx) ? 1 : 0) +
        std::string(" -DCK_USE_AMD_XDLOPS_INLINE_ASM=") + std::to_string(miopen::IsEnabled(MIOPEN_DEBUG_IMPLICIT_GEMM_XDLOPS_INLINE_ASM{}) ? 1 : 0) +
        std::string(" -DCK_USE_AMD_XDLOPS_EMULATE=") + (miopen::IsEnabled(MIOPEN_DEBUG_CONV_IMPLICIT_GEMM_XDLOPS_EMULATE{}) ? '1' : '0') +
        get_static_ck_common_compiler_flag(ctx) +
        ctx.general_compile_options;
    // clang-format on

    result.construction_params.push_back(construction_parameters);

    const auto& conv       = ctx.conv_problem.GetConv();
    const auto& lowp_quant = conv.lowp_quant;
    result.invoker_factory = [=](const std::vector<Kernel>& kernels) {
        return [=](const Handle& handle, const AnyInvokeParams& primitive_params) {
            const auto& invoke_params = primitive_params.CastTo<conv::WrWInvokeParams>();
            const auto& tensors       = invoke_params.tensors;
            auto kernel               = handle.Run(kernels[0]);
            float elapsed             = 0;
            float zero                = 0.f;
            if(ctx.IsFp16() || ctx.IsBfp16())
            {
                const auto& workSpace = invoke_params.workSpace;
                TensorDescriptor workspaceDesc(
                    miopenFloat, tensors.dwDesc.GetLengths(), tensors.dwDesc.GetStrides());
                SetTensor(handle, workspaceDesc, workSpace, &zero);
                if(handle.IsProfilingEnabled())
                {
                    elapsed += handle.GetKernelTime();
                }
                kernel(tensors.x, tensors.dy, workSpace);
                if(handle.IsProfilingEnabled())
                {
                    elapsed += handle.GetKernelTime();
                }
                CastTensor(handle,
                           &lowp_quant,
                           workspaceDesc,
                           workSpace,
                           tensors.dwDesc,
                           tensors.dw,
                           0,
                           0);
            }
            else
            {
                SetTensor(handle, tensors.dwDesc, tensors.dw, &zero);
                if(handle.IsProfilingEnabled())
                {
                    elapsed += handle.GetKernelTime();
                }
                handle.Run(kernels[0])(tensors.x, tensors.dy, tensors.dw);
            }

            if(handle.IsProfilingEnabled())
            {
                elapsed += handle.GetKernelTime();
                handle.ResetKernelTime();
                handle.AccumKernelTime(elapsed);
            }
        };
    };
    result.workspce_sz = GetWorkspaceSize(ctx);
    return result;
}

bool ConvHipImplicitGemmWrwV4R4Xdlops_Padded_Gemm::IsApplicable(const ConvolutionContext& ctx) const
{
    return false; // disable XDLOPS
    if(miopen::IsDisabled(MIOPEN_DEBUG_CONV_IMPLICIT_GEMM_HIP_WRW_V4R4_PADDED_GEMM_XDLOPS{}))
        return false;

    if(ctx.skip_solutions_that_take_long_time_to_build_and_have_narrow_coverage)
        return false;

    if(!IsComposableKernelSupportedHardware(ctx))
        return false;

    if(!IsXdlopsSupport(ctx))
        return false;

    if(!ctx.use_hip_kernels)
        return false;

    if(!(ctx.IsFp32() || ctx.IsFp16() || ctx.IsBfp16()))
        return false;

    if(!ctx.direction.IsBackwardWrW())
        return false;

    if(!ctx.Is2d())
        return false;

    if(!IsIndexRangeLargeEnough(ctx))
        return false;

    if(!ctx.IsLayoutDefault())
    {
        return false;
    }

// this particular HeuristicInit is so comprehensive, that if it cannot predict a valid
#if WORKAROUND_MI100_BF16_FATAL_COMPILER_ERRORS
    if(ctx.GetStream().GetDeviceName() == "gfx908" && ctx.IsBfp16())
        return false;
#endif

    // this particular EuristicInit is so comprehensive, that if it cannot predict a valid
    // performance config, the problem is probably not applicable
    PerformanceImplicitGemmWrwV4R4Xdlops_Padded_Gemm config;
    config.HeuristicInit(ctx);

    if(!config.IsReallyValid(ctx))
        return false;

    // gemm size
    int gemm_g           = -1;
    int gemm_m           = -1;
    int gemm_n           = -1;
    int gemm_k_total     = -1;
    int gemm_m_pad       = -1;
    int gemm_n_pad       = -1;
    int gemm_k_total_pad = -1;

    std::tie(gemm_g,
             gemm_m,
             gemm_n,
             gemm_k_total,
             std::ignore,
             gemm_m_pad,
             gemm_n_pad,
             gemm_k_total_pad,
             std::ignore) = config.CalculateGemmSizeAndGemmKBlock(ctx);

    // hack: make itself not applicable if padding is not needed, and fall back to other solver
    // (likely ConvHipImplicitGemmWrwV4R4Xdlops)
    if(gemm_m_pad == 0 && gemm_n_pad == 0 && gemm_k_total_pad == 0)
        return false;

    return IsValidGridGemmXdlops(gemm_m, gemm_n, gemm_k_total);
}

PerformanceImplicitGemmWrwV4R4Xdlops_Padded_Gemm
ConvHipImplicitGemmWrwV4R4Xdlops_Padded_Gemm::Search(const ConvolutionContext& ctx,
                                                     const AnyInvokeParams& invoke_ctx) const
{
    // fp16/bfp16 uses fp32 workspace to leverage fp32 atomic add
    return GenericSearch(*this, ctx, invoke_ctx);
}

std::size_t
ConvHipImplicitGemmWrwV4R4Xdlops_Padded_Gemm::GetWorkspaceSize(const ConvolutionContext& ctx) const
{
    if(ctx.IsFp32())
        return 0;
    else
    {
        const auto k = ConvolutionContextInterpreter::GetOutputChannelK(ctx);
        const auto c = ConvolutionContextInterpreter::GetInputChannelC(ctx);
        const auto y = ConvolutionContextInterpreter::GetFilterHeightY(ctx);
        const auto x = ConvolutionContextInterpreter::GetFilterWidthX(ctx);

        return k * c * y * x * miopen::GetTypeSize(miopenFloat);
    }
}
} // namespace solver
} // namespace miopen
