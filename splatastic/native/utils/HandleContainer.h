#pragma once
#include <utils/Assert.h>
#include <vector>
#include <functional>

namespace splatastic
{

template<typename HandleType, typename DataType, int fixedContainerSize = -1>
class HandleContainer
{
public:
    using BaseHandleType = typename HandleType::BaseType;
    using OnElementFn = std::function<void(HandleType handle, DataType& data)>;

    HandleContainer()
    {
        if(fixedContainerSize != -1)
            m_data.reserve(fixedContainerSize);
    }

    DataType& allocate(HandleType& outHandle)
    {
        if (m_freeHandles.empty())
        {
            if (fixedContainerSize != -1 && m_data.size() == fixedContainerSize)
            {
                SPT_ASSERT_MSG(false, "Fixed sized container exceeded capacity.");
                return m_data.back().data;
            }

            outHandle.handleId = (BaseHandleType)m_data.size();
            m_data.emplace_back();
        }
        else
        {
            outHandle.handleId = m_freeHandles.back();
            m_freeHandles.pop_back();
        }
        auto& container = m_data[outHandle.handleId];
        container.handle = outHandle; 
        container.valid = true;
        ++m_numElements;
        return container.data;
    }

    void free(HandleType handle, bool resetObject = true)
    {
        if (handle.handleId >= (BaseHandleType)m_data.size())
            return;

        m_freeHandles.push_back(handle);
        m_data[handle].valid = false;
        if (resetObject)
            m_data[handle].data = DataType();
        --m_numElements;
    }

    void forEach(OnElementFn elementfn)
    {
        for (DataContainer& c : m_data)
        {
            if (c.valid)
                elementfn(c.handle, c.data);
        }
    }

    bool contains(HandleType h) const
    {
        if (h.handleId >= (BaseHandleType)m_data.size() || !h.valid())
            return false;

        return m_data[h.handleId].valid;
    }

    DataType& operator[](HandleType h)
    {
        return m_data[h.handleId].data;
    }

    const DataType& operator[](HandleType h) const
    {
        return m_data[h.handleId].data;
    }

    const int elementsCount() const { return m_numElements; }

    void clear()
    {
        m_numElements = 0;
        m_freeHandles.clear();
        m_data.clear();
    }

private:
    struct DataContainer
    {
        HandleType handle;
        bool valid = false;
        DataType data;
    };

    int m_numElements = 0;
    std::vector<HandleType> m_freeHandles;
    std::vector<DataContainer> m_data;
};

}
