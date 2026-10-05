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

#include "pch.h"
#include "PipelineStateCacheD3D12Impl.hpp"

#include <d3dcompiler.h>

#include "RenderDeviceD3D12Impl.hpp"
#include "DataBlobImpl.hpp"
#include "StringTools.hpp"

#include "xxhash.h"

namespace Diligent
{

namespace
{

// The cache's data: this header, then each key as its length in characters and its characters,
// then the D3D12 pipeline library's own data. The checksum covers everything after the header.
struct PipelineStateCacheDataHeader
{
    Uint32 Magic       = 0;
    Uint32 Version     = 0;
    Uint64 Checksum    = 0;
    Uint64 NumKeys     = 0;
    Uint64 KeysSize    = 0;
    Uint64 LibrarySize = 0;
};

constexpr Uint32 PipelineStateCacheDataMagic   = 0x4C505344; // 'DSPL'
constexpr Uint32 PipelineStateCacheDataVersion = 1;

// The library keeps the pointer to its data that it was created with, rather than a copy, so the
// cache gives it a copy of its own, held as the library's private data so that it lives as long as
// the library does.
// {6A0F3C1E-8D42-4B57-9E1A-3C5D7F2B9A64}
constexpr GUID PipelineLibraryDataGUID = {0x6a0f3c1e, 0x8d42, 0x4b57, {0x9e, 0x1a, 0x3c, 0x5d, 0x7f, 0x2b, 0x9a, 0x64}};

// Hashes the content of a D3D12 pipeline description member by member, never its padding, and
// what its pointers point to, never the pointers.
class PipelineDescHasher
{
public:
    PipelineDescHasher() :
        m_pState{XXH3_createState()}
    {
        XXH3_64bits_reset(m_pState);
    }

    ~PipelineDescHasher()
    {
        XXH3_freeState(m_pState);
    }

    // clang-format off
    PipelineDescHasher           (const PipelineDescHasher&) = delete;
    PipelineDescHasher& operator=(const PipelineDescHasher&) = delete;
    // clang-format on

    template <typename T>
    typename std::enable_if<std::is_arithmetic<T>::value || std::is_enum<T>::value>::type Update(const T& Val)
    {
        XXH3_64bits_update(m_pState, &Val, sizeof(Val));
    }

    void Update(const char* Str)
    {
        const Uint64 Length = Str != nullptr ? strlen(Str) : ~Uint64{0};
        Update(Length);
        if (Str != nullptr)
            XXH3_64bits_update(m_pState, Str, static_cast<size_t>(Length));
    }

    void Update(const D3D12_SHADER_BYTECODE& ByteCode)
    {
        Update(Uint64{ByteCode.BytecodeLength});
        if (ByteCode.pShaderBytecode != nullptr && ByteCode.BytecodeLength != 0)
            XXH3_64bits_update(m_pState, ByteCode.pShaderBytecode, ByteCode.BytecodeLength);
    }

    template <typename FirstArgType, typename SecondArgType, typename... RestArgsType>
    void Update(const FirstArgType& FirstArg, const SecondArgType& SecondArg, const RestArgsType&... RestArgs)
    {
        Update(FirstArg);
        Update(SecondArg, RestArgs...);
    }

    Uint64 Digest() const
    {
        return XXH3_64bits_digest(m_pState);
    }

private:
    XXH3_state_t* const m_pState;
};

std::wstring MakePipelineKey(const std::wstring& Name, Uint64 Hash)
{
    static constexpr wchar_t Digits[] = L"0123456789abcdef";

    std::wstring Key = Name + L" [";
    for (int Shift = 60; Shift >= 0; Shift -= 4)
        Key += Digits[(Hash >> Shift) & 0xF];
    Key += L']';
    return Key;
}

const char* GetLibraryRefusalReason(HRESULT hr)
{
    switch (hr)
    {
        case D3D12_ERROR_DRIVER_VERSION_MISMATCH: return "it was written by another driver version";
        case D3D12_ERROR_ADAPTER_NOT_FOUND: return "it was written on another adapter";
        case DXGI_ERROR_UNSUPPORTED: return "this system does not support pipeline libraries";
        default: return "the runtime could not read it";
    }
}

} // namespace

PipelineStateCacheD3D12Impl::PipelineStateCacheD3D12Impl(IReferenceCounters*                 pRefCounters,
                                                         RenderDeviceD3D12Impl*              pRenderDeviceD3D12,
                                                         const PipelineStateCacheCreateInfo& CreateInfo) :
    // clang-format off
    TPipelineStateCacheBase
    {
        pRefCounters,
        pRenderDeviceD3D12,
        CreateInfo,
        false
    }
// clang-format on
{
    ID3D12Device1* const pd3d12Device = pRenderDeviceD3D12->GetD3D12Device1();

    if (CreateInfo.pCacheData != nullptr && CreateInfo.CacheDataSize != 0)
        InitFromData(pd3d12Device, CreateInfo.pCacheData, CreateInfo.CacheDataSize);

    if (!m_pLibrary)
    {
        HRESULT hr = pd3d12Device->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&m_pLibrary));
        if (FAILED(hr))
            LOG_ERROR_AND_THROW("Failed to create D3D12 pipeline library");
    }
}

// Opens the library that the data holds. Data that is damaged, or that another version of the cache
// wrote, never reaches the runtime, because the D3D12 debug layer reports a library it can't read
// as an error. Data the runtime refuses, as from another adapter or driver, leaves the cache with
// no library, and the constructor starts an empty one. Neither is an error, as the cache can only
// be told whether data it was given is still good by trying it.
void PipelineStateCacheD3D12Impl::InitFromData(ID3D12Device1* pd3d12Device, const void* pData, size_t Size)
{
    const Uint8* const pBytes = static_cast<const Uint8*>(pData);

    PipelineStateCacheDataHeader Header;
    if (Size >= sizeof(Header))
        memcpy(&Header, pBytes, sizeof(Header));

    const Uint64 BodySize = Size >= sizeof(Header) ? Uint64{Size - sizeof(Header)} : 0;
    if (Size < sizeof(Header) ||
        Header.Magic != PipelineStateCacheDataMagic ||
        Header.Version != PipelineStateCacheDataVersion ||
        Header.KeysSize > BodySize ||
        Header.LibrarySize != BodySize - Header.KeysSize ||
        Header.LibrarySize == 0 ||
        XXH3_64bits(pBytes + sizeof(Header), static_cast<size_t>(BodySize)) != Header.Checksum)
    {
        LOG_WARNING_MESSAGE("D3D12 pipeline state cache '", (m_Desc.Name != nullptr ? m_Desc.Name : ""),
                            "': the cache data is damaged or was written by another version of the cache. Starting with an empty cache.");
        return;
    }

    std::set<std::wstring> Keys;
    {
        const Uint8* pKey    = pBytes + sizeof(Header);
        const Uint8* pKeyEnd = pKey + Header.KeysSize;
        for (Uint64 i = 0; i < Header.NumKeys; ++i)
        {
            Uint32 Length = 0;
            if (static_cast<size_t>(pKeyEnd - pKey) < sizeof(Length))
                break;
            memcpy(&Length, pKey, sizeof(Length));
            pKey += sizeof(Length);
            if (static_cast<size_t>(pKeyEnd - pKey) / sizeof(wchar_t) < Length)
                break;
            std::wstring Key(Length, L'\0');
            memcpy(&Key[0], pKey, Length * sizeof(wchar_t));
            pKey += Length * sizeof(wchar_t);
            Keys.emplace(std::move(Key));
        }
        if (Keys.size() != Header.NumKeys || pKey != pKeyEnd)
        {
            // The checksum held, so the cache that wrote the data wrote it wrong.
            LOG_ERROR_MESSAGE("D3D12 pipeline state cache '", (m_Desc.Name != nullptr ? m_Desc.Name : ""),
                              "': the cache data's keys don't match its header. Starting with an empty cache.");
            return;
        }
    }

    CComPtr<ID3DBlob> pLibraryData;
    if (FAILED(D3DCreateBlob(static_cast<SIZE_T>(Header.LibrarySize), &pLibraryData)))
    {
        LOG_ERROR_MESSAGE("Failed to allocate ", Header.LibrarySize, " bytes for the D3D12 pipeline library's data");
        return;
    }
    memcpy(pLibraryData->GetBufferPointer(), pBytes + sizeof(Header) + Header.KeysSize, static_cast<size_t>(Header.LibrarySize));

    CComPtr<ID3D12PipelineLibrary> pLibrary;
    HRESULT                        hr = pd3d12Device->CreatePipelineLibrary(pLibraryData->GetBufferPointer(), pLibraryData->GetBufferSize(), IID_PPV_ARGS(&pLibrary));
    if (FAILED(hr))
    {
        LOG_INFO_MESSAGE("D3D12 pipeline state cache '", (m_Desc.Name != nullptr ? m_Desc.Name : ""),
                         "': the runtime refused the cache data, as ", GetLibraryRefusalReason(hr), ". Starting with an empty cache.");
        return;
    }

    hr = pLibrary->SetPrivateDataInterface(PipelineLibraryDataGUID, pLibraryData);
    if (FAILED(hr))
    {
        // The library must not outlive its data, so it is dropped.
        LOG_ERROR_MESSAGE("Failed to attach the data to the D3D12 pipeline library. Starting with an empty cache.");
        return;
    }

    m_pLibrary = std::move(pLibrary);
    m_Keys     = std::move(Keys);
}

PipelineStateCacheD3D12Impl::~PipelineStateCacheD3D12Impl()
{
    // D3D12 object can only be destroyed when it is no longer used by the GPU
    GetDevice()->SafeReleaseDeviceObject(std::move(m_pLibrary), ~Uint64{0});
}

std::wstring PipelineStateCacheD3D12Impl::GetPipelineKey(const std::wstring& Name, const D3D12_COMPUTE_PIPELINE_STATE_DESC& Desc, Uint64 RootSignatureHash)
{
    PipelineDescHasher Hasher;
    Hasher.Update(PIPELINE_TYPE_COMPUTE, RootSignatureHash, Desc.CS, Desc.NodeMask, Desc.Flags);
    return MakePipelineKey(Name, Hasher.Digest());
}

std::wstring PipelineStateCacheD3D12Impl::GetPipelineKey(const std::wstring& Name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& Desc, Uint64 RootSignatureHash)
{
    PipelineDescHasher Hasher;
    Hasher.Update(PIPELINE_TYPE_GRAPHICS, RootSignatureHash, Desc.VS, Desc.PS, Desc.DS, Desc.HS, Desc.GS);

    const D3D12_STREAM_OUTPUT_DESC& StreamOutput = Desc.StreamOutput;
    Hasher.Update(StreamOutput.NumEntries);
    for (UINT i = 0; i < StreamOutput.NumEntries; ++i)
    {
        const D3D12_SO_DECLARATION_ENTRY& Entry = StreamOutput.pSODeclaration[i];
        Hasher.Update(Entry.Stream, Entry.SemanticName, Entry.SemanticIndex, Entry.StartComponent, Entry.ComponentCount, Entry.OutputSlot);
    }
    Hasher.Update(StreamOutput.NumStrides);
    for (UINT i = 0; i < StreamOutput.NumStrides; ++i)
        Hasher.Update(StreamOutput.pBufferStrides[i]);
    Hasher.Update(StreamOutput.RasterizedStream);

    const D3D12_BLEND_DESC& Blend = Desc.BlendState;
    Hasher.Update(Blend.AlphaToCoverageEnable, Blend.IndependentBlendEnable);
    for (const D3D12_RENDER_TARGET_BLEND_DESC& RT : Blend.RenderTarget)
    {
        Hasher.Update(RT.BlendEnable, RT.LogicOpEnable, RT.SrcBlend, RT.DestBlend, RT.BlendOp,
                      RT.SrcBlendAlpha, RT.DestBlendAlpha, RT.BlendOpAlpha, RT.LogicOp, RT.RenderTargetWriteMask);
    }
    Hasher.Update(Desc.SampleMask);

    const D3D12_RASTERIZER_DESC& Rasterizer = Desc.RasterizerState;
    Hasher.Update(Rasterizer.FillMode, Rasterizer.CullMode, Rasterizer.FrontCounterClockwise, Rasterizer.DepthBias,
                  Rasterizer.DepthBiasClamp, Rasterizer.SlopeScaledDepthBias, Rasterizer.DepthClipEnable,
                  Rasterizer.MultisampleEnable, Rasterizer.AntialiasedLineEnable, Rasterizer.ForcedSampleCount,
                  Rasterizer.ConservativeRaster);

    const D3D12_DEPTH_STENCIL_DESC& DepthStencil = Desc.DepthStencilState;
    Hasher.Update(DepthStencil.DepthEnable, DepthStencil.DepthWriteMask, DepthStencil.DepthFunc, DepthStencil.StencilEnable,
                  DepthStencil.StencilReadMask, DepthStencil.StencilWriteMask);
    for (const D3D12_DEPTH_STENCILOP_DESC* pFace : {&DepthStencil.FrontFace, &DepthStencil.BackFace})
        Hasher.Update(pFace->StencilFailOp, pFace->StencilDepthFailOp, pFace->StencilPassOp, pFace->StencilFunc);

    const D3D12_INPUT_LAYOUT_DESC& InputLayout = Desc.InputLayout;
    Hasher.Update(InputLayout.NumElements);
    for (UINT i = 0; i < InputLayout.NumElements; ++i)
    {
        const D3D12_INPUT_ELEMENT_DESC& Elem = InputLayout.pInputElementDescs[i];
        Hasher.Update(Elem.SemanticName, Elem.SemanticIndex, Elem.Format, Elem.InputSlot, Elem.AlignedByteOffset,
                      Elem.InputSlotClass, Elem.InstanceDataStepRate);
    }

    Hasher.Update(Desc.IBStripCutValue, Desc.PrimitiveTopologyType, Desc.NumRenderTargets);
    for (DXGI_FORMAT RTVFormat : Desc.RTVFormats)
        Hasher.Update(RTVFormat);
    Hasher.Update(Desc.DSVFormat, Desc.SampleDesc.Count, Desc.SampleDesc.Quality, Desc.NodeMask, Desc.Flags);

    return MakePipelineKey(Name, Hasher.Digest());
}

template <typename LoadPipelineType>
CComPtr<ID3D12DeviceChild> PipelineStateCacheD3D12Impl::LoadPipeline(const std::wstring& Key, LoadPipelineType&& Load)
{
    if ((m_Desc.Mode & PSO_CACHE_MODE_LOAD) == 0)
        return {};

    {
        std::unique_lock<std::mutex> Lock{m_Mtx};
        // The Thread Safety remarks of Microsoft's page on ID3D12Device1::CreatePipelineLibrary let
        // several threads use the library at once, except two threads loading the same pipeline.
        m_KeysChanged.wait(Lock, [&]() { return m_KeysBeingLoaded.count(Key) == 0; });
        if (m_Keys.count(Key) == 0 || m_FailedKeys.count(Key) != 0)
        {
            if ((m_Desc.Flags & PSO_CACHE_FLAG_VERBOSE) != 0)
                LOG_INFO_MESSAGE("Pipeline '", NarrowString(Key), "' is not in the cache");
            return {};
        }
        m_KeysBeingLoaded.insert(Key);
    }

    CComPtr<ID3D12DeviceChild> d3d12PSO;
    const HRESULT              hr = Load(d3d12PSO);
    {
        std::lock_guard<std::mutex> Lock{m_Mtx};
        m_KeysBeingLoaded.erase(Key);
        if (FAILED(hr))
            m_FailedKeys.insert(Key);
    }
    m_KeysChanged.notify_all();

    if (FAILED(hr))
    {
        LOG_ERROR_MESSAGE("Failed to load pipeline '", NarrowString(Key), "' from the D3D12 pipeline library, which holds it (HRESULT 0x",
                          std::hex, static_cast<Uint32>(hr), std::dec, ")");
        d3d12PSO.Release();
    }
    return d3d12PSO;
}

CComPtr<ID3D12DeviceChild> PipelineStateCacheD3D12Impl::LoadComputePipeline(const std::wstring& Key, const D3D12_COMPUTE_PIPELINE_STATE_DESC& Desc)
{
    return LoadPipeline(Key, [&](CComPtr<ID3D12DeviceChild>& d3d12PSO) {
        return m_pLibrary->LoadComputePipeline(Key.c_str(), &Desc, IID_PPV_ARGS(&d3d12PSO));
    });
}

CComPtr<ID3D12DeviceChild> PipelineStateCacheD3D12Impl::LoadGraphicsPipeline(const std::wstring& Key, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& Desc)
{
    return LoadPipeline(Key, [&](CComPtr<ID3D12DeviceChild>& d3d12PSO) {
        return m_pLibrary->LoadGraphicsPipeline(Key.c_str(), &Desc, IID_PPV_ARGS(&d3d12PSO));
    });
}

bool PipelineStateCacheD3D12Impl::StorePipeline(const std::wstring& Key, ID3D12DeviceChild* pPSO)
{
    VERIFY_EXPR(!Key.empty() && pPSO != nullptr);
    if ((m_Desc.Mode & PSO_CACHE_MODE_STORE) == 0)
        return false;

    {
        std::lock_guard<std::mutex> Lock{m_Mtx};
        // The library holds the pipeline already, or another thread created the same pipeline and
        // stores it, or a store of it failed.
        if (m_Keys.count(Key) != 0 || !m_StoredKeys.insert(Key).second)
            return false;
        ++m_NumStoresInFlight;
    }

    const HRESULT hr = m_pLibrary->StorePipeline(Key.c_str(), static_cast<ID3D12PipelineState*>(pPSO));
    {
        std::lock_guard<std::mutex> Lock{m_Mtx};
        if (SUCCEEDED(hr))
            m_Keys.insert(Key);
        --m_NumStoresInFlight;
    }
    m_KeysChanged.notify_all();

    if (FAILED(hr))
    {
        LOG_ERROR_MESSAGE("Failed to add pipeline '", NarrowString(Key), "' to the D3D12 pipeline library (HRESULT 0x",
                          std::hex, static_cast<Uint32>(hr), std::dec, ")");
    }
    return SUCCEEDED(hr);
}

void PipelineStateCacheD3D12Impl::GetData(IDataBlob** ppBlob)
{
    DEV_CHECK_ERR(ppBlob != nullptr, "ppBlob must not be null");
    *ppBlob = nullptr;

    // No store starts while the lock is held, so the keys written are those the library holds.
    std::unique_lock<std::mutex> Lock{m_Mtx};
    m_KeysChanged.wait(Lock, [&]() { return m_NumStoresInFlight == 0; });

    PipelineStateCacheDataHeader Header;
    Header.Magic       = PipelineStateCacheDataMagic;
    Header.Version     = PipelineStateCacheDataVersion;
    Header.NumKeys     = m_Keys.size();
    Header.LibrarySize = m_pLibrary->GetSerializedSize();
    for (const std::wstring& Key : m_Keys)
        Header.KeysSize += sizeof(Uint32) + Key.size() * sizeof(wchar_t);

    RefCntAutoPtr<DataBlobImpl> pDataBlob = DataBlobImpl::Create(static_cast<size_t>(sizeof(Header) + Header.KeysSize + Header.LibrarySize));
    Uint8* const                pBytes    = pDataBlob->GetDataPtr<Uint8>();

    Uint8* pKey = pBytes + sizeof(Header);
    for (const std::wstring& Key : m_Keys)
    {
        const Uint32 Length = static_cast<Uint32>(Key.size());
        memcpy(pKey, &Length, sizeof(Length));
        pKey += sizeof(Length);
        memcpy(pKey, Key.data(), Length * sizeof(wchar_t));
        pKey += Length * sizeof(wchar_t);
    }

    HRESULT hr = m_pLibrary->Serialize(pKey, static_cast<SIZE_T>(Header.LibrarySize));
    if (FAILED(hr))
    {
        LOG_ERROR_MESSAGE("Failed to serialize D3D12 pipeline library");
        return;
    }

    Header.Checksum = XXH3_64bits(pBytes + sizeof(Header), static_cast<size_t>(Header.KeysSize + Header.LibrarySize));
    memcpy(pBytes, &Header, sizeof(Header));

    *ppBlob = pDataBlob.Detach();
}

} // namespace Diligent
