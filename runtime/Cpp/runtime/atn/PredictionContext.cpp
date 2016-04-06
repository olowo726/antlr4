﻿/*
 * [The "BSD license"]
 *  Copyright (c) 2016 Mike Lischke
 *  Copyright (c) 2013 Terence Parr
 *  Copyright (c) 2013 Dan McLaughlin
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. The name of the author may not be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 *  IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 *  OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 *  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 *  NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 *  THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "EmptyPredictionContext.h"
#include "MurmurHash.h"
#include "ArrayPredictionContext.h"
#include "RuleContext.h"
#include "RuleTransition.h"
#include "stringconverter.h"
#include "Arrays.h"
#include "CPPUtils.h"

#include "PredictionContext.h"

using namespace org::antlr::v4::runtime::misc;
using namespace org::antlr::v4::runtime::atn;

using namespace antlrcpp;

int PredictionContext::globalNodeCount = 0;
const std::shared_ptr<EmptyPredictionContext> PredictionContext::EMPTY;
const int PredictionContext::EMPTY_RETURN_STATE;
const int PredictionContext::INITIAL_HASH;

PredictionContext::PredictionContext(size_t cachedHashCode) : id(globalNodeCount++), cachedHashCode(cachedHashCode)  {
}

PredictionContextRef PredictionContext::fromRuleContext(const ATN &atn, RuleContext *outerContext) {
  if (outerContext == nullptr) {
    outerContext = (RuleContext*)RuleContext::EMPTY;
  }

  // if we are in RuleContext of start rule, s, then PredictionContext
  // is EMPTY. Nobody called us. (if we are empty, return empty)
  if (outerContext->parent == nullptr || outerContext == (RuleContext*)RuleContext::EMPTY) {
    return PredictionContext::EMPTY;
  }

  // If we have a parent, convert it to a PredictionContext graph
  PredictionContextRef parent = PredictionContext::fromRuleContext(atn, outerContext->parent);

  ATNState *state = atn.states[(size_t)outerContext->invokingState];
  RuleTransition *transition = (RuleTransition *)state->transition(0);
  return SingletonPredictionContext::create(parent, transition->followState->stateNumber);
}

bool PredictionContext::operator != (const PredictionContext &o) const {
  return !(*this == o);
};

bool PredictionContext::isEmpty() const {
  return this == EMPTY.get();
}

bool PredictionContext::hasEmptyPath() const {
  return getReturnState(size() - 1) == EMPTY_RETURN_STATE;
}

size_t PredictionContext::hashCode() const {
  return cachedHashCode;
}

size_t PredictionContext::calculateEmptyHashCode() {
  size_t hash = MurmurHash::initialize(INITIAL_HASH);
  hash = MurmurHash::finish(hash, 0);
  return hash;
}

size_t PredictionContext::calculateHashCode(std::weak_ptr<PredictionContext> parent, int returnState) {
  size_t hash = MurmurHash::initialize(INITIAL_HASH);
  hash = MurmurHash::update(hash, (size_t)parent.lock().get());
  hash = MurmurHash::update(hash, (size_t)returnState);
  hash = MurmurHash::finish(hash, 2);
  return hash;
}

size_t PredictionContext::calculateHashCode(const std::vector<std::weak_ptr<PredictionContext>> &parents,
                                            const std::vector<int> &returnStates) {
  size_t hash = MurmurHash::initialize(INITIAL_HASH);

  for (auto parent : parents) {
    hash = MurmurHash::update(hash, (size_t)parent.lock().get());
  }

  for (auto returnState : returnStates) {
    hash = MurmurHash::update(hash, (size_t)returnState);
  }

  return MurmurHash::finish(hash, parents.size() + returnStates.size());
}

PredictionContextRef PredictionContext::merge(PredictionContextRef a,
  PredictionContextRef b, bool rootIsWildcard, PredictionContextMergeCache *mergeCache) {
  
  assert(a && b);

  // share same graph if both same
  if (a == b) {
    return a;
  }

  if (is<SingletonPredictionContext>(a) && is<SingletonPredictionContext>(b)) {
    return mergeSingletons(std::dynamic_pointer_cast<SingletonPredictionContext>(a),
                           std::dynamic_pointer_cast<SingletonPredictionContext>(b), rootIsWildcard, mergeCache);
  }

  // At least one of a or b is array
  // If one is $ and rootIsWildcard, return $ as * wildcard
  if (rootIsWildcard) {
    if (is<EmptyPredictionContext>(a)) {
      return a;
    }
    if (is<EmptyPredictionContext>(b)) {
      return b;
    }
  }

  // convert singleton so both are arrays to normalize
  if (is<SingletonPredictionContext>(a)) {
    a.reset(new ArrayPredictionContext(std::dynamic_pointer_cast<SingletonPredictionContext>(a)));
  }
  if (is<SingletonPredictionContext>(b)) {
    b.reset(new ArrayPredictionContext(std::dynamic_pointer_cast<SingletonPredictionContext>(b)));
  }
  return mergeArrays(std::dynamic_pointer_cast<ArrayPredictionContext>(a),
                     std::dynamic_pointer_cast<ArrayPredictionContext>(b), rootIsWildcard, mergeCache);
}

PredictionContextRef PredictionContext::mergeSingletons(SingletonPredictionContextRef a,
  SingletonPredictionContextRef b, bool rootIsWildcard, PredictionContextMergeCache *mergeCache) {

  if (mergeCache != nullptr) { // Can be null if not given to the ATNState from which this call originates.
    auto iterator = mergeCache->find({ a.get(), b.get() });
    if (iterator != mergeCache->end()) {
      return iterator->second;
    }
    iterator = mergeCache->find({ b.get(), a.get() });
    if (iterator != mergeCache->end()) {
      return iterator->second;
    }
  }

  PredictionContextRef rootMerge = mergeRoot(a, b, rootIsWildcard);
  if (rootMerge) {
    if (mergeCache != nullptr) {
      (*mergeCache)[{ a.get(), b.get() }] = rootMerge;
    }
    return rootMerge;
  }

  PredictionContextRef parentA = a->parent.lock();
  PredictionContextRef parentB = b->parent.lock();
  if (a->returnState == b->returnState) { // a == b
    PredictionContextRef parent = merge(parentA, parentB, rootIsWildcard, mergeCache);

    // If parent is same as existing a or b parent or reduced to a parent, return it.
    if (parent == parentA) { // ax + bx = ax, if a=b
      return a;
    }
    if (parent == parentB) { // ax + bx = bx, if a=b
      return b;
    }

    // else: ax + ay = a'[x,y]
    // merge parents x and y, giving array node with x,y then remainders
    // of those graphs.  dup a, a' points at merged array
    // new joined parent so create new singleton pointing to it, a'
    PredictionContextRef a_ = SingletonPredictionContext::create(parent, a->returnState);
    if (mergeCache != nullptr) {
      (*mergeCache)[{ a.get(), b.get() }] = a_;
    }
    return a_;
  } else {
    // a != b payloads differ
    // see if we can collapse parents due to $+x parents if local ctx
    std::weak_ptr<PredictionContext> singleParent;
    if (a == b || (parentA && parentA == parentB)) { // ax + bx = [a,b]x
      singleParent = a->parent;
    }
    if (!singleParent.expired()) { // parents are same, sort payloads and use same parent
      std::vector<int> payloads = { a->returnState, b->returnState };
      if (a->returnState > b->returnState) {
        payloads[0] = b->returnState;
        payloads[1] = a->returnState;
      }
      std::vector<std::weak_ptr<PredictionContext>> parents = { singleParent, singleParent };
      PredictionContextRef a_(new ArrayPredictionContext(parents, payloads));
      if (mergeCache != nullptr) {
        (*mergeCache)[{ a.get(), b.get() }] = a_;
      }
      return a_;
    }

    // parents differ and can't merge them. Just pack together
    // into array; can't merge.
    // ax + by = [ax,by]
    PredictionContextRef a_;
    if (a->returnState > b->returnState) { // sort by payload
      std::vector<int> payloads = { b->returnState, a->returnState };
      std::vector<std::weak_ptr<PredictionContext>> parents = { b->parent, a->parent };
      a_.reset(new ArrayPredictionContext(parents, payloads));
    } else {
      std::vector<int> payloads = {a->returnState, b->returnState};
      std::vector<std::weak_ptr<PredictionContext>> parents = { a->parent, b->parent };
      a_.reset(new ArrayPredictionContext(parents, payloads));
    }

    if (mergeCache != nullptr) {
      (*mergeCache)[{ a.get(), b.get() }] = a_;
    }
    return a_;
  }
}

PredictionContextRef PredictionContext::mergeRoot(SingletonPredictionContextRef a, SingletonPredictionContextRef b,
                                                  bool rootIsWildcard) {
  if (rootIsWildcard) {
    if (a == EMPTY) { // * + b = *
      return EMPTY;
    }
    if (b == EMPTY) { // a + * = *
      return EMPTY;
    }
  } else {
    if (a == EMPTY && b == EMPTY) { // $ + $ = $
      return EMPTY;
    }
    if (a == EMPTY) { // $ + x = [$,x]
      std::vector<int> payloads = { b->returnState, EMPTY_RETURN_STATE };
      std::vector<std::weak_ptr<PredictionContext>> parents = { b->parent, EMPTY };
      PredictionContextRef joined(new ArrayPredictionContext(parents, payloads));
      return joined;
    }
    if (b == EMPTY) { // x + $ = [$,x] ($ is always first if present)
      std::vector<int> payloads = { a->returnState, EMPTY_RETURN_STATE };
      std::vector<std::weak_ptr<PredictionContext>> parents = { a->parent, EMPTY };
      PredictionContextRef joined(new ArrayPredictionContext(parents, payloads));
      return joined;
    }
  }
  return nullptr;
}

PredictionContextRef PredictionContext::mergeArrays(std::shared_ptr<ArrayPredictionContext> a,
  std::shared_ptr<ArrayPredictionContext> b, bool rootIsWildcard, PredictionContextMergeCache *mergeCache) {

  if (mergeCache != nullptr) {
    auto iterator = mergeCache->find({ a.get(), b.get() });
    if (iterator != mergeCache->end()) {
      return iterator->second;
    }
    iterator = mergeCache->find({ b.get(), a.get() });
    if (iterator != mergeCache->end()) {
      return iterator->second;
    }
  }

  // merge sorted payloads a + b => M
  size_t i = 0; // walks a
  size_t j = 0; // walks b
  size_t k = 0; // walks target M array

  std::vector<int> mergedReturnStates;
  mergedReturnStates.resize(a->returnStates.size() + b->returnStates.size());
  std::vector<std::weak_ptr<PredictionContext>> mergedParents;
  mergedParents.resize(a->returnStates.size() + b->returnStates.size());

  // walk and merge to yield mergedParents, mergedReturnStates
  while (i < a->returnStates.size() && j < b->returnStates.size()) {
    PredictionContextRef a_parent = a->parents[i].lock();
    PredictionContextRef b_parent = b->parents[j].lock();
    if (a->returnStates[i] == b->returnStates[j]) {
      // same payload (stack tops are equal), must yield merged singleton
      int payload = a->returnStates[i];
      // $+$ = $
      bool both$ = payload == EMPTY_RETURN_STATE && a_parent && b_parent;
      bool ax_ax = (a_parent && b_parent) && a_parent == b_parent; // ax+ax -> ax
      if (both$ || ax_ax) {
        mergedParents[k] = a_parent; // choose left
        mergedReturnStates[k] = payload;
      }
      else { // ax+ay -> a'[x,y]
        PredictionContextRef mergedParent = merge(a_parent, b_parent, rootIsWildcard, mergeCache);
        mergedParents[k] = mergedParent;
        mergedReturnStates[k] = payload;
      }
      i++; // hop over left one as usual
      j++; // but also skip one in right side since we merge
    } else if (a->returnStates[i] < b->returnStates[j]) { // copy a[i] to M
      mergedParents[k] = a_parent;
      mergedReturnStates[k] = a->returnStates[i];
      i++;
    }
    else { // b > a, copy b[j] to M
      mergedParents[k] = b_parent;
      mergedReturnStates[k] = b->returnStates[j];
      j++;
    }
    k++;
  }

  // copy over any payloads remaining in either array
  if (i < a->returnStates.size()) {
    for (std::vector<int>::size_type p = i; p < a->returnStates.size(); p++) {
      mergedParents[k] = a->parents[p];
      mergedReturnStates[k] = a->returnStates[p];
      k++;
    }
  } else {
    for (std::vector<int>::size_type p = j; p < b->returnStates.size(); p++) {
      mergedParents[k] = b->parents[p];
      mergedReturnStates[k] = b->returnStates[p];
      k++;
    }
  }

  // trim merged if we combined a few that had same stack tops
  if (k < mergedParents.size()) { // write index < last position; trim
    if (k == 1) { // for just one merged element, return singleton top
      PredictionContextRef a_ = SingletonPredictionContext::create(mergedParents[0].lock(), mergedReturnStates[0]);
      if (mergeCache != nullptr) {
        (*mergeCache)[{ a.get(), b.get() }] = a_;
      }
      return a_;
    }
    mergedParents = Arrays::copyOf(mergedParents, k);
    mergedReturnStates = Arrays::copyOf(mergedReturnStates, k);
  }

  std::shared_ptr<ArrayPredictionContext> M(new ArrayPredictionContext(mergedParents, mergedReturnStates));

  // if we created same array as a or b, return that instead
  // TO_DO: track whether this is possible above during merge sort for speed
  if (M == a) {
    if (mergeCache != nullptr) {
      (*mergeCache)[{ a.get(), b.get() }] = a;
    }
    return a;
  }
  if (M == b) {
    if (mergeCache != nullptr) {
      (*mergeCache)[{ a.get(), b.get() }] = b;
    }
    return b;
  }

  if (combineCommonParents(mergedParents)) // Need to recreate the context as the parents array is copied on creation.
    M.reset(new ArrayPredictionContext(mergedParents, mergedReturnStates));

  if (mergeCache != nullptr) {
    (*mergeCache)[{ a.get(), b.get() }] = M;
  }
  return M;
}

bool PredictionContext::combineCommonParents(std::vector<std::weak_ptr<PredictionContext>> &parents) {
  std::set<PredictionContextRef> uniqueParents;

  for (size_t p = 0; p < parents.size(); p++) {
    PredictionContextRef parent = parents[p].lock();
    // ml: it's assumed that the == operator of PredictionContext kicks in here.
    if (uniqueParents.find(parent) == uniqueParents.end()) { // don't replace
      uniqueParents.insert(parent);
    }
  }

  if (uniqueParents.size() == parents.size())
    return false;

  parents.clear();
  std::copy(uniqueParents.begin(), uniqueParents.end(), parents.begin());
  return true;
}

std::wstring PredictionContext::toDOTString(PredictionContextRef context) {
  if (context == nullptr) {
    return L"";
  }
  antlrcpp::StringBuilder *buf = new antlrcpp::StringBuilder();
  buf->append(L"digraph G {\n");
  buf->append(L"rankdir=LR;\n");

  std::vector<PredictionContextRef> nodes = getAllContextNodes(context);
  std::sort(nodes.begin(), nodes.end(), [](PredictionContextRef o1, PredictionContextRef o2) {
    return o1->id - o2->id;
  });

  for (auto current : nodes) {
    if (is<SingletonPredictionContext>(current)) {
      std::wstring s = antlrcpp::StringConverterHelper::toString(current->id);
      buf->append(L"  s").append(s);
      std::wstring returnState = antlrcpp::StringConverterHelper::toString(current->getReturnState(0));
      if (is<EmptyPredictionContext>(current)) {
        returnState = L"$";
      }
      buf->append(L" [label=\"").append(returnState).append(L"\"];\n");
      continue;
    }
    std::shared_ptr<ArrayPredictionContext> arr = std::static_pointer_cast<ArrayPredictionContext>(current);
    buf->append(L"  s").append(arr->id);
    buf->append(L" [shape=box, label=\"");
    buf->append(L"[");
    bool first = true;
    for (auto inv : arr->returnStates) {
      if (!first) {
        buf->append(L", ");
      }
      if (inv == EMPTY_RETURN_STATE) {
        buf->append(L"$");
      } else {
        buf->append(inv);
      }
      first = false;
    }
    buf->append(L"]");
    buf->append(L"\"];\n");
  }

  for (auto current : nodes) {
    if (current == EMPTY) {
      continue;
    }
    for (size_t i = 0; i < current->size(); i++) {
      if (current->getParent(i).expired()) {
        continue;
      }
      std::wstring s = antlrcpp::StringConverterHelper::toString(current->id);
      buf->append(L"  s").append(s);
      buf->append(L"->");
      buf->append(L"s");
      buf->append(current->getParent(i).lock()->id);
      if (current->size() > 1) {
        buf->append(std::wstring(L" [label=\"parent[") + antlrcpp::StringConverterHelper::toString(i) + std::wstring(L"]\"];\n"));
      } else {
        buf->append(L";\n");
      }
    }
  }

  buf->append(L"}\n");
  return buf->toString();
}

// The "visited" map is just a temporary structure to control the retrieval process (which is recursive).
PredictionContextRef PredictionContext::getCachedContext(PredictionContextRef context,
  std::shared_ptr<PredictionContextCache> contextCache, std::map<PredictionContextRef, PredictionContextRef> &visited) {
  if (context->isEmpty()) {
    return context;
  }

  {
    auto iterator = visited.find(context);
    if (iterator != visited.end())
      return iterator->second; // Not necessarly the same as context.
  }

  auto iterator = contextCache->find(context);
  if (iterator != contextCache->end()) {
    visited[context] = *iterator;

    return *iterator;
  }

  bool changed = false;

  std::vector<std::weak_ptr<PredictionContext>> parents;
  parents.resize(context->size());
  for (size_t i = 0; i < parents.size(); i++) {
    std::weak_ptr<PredictionContext> parent = getCachedContext(context->getParent(i).lock(), contextCache, visited);
    if (changed || parent.lock() != context->getParent(i).lock()) {
      if (!changed) {
        parents.clear();
        for (size_t j = 0; j < context->size(); j++) {
          parents.push_back(context->getParent(j));
        }

        changed = true;
      }

      parents[i] = parent;
    }
  }

  if (!changed) {
    contextCache->insert(context);
    visited[context] = context;

    return context;
  }

  PredictionContextRef updated;
  if (parents.empty()) {
    updated = EMPTY;
  } else if (parents.size() == 1) {
    updated = SingletonPredictionContext::create(parents[0], context->getReturnState(0));
  } else {
    updated = std::make_shared<ArrayPredictionContext>(parents, std::dynamic_pointer_cast<ArrayPredictionContext>(context)->returnStates);
  }

  contextCache->insert(updated);
  visited[updated] = updated;
  visited[context] = updated;

  return updated;
}

std::vector<PredictionContextRef> PredictionContext::getAllContextNodes(PredictionContextRef context) {
  std::vector<PredictionContextRef> nodes;
  std::map<PredictionContextRef, PredictionContextRef> visited;
  getAllContextNodes_(context, nodes, visited);
  return nodes;
}


void PredictionContext::getAllContextNodes_(PredictionContextRef context,
  std::vector<PredictionContextRef> &nodes,
  std::map<PredictionContextRef, PredictionContextRef> &visited) {

  if (visited.find(context) != visited.end()) {
    return; // Already done.
  }

  visited[context] = context;
  nodes.push_back(context);

  for (size_t i = 0; i < context->size(); i++) {
    getAllContextNodes_(context->getParent(i).lock(), nodes, visited);
  }
}

std::wstring PredictionContext::toString() {
  //TODO: what should this return?  (Return empty string
  // for now.)
  return L"TODO PredictionContext::toString()";
}

std::wstring PredictionContext::toString(Recognizer *recog)  {
  return toString();
}

std::vector<std::wstring> PredictionContext::toStrings(Recognizer *recognizer, int currentState) {
  return toStrings(recognizer, EMPTY, currentState);
}

std::vector<std::wstring> PredictionContext::toStrings(Recognizer *recognizer, PredictionContextRef stop, int currentState) {

  std::vector<std::wstring> result;

  for (size_t perm = 0; ; perm++) {
    size_t offset = 0;
    bool last = true;
    PredictionContext *p = this;
    int stateNumber = currentState;
    antlrcpp::StringBuilder localBuffer;
    localBuffer.append(L"[");

    bool outerContinue = false;
    while (!p->isEmpty() && p != stop.get()) {
      size_t index = 0;
      if (p->size() > 0) {
        size_t bits = 1;
        while ((1 << bits) < p->size()) {
          bits++;
        }

        size_t mask = (1 << bits) - 1;
        index = (perm >> offset) & mask;
        last &= index >= p->size() - 1;
        if (index >= p->size()) {
          outerContinue = true;
          break;
        }
        offset += bits;
      }

      if (recognizer != nullptr) {
        if (localBuffer.length() > 1) {
          // first char is '[', if more than that this isn't the first rule
          localBuffer.append(L' ');
        }

        const ATN &atn = recognizer->getATN();
        ATNState *s = atn.states[(size_t)stateNumber];
        std::wstring ruleName = recognizer->getRuleNames()[(size_t)s->ruleIndex];
        localBuffer.append(ruleName);
      } else if (p->getReturnState(index) != EMPTY_RETURN_STATE) {
        if (!p->isEmpty()) {
          if (localBuffer.length() > 1) {
            // first char is '[', if more than that this isn't the first rule
            localBuffer.append(L' ');
          }

          localBuffer.append(p->getReturnState(index));
        }
      }
      stateNumber = p->getReturnState(index);
      p = p->getParent(index).lock().get();
    }

    if (outerContinue)
      continue;

    localBuffer.append(L"]");
    result.push_back(localBuffer.toString());

    if (last) {
      break;
    }
  }

  return result;
}
