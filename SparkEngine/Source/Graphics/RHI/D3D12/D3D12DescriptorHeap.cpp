/**
 * @file D3D12DescriptorHeap.cpp
 * @brief D3D12 descriptor heaps and per-draw descriptor-table binding
 *
 * Views and samplers are created in CPU-only heaps (DescriptorHeapAllocator). A
 * D3D12CommandList stages the CPU handles its caller binds and, before each draw, copies
 * them into a table carved from a page of the shader-visible heaps
 * (D3D12DescriptorPagePool), then binds that table. Split from D3D12Device.cpp and
 * D3D12CommandList.cpp so the binding path reads as one unit.
 */

#ifdef _WIN32

#include "D3D12Device.h"
#include "../../../Utils/LogMacros.h"

#include <algorithm>
#include <bit>

namespace Spark
{
    namespace RHI
    {
        namespace D3D12
        {

            // ============================================================================
            // DESCRIPTOR HEAP ALLOCATOR
            // ============================================================================

            bool DescriptorHeapAllocator::Initialize(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                                     uint32_t descriptorCount, bool shaderVisible)
            {
                D3D12_DESCRIPTOR_HEAP_DESC desc = {};
                desc.Type = type;
                desc.NumDescriptors = descriptorCount;
                desc.Flags =
                    shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

                HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_heap));
                if (FAILED(hr))
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "D3D12: Failed to create descriptor heap (type=%d)",
                                    type);
                    return false;
                }

                m_cpuStart = m_heap->GetCPUDescriptorHandleForHeapStart();
                m_gpuStart = {};
                if (shaderVisible)
                    m_gpuStart = m_heap->GetGPUDescriptorHandleForHeapStart();
                m_descriptorSize = device->GetDescriptorHandleIncrementSize(type);
                m_capacity = descriptorCount;
                m_nextFreeIndex = 0;
                return true;
            }

            DescriptorAllocation DescriptorHeapAllocator::Allocate(uint32_t count)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                DescriptorAllocation alloc = {};

                // A CPU-only heap has no GPU address: leave gpuHandle null rather than an offset from 0.
                const auto gpuHandleAt = [this](uint32_t index)
                {
                    D3D12_GPU_DESCRIPTOR_HANDLE handle = {};
                    if (m_gpuStart.ptr != 0)
                        handle.ptr = m_gpuStart.ptr + static_cast<UINT64>(index) * m_descriptorSize;
                    return handle;
                };

                // Try free list first for single descriptors
                while (count == 1 && !m_freeList.empty())
                {
                    uint32_t index = m_freeList.back();
                    m_freeList.pop_back();
                    if (index >= m_capacity)
                    {
                        // Corrupt entry (e.g., index from a different heap) — drop and try next
                        SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                        "D3D12: Descriptor free list contained out-of-range index %u (capacity %u)",
                                        index, m_capacity);
                        continue;
                    }
                    alloc.index = index;
                    alloc.count = 1;
                    alloc.cpuHandle.ptr = m_cpuStart.ptr + static_cast<SIZE_T>(index) * m_descriptorSize;
                    alloc.gpuHandle = gpuHandleAt(index);
                    return alloc;
                }

                // Bump allocator
                if (m_nextFreeIndex + count > m_capacity)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "D3D12: Descriptor heap exhausted");
                    return alloc;
                }

                alloc.index = m_nextFreeIndex;
                alloc.count = count;
                alloc.cpuHandle.ptr = m_cpuStart.ptr + static_cast<SIZE_T>(m_nextFreeIndex) * m_descriptorSize;
                alloc.gpuHandle = gpuHandleAt(m_nextFreeIndex);
                m_nextFreeIndex += count;
                return alloc;
            }

            void DescriptorHeapAllocator::Free(const DescriptorAllocation& allocation)
            {
                if (!allocation.IsValid())
                    return;
                std::lock_guard<std::mutex> lock(m_mutex);
                for (uint32_t i = 0; i < allocation.count; i++)
                    m_freeList.push_back(allocation.index + i);
            }

            // ============================================================================
            // SHADER-VISIBLE DESCRIPTOR TABLE PAGES
            // ============================================================================

            bool D3D12DescriptorPagePool::Initialize(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                                     uint32_t pageSize, uint32_t pageCount, ID3D12Fence* fence)
            {
                D3D12_DESCRIPTOR_HEAP_DESC desc = {};
                desc.Type = type;
                desc.NumDescriptors = pageSize * pageCount;
                desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
                if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_heap))))
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                    "D3D12: Failed to create shader-visible descriptor table heap (type=%d, %u "
                                    "descriptors)",
                                    type, desc.NumDescriptors);
                    return false;
                }

                std::lock_guard<std::mutex> lock(m_mutex);
                m_fence = fence;
                m_cpuStart = m_heap->GetCPUDescriptorHandleForHeapStart();
                m_gpuStart = m_heap->GetGPUDescriptorHandleForHeapStart();
                m_descriptorSize = device->GetDescriptorHandleIncrementSize(type);
                m_pageSize = pageSize;

                // Both lists hold at most pageCount entries, so reserving here keeps Acquire and
                // Retire free of reallocation. Pushed in reverse so page 0 is handed out first.
                m_freePages.clear();
                m_freePages.reserve(pageCount);
                for (uint32_t page = pageCount; page-- > 0;)
                    m_freePages.push_back(page);
                m_retired.clear();
                m_retired.reserve(pageCount);
                m_retiredHead = 0;
                return true;
            }

            void D3D12DescriptorPagePool::ReclaimCompleted()
            {
                if (!m_fence)
                    return;
                const uint64_t completed = m_fence->GetCompletedValue();
                while (m_retiredHead < m_retired.size() && m_retired[m_retiredHead].fenceValue <= completed)
                {
                    m_freePages.push_back(m_retired[m_retiredHead].page);
                    ++m_retiredHead;
                }
                if (m_retiredHead == m_retired.size())
                {
                    m_retired.clear();
                    m_retiredHead = 0;
                }
            }

            bool D3D12DescriptorPagePool::Acquire(uint32_t& page)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_freePages.empty())
                    ReclaimCompleted();
                if (m_freePages.empty())
                    return false;
                page = m_freePages.back();
                m_freePages.pop_back();
                return true;
            }

            void D3D12DescriptorPagePool::Retire(std::span<const uint32_t> pages, uint64_t fenceValue)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                for (const uint32_t page : pages)
                {
                    if (fenceValue == 0)
                    {
                        m_freePages.push_back(page);
                        continue;
                    }
                    // Compact the consumed prefix instead of growing: live entries never exceed
                    // the page count the vector was reserved for.
                    if (m_retired.size() == m_retired.capacity() && m_retiredHead > 0)
                    {
                        m_retired.erase(m_retired.begin(), m_retired.begin() + static_cast<ptrdiff_t>(m_retiredHead));
                        m_retiredHead = 0;
                    }
                    // Keep the queue ordered so ReclaimCompleted can stop at the first pending entry.
                    const uint64_t ordered = m_retired.size() > m_retiredHead
                                                 ? std::max(fenceValue, m_retired.back().fenceValue)
                                                 : fenceValue;
                    m_retired.push_back({ordered, page});
                }
            }

            D3D12_CPU_DESCRIPTOR_HANDLE D3D12DescriptorPagePool::CpuStart(uint32_t page) const
            {
                D3D12_CPU_DESCRIPTOR_HANDLE handle = m_cpuStart;
                handle.ptr += static_cast<SIZE_T>(page) * m_pageSize * m_descriptorSize;
                return handle;
            }

            D3D12_GPU_DESCRIPTOR_HANDLE D3D12DescriptorPagePool::GpuStart(uint32_t page) const
            {
                D3D12_GPU_DESCRIPTOR_HANDLE handle = m_gpuStart;
                handle.ptr += static_cast<UINT64>(page) * m_pageSize * m_descriptorSize;
                return handle;
            }

            size_t D3D12DescriptorPagePool::GetFreePageCount()
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                ReclaimCompleted();
                return m_freePages.size();
            }

            // ============================================================================
            // D3D12 COMMAND LIST — RESOURCE BINDING
            // ============================================================================

            D3D12CommandList::~D3D12CommandList()
            {
                // Pages still held were filled by recordings that were never submitted: the GPU
                // never saw them, so they can go straight back.
                RetireDescriptorPages(0);
            }

            void D3D12CommandList::SetConstantBuffer(RHIShaderStage /*stage*/, uint32_t slot, IRHIBuffer* buffer)
            {
                if (slot >= DefaultRootLayout::kConstantBufferCount)
                {
                    SPARK_LOG_EVERY_SECONDS(Spark::LogLevel::Error, Spark::LogCategory::Graphics, 5,
                                            "D3D12CommandList::SetConstantBuffer: slot b%u outside the root CBVs "
                                            "b0-b%u - binding dropped",
                                            slot, DefaultRootLayout::kConstantBufferCount - 1);
                    return;
                }
                auto* buf = static_cast<D3D12Buffer*>(buffer);
                const D3D12_GPU_VIRTUAL_ADDRESS address = buf ? buf->GetGPUVirtualAddress() : 0;
                if (address != m_constantBuffers[slot])
                {
                    m_constantBuffers[slot] = address;
                    m_dirtyConstantBuffers |= 1u << slot;
                }
            }

            void D3D12CommandList::SetShaderResource(RHIShaderStage /*stage*/, uint32_t slot, IRHITexture* texture)
            {
                if (slot >= DefaultRootLayout::kShaderResourceSlots)
                {
                    SPARK_LOG_EVERY_SECONDS(Spark::LogLevel::Error, Spark::LogCategory::Graphics, 5,
                                            "D3D12CommandList::SetShaderResource: slot t%u outside the SRV table "
                                            "t0-t%u - binding dropped",
                                            slot, DefaultRootLayout::kShaderResourceSlots - 1);
                    return;
                }

                D3D12_CPU_DESCRIPTOR_HANDLE handle = {};
                auto* tex = static_cast<D3D12Texture*>(texture);
                if (tex && !tex->GetSRVDescriptor().IsValid())
                {
                    SPARK_LOG_EVERY_SECONDS(Spark::LogLevel::Error, Spark::LogCategory::Graphics, 5,
                                            "D3D12CommandList::SetShaderResource: '%s' has no SRV (created without "
                                            "ShaderResource usage) - slot t%u left unbound",
                                            tex->GetDebugName().c_str(), slot);
                }
                else if (tex)
                {
                    handle = tex->GetSRVDescriptor().cpuHandle;
                    // D3D11 moves a render target to shader-read implicitly; D3D12 needs a barrier.
                    RequireState(tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                }

                if (handle.ptr != m_shaderResources[slot].ptr)
                {
                    m_shaderResources[slot] = handle;
                    m_shaderResourcesDirty = true;
                }
                if (handle.ptr != 0)
                    m_boundShaderResources |= 1u << slot;
                else
                    m_boundShaderResources &= ~(1u << slot);
            }

            void D3D12CommandList::SetSampler(RHIShaderStage /*stage*/, uint32_t slot, IRHISampler* sampler)
            {
                if (slot >= DefaultRootLayout::kSamplerSlots)
                {
                    SPARK_LOG_EVERY_SECONDS(Spark::LogLevel::Error, Spark::LogCategory::Graphics, 5,
                                            "D3D12CommandList::SetSampler: slot s%u outside the sampler table s0-s%u "
                                            "- binding dropped",
                                            slot, DefaultRootLayout::kSamplerSlots - 1);
                    return;
                }
                auto* d3dSampler = static_cast<D3D12Sampler*>(sampler);
                const D3D12_CPU_DESCRIPTOR_HANDLE handle =
                    d3dSampler ? d3dSampler->GetDescriptor().cpuHandle : D3D12_CPU_DESCRIPTOR_HANDLE{};
                if (handle.ptr != m_samplers[slot].ptr)
                {
                    m_samplers[slot] = handle;
                    m_samplersDirty = true;
                }
                if (handle.ptr != 0)
                    m_boundSamplers |= 1u << slot;
                else
                    m_boundSamplers &= ~(1u << slot);
            }

            bool D3D12CommandList::BindTable(D3D12DescriptorPagePool& pool, TablePage& cursor,
                                             std::array<uint32_t, kMaxHeldPages>& held, uint32_t& heldCount,
                                             const D3D12_CPU_DESCRIPTOR_HANDLE* slots, uint32_t slotCount,
                                             D3D12_CPU_DESCRIPTOR_HANDLE nullDescriptor,
                                             D3D12_DESCRIPTOR_HEAP_TYPE heapType, uint32_t rootParameter)
            {
                if (cursor.page == UINT32_MAX || cursor.used + slotCount > pool.GetPageSize())
                {
                    uint32_t page = 0;
                    if (heldCount >= kMaxHeldPages || !pool.Acquire(page))
                    {
                        SPARK_LOG_EVERY_SECONDS(Spark::LogLevel::Error, Spark::LogCategory::Graphics, 5,
                                                "D3D12CommandList: no free %s descriptor-table page (%u held by this "
                                                "list) - draw skipped; submit the list more often",
                                                heapType == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? "sampler" : "SRV",
                                                heldCount);
                        return false;
                    }
                    held[heldCount++] = page;
                    cursor.page = page;
                    cursor.used = 0;
                }

                const SIZE_T offset = static_cast<SIZE_T>(cursor.used) * pool.GetDescriptorSize();
                D3D12_CPU_DESCRIPTOR_HANDLE destination = pool.CpuStart(cursor.page);
                destination.ptr += offset;
                D3D12_GPU_DESCRIPTOR_HANDLE table = pool.GpuStart(cursor.page);
                table.ptr += offset;

                // Unbound slots get a null descriptor: GPU-based validation rejects a table
                // with uninitialized entries, and a null SRV reads zero as on D3D11.
                std::array<D3D12_CPU_DESCRIPTOR_HANDLE, DefaultRootLayout::kShaderResourceSlots> sources = {};
                for (uint32_t i = 0; i < slotCount; ++i)
                    sources[i] = slots[i].ptr != 0 ? slots[i] : nullDescriptor;
                const UINT destinationSize = slotCount;
                // Null source-size array: every source range is one descriptor.
                m_device->CopyDescriptors(1, &destination, &destinationSize, slotCount, sources.data(), nullptr,
                                          heapType);
                m_commandList->SetGraphicsRootDescriptorTable(rootParameter, table);
                cursor.used += slotCount;
                return true;
            }

            bool D3D12CommandList::PrepareDraw()
            {
                FlushBarriers();
                if (!m_currentRootSignature || !m_tables)
                    return true; // No pipeline yet: nothing to bind against.

                while (m_dirtyConstantBuffers != 0)
                {
                    const uint32_t slot = static_cast<uint32_t>(std::countr_zero(m_dirtyConstantBuffers));
                    m_dirtyConstantBuffers &= m_dirtyConstantBuffers - 1;
                    if (m_constantBuffers[slot] != 0)
                        m_commandList->SetGraphicsRootConstantBufferView(slot, m_constantBuffers[slot]);
                }

                // Flags stay set on failure so the next draw retries the table.
                if (m_shaderResourcesDirty)
                {
                    if (!BindTable(m_tables->shaderResources, m_shaderResourcePage, m_heldShaderResourcePages,
                                   m_heldShaderResourcePageCount, m_shaderResources.data(),
                                   DefaultRootLayout::kShaderResourceSlots, m_tables->nullShaderResource,
                                   D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, DefaultRootLayout::kShaderResourceTable))
                        return false;
                    m_shaderResourcesDirty = false;
                }
                if (m_samplersDirty)
                {
                    if (!BindTable(m_tables->samplers, m_samplerPage, m_heldSamplerPages, m_heldSamplerPageCount,
                                   m_samplers.data(), DefaultRootLayout::kSamplerSlots, m_tables->nullSampler,
                                   D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, DefaultRootLayout::kSamplerTable))
                        return false;
                    m_samplersDirty = false;
                }
                return true;
            }

            void D3D12CommandList::ResetBindings()
            {
                m_constantBuffers = {};
                m_shaderResources = {};
                m_samplers = {};
                m_dirtyConstantBuffers = 0;
                m_boundShaderResources = 0;
                m_boundSamplers = 0;
                m_shaderResourcesDirty = false;
                m_samplersDirty = false;
            }

            void D3D12CommandList::BindDescriptorHeaps()
            {
                if (!m_tables || m_type == D3D12_COMMAND_LIST_TYPE_COPY)
                    return;
                ID3D12DescriptorHeap* heaps[] = {m_tables->shaderResources.GetHeap(), m_tables->samplers.GetHeap()};
                m_commandList->SetDescriptorHeaps(2, heaps);
            }

            void D3D12CommandList::RetireDescriptorPages(uint64_t fenceValue)
            {
                if (!m_tables)
                    return;
                m_tables->shaderResources.Retire(
                    std::span<const uint32_t>(m_heldShaderResourcePages.data(), m_heldShaderResourcePageCount),
                    fenceValue);
                m_tables->samplers.Retire(std::span<const uint32_t>(m_heldSamplerPages.data(), m_heldSamplerPageCount),
                                          fenceValue);
                m_heldShaderResourcePageCount = 0;
                m_heldSamplerPageCount = 0;
                m_shaderResourcePage = {};
                m_samplerPage = {};
            }

        } // namespace D3D12
    } // namespace RHI
} // namespace Spark

#endif // _WIN32
