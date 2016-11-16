// Copyright 2016 Yahoo Inc. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#pragma once

#include <vector>
#include <deque>
#include <vespa/vespalib/util/alloc.h>
#include <vespa/vespalib/util/array.h>

#include "entryref.h"
#include <vespa/vespalib/util/generationhandler.h>

namespace search {
namespace datastore {

class BufferTypeBase
{
protected:
    uint32_t _clusterSize;	// Number of elements in an allocation unit
    uint32_t _minClusters;	// Minimum number of clusters to allocate
    uint32_t _maxClusters;	// Maximum number of clusters to allocate
    uint32_t _activeBuffers;
    uint32_t _holdBuffers;
    size_t _activeUsedElems;	// used elements in all but last active buffer
    size_t _holdUsedElems;	// used elements in all held buffers
    const size_t *_lastUsedElems; // used elements in last active buffer

public:
    
    BufferTypeBase(const BufferTypeBase &rhs) = delete;
    BufferTypeBase & operator=(const BufferTypeBase &rhs) = delete;
    BufferTypeBase(uint32_t clusterSize, uint32_t minClusters, uint32_t maxClusters);
    virtual ~BufferTypeBase();
    virtual void destroyElements(void *buffer, size_t numElements) = 0;
    virtual void fallbackCopy(void *newBuffer, const void *oldBuffer, size_t numElements) = 0;
    // Return number of reserved elements at start of buffer, to avoid
    // invalid reference and handle data at negative offset (alignment
    // hacks) as used by dense tensor store.
    virtual size_t getReservedElements(uint32_t bufferId) const;
    // Initialize reserved elements at start of buffer.
    virtual void initializeReservedElements(void *buffer, size_t reservedElements) = 0;
    virtual size_t elementSize() const = 0;
    virtual void cleanHold(void *buffer, uint64_t offset, uint64_t len) = 0;
    uint32_t getClusterSize() const { return _clusterSize; }
    void flushLastUsed();
    virtual void onActive(uint32_t bufferId, size_t *usedElems, size_t &deadElems, void *buffer);
    void onHold(const size_t *usedElems);
    virtual void onFree(size_t usedElems);

    /**
     * Calculate number of clusters to allocate for new buffer.
     *
     * @param sizeNeeded number of elements needed now
     * @param clusterRefSize number of clusters expressable via reference type
     *
     * @return number of clusters to allocate for new buffer
     */
    virtual size_t calcClustersToAlloc(uint32_t bufferId, size_t sizeNeeded, uint64_t clusterRefSize) const;

    uint32_t getActiveBuffers() const { return _activeBuffers; }
};


template <typename EntryType>
class BufferType : public  BufferTypeBase
{
public:
    EntryType _emptyEntry;

    BufferType(const BufferType &rhs) = delete;
    BufferType & operator=(const BufferType &rhs) = delete;
    BufferType(uint32_t clusterSize, uint32_t minClusters, uint32_t maxClusters)
        : BufferTypeBase(clusterSize, minClusters, maxClusters),
          _emptyEntry()
    { }

    void destroyElements(void *buffer, size_t numElements) override;
    void fallbackCopy(void *newBuffer, const void *oldBuffer, size_t numElements) override;
    void initializeReservedElements(void *buffer, size_t reservedElements) override;
    void cleanHold(void *buffer, uint64_t offset, uint64_t len) override;
    size_t elementSize() const override { return sizeof(EntryType); }
};


template <typename EntryType>
void
BufferType<EntryType>::destroyElements(void *buffer, size_t numElements)
{
    EntryType *e = static_cast<EntryType *>(buffer);
    for (size_t j = numElements; j != 0; --j) {
        e->~EntryType();
        ++e;
    }
}


template <typename EntryType>
void
BufferType<EntryType>::fallbackCopy(void *newBuffer,
                                    const void *oldBuffer,
                                    size_t numElements)
{
    EntryType *d = static_cast<EntryType *>(newBuffer);
    const EntryType *s = static_cast<const EntryType *>(oldBuffer);
    for (size_t j = numElements; j != 0; --j) {
        new (static_cast<void *>(d)) EntryType(*s);
        ++s;
        ++d;
    }
}


template <typename EntryType>
void
BufferType<EntryType>::initializeReservedElements(void *buffer, size_t reservedElems)
{
    EntryType *e = static_cast<EntryType *>(buffer);
    for (size_t j = reservedElems; j != 0; --j) {
        new (static_cast<void *>(e)) EntryType(_emptyEntry);
        ++e;
    }
}


template <typename EntryType>
void
BufferType<EntryType>::cleanHold(void *buffer, uint64_t offset, uint64_t len)
{
    EntryType *e = static_cast<EntryType *>(buffer) + offset;
    for (size_t j = len; j != 0; --j) {
        *e = _emptyEntry;
        ++e;
    }
}


class BufferState
{
public:
    typedef vespalib::alloc::Alloc Alloc;

    class FreeListList
    {
    public:
        BufferState *_head;

        FreeListList() : _head(NULL) { }
        ~FreeListList();
    };

    typedef vespalib::Array<EntryRef> FreeList;

    enum State {
        FREE,
        ACTIVE,
        HOLD
    };

    size_t _usedElems;
    size_t _allocElems;
    uint64_t _deadElems;
    State _state;
    bool  _disableElemHoldList;
    uint64_t _holdElems;
    FreeList _freeList;
    FreeListList *_freeListList;	// non-NULL if free lists are enabled

    // NULL pointers if not on circular list of buffer states with free elems
    BufferState *_nextHasFree;
    BufferState *_prevHasFree;

    BufferTypeBase *_typeHandler;
    uint32_t        _typeId;
    uint32_t        _clusterSize;
    bool            _compacting;

    /*
     * TODO: Check if per-buffer free lists are useful, or if
     *compaction should always be used to free up whole buffers.
     */

    BufferState();
    ~BufferState();

    /**
     * Transition from FREE to ACTIVE state.
     *
     * @param bufferId		Id of buffer to be active.
     * @param typeId		registered data type for buffer.
     * @param typeHandler	type handler for registered data type.
     * @param sizeNeeded	Number of elements needed to be free
     * @param maxSize		number of clusters expressable via reference
     * 				type
     * @param buffer		start of buffer.
     */
    void
    onActive(uint32_t bufferId, uint32_t typeId, BufferTypeBase *typeHandler,
             size_t sizeNeeded, size_t maxSize, void *&buffer);

    /**
     * Transition from ACTIVE to HOLD state.
     */
    void onHold();

    /**
     * Transition from HOLD to FREE state.
     */
    void onFree(void *&buffer);

    /**
     * Set list of buffer states with nonempty free lists.
     *
     * @param freeListList	List of buffer states.  If NULL then free lists
     *				are disabled.
     */
    void setFreeListList(FreeListList *freeListList);

    /**
     * Add buffer state to list of buffer states with nonempty free lists.
     */
    void addToFreeListList();

    /**
     * Remove buffer state from list of buffer states with nonempty free lists.
     */
    void removeFromFreeListList();

    /**
     * Disable hold of elements, just mark then as dead without
     * cleanup.  Typically used when tearing down data structure in a
     * controlled manner.
     */
    void disableElemHoldList();

    /**
     * Pop element from free list.
     */
    EntryRef popFreeList() {
        EntryRef ret = _freeList.back();
        _freeList.pop_back();
        if (_freeList.empty()) {
            removeFromFreeListList();
        }
        _deadElems -= _clusterSize;
        return ret;
    }

    size_t size() const { return _usedElems; }
    size_t capacity() const { return _allocElems; }
    size_t remaining() const { return _allocElems - _usedElems; }
    void pushed_back(uint64_t len) { _usedElems += len; }
    void cleanHold(void *buffer, uint64_t offset, uint64_t len) { _typeHandler->cleanHold(buffer, offset, len); }
    void dropBuffer(void *&buffer);
    uint32_t getTypeId() const { return _typeId; }
    uint32_t getClusterSize() const { return _clusterSize; }
    uint64_t getDeadElems() const { return _deadElems; }
    bool getCompacting() const { return _compacting; }
    void setCompacting() { _compacting = true; }
    void fallbackResize(uint32_t bufferId, uint64_t sizeNeeded, size_t maxClusters, void *&buffer, Alloc &holdBuffer);

    bool isActive(uint32_t typeId) const {
        return ((_state == ACTIVE) && (_typeId == typeId));
    }
    bool isActive() const { return (_state == ACTIVE); }
    bool isOnHold() const { return (_state == HOLD); }
    bool isFree() const { return (_state == FREE); }
    const BufferTypeBase *getTypeHandler() const { return _typeHandler; }

private:
    Alloc _buffer;
};


} // namespace search::datastore
} // namespace search

