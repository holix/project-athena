//
//  Scene.cpp
//  render/src/render
//
//  Created by Sam Gateau on 1/11/15.
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
#include "Scene.h"

#include <numeric>
#include <gpu/Batch.h>
#include "Logging.h"
#include "TransitionStage.h"

using namespace render;

void Transaction::resetItem(ItemID id, const PayloadPointer& payload) {
    if (payload) {
        _resetItems.emplace_back(id);
        _resetPayloads.emplace_back(payload);
    } else {
        qCDebug(renderlogging) << "WARNING: Transaction::resetItem with a null payload!";
        removeItem(id);
    }
}

void Transaction::removeItem(ItemID id) {
    _removedItems.emplace_back(id);
}

void Transaction::addTransitionToItem(ItemID id, Transition::Type transition, ItemID boundId) {
    _transitioningItems.emplace_back(id);
    _transitioningItemBounds.emplace_back(boundId);
    _transitionTypes.emplace_back(transition);
}

void Transaction::removeTransitionFromItem(ItemID id) {
    _transitioningItems.emplace_back(id);
    _transitioningItemBounds.emplace_back(render::Item::INVALID_ITEM_ID);
    _transitionTypes.emplace_back(render::Transition::NONE);
}

void Transaction::updateItem(ItemID id, const UpdateFunctorPointer& functor) {
    _updatedItems.emplace_back(id);
    _updateFunctors.emplace_back(functor);
}

void Transaction::resetSelection(const Selection& selection) {
    _resetSelections.emplace_back(selection);
}

void Transaction::merge(const Transaction& transaction) {
    _resetItems.insert(_resetItems.end(), transaction._resetItems.begin(), transaction._resetItems.end());
    _resetPayloads.insert(_resetPayloads.end(), transaction._resetPayloads.begin(), transaction._resetPayloads.end());
    _removedItems.insert(_removedItems.end(), transaction._removedItems.begin(), transaction._removedItems.end());
    _updatedItems.insert(_updatedItems.end(), transaction._updatedItems.begin(), transaction._updatedItems.end());
    _updateFunctors.insert(_updateFunctors.end(), transaction._updateFunctors.begin(), transaction._updateFunctors.end());
    _resetSelections.insert(_resetSelections.end(), transaction._resetSelections.begin(), transaction._resetSelections.end());
    _transitioningItems.insert(_transitioningItems.end(), transaction._transitioningItems.begin(), transaction._transitioningItems.end());
    _transitioningItemBounds.insert(_transitioningItemBounds.end(), transaction._transitioningItemBounds.begin(), transaction._transitioningItemBounds.end());
    _transitionTypes.insert(_transitionTypes.end(), transaction._transitionTypes.begin(), transaction._transitionTypes.end());
}


Scene::Scene(glm::vec3 origin, float size) :
    _masterSpatialTree(origin, size)
{
    _items.push_back(Item()); // add the itemID #0 to nothing
}

Scene::~Scene() {
    qCDebug(renderlogging) << "Scene::~Scene()";
}

ItemID Scene::allocateID() {
    // Just increment and return the proevious value initialized at 0
    return _IDAllocator.fetch_add(1);
}

bool Scene::isAllocatedID(const ItemID& id) const {
    return Item::isValidID(id) && (id < _numAllocatedItems.load());
}

/// Enqueue change batch to the scene
void Scene::enqueueTransaction(const Transaction& transaction) {
    _transactionQueueMutex.lock();
    _transactionQueue.push(transaction);
    _transactionQueueMutex.unlock();
}

void consolidateTransaction(TransactionQueue& queue, Transaction& singleBatch) {
    while (!queue.empty()) {
        const auto& transaction = queue.front();
        singleBatch.merge(transaction);
        queue.pop();
    };
}
 
void Scene::processTransactionQueue() {
    PROFILE_RANGE(render, __FUNCTION__);
    Transaction consolidatedTransaction;

    {
        std::unique_lock<std::mutex> lock(_transactionQueueMutex);
        consolidateTransaction(_transactionQueue, consolidatedTransaction);
    }
    
    {
        std::unique_lock<std::mutex> lock(_itemsMutex);
        // Here we should be able to check the value of last ItemID allocated 
        // and allocate new items accordingly
        ItemID maxID = _IDAllocator.load();
        if (maxID > _items.size()) {
            _items.resize(maxID + 100); // allocate the maxId and more
        }
        // Now we know for sure that we have enough items in the array to
        // capture anything coming from the transaction

        // resets and potential NEW items
        resetItems(consolidatedTransaction._resetItems, consolidatedTransaction._resetPayloads);

        // Update the numItemsAtomic counter AFTER the reset changes went through
        _numAllocatedItems.exchange(maxID);

        // updates
        updateItems(consolidatedTransaction._updatedItems, consolidatedTransaction._updateFunctors);

        // removes
        removeItems(consolidatedTransaction._removedItems);

        // Transitions
        transitionItems(consolidatedTransaction._transitioningItems, consolidatedTransaction._transitionTypes, consolidatedTransaction._transitioningItemBounds);

        // Update the numItemsAtomic counter AFTER the pending changes went through
        _numAllocatedItems.exchange(maxID);
    }

    if (consolidatedTransaction.touchTransactions()) {
        std::unique_lock<std::mutex> lock(_selectionsMutex);

        // resets and potential NEW items
        resetSelections(consolidatedTransaction._resetSelections);
    }
}

void Scene::resetItems(const ItemIDs& ids, Payloads& payloads) {
    auto resetPayload = payloads.begin();
    for (auto resetID : ids) {
        // Access the true item
        auto& item = _items[resetID];
        auto oldKey = item.getKey();
        auto oldCell = item.getCell();

        // Reset the item with a new payload
        item.resetPayload(*resetPayload);
        auto newKey = item.getKey();

        // Update the item's container
        assert((oldKey.isSpatial() == newKey.isSpatial()) || oldKey._flags.none());
        if (newKey.isSpatial()) {
            auto newCell = _masterSpatialTree.resetItem(oldCell, oldKey, item.getBound(), resetID, newKey);
            item.resetCell(newCell, newKey.isSmall());
        } else {
            _masterNonspatialSet.insert(resetID);
        }

        // next loop
        resetPayload++;
    }
}

void Scene::removeItems(const ItemIDs& ids) {
    for (auto removedID :ids) {
        // Access the true item
        auto& item = _items[removedID];
        auto oldCell = item.getCell();
        auto oldKey = item.getKey();

        // Remove the item
        if (oldKey.isSpatial()) {
            _masterSpatialTree.removeItem(oldCell, oldKey, removedID);
        } else {
            _masterNonspatialSet.erase(removedID);
        }

        // If there is a transition on this item, remove it
        if (item.getTransitionId() != render::TransitionStage::INVALID_INDEX) {
            auto transitionStage = getStage<TransitionStage>(TransitionStage::getName());
            transitionStage->removeTransition(item.getTransitionId());
        }

        // Kill it
        item.kill();
    }
}

void Scene::updateItems(const ItemIDs& ids, UpdateFunctors& functors) {

    auto updateFunctor = functors.begin();
    for (auto updateID : ids) {
        if (updateID == Item::INVALID_ITEM_ID) {
            updateFunctor++;
            continue;
        }

        // Access the true item
        auto& item = _items[updateID];
        auto oldCell = item.getCell();
        auto oldKey = item.getKey();

        // Update the item
        item.update((*updateFunctor));
        auto newKey = item.getKey();

        // Update the item's container
        if (oldKey.isSpatial() == newKey.isSpatial()) {
            if (newKey.isSpatial()) {
                auto newCell = _masterSpatialTree.resetItem(oldCell, oldKey, item.getBound(), updateID, newKey);
                item.resetCell(newCell, newKey.isSmall());
            }
        } else {
            if (newKey.isSpatial()) {
                _masterNonspatialSet.erase(updateID);

                auto newCell = _masterSpatialTree.resetItem(oldCell, oldKey, item.getBound(), updateID, newKey);
                item.resetCell(newCell, newKey.isSmall());
            } else {
                _masterSpatialTree.removeItem(oldCell, oldKey, updateID);
                item.resetCell();

                _masterNonspatialSet.insert(updateID);
            }
        }


        // next loop
        updateFunctor++;
    }
}

void Scene::transitionItems(const ItemIDs& ids, const TransitionTypes& types, const ItemIDs& boundIds) {
    auto transitionType = types.begin();
    auto boundId = boundIds.begin();
    auto transitionStage = getStage<TransitionStage>(TransitionStage::getName());

    for (auto itemId : ids) {
        // Access the true item
        const auto& item = _items[itemId];
        if (item.exist()) {
            auto transitionId = INVALID_INDEX;

            // Remove pre-existing transition, if need be
            if (item.getTransitionId() != render::TransitionStage::INVALID_INDEX) {
                transitionStage->removeTransition(item.getTransitionId());
            }
            // Add a new one.
            if (*transitionType != Transition::NONE) {
                transitionId = transitionStage->addTransition(itemId, *transitionType, *boundId);
            }

            setItemTransition(itemId, transitionId);
        }

        // next loop
        transitionType++;
        boundId++;
    }
}

void Scene::collectSubItems(ItemID parentId, ItemIDs& subItems) const {
    // Access the true item
    auto& item = _items[parentId];

    if (item.exist()) {
        // Recursivelly collect the subitems
        auto subItemBeginIndex = subItems.size();
        auto subItemCount = item.fetchMetaSubItems(subItems);
        for (auto i = subItemBeginIndex; i < (subItemBeginIndex + subItemCount); i++) {
            collectSubItems(subItems[i], subItems);
        }
    }
}

void Scene::setItemTransition(ItemID itemId, Index transitionId) {
    // Access the true item
    auto& item = _items[itemId];

    if (item.exist()) {
        ItemIDs subItems;

        item.setTransitionId(transitionId);

        // Sub-items share the same transition Id
        collectSubItems(itemId, subItems);
        for (auto subItemId : subItems) {
            auto& subItem = _items[subItemId];
            subItem.setTransitionId(transitionId);
        }
    }
    else {
        qWarning() << "Collecting sub items on item without payload";
    }
}

void Scene::resetItemTransition(ItemID itemId) {
    // Access the true item
    auto& item = _items[itemId];
    auto transitionStage = getStage<TransitionStage>(TransitionStage::getName());

    transitionStage->removeTransition(item.getTransitionId());
    setItemTransition(itemId, Transition::NONE);
}

// THis fucntion is thread safe
Selection Scene::getSelection(const Selection::Name& name) const {
    std::unique_lock<std::mutex> lock(_selectionsMutex);
    auto found = _selections.find(name);
    if (found == _selections.end()) {
        return Selection();
    } else {
        return (*found).second;
    }
}

void Scene::resetSelections(const Selections& selections) {
    for (auto selection : selections) {
        auto found = _selections.find(selection.getName());
        if (found == _selections.end()) {
            _selections.insert(SelectionMap::value_type(selection.getName(), selection));
        } else {
            (*found).second = selection;
        }
    }
}

// Access a particular Stage (empty if doesn't exist)
// Thread safe
StagePointer Scene::getStage(const Stage::Name& name) const {
    std::unique_lock<std::mutex> lock(_stagesMutex);
    auto found = _stages.find(name);
    if (found == _stages.end()) {
        return StagePointer();
    } else {
        return (*found).second;
    }

}

void Scene::resetStage(const Stage::Name& name, const StagePointer& stage) {
    std::unique_lock<std::mutex> lock(_stagesMutex);
    auto found = _stages.find(name);
    if (found == _stages.end()) {
        _stages.insert(StageMap::value_type(name, stage));
    } else {
        (*found).second = stage;
    }
}