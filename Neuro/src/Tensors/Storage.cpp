#include <cuda.h>
#include <cuda_runtime.h>

#include "Tensors/Storage.h"
#include "Memory/MemoryManager.h"
#include "Tensors/Cuda/CudaErrorCheck.h"
#include "Tools.h"
#include "Stopwatch.h"

//#define ENABLE_STORAGE_LOGS
//#define DISABLE_OFFLOADABLE_SUPPORT

#ifdef ENABLE_STORAGE_LOGS
#include <windows.h>
#include <debugapi.h>
#define STORAGE_DEBUG_INFO_NO_TS(...) do { static char buffer[1024]; sprintf(buffer, __VA_ARGS__); OutputDebugString(buffer); } while (0)
#define STORAGE_DEBUG_INFO(...) do { static char timeBuffer[128]; SYSTEMTIME sysTime; GetLocalTime(&sysTime); sprintf(timeBuffer, "%02d:%02d:%02d.%03d - ", sysTime.wHour, sysTime.wMinute, sysTime.wSecond, sysTime.wMilliseconds); OutputDebugString(timeBuffer); static char buffer[1024]; sprintf(buffer, __VA_ARGS__); OutputDebugString(buffer); } while (0)
#else
#define STORAGE_DEBUG_INFO_NO_TS(...) {}
#define STORAGE_DEBUG_INFO(...) {}
#endif

namespace Neuro
{
    static const uint32_t MIN_SIZE_TO_OFFLOAD = 4*1024*1024; // 4MB

    //////////////////////////////////////////////////////////////////////////
    Storage::Storage(int type, size_t size, const string& name)
        : m_Type(type), m_AllocSize(size), m_Size(size), m_Name(name), m_DataLocation(None)
    {
        if (m_Type & ST_Offloadable)
        {
            CUDA_CHECK(cudaEventCreate(&m_OffloadEvent));
            CUDA_CHECK(cudaEventCreate(&m_PreloadEvent));
        }
    }

    //////////////////////////////////////////////////////////////////////////
    Storage::Storage(const Storage& other)
    {
        *this = other;
    }

    //////////////////////////////////////////////////////////////////////////
    Storage::Storage(Storage&& other)
    {
        *this = move(other);
    }

    //////////////////////////////////////////////////////////////////////////
    Storage& Storage::operator=(const Storage& other)
    {
        if (this != &other)
        {
            m_AllocSize = other.m_AllocSize;
            m_Size = other.m_Size;
            m_DataRefCount = m_DeviceDataRefCount = 0;
            FreeOnDevice(true, true);
            FreeOnHost();
            ChangeType(other.m_Type);
            if (other.m_DataPtr)
            {
                NEURO_ASSERT(other.m_DataLocation != None, "");
                m_DataLocation = Host;
                AllocateOnHost();
                other.SyncToHost();
                memcpy(m_DataPtr, other.m_DataPtr, SizeInBytes());
            }
            else
            {
                m_DataLocation = None;
                m_DataPtr = nullptr;
            }
            m_DeviceDataPtr = nullptr;
            m_PreloadRequested = false;
            m_OffloadRequested = false;
            m_FreeDeviceMemOnOffloadDone = false;
            m_FreePinnedMemOnOffloadDone = false;
        }
        return *this;
    }

    //////////////////////////////////////////////////////////////////////////
    Storage& Storage::operator=(Storage&& other)
    {
        if (this != &other)
        {
            if (m_OffloadEvent)
                CUDA_CHECK(cudaEventDestroy(m_OffloadEvent));
            if (m_PreloadEvent)
                CUDA_CHECK(cudaEventDestroy(m_PreloadEvent));
            FreeOnDevice(true, true);
            FreeOnHost();
            m_Type = other.m_Type;
            m_AllocSize = other.m_AllocSize;
            m_Size = other.m_Size;
            m_DataRefCount = other.m_DataRefCount;
            m_DeviceDataRefCount = other.m_DeviceDataRefCount;
            m_Name = other.m_Name;
            m_DataLocation = other.m_DataLocation;
            m_DeviceDataPtr = other.m_DeviceDataPtr;
            other.m_DeviceDataPtr = nullptr;
            m_DataPtr = other.m_DataPtr;
            other.m_DataPtr = nullptr;
            m_OffloadEvent = other.m_OffloadEvent;
            other.m_OffloadEvent = nullptr;
            NEURO_ASSERT(!other.m_OffloadRequested, "Moving while offload in progress, this may not end well...");
            other.WaitForOffload();
            m_OffloadRequested = other.m_OffloadRequested;
            m_FreeDeviceMemOnOffloadDone = other.m_FreeDeviceMemOnOffloadDone;
            m_FreePinnedMemOnOffloadDone = other.m_FreePinnedMemOnOffloadDone;
            NEURO_ASSERT(!other.m_PreloadRequested, "Moving while preload in progress, this may not end well...");
            other.WaitForPreload();
            m_PreloadRequested = other.m_PreloadRequested;
            m_PreloadEvent = other.m_PreloadEvent;
            other.m_PreloadEvent = nullptr;
            m_OffloadPromise = move(other.m_OffloadPromise);
            m_OffloadFuture = move(other.m_OffloadFuture);
            m_PreloadPromise = move(other.m_PreloadPromise);
            m_PreloadFuture = move(other.m_PreloadFuture);
        }
        return *this;
    }

    //////////////////////////////////////////////////////////////////////////
    Storage::~Storage()
    {
        FreeOnDevice(true, true);
        FreeOnHost();
        if (m_OffloadEvent)
            CUDA_CHECK(cudaEventDestroy(m_OffloadEvent));
        if (m_PreloadEvent)
            CUDA_CHECK(cudaEventDestroy(m_PreloadEvent));
    }

    //////////////////////////////////////////////////////////////////////////
    void Storage::ChangeType(int type)
    {
        if (m_Type == type)
            return;

        NEURO_ASSERT(!m_DataPtr && !m_DeviceDataPtr, "Changing type of allocated storage is not allowed.");

        if ((m_Type & ST_Offloadable) && !(type & ST_Offloadable))
        {
            CUDA_CHECK(cudaEventDestroy(m_OffloadEvent));
            m_OffloadEvent = nullptr;
            CUDA_CHECK(cudaEventDestroy(m_PreloadEvent));
            m_PreloadEvent = nullptr;
        }
        else if (!(m_Type & ST_Offloadable) && (type & ST_Offloadable))
        {
            CUDA_CHECK(cudaEventCreate(&m_OffloadEvent));
            CUDA_CHECK(cudaEventCreate(&m_PreloadEvent));
        }

        m_Type = type;
    }

    //////////////////////////////////////////////////////////////////////////
    void Storage::Resize(size_t size)
    {
        STORAGE_DEBUG_INFO("Resizing '%s' from %zu to %zu (alloc size %zu)", m_Name.c_str(), m_Size, size, m_AllocSize);
        if (size < m_AllocSize)
        {
            STORAGE_DEBUG_INFO_NO_TS(" <<< no reallocation required.\n");
            m_Size = size;
            return;
        }

        STORAGE_DEBUG_INFO_NO_TS(" <<< reallocating.\n");

        m_AllocSize = m_Size = size;

        bool wasAllocatedOnDevice = m_DeviceDataPtr != nullptr;
        bool wasAllocatedOnHost = m_DataPtr != nullptr;

        if (m_DeviceDataPtr)
            FreeOnDevice(true, true);
        if (m_DataPtr)
            FreeOnHost();

        if (wasAllocatedOnHost)
            AllocateOnHost();
        if (wasAllocatedOnDevice)
            AllocateOnDevice();
    }

    //////////////////////////////////////////////////////////////////////////
    void Storage::Rename(const string& name)
    {
        m_Name = name;
        HostMemoryManager::Default().UpdateAnnotation(m_DataPtr, name);
        HostPinnedMemoryManager::Default().UpdateAnnotation(m_DataPtr, name);
        DeviceMemoryManager::Default().UpdateAnnotation(m_DeviceDataPtr, name);
    }

    //////////////////////////////////////////////////////////////////////////
    void Storage::Release()
    {
        FreeOnDevice(false, true);
        FreeOnHost();
        m_DataLocation = None;
        m_DeviceDataRefCount = 0;
        m_DataRefCount = 0;
    }

    //////////////////////////////////////////////////////////////////////////
    void Storage::AllocateOnHost() const
    {
        if (m_AllocSize == 0)
            return;

        NEURO_ASSERT(!m_DeviceDataPtr, "");
        STORAGE_DEBUG_INFO("Allocating on host '%s' ", m_Name.c_str());
        if (m_DataPtr)
        {
            STORAGE_DEBUG_INFO_NO_TS("<<< already allocated.\n");
            return;
        }
        STORAGE_DEBUG_INFO_NO_TS("<<< allocating.\n");
        if (m_Type & ST_Offloadable)
            HostPinnedMemoryManager::Default().Allocate((void**)&m_DataPtr, AllocSizeInBytes(), m_Name);
        else
            HostMemoryManager::Default().Allocate((void**)&m_DataPtr, AllocSizeInBytes(), m_Name);

        m_DataLocation = Host;
    }

    //////////////////////////////////////////////////////////////////////////
    void Storage::FreeOnHost()
    {
        STORAGE_DEBUG_INFO("Releasing on host '%s' ", m_Name.c_str());

        if (m_OffloadRequested)
        {
            unique_lock<mutex> mtx(m_OffloadDoneCallbackMtx);
            if (!m_OffloadDone)
            {
                m_FreePinnedMemOnOffloadDone = true;
                STORAGE_DEBUG_INFO_NO_TS("<<< release will take place on offload-done callback.\n");
                return;
            }
        }

        NEURO_ASSERT(!m_DeviceDataPtr, "Data cannot be only on device.");
        
        if (!m_DataPtr)
        {
            STORAGE_DEBUG_INFO_NO_TS("<<< not allocated.\n");
            return;
        }
        STORAGE_DEBUG_INFO_NO_TS("<<< release incoming.\n");
        if (m_Type & ST_Offloadable)
            HostPinnedMemoryManager::Default().Free(m_DataPtr);
        else
            HostMemoryManager::Default().Free(m_DataPtr);
        
        m_DataPtr = nullptr;
        m_DataLocation = None;
    }

    //////////////////////////////////////////////////////////////////////////
    void Storage::AllocateOnDevice() const
    {
        if (m_AllocSize == 0)
            return;

        if (!m_DataPtr)
            AllocateOnHost();

        if (m_OffloadFuture.valid())
        {
            m_OffloadRequested = false;
            m_OffloadFuture.get();
            m_OffloadPromise = promise<void>();
        }

        if (m_PreloadFuture.valid())
        {
            m_PreloadRequested = false;
            m_PreloadFuture.get();
            m_PreloadPromise = promise<void>();
        }

        NEURO_ASSERT(m_DataPtr, "Data cannot be only on device.");
        STORAGE_DEBUG_INFO("Allocating on device '%s' ", m_Name.c_str());
        if (m_DeviceDataPtr)
        {
            STORAGE_DEBUG_INFO_NO_TS("<<< already allocated.\n");
            return;
        }

        STORAGE_DEBUG_INFO_NO_TS("<<< allocating.\n");
        CUDA_CHECK(DeviceMemoryManager::Default().Allocate((void**)&m_DeviceDataPtr, AllocSizeInBytes(), m_Name));
    }

    //////////////////////////////////////////////////////////////////////////
    void Storage::FreeOnDevice(bool force, bool forceWaitForOffload)
    {
        WaitForPreload();

        if (forceWaitForOffload)
            WaitForOffload();

        if (m_OffloadRequested)
        {
            unique_lock<mutex> mtx(m_OffloadDoneCallbackMtx);
            if (!m_OffloadDone)
            {
                m_FreeDeviceMemOnOffloadDone = true;
                STORAGE_DEBUG_INFO_NO_TS("<<< release will take place on offload-done callback.\n");
                return;
            }
        }

        STORAGE_DEBUG_INFO("Releasing on device '%s' ", m_Name.c_str());
        if (!m_DeviceDataPtr)
        {
            STORAGE_DEBUG_INFO_NO_TS("<<< not allocated.\n");
            return;
        }

        if (!force && (m_Type & ST_KeepDevMem))
        {
            STORAGE_DEBUG_INFO_NO_TS("<<< not allowed.\n");
            return;
        }

        m_FreeDeviceMemOnOffloadDone = false;

        STORAGE_DEBUG_INFO_NO_TS("<<< release incoming.\n");
        CUDA_CHECK(DeviceMemoryManager::Default().Free((void*)m_DeviceDataPtr));
        m_DeviceDataPtr = nullptr;

        // at this point the only place where values are stored is host memory
        if (m_DataPtr)
            m_DataLocation = Host;
    }

    //////////////////////////////////////////////////////////////////////////
    void Storage::OffloadDoneCallback(void* userData)
    {
        NVTXProfile nvtxProfile(__FUNCTION__, 0xFFB200FF);
        Storage* storage = (Storage*)userData;

        unique_lock<mutex> mtx(storage->m_OffloadDoneCallbackMtx);
        if (storage->m_FreeDeviceMemOnOffloadDone)
        {
            STORAGE_DEBUG_INFO("Offload done '%s'[%d]\n", storage->m_Name.c_str(), storage->m_Type);
            CUDA_CHECK(DeviceMemoryManager::Default().ScheduleFree((void*)storage->m_DeviceDataPtr));
            storage->m_DeviceDataPtr = nullptr;

            if (storage->m_DataPtr)
                storage->m_DataLocation = Host;
        }
        else
            STORAGE_DEBUG_INFO("Offload done '%s'[%d] <<< not releasing device memory\n", storage->m_Name.c_str(), storage->m_Type);

        if (storage->m_FreePinnedMemOnOffloadDone)
        {
            HostPinnedMemoryManager::Default().Free((void*)storage->m_DataPtr);
            storage->m_DataPtr = nullptr;

            storage->m_DataLocation = None;
        }

        storage->m_OffloadDone = true;
        storage->m_FreeDeviceMemOnOffloadDone = false;
        storage->m_FreePinnedMemOnOffloadDone = false;
        storage->m_OffloadPromise.set_value();
    }

    //////////////////////////////////////////////////////////////////////////
    void Storage::PreloadDoneCallback(void* userData)
    {
        NVTXProfile nvtxProfile(__FUNCTION__, 0xFFB200FF);
        Storage* storage = (Storage*)userData;

        // user better not deallocate storage/device memory before this callback is called
        // perhaps in the future I will add appropriate locks
        if (storage->m_DeviceDataPtr)
            storage->m_DataLocation = Device;
        storage->m_PreloadPromise.set_value();
        STORAGE_DEBUG_INFO("Preload done '%s'[%d]\n", storage->m_Name.c_str(), storage->m_Type);
    }

    //////////////////////////////////////////////////////////////////////////
    void Storage::WaitForOffload() const
    {
        if (m_OffloadRequested)
        {
            NVTXProfile p((string(__FUNCTION__) + " " + m_Name).c_str(), 0xFFB200FF);

            AutoStopwatch prof(Microseconds);
            STORAGE_DEBUG_INFO("Waiting for offload callback... '%s'[%d]\n", m_Name.c_str(), m_Type);
            m_OffloadFuture.get(); // wait for callback
            m_OffloadPromise = promise<void>();
            m_OffloadRequested = false;
            STORAGE_DEBUG_INFO("--> waited %s\n", prof.ToString().c_str());
        }
    }

    //////////////////////////////////////////////////////////////////////////
    void Storage::WaitForPreload() const
    {
        if (m_PreloadRequested)
        {
            NVTXProfile p((string(__FUNCTION__) + " " + m_Name).c_str(), 0xFFB200FF);

            AutoStopwatch prof(Microseconds);
            STORAGE_DEBUG_INFO("Waiting for preload callback... '%s'[%d]\n", m_Name.c_str(), m_Type);
            m_PreloadFuture.get(); // wait for callback
            m_PreloadPromise = promise<void>();
            m_PreloadRequested = false;
            STORAGE_DEBUG_INFO("--> waited %s\n", prof.ToString().c_str());
        }
    }

    void Storage::OffloadTriggerCallback(void* userData)
    {
        Storage* storage = (Storage*)userData;
        storage->Offload(true);
    }

    //////////////////////////////////////////////////////////////////////////
    void Storage::ScheduleOffload() const
    {
        CUDA_CHECK(cudaLaunchHostFunc(0, OffloadTriggerCallback, (void*)this));
    }

    //////////////////////////////////////////////////////////////////////////
    void Storage::Offload(bool force) const
    {
#ifdef DISABLE_OFFLOADABLE_SUPPORT
        STORAGE_DEBUG_INFO_NO_TS("<<< disabled.\n");
        IncDeviceRef(1); // artificially increase device ref count so we do not deallocate device memory where only valid copy of data is located
        return;
#endif

        if (!m_AllocSize)
            return;

        STORAGE_DEBUG_INFO("Offload '%s'[%d] %s ", m_Name.c_str(), m_Type, force ? "(FORCED)" : "");
        if (m_Type & ST_Offloadable)
        {
            NEURO_ASSERT(m_DataPtr, "Attempting to offload to deallocated host storage.");
            if (!m_DeviceDataPtr || m_DataLocation == Host)
            {
                STORAGE_DEBUG_INFO_NO_TS("<<< data already on host.\n");
                return;
            }

            NEURO_ASSERT(m_DataPtr && m_DeviceDataPtr, "");

            if (!force && SizeInBytes() < MIN_SIZE_TO_OFFLOAD)
            {
                STORAGE_DEBUG_INFO_NO_TS("<<< too small.\n");
                IncDeviceRef(1); // artificially increase device ref count so we do not deallocate device memory where only valid copy of data is located
                return;
            }

            if (m_OffloadRequested)
            {
                STORAGE_DEBUG_INFO_NO_TS("<<< requested already.\n");
            }
            else
            {
                m_OffloadFuture = m_OffloadPromise.get_future();
                m_OffloadRequested = true;
                m_OffloadDone = false;
                STORAGE_DEBUG_INFO_NO_TS("<<< requested - %d bytes.\n", (int)SizeInBytes());
                CUDA_CHECK(DeviceMemoryManager::Default().Offload((void*)m_DataPtr, (void*)m_DeviceDataPtr, SizeInBytes(), m_OffloadEvent, OffloadDoneCallback, (void*)this));
            }
        }
        else
            STORAGE_DEBUG_INFO_NO_TS("<<< not supported.\n");
    }

    //////////////////////////////////////////////////////////////////////////
    void Storage::Preload() const
    {
#ifdef DISABLE_OFFLOADABLE_SUPPORT
        STORAGE_DEBUG_INFO_NO_TS("<<< disabled.\n");
        return;
#endif

        if (!m_AllocSize)
            return;

        if (m_Type & ST_Offloadable)
        {
            // If we didn't finish offloading yet, cancel device memory deallocation on offload to avoid preload
            if (m_OffloadRequested)
            {
                unique_lock<mutex> mtx(m_OffloadDoneCallbackMtx);
                if (!m_OffloadDone)
                {
                    STORAGE_DEBUG_INFO("Cancelling free device memory on offload done '%s'\n", m_Name.c_str());
                    m_FreeDeviceMemOnOffloadDone = false;
                    NEURO_ASSERT(!m_FreePinnedMemOnOffloadDone, "Wtf.");
                }
            }

            if (m_DataLocation == Device)
            {                
                STORAGE_DEBUG_INFO("Preload '%s'[%d] <<< data already on device.\n", m_Name.c_str(), m_Type);
                return;
            }

            NEURO_ASSERT(m_DataPtr, "Attempting to preload from deallocated host storage.");
            if (!m_DeviceDataPtr)
                AllocateOnDevice();

            NEURO_ASSERT(m_DataPtr && m_DeviceDataPtr, "");

            if (m_PreloadRequested)
            {
                STORAGE_DEBUG_INFO("Preload '%s'[%d] <<< requested already.\n", m_Name.c_str(), m_Type);
            }
            else
            {
                m_PreloadFuture = m_PreloadPromise.get_future();
                m_PreloadRequested = true;
                STORAGE_DEBUG_INFO("Preload '%s'[%d] <<< requested.\n", m_Name.c_str(), m_Type);
                CUDA_CHECK(DeviceMemoryManager::Default().Preload((void*)m_DeviceDataPtr, (void*)m_DataPtr, SizeInBytes(), m_PreloadEvent, PreloadDoneCallback, (void*)this));
            }
        }
        else
            STORAGE_DEBUG_INFO("Preload '%s'[%d] <<< not supported.\n", m_Name.c_str(), m_Type);
    }

    //////////////////////////////////////////////////////////////////////////
    void Storage::CopyToDevice() const
    {
        if (m_PreloadRequested)
        {
            STORAGE_DEBUG_INFO("Copy to device '%s'[%d] <<< preload completed check\n", m_Name.c_str(), m_Type);
            WaitForPreload();
            NEURO_ASSERT(m_DataLocation == Device, "Data is not on device after preload...");
        }

        if (m_DataLocation == Device)
        {
            NEURO_ASSERT(m_DeviceDataPtr, "Data location is 'Device' but device data pointer is null.");
            return;
        }

        NVTXProfile p((string(__FUNCTION__) + " " + m_Name).c_str(), 0xFFB200FF);

        NEURO_ASSERT(m_DataLocation == Host, "Attempting to copy from unallocated host memory to device.");

        if (!m_DeviceDataPtr)
            AllocateOnDevice();

        NEURO_ASSERT(m_DataPtr, "");
        NEURO_ASSERT(m_DeviceDataPtr, "");

        STORAGE_DEBUG_INFO("Copy to device '%s'[%d]\n", m_Name.c_str(), m_Type);
        CUDA_CHECK(cudaMemcpy((void*)m_DeviceDataPtr, (void*)m_DataPtr, SizeInBytes(), cudaMemcpyHostToDevice));

        m_DataLocation = Device;
    }

    //////////////////////////////////////////////////////////////////////////
    void Storage::CopyToHost(bool allowAlloc) const
    {
        if (m_PreloadRequested)
        {
            STORAGE_DEBUG_INFO("Copy to host '%s'[%d] <<< preload completed check\n", m_Name.c_str(), m_Type);
            WaitForPreload();
        }

        if (m_DataLocation == Host)
        {
            NEURO_ASSERT(m_DataPtr, "Data location is 'Host' but data pointer is null.");
            return;
        }

        NVTXProfile p((string(__FUNCTION__) + " " + m_Name).c_str(), 0xFFB200FF);

        if (allowAlloc && !m_DataPtr)
        {
            AllocateOnHost();
        }
        else
        {
            NEURO_ASSERT(m_DataLocation != None, "Attempting to copy to unallocated host memory");
            NEURO_ASSERT(m_DataPtr && m_DeviceDataPtr, "");

            if (m_OffloadRequested && (m_Type & ST_Offloadable))
            {
                STORAGE_DEBUG_INFO("Copy to host '%s'[%d] <<< offload completed check\n", m_Name.c_str(), m_Type);
                WaitForOffload();
            }
            else
            {
                STORAGE_DEBUG_INFO("Copy to host '%s'[%d] <<< %s\n", m_Name.c_str(), m_Type, (m_Type & ST_Offloadable) ? "offloadable but offload wasn't requested" : "not offloadable");
                CUDA_CHECK(cudaMemcpy((void*)m_DataPtr, (void*)m_DeviceDataPtr, SizeInBytes(), cudaMemcpyDeviceToHost));
            }
        }

        m_DataLocation = Host;
    }

    //////////////////////////////////////////////////////////////////////////
    void Storage::SyncToHost() const
    {
        if (m_DataLocation == Host)
        {
            NEURO_ASSERT(m_DataPtr, "Data location is 'Host' but data pointer is null.");
            return;
        }

        NVTXProfile p((string(__FUNCTION__) + " " + m_Name).c_str(), 0xFFB200FF);

        NEURO_ASSERT(m_DataLocation != None, "Attempting to sync to unallocated host memory");
        NEURO_ASSERT(m_DataPtr && m_DeviceDataPtr, "");

        STORAGE_DEBUG_INFO("Sync to host '%s'\n", m_Name.c_str());
        CUDA_CHECK(cudaMemcpy((void*)m_DataPtr, (void*)m_DeviceDataPtr, SizeInBytes(), cudaMemcpyDeviceToHost));
    }

    //////////////////////////////////////////////////////////////////////////
    void Storage::OverrideHost()
    {
        if (m_DataLocation == Host)
        {
            NEURO_ASSERT(m_DataPtr, "Data location is 'Host' but data pointer is null.");
            return;
        }

        if (!m_DataPtr)
            AllocateOnHost();
        m_DataLocation = Host;
        STORAGE_DEBUG_INFO("Override host '%s'[%d]\n", m_Name.c_str(), m_Type);
    }

    //////////////////////////////////////////////////////////////////////////
    void Storage::OverrideDevice()
    {
        if (m_DataLocation == Device)
        {
            NEURO_ASSERT(m_DeviceDataPtr, "Data location is 'Device' but device data pointer is null.");
            return;
        }

        if (!m_DataPtr)
            AllocateOnHost();
        if (!m_DeviceDataPtr)
            AllocateOnDevice();
        m_DataLocation = Device;
        STORAGE_DEBUG_INFO("Override device '%s'[%d]\n", m_Name.c_str(), m_Type);
    }

    //////////////////////////////////////////////////////////////////////////
    void Storage::ResetDeviceRef(size_t n) const
    {
        if (!(m_Type & ST_DeviceRefCounted))
            return;

        STORAGE_DEBUG_INFO("Device ref count reset '%s' to %zu.\n", m_Name.c_str(), n);
        m_DeviceDataRefCount = (int)n;
    }

    //////////////////////////////////////////////////////////////////////////
    void Storage::IncDeviceRef(size_t n) const
    {
        if (!(m_Type & ST_DeviceRefCounted))
            return;

        NEURO_ASSERT(m_Type & ST_DeviceRefCounted, "Increasing ref count for non-refcounted storage.");
        m_DeviceDataRefCount += (int)n;
        STORAGE_DEBUG_INFO("Device ref count increased '%s' by %zu <<< currently %d.\n", m_Name.c_str(), n, m_DeviceDataRefCount);
    }

    //////////////////////////////////////////////////////////////////////////
    void Storage::DecDeviceRef(size_t n)
    {
        if (!(m_Type & ST_DeviceRefCounted))
            return;

        NEURO_ASSERT(m_Type & ST_DeviceRefCounted, "Decreasing ref count for non-refcounted storage.");
        NEURO_ASSERT(n <= m_DeviceDataRefCount, "Over-decresing ref count.");
        m_DeviceDataRefCount -= (int)n;
        STORAGE_DEBUG_INFO("Device ref count decreased '%s' by %zu <<< currently %d.\n", m_Name.c_str(), n, m_DeviceDataRefCount);

        if (m_DeviceDataRefCount <= 0 && (m_Type & ST_DeviceRefCounted))
        {
            STORAGE_DEBUG_INFO("Device ref count zeroed '%s' <<< deallocating device memory.\n", m_Name.c_str());
            FreeOnDevice();
        }
    }

    //////////////////////////////////////////////////////////////////////////
    void Storage::ResetRef(size_t n) const
    {
        STORAGE_DEBUG_INFO("Ref count reset '%s' to %zu.\n", m_Name.c_str(), n);
        m_DataRefCount = (int)n;
    }

    //////////////////////////////////////////////////////////////////////////
    void Storage::IncRef(size_t n) const
    {
        NEURO_ASSERT(m_Type & ST_RefCounted, "Increasing ref count for non-refcounted storage.");
        m_DataRefCount += (int)n;
        STORAGE_DEBUG_INFO("Ref count increased '%s' by %zu <<< currently %d.\n", m_Name.c_str(), n, m_DataRefCount);
    }

    //////////////////////////////////////////////////////////////////////////
    void Storage::DecRef(size_t n)
    {
        NEURO_ASSERT(m_Type & ST_RefCounted, "Decreasing ref count for non-refcounted storage.");
        NEURO_ASSERT(n <= m_DataRefCount, "Over-decresing ref count.");
        m_DataRefCount -= (int)n;
        STORAGE_DEBUG_INFO("Ref count decreased '%s' by %zu <<< currently %d.\n", m_Name.c_str(), n, m_DataRefCount);

        if (m_DataRefCount <= 0 && (m_Type & ST_RefCounted))
        {
            STORAGE_DEBUG_INFO("Ref count zeroed '%s' <<< deallocating memory.\n", m_Name.c_str());
            FreeOnDevice();
            FreeOnHost();
        }
    }

    //////////////////////////////////////////////////////////////////////////
    float* Storage::Data()
    {
        if (!m_DataPtr)
            AllocateOnHost();

        NEURO_ASSERT(m_DataLocation == Host, "Trying to access data that is currently located on device or unallocated.");
        return m_DataPtr;
    }

    //////////////////////////////////////////////////////////////////////////
    const float* Storage::Data() const
    {
        NEURO_ASSERT(m_DataLocation == Host, "Trying to access data that is currently located on device or unallocated.");
        return m_DataPtr;
    }

    //////////////////////////////////////////////////////////////////////////
    float* Storage::DeviceData()
    {
        NEURO_ASSERT(m_DeviceDataPtr, "Attempting to write to unallocated device memory.");
        NEURO_ASSERT(m_DataLocation == Device, "Attempting to write to data not located on device.");
        NEURO_ASSERT(!m_OffloadRequested || m_OffloadDone, "Attempting to write to data being offloaded from device.");
        return m_DeviceDataPtr;
    }

    //////////////////////////////////////////////////////////////////////////
    const float* Storage::DeviceData() const
    {
        NEURO_ASSERT(m_DataLocation == Device, "Trying to access data that is currently located on host.");
        return m_DeviceDataPtr;
    }

    //////////////////////////////////////////////////////////////////////////
    void Storage::CopyWithinDevice(void* destDevPtr) const
    {
        CopyWithinDevice(destDevPtr, (void*)m_DeviceDataPtr, SizeInBytes());
    }

    //////////////////////////////////////////////////////////////////////////
    void Storage::CopyWithinDevice(void* destDevPtr, const void* srcDevPtr, size_t sizeInBytes) const
    {
        NEURO_ASSERT(srcDevPtr, "Invalid device pointer.");
        NEURO_ASSERT(destDevPtr, "Invalid destination device pointer.");
        CUDA_CHECK(cudaMemcpy(destDevPtr, srcDevPtr, sizeInBytes, cudaMemcpyDeviceToDevice));
        // make sure CPU waits for this to be finished (by default this operation is asynchronous with respect to the host code)
        cudaStreamSynchronize(0);
    }

    //////////////////////////////////////////////////////////////////////////
    void Storage::CopyWithinHost(void* destPtr) const
    {
        CopyWithinHost(destPtr, m_DataPtr, SizeInBytes());
    }

    //////////////////////////////////////////////////////////////////////////
    void Storage::CopyWithinHost(void* destPtr, const void* srcPtr, size_t sizeInBytes) const
    {
        NEURO_ASSERT(srcPtr, "Invalid pointer.");
        NEURO_ASSERT(destPtr, "Invalid destination pointer.");
        memcpy(destPtr, srcPtr, sizeInBytes);
    }
}
