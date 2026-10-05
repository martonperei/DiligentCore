/*
 *  Copyright 2019-2022 Diligent Graphics LLC
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

#pragma once

/// \file
/// Declaration of Diligent::PipelineStateCacheD3D12Impl class

#include <condition_variable>
#include <mutex>
#include <set>
#include <string>
#include <unordered_set>

#include "EngineD3D12ImplTraits.hpp"
#include "PipelineStateCacheBase.hpp"

namespace Diligent
{

/// Pipeline state cache implementation in Direct3D12 backend.
///
/// The cache keeps each pipeline in a D3D12 pipeline library under a key made of the pipeline's
/// name and a hash of its description's content, because the library finds a pipeline by its key
/// alone, and two pipelines may share a name, or a pipeline may keep its name while its shaders
/// change. The cache's data holds the library's keys beside the library, and a checksum over both.
class PipelineStateCacheD3D12Impl final : public PipelineStateCacheBase<EngineD3D12ImplTraits>
{
public:
    using TPipelineStateCacheBase = PipelineStateCacheBase<EngineD3D12ImplTraits>;

    PipelineStateCacheD3D12Impl(IReferenceCounters*                 pRefCounters,
                                RenderDeviceD3D12Impl*              pDevice,
                                const PipelineStateCacheCreateInfo& CreateInfo);
    ~PipelineStateCacheD3D12Impl();

    IMPLEMENT_QUERY_INTERFACE_IN_PLACE(IID_PipelineStateCacheD3D12, TPipelineStateCacheBase)

    /// Implementation of IPipelineStateCache::GetData().
    virtual void DILIGENT_CALL_TYPE GetData(IDataBlob** ppBlob) override final;

    /// Returns the key the cache keeps a pipeline under: its name and a hash of its description,
    /// with the hash of its root signature's content.
    static std::wstring GetPipelineKey(const std::wstring& Name, const D3D12_COMPUTE_PIPELINE_STATE_DESC& Desc, Uint64 RootSignatureHash);
    static std::wstring GetPipelineKey(const std::wstring& Name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& Desc, Uint64 RootSignatureHash);

    CComPtr<ID3D12DeviceChild> LoadComputePipeline(const std::wstring& Key, const D3D12_COMPUTE_PIPELINE_STATE_DESC& Desc);
    CComPtr<ID3D12DeviceChild> LoadGraphicsPipeline(const std::wstring& Key, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& Desc);

    bool StorePipeline(const std::wstring& Key, ID3D12DeviceChild* pPSO);

private:
    void InitFromData(ID3D12Device1* pd3d12Device, const void* pData, size_t Size);

    template <typename LoadPipelineType>
    CComPtr<ID3D12DeviceChild> LoadPipeline(const std::wstring& Key, LoadPipelineType&& Load);

    CComPtr<ID3D12PipelineLibrary> m_pLibrary;

    std::mutex              m_Mtx;
    std::condition_variable m_KeysChanged;

    // The keys the library holds, from the cache's data and from the stores that succeeded.
    std::set<std::wstring> m_Keys;
    // The keys being loaded. The library lets threads load at once, except one pipeline from two threads.
    std::unordered_set<std::wstring> m_KeysBeingLoaded;
    // The keys whose load failed, and those that a store has been attempted for, so that neither
    // is tried, nor reported, twice.
    std::unordered_set<std::wstring> m_FailedKeys;
    std::unordered_set<std::wstring> m_StoredKeys;
    // The number of stores under way, which GetData() waits for so that the keys it writes match
    // the library it serializes.
    Uint32 m_NumStoresInFlight = 0;
};

} // namespace Diligent
