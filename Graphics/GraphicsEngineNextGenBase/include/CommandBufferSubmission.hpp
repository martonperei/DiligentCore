/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *  Copyright 2015-2019 Egor Yusov
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
/// Declaration of Diligent::CommandBufferSubmission class

#include <atomic>
#include <memory>

#include "BasicTypes.h"

namespace Diligent
{

/// The submission of a command buffer that a device context records.

/// The fence value that a command queue signals once it has executed a command buffer is only known
/// when the command buffer is submitted. The value the queue is going to signal next is not it:
/// any submission to the queue before the context submits the command buffer, such as a transient
/// command buffer that uploads a resource's initial data, takes that value.
///
/// An object that a command buffer uses until the GPU has executed it, such as a query that the
/// command buffer ends, keeps a reference to the command buffer's submission, which the context
/// creates while it records the command buffer and sets the fence value of when it submits it.
/// Until then, the fence value is NotSubmitted, which no completed fence value reaches.
class CommandBufferSubmission
{
public:
    static constexpr Uint64 NotSubmitted = ~Uint64{0};

    /// Returns the fence value that the command queue signals once it has executed the command
    /// buffer, or NotSubmitted if the command buffer has not been submitted yet.
    Uint64 GetFenceValue() const
    {
        return m_FenceValue.load();
    }

    void SetFenceValue(Uint64 FenceValue)
    {
        m_FenceValue.store(FenceValue);
    }

    /// Returns true if the command queue has executed the command buffer, given the queue's
    /// completed fence value.
    bool IsComplete(Uint64 CompletedFenceValue) const
    {
        return CompletedFenceValue >= GetFenceValue();
    }

private:
    std::atomic<Uint64> m_FenceValue{NotSubmitted};
};

using CommandBufferSubmissionPtr = std::shared_ptr<CommandBufferSubmission>;

} // namespace Diligent
