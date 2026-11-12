/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#pragma once

#include <vector>

#include "hphp/runtime/vm/jit/translator.h"
#include "hphp/runtime/vm/jit/translator-inline.h"

namespace HPHP { namespace jit {

/**
 * A dynamic control-flow graph of single-block translations.
 */
struct TransCFG {
  struct Arc {
    static const int64_t kUnknownWeight = -1;

    Arc(TransID src, TransID dst, int64_t w)
        : m_src(src)
        , m_dst(dst)
        , m_weight(w)
        , m_guessed(false)
      {}
    TransID src()          const { return m_src;     }
    TransID dst()          const { return m_dst;     }
    int64_t weight()       const { return m_weight;  }
    bool    guessed()      const { return m_guessed; }
    void    setWeight(int64_t w) { m_weight = w;     }
    void    setGuessed()         { m_guessed = true; }
   private:
    TransID m_src;
    TransID m_dst;
    int64_t m_weight;
    bool    m_guessed; // whether or not m_weight was guessed
  };

  using ArcPtrVec = std::vector<Arc*>                     ;
  using ArcPtrSet = hphp_hash_set<Arc*, pointer_hash<Arc>>;

  struct Node {
    Node(TransID id, int64_t w)
        : m_id(id)
        , m_weight(w)
      {}
    ~Node();

    TransID          transId()     const { return m_id;              }
    int64_t          weight()      const { return m_weight;          }
    const ArcPtrVec& inArcs()      const { return m_inArcs;          }
    const ArcPtrVec& outArcs()     const { return m_outArcs;         }
    void             addInArc (Arc* arc) { m_inArcs.push_back(arc);  }
    void             addOutArc(Arc* arc) { m_outArcs.push_back(arc); }
    void             removeInArc(Arc* arc);
    void             removeOutArc(Arc* arc);
   private:
    TransID   m_id;
    int64_t   m_weight;
    ArcPtrVec m_inArcs;
    ArcPtrVec m_outArcs;
  };

  TransCFG() {}
  TransCFG(const Func* func,
           const ProfData* profData,
           bool inlining = false);

  const std::vector<TransID>& nodes() const { return m_transIds; }
  ArcPtrVec                   arcs()  const;
  int64_t                     weight(TransID id) const;
  void                        setNodeWeight(TransID id, int64_t weight);
  const ArcPtrVec&            inArcs(TransID id) const;
  const ArcPtrVec&            outArcs(TransID id) const;
  void                        addNode(TransID id, int64_t weight);
  bool                        hasNode(TransID id) const;
  void                        addArc(TransID srcId, TransID dstId,
                                     int64_t weight=0);
  void                        removeArc(TransID srcId, TransID dstId);
  bool                        hasArc(TransID srcId, TransID dstId) const;
  void                        print(std::ostream& out,
                                    FuncId funcId,
                                    const ProfData* profData) const;

 private:
  std::vector<TransID>           m_transIds;  // vector of TransIDs in the graph
  std::vector<Node>              m_nodeInfo;  // info about each node
  hphp_hash_map<TransID, size_t> m_idToIdx;   // map from TransIDs to indices
                                              // in m_nodeInfo
};

// Returns the set of all predecessor profiling translations.
TransIDSet findPredTrans(const RegionDesc& rd, const ProfData* profData);

} }
