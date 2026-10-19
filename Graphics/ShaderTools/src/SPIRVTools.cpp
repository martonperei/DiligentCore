/*
 *  Copyright 2019-2025 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  In no event and under no legal theory, whether in tort (including negligence),
 *  contract, or otherwise, unless required by applicable law (such as deliberate
 *  and grossly negligent acts) or agreed to in writing, shall any Contributor be
 *  liable for any damages, including any direct, indirect, special, incidental,
 *  or consequential damages of any character arising as a result of this License or
 *  out of the use or inability to use the software (including but not limited to damages
 *  for loss of goodwill, work stoppage, computer failure or malfunction, or any and
 *  all other commercial damages or losses), even if such Contributor has been advised
 *  of the possibility of such damages.
 */

#include "SPIRVTools.hpp"
#include "SPIRVUtils.hpp"
#include "DebugUtilities.hpp"

// Temporarily disable warning C4127: conditional expression is constant
// This warning is triggered by SPIRV-Tools headers in ThirdParty
#ifdef _MSC_VER
#    pragma warning(push)
#    pragma warning(disable : 4127)
#endif

#include "spirv-tools/optimizer.hpp"

// SPIRV-Tools internal headers for custom pass implementation
#include "source/opt/pass.h"
#include "source/opt/ir_context.h"

// Restore warning settings
#ifdef _MSC_VER
#    pragma warning(pop)
#endif

#include <string>
#include <vector>

namespace Diligent
{

namespace
{

// A pass that strips the reflection that DXC's -fspv-reflect adds, as SPIRV-Tools' strip-nonsemantic-info
// pass does: the decorations of SPV_GOOGLE_hlsl_functionality1 and SPV_GOOGLE_user_type, and the extensions
// that declare them. Unlike that pass, it keeps the module's non-semantic instructions, which that pass
// strips as well, so that NonSemantic.Shader.DebugInfo.100 debug information reaches the driver.
class StripReflectionPass final : public spvtools::opt::Pass
{
public:
    static spvtools::Optimizer::PassToken Create()
    {
        return spvtools::Optimizer::PassToken{std::make_unique<StripReflectionPass>()};
    }

    const char* name() const override { return "diligent-strip-reflection"; }

    Status Process() override
    {
        std::vector<spvtools::opt::Instruction*> ToRemove;

        bool OtherUsesForDecorateString = false;
        for (spvtools::opt::Instruction& Inst : context()->module()->annotations())
        {
            switch (Inst.opcode())
            {
                case spv::Op::OpDecorateStringGOOGLE:
                case spv::Op::OpMemberDecorateStringGOOGLE:
                {
                    // The decoration follows the target, and for a member decoration the member's index.
                    const uint32_t        DecorationOperand = Inst.opcode() == spv::Op::OpDecorateStringGOOGLE ? 1 : 2;
                    const spv::Decoration Decoration        = spv::Decoration(Inst.GetSingleWordInOperand(DecorationOperand));
                    if (Decoration == spv::Decoration::HlslSemanticGOOGLE || Decoration == spv::Decoration::UserTypeGOOGLE)
                        ToRemove.push_back(&Inst);
                    else
                        OtherUsesForDecorateString = true;
                    break;
                }

                case spv::Op::OpDecorateId:
                    if (spv::Decoration(Inst.GetSingleWordInOperand(1)) == spv::Decoration::HlslCounterBufferGOOGLE)
                        ToRemove.push_back(&Inst);
                    break;

                default:
                    break;
            }
        }

        for (spvtools::opt::Instruction& Inst : context()->module()->extensions())
        {
            const std::string Extension = Inst.GetInOperand(0).AsString();
            if (Extension == "SPV_GOOGLE_hlsl_functionality1" ||
                Extension == "SPV_GOOGLE_user_type" ||
                (Extension == "SPV_GOOGLE_decorate_string" && !OtherUsesForDecorateString))
            {
                ToRemove.push_back(&Inst);
            }
        }

        for (spvtools::opt::Instruction* Inst : ToRemove)
            context()->KillInst(Inst);

        return ToRemove.empty() ? Status::SuccessWithoutChange : Status::SuccessWithChange;
    }
};

} // namespace

namespace SPIRVToolsInternal
{

void SpvOptimizerMessageConsumer(
    spv_message_level_t level,
    const char* /* source */,
    const spv_position_t& /* position */,
    const char* message)
{
    const char*            LevelText   = "message";
    DEBUG_MESSAGE_SEVERITY MsgSeverity = DEBUG_MESSAGE_SEVERITY_INFO;
    switch (level)
    {
        case SPV_MSG_FATAL:
            // Unrecoverable error due to environment (e.g. out of memory)
            LevelText   = "fatal error";
            MsgSeverity = DEBUG_MESSAGE_SEVERITY_FATAL_ERROR;
            break;

        case SPV_MSG_INTERNAL_ERROR:
            // Unrecoverable error due to SPIRV-Tools internals (e.g. unimplemented feature)
            LevelText   = "internal error";
            MsgSeverity = DEBUG_MESSAGE_SEVERITY_ERROR;
            break;

        case SPV_MSG_ERROR:
            // Normal error due to user input.
            LevelText   = "error";
            MsgSeverity = DEBUG_MESSAGE_SEVERITY_ERROR;
            break;

        case SPV_MSG_WARNING:
            LevelText   = "warning";
            MsgSeverity = DEBUG_MESSAGE_SEVERITY_WARNING;
            break;

        case SPV_MSG_INFO:
            LevelText   = "info";
            MsgSeverity = DEBUG_MESSAGE_SEVERITY_INFO;
            break;

        case SPV_MSG_DEBUG:
            LevelText   = "debug";
            MsgSeverity = DEBUG_MESSAGE_SEVERITY_INFO;
            break;
    }

    if (level == SPV_MSG_FATAL || level == SPV_MSG_INTERNAL_ERROR || level == SPV_MSG_ERROR || level == SPV_MSG_WARNING)
        LOG_DEBUG_MESSAGE(MsgSeverity, "Spirv optimizer ", LevelText, ": ", message);
}

spv_target_env SpvTargetEnvFromSPIRV(const std::vector<uint32_t>& SPIRV)
{
    if (SPIRV.size() < 2)
    {
        // Invalid SPIRV
        return SPV_ENV_VULKAN_1_0;
    }

#define SPV_SPIRV_VERSION_WORD(MAJOR, MINOR) ((uint32_t(uint8_t(MAJOR)) << 16) | (uint32_t(uint8_t(MINOR)) << 8))
    switch (SPIRV[1])
    {
        case SPV_SPIRV_VERSION_WORD(1, 0): return SPV_ENV_VULKAN_1_0;
        case SPV_SPIRV_VERSION_WORD(1, 1): return SPV_ENV_VULKAN_1_0;
        case SPV_SPIRV_VERSION_WORD(1, 2): return SPV_ENV_VULKAN_1_0;
        case SPV_SPIRV_VERSION_WORD(1, 3): return SPV_ENV_VULKAN_1_1;
        case SPV_SPIRV_VERSION_WORD(1, 4): return SPV_ENV_VULKAN_1_1_SPIRV_1_4;
        case SPV_SPIRV_VERSION_WORD(1, 5): return SPV_ENV_VULKAN_1_2;
        case SPV_SPIRV_VERSION_WORD(1, 6): return SPV_ENV_VULKAN_1_3;
        default: return SPV_ENV_VULKAN_1_3;
    }
#undef SPV_SPIRV_VERSION_WORD
}

} // namespace SPIRVToolsInternal

namespace
{

constexpr char DebugInfoSetName[] = "NonSemantic.Shader.DebugInfo.100";

// Tests whether a device takes the SPIR-V extensions that the code's debug information declares, as
// Passes says it does (see SPIRV_OPTIMIZATION_FLAG_KEEP_DEBUG_INFO): SPV_KHR_non_semantic_info, which
// SPIRV_OPTIMIZATION_FLAG_KEEP_DEBUG_INFO itself stands for, and SPV_KHR_relaxed_extended_instruction,
// which debug information declares for OpExtInstWithForwardRefsKHR.
bool DebugInfoExtensionsTaken(const std::vector<uint32_t>& SPIRV, SPIRV_OPTIMIZATION_FLAGS Passes)
{
    if ((Passes & SPIRV_OPTIMIZATION_FLAG_KEEP_DEBUG_INFO) == 0)
        return false;
    return (Passes & SPIRV_OPTIMIZATION_FLAG_KEEP_DEBUG_INFO_FORWARD_REFS) != 0 ||
        !SPIRVDeclaresExtension(SPIRV, "SPV_KHR_relaxed_extended_instruction");
}

std::vector<uint32_t> RunOptimizer(const std::vector<uint32_t>& SrcSPIRV,
                                   int                          TargetEnv,
                                   SPIRV_OPTIMIZATION_FLAGS     Passes,
                                   bool                         HasDebugInfo,
                                   bool                         KeepDebugInfo)
{
    spvtools::Optimizer SpirvOptimizer(static_cast<spv_target_env>(TargetEnv));
    SpirvOptimizer.SetMessageConsumer(SPIRVToolsInternal::SpvOptimizerMessageConsumer);

    spvtools::OptimizerOptions Options;
#ifndef DILIGENT_DEVELOPMENT
    // Do not run validator in release build
    Options.set_run_validator(false);
#endif
    if (Passes & SPIRV_OPTIMIZATION_FLAG_SKIP_VALIDATION)
        Options.set_run_validator(false);

    // SPIR-V bytecode generated from HLSL must be legalized to
    // turn it into a valid vulkan SPIR-V shader.
    if (Passes & SPIRV_OPTIMIZATION_FLAG_LEGALIZATION)
    {
        SpirvOptimizer.RegisterLegalizationPasses();

        spvtools::ValidatorOptions ValidatorOptions;
        ValidatorOptions.SetBeforeHlslLegalization(true);
        Options.set_validator_options(ValidatorOptions);
    }

    if (Passes & SPIRV_OPTIMIZATION_FLAG_PERFORMANCE)
    {
        SpirvOptimizer.RegisterPerformancePasses();
    }

    if (Passes & SPIRV_OPTIMIZATION_FLAG_STRIP_REFLECTION)
    {
        // Decorations defined in SPV_GOOGLE_hlsl_functionality1 are the only instructions
        // removed by strip-reflect-info pass. SPIRV offsets become INVALID after this operation.
        // The pass also removes every non-semantic instruction, which byte code with debug information
        // keeps where the device takes it.
        if (KeepDebugInfo)
        {
            SpirvOptimizer.RegisterPass(StripReflectionPass::Create());
        }
        else
        {
            SpirvOptimizer.RegisterPass(spvtools::CreateStripReflectInfoPass());
            // The pass removes the debug information's OpExtInstWithForwardRefsKHR instructions but leaves
            // the SPV_KHR_relaxed_extended_instruction extension that declares them, which a device without
            // VK_KHR_shader_relaxed_extended_instruction refuses. SPIRV-Tools' fix-opextinst-opcodes pass
            // removes the extension once no instruction needs it.
            if (HasDebugInfo)
                SpirvOptimizer.RegisterPass(spvtools::CreateOpExtInstWithForwardReferenceFixupPass());
        }
    }

    std::vector<uint32_t> OptimizedSPIRV;
    if (!SpirvOptimizer.Run(SrcSPIRV.data(), SrcSPIRV.size(), &OptimizedSPIRV, Options))
        OptimizedSPIRV.clear();

    return OptimizedSPIRV;
}

} // namespace

std::vector<uint32_t> OptimizeSPIRV(const std::vector<uint32_t>& SrcSPIRV, int TargetEnv, SPIRV_OPTIMIZATION_FLAGS Passes)
{
    VERIFY_EXPR(Passes != SPIRV_OPTIMIZATION_FLAG_NONE);

    if (TargetEnv == SPV_ENV_MAX)
        TargetEnv = SPIRVToolsInternal::SpvTargetEnvFromSPIRV(SrcSPIRV);

    // Debug information is stripped with the reflection unless the device takes every extension it
    // declares. The passes before the strip, legalization among them, may add a forward reference, so
    // byte code whose optimized debug information declares an extension the device doesn't take is
    // optimized again without it.
    const bool HasDebugInfo  = (Passes & SPIRV_OPTIMIZATION_FLAG_STRIP_REFLECTION) != 0 && FindSPIRVExtInstImport(SrcSPIRV, DebugInfoSetName) != 0;
    const bool KeepDebugInfo = HasDebugInfo && DebugInfoExtensionsTaken(SrcSPIRV, Passes);

    std::vector<uint32_t> OptimizedSPIRV = RunOptimizer(SrcSPIRV, TargetEnv, Passes, HasDebugInfo, KeepDebugInfo);
    if (KeepDebugInfo && !OptimizedSPIRV.empty() && !DebugInfoExtensionsTaken(OptimizedSPIRV, Passes))
        OptimizedSPIRV = RunOptimizer(SrcSPIRV, TargetEnv, Passes, HasDebugInfo, false);

    return OptimizedSPIRV;
}

std::vector<uint32_t> OptimizeSPIRV(const std::vector<uint32_t>& SrcSPIRV,
                                    SPIRV_OPTIMIZATION_FLAGS     Passes)
{
    return OptimizeSPIRV(SrcSPIRV, SPV_ENV_MAX, Passes);
}

} // namespace Diligent
