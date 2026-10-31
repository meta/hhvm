/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   | Copyright (c) 1997-2010 The PHP Group                                |
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

#include "hphp/runtime/ext/extension.h"
#include "hphp/runtime/base/builtin-functions.h"
/*
 *                       Heapgraph Extension
 * What is it?
 * Set of methods to wrap around HHVM's heap graph implementation
 *
 * How does it work?
 * Create a heap graph in HHVM and uses it as a Resource with a
 * set of functions that can operate on it.
 *
 * How do I use it?
 * Call heapgraph_create, and then any of the other heapgraph
 * function
 */

#include <array>
#include <vector>

#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/collections.h"
#include "hphp/runtime/base/memory-manager-defs.h"
#include "hphp/runtime/vm/class.h"
#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/func-id.h"
#include "hphp/runtime/base/heap-graph.h"
#include "hphp/runtime/base/heap-algorithms.h"
#include "hphp/runtime/vm/vm-regs.h"

namespace HPHP {

struct PhpStack;
struct CppStack;

namespace {

TRACE_SET_MOD(heapgraph)

///////////////////////////////////////////////////////////////////////////////

const StaticString
  s_nodes("nodes"),
  s_edges("edges"),
  s_roots("roots"),
  s_root_nodes("root_nodes"),
  s_root_path("root_path"),
  s_exact("exact"),
  s_format("format"),

  // Node description
  s_kind("kind"),
  s_size("size"),
  s_index("index"),
  s_class("class"),
  s_func("func"),
  s_local("local"),
  s_prop("prop"),
  s_Root("Root"),
  s_type("type"),

  // Edge description
  s_from("from"),
  s_to("to"),
  s_key("key"),
  s_value("value"),
  s_offset("offset");

}

///////////////////////////////////////////////////////////////////////////////
// CONTEXT OBJECTS

// Extra information about a HeapGraph::Node.
union CapturedNode {
  CapturedNode()  {}
  CapturedNode(const CapturedNode& other) {
    memcpy(this, &other, sizeof other);
  }
  rds::SPropCache sprop_cache; // only for HPHP::SPropCache
  struct {
    HeaderKind kind;
    const Class* cls;
  } heap_object; // only for non-roots
};

// Extra information about a HeapGraph::Ptr
struct CapturedPtr {
  enum Kind { Key, Value, Property, Offset, Memo } index_kind;
  union {
    uint32_t index; // location of ptr within it's from node (for Key, Value, Property, Offset)
    FuncId funcId;  // memoized function ID (for Memo)
  };

  // Constructors needed because FuncId has a non-trivial default constructor
  CapturedPtr(Kind kind, uint32_t idx) : index_kind(kind), index(idx) {}
  CapturedPtr(FuncId fid) : index_kind(Memo), funcId(fid) {}

  static CapturedPtr makeIndex(Kind kind, uint32_t idx) {
    return CapturedPtr(kind, idx);
  }

  static CapturedPtr makeMemo(FuncId fid) {
    return CapturedPtr(fid);
  }
};

struct HeapGraphContext : SweepableResourceData {
  explicit HeapGraphContext(const HeapGraph& hg) : hg(hg) {}
  explicit HeapGraphContext(HeapGraph&& hg) : hg(std::move(hg)) {}
  ~HeapGraphContext() override {}

  bool isInvalid() const override {
    return false;
  }

  CLASSNAME_IS("HeapGraphContext")
  DECLARE_RESOURCE_ALLOCATION(HeapGraphContext)

  // overriding ResourceData
  const OptString& o_getClassNameHook() const override { return classnameof(); }

  std::vector<CapturedNode> cnodes;
  std::vector<CapturedPtr> cptrs;
  const HeapGraph hg;
};

IMPLEMENT_RESOURCE_ALLOCATION(HeapGraphContext)

namespace {

using HeapGraphContextPtr = req::ptr<HeapGraphContext>;
static HeapGraphContextPtr get_valid_heapgraph_context_resource(
  const OptResource& resource,
  const char* func_name
) {
  auto hgcontext = dyn_cast_or_null<HeapGraphContext>(resource);
  if (hgcontext == nullptr || hgcontext->isInvalid()) {
    raise_warning(
      "%s(): supplied resource is not a valid HeapGraph Context resource",
      func_name + 2
    );
    return nullptr;
  }
  return hgcontext;
}

///////////////////////////////////////////////////////////////////////////////
// TRAVERSAL FUNCTIONS

static std::array<const StringData*, 3> edge_kind_strs{};
static const char* edge_kind_cstrs[] = {
  "Ptr:Counted", "Ptr:Ambiguous", "Ptr:Weak",
};

const StringData* edgeKindName(HeapGraph::PtrKind kind) {
  auto s = edge_kind_strs[(int)kind];
  if (!s) {
    s = makeStaticString(edge_kind_cstrs[(int)kind]);
    edge_kind_strs[(int)kind] = s;
  }
  return s;
  static_assert(HeapGraph::NumPtrKinds == 3, "");
  static_assert(HeapGraph::Counted == 0, "");
  static_assert(HeapGraph::Ambiguous == 1, "");
  static_assert(HeapGraph::Weak == 2, "");
}

// return metadata about this pointer, in the form of a CapturedPtr
CapturedPtr getEdgeInfo(const HeapGraph& g, int ptr) {
  // Try to drill down and resolve the edge name
  assertx(g.ptrs[ptr].from != -1 && g.ptrs[ptr].to != -1);
  auto& edge = g.ptrs[ptr];
  auto& from = g.nodes[edge.from];
  int prop_offset = edge.offset;
  if (!from.is_root()) {
    auto from_hdr = from.h;

    // get the actual ObjectData*. This deals with object kinds that
    // have data before the object: AFWH, Native, and Closure. Compute
    // prop_offset relative to the inner ObjectData.
    const ObjectData* from_obj{nullptr};
    if (from_hdr->kind() == HeaderKind::AsyncFuncFrame) {
      from_obj = asyncFuncWH(from_hdr);
      prop_offset = edge.offset - (uintptr_t(from_obj) - uintptr_t(from_hdr));
    } else if (from_hdr->kind() == HeaderKind::NativeData) {
      from_obj = Native::obj(static_cast<const NativeNode*>(from_hdr));
      prop_offset = edge.offset - (uintptr_t(from_obj) - uintptr_t(from_hdr));
    } else if (from_hdr->kind() == HeaderKind::ClosureHdr) {
      from_obj = closureObj(from_hdr);
      prop_offset = edge.offset - (uintptr_t(from_obj) - uintptr_t(from_hdr));
    } else if (from_hdr->kind() == HeaderKind::MemoData) {
      from_obj = memoObj(from_hdr);
      prop_offset = edge.offset - (uintptr_t(from_obj) - uintptr_t(from_hdr));
    } else if (isObjectKind(from_hdr->kind())) {
      from_obj = static_cast<const ObjectData*>(from_hdr);
      prop_offset = edge.offset;
    }

    switch (from_hdr->kind()) {
      // Known generalized cases that don't really need pointer kind
      case HeaderKind::Dict:
      case HeaderKind::Keyset: {
        if (edge.offset >= sizeof(VanillaDict)) {
          using Elm = VanillaDict::Elm;
          auto elm_offset = edge.offset - sizeof(VanillaDict);
          uint32_t index = elm_offset / sizeof(Elm);
          if (index < static_cast<const VanillaDict*>(from_hdr)->iterLimit()) {
            auto field = elm_offset - index * sizeof(Elm);
            if (field == Elm::keyOff()) {
              return CapturedPtr::makeIndex(CapturedPtr::Key, index);
            }
            if (field == Elm::dataOff() + offsetof(TypedValue, m_data)) {
              return CapturedPtr::makeIndex(CapturedPtr::Value, index);
            }
          }
        }
        break;
      }

      case HeaderKind::Vec: {
        if (edge.offset >= sizeof(ArrayData)) {
          auto elm_offset = edge.offset - sizeof(ArrayData);
          uint32_t index = elm_offset / sizeof(TypedValue);
          if (index < static_cast<const ArrayData*>(from_hdr)->size()) {
            return CapturedPtr::makeIndex(CapturedPtr::Value, index);
          }
        }
        break;
      }

      case HeaderKind::BespokeVec:
      case HeaderKind::BespokeDict:
      case HeaderKind::BespokeKeyset:
        // TODO(kshaunak): Expose an address -> element API for bespokes.
        break;

      case HeaderKind::Pair: {
        if (edge.offset >= c_Pair::dataOffset()) {
          auto elm_offset = edge.offset - c_Pair::dataOffset();
          uint32_t index = elm_offset / sizeof(TypedValue);
          if (index < 2) {
            return CapturedPtr::makeIndex(CapturedPtr::Value, index);
          }
        }
        break;
      }

      case HeaderKind::AwaitAllWH:
      case HeaderKind::ConcurrentWH:
      case HeaderKind::WaitHandle:
      case HeaderKind::ClsMeth:
      case HeaderKind::RClsMeth:
        break;

      // cases that have explicit pointer name
      case HeaderKind::AsyncFuncFrame:
      case HeaderKind::ClosureHdr:
      case HeaderKind::Closure:
        // the class of a c_Closure describes the captured variables
      case HeaderKind::NativeData:
      case HeaderKind::Object: {
        auto cls = from_obj->getVMClass();
        FTRACE(5, "HG: Getting connection name for class {} at {}\n",
               from_obj->getClassName().data(), from_obj);
        if (prop_offset >= sizeof(ObjectData)) {
          uint32_t index = ObjectProps::offset2Idx(
            prop_offset - sizeof(ObjectData)
          );
          if (index < cls->numDeclProperties()) {
            return CapturedPtr::makeIndex(CapturedPtr::Property, index);
          }
        } else {
          // edge_offset > 0 && prop_offset < 0 means nativedata fields
        }

        break;
      }

      case HeaderKind::MemoData: {
        auto cls = from_obj->getVMClass();
        if (prop_offset < 0 && cls->hasMemoSlots()) {
          // Edge points to memo slots area (before ObjectData)
          // prop_offset is relative to ObjectData, so negative means before it
          // Layout: [MemoNode][MemoSlot N-1][MemoSlot N-2]...[MemoSlot 0][ObjectData]
          // The offset from ObjectData to MemoSlot i is -(i+1) * sizeof(MemoSlot)
          // So: slotIdx = (-prop_offset / sizeof(MemoSlot)) - 1
          auto negOffset = static_cast<size_t>(-prop_offset);
          if (negOffset % sizeof(MemoSlot) == 0) {
            auto numSlots = cls->numMemoSlots();
            auto slotIdx = (negOffset / sizeof(MemoSlot)) - 1;
            if (slotIdx < numSlots) {
              auto funcId = cls->funcForMemoSlot(slotIdx);
              return CapturedPtr::makeMemo(funcId);
            }
          }
        }
        break;
      }

      case HeaderKind::NativeObject:
      case HeaderKind::AsyncFuncWH:
      case HeaderKind::Vector:
      case HeaderKind::ImmVector:
      case HeaderKind::Set:
      case HeaderKind::ImmSet:
      case HeaderKind::Map:
      case HeaderKind::ImmMap:
      case HeaderKind::String:
      case HeaderKind::Resource:
      case HeaderKind::BigMalloc:
      case HeaderKind::Cpp:
      case HeaderKind::SmallMalloc:
      case HeaderKind::Free:
      case HeaderKind::Hole:
      case HeaderKind::Slab:
      case HeaderKind::RFunc:
        // just provide raw prop_offset
        break;
    }
  }

  return CapturedPtr::makeIndex(CapturedPtr::Offset, uint32_t(prop_offset));
}

void heapgraphCallback(Array fields, const Variant& callback) {
  VMRegAnchor _;
  auto params = make_vec_array(fields);
  vm_call_user_func(callback, params);
}

void heapgraphCallback(Array fields, Array fields2, const Variant& callback) {
  VMRegAnchor _;
  auto params = make_vec_array(fields, fields2);
  vm_call_user_func(callback, params);
}

static const StringData* header_name_strs[NumHeaderKinds];

Array createPhpNode(HeapGraphContextPtr hgptr, int index) {
  const auto& node = hgptr->hg.nodes[index];
  const auto& cnode = hgptr->cnodes[index];

  const StringData* kind_str;
  if (!node.is_root()) {
    auto k = int(cnode.heap_object.kind);
    kind_str = header_name_strs[k];
    if (!kind_str) {
      kind_str = makeStaticString(header_names[k]);
      header_name_strs[k] = kind_str;
    }
  } else {
    kind_str = s_Root.get(); // fake HeaderKind "Root"
  }

  auto node_arr = make_dict_array(
    s_index, VarNR(index),
    s_kind, VarNR(kind_str),
    s_size, VarNR(int64_t(node.size))
  );
  if (type_scan::hasNonConservative()) {
    auto ty = node.tyindex;
    if (ty > type_scan::kIndexUnknownNoPtrs) {
      auto type = type_scan::getName(ty);
      node_arr.set(s_type,
                   make_tv<KindOfPersistentString>(makeStaticString(type)));
    }
  }
  if (!node.is_root()) {
    if (auto cls = cnode.heap_object.cls) {
      node_arr.set(s_class, make_tv<KindOfPersistentString>(cls->name()));
    }
  } else if (node.rootKind == HeapGraph::RootKind::MemoCache) {
    // Add memo function info
    if (!node.funcId.isInvalid()) {
      auto func = Func::fromFuncId(node.funcId);
      node_arr.set(s_func, make_tv<KindOfPersistentString>(func->name()));
      if (func->cls()) {
        node_arr.set(s_class, make_tv<KindOfPersistentString>(func->cls()->name()));
      }
    }
  } else if (node.rootKind == HeapGraph::RootKind::SPropCache) {
    // Add static property info (stored as "ClassName::PropertyName")
    if (node.sPropName) {
      node_arr.set(s_prop, make_tv<KindOfPersistentString>(node.sPropName));
    }
  }

  return node_arr;
}

Array createPhpEdge(HeapGraphContextPtr hgptr, int index) {
  const auto& ptr = hgptr->hg.ptrs[index];
  const auto& cptr = hgptr->cptrs[index];
  const auto& cfrom = hgptr->cnodes[ptr.from];

  auto ptr_arr = make_dict_array(
    s_index, VarNR(index),
    s_kind, VarNR(edgeKindName(ptr.ptr_kind)),
    s_from, VarNR(ptr.from),
    s_to, VarNR(ptr.to)
  );
  switch (cptr.index_kind) {
    case CapturedPtr::Key:
      ptr_arr.set(s_key, make_tv<KindOfInt64>(cptr.index));
      break;
    case CapturedPtr::Value:
      ptr_arr.set(s_value, make_tv<KindOfInt64>(cptr.index));
      break;
    case CapturedPtr::Property: {
      auto cls = cfrom.heap_object.cls;
      auto slot = cls->propIndexToSlot(cptr.index);
      auto& prop = cls->declProperties()[slot];
      ptr_arr.set(s_prop, make_tv<KindOfPersistentString>(prop.name));
      break;
    }
    case CapturedPtr::Memo: {
      if (cptr.funcId != FuncId::Invalid) {
        auto func = Func::fromFuncId(cptr.funcId);
        ptr_arr.set(s_func, make_tv<KindOfPersistentString>(func->name()));
        if (func->cls()) {
          ptr_arr.set(s_class, make_tv<KindOfPersistentString>(func->cls()->name()));
        }
      }
      break;
    }
    case CapturedPtr::Offset:
      if (cptr.index) ptr_arr.set(s_offset, make_tv<KindOfInt64>(cptr.index));
      break;
  }

  return ptr_arr;
}

std::vector<int> toBoundIntVector(const Array& arr, int64_t max) {
  std::vector<int> result;
  result.reserve(arr.size());
  for (ArrayIter iter(arr); iter; ++iter) {
    auto index = iter.second().toInt64(); // Cannot re-enter.
    if (index < 0 || index >= max) {
      continue;
    }

    result.push_back(index);
  }
  return result;
}

///////////////////////////////////////////////////////////////////////////////
// Exports

OptResource HHVM_FUNCTION(heapgraph_create, void) {
  HeapGraph hg = makeHeapGraph();
  std::vector<CapturedNode> cnodes;
  std::vector<CapturedPtr> cptrs;

  // Copy edges into captured edges
  // Capturing edges first because after capturing nodes we nullify the header
  cptrs.reserve(hg.ptrs.size());
  for (int i = 0; i < hg.ptrs.size(); ++i) {
    auto new_ptr = getEdgeInfo(hg, i); // edge name
    cptrs.push_back(new_ptr);
  }

  // Copy useful information from heap into cnodes
  cnodes.resize(hg.nodes.size());
  for (size_t i = 0, n = hg.nodes.size(); i < n; ++i) {
    auto& node = hg.nodes[i];
    auto& cnode = cnodes[i];
    if (!node.is_root()) {
      // For non-root nodes, only touch heap_object union member
      auto obj = innerObj(node.h);
      cnode.heap_object.kind = node.h->kind();
      cnode.heap_object.cls = obj ? obj->getVMClass() : nullptr;
      node.h = nullptr;
    }
  }

  auto hgcontext = req::make<HeapGraphContext>(std::move(hg));
  std::swap(hgcontext->cnodes, cnodes);
  std::swap(hgcontext->cptrs, cptrs);
  return OptResource(hgcontext);
}

void HHVM_FUNCTION(heapgraph_foreach_node,
  const OptResource& resource,
  const Variant& callback
) {
  auto hgptr = get_valid_heapgraph_context_resource(resource, __FUNCTION__);
  if (!hgptr || callback.isNull()) return;
  for (int i = 0; i < hgptr->hg.nodes.size(); i++) {
    auto phpnode = createPhpNode(hgptr, i);
    heapgraphCallback(phpnode, callback);
  }
}

void HHVM_FUNCTION(heapgraph_foreach_edge,
  const OptResource& resource,
  const Variant& callback
) {
  auto hgptr = get_valid_heapgraph_context_resource(resource, __FUNCTION__);
  if (!hgptr || callback.isNull()) return;
  for (int i = 0; i < hgptr->hg.ptrs.size(); i++) {
    auto phpedge = createPhpEdge(hgptr, i);
    heapgraphCallback(phpedge, callback);
  }
}

void HHVM_FUNCTION(heapgraph_foreach_root,
  const OptResource& resource,
  const Variant& callback
) {
  auto hgptr = get_valid_heapgraph_context_resource(resource, __FUNCTION__);
  if (!hgptr || callback.isNull()) return;
  for (int i = 0, n = hgptr->hg.root_ptrs.size(); i < n; ++i) {
    auto phpedge = createPhpEdge(hgptr, hgptr->hg.root_ptrs[i]);
    heapgraphCallback(phpedge, callback);
  }
}

void HHVM_FUNCTION(heapgraph_foreach_root_node,
  const OptResource& resource,
  const Variant& callback
) {
  auto hgptr = get_valid_heapgraph_context_resource(resource, __FUNCTION__);
  if (!hgptr || callback.isNull()) return;
  for (auto n : hgptr->hg.root_nodes) {
    auto phpnode = createPhpNode(hgptr, n);
    heapgraphCallback(phpnode, callback);
  }
}

void HHVM_FUNCTION(heapgraph_dfs_nodes,
  const OptResource& resource,
  const Array& roots_arr,
  const Array& skips_arr,
  const Variant& callback
) {
  auto hgptr = get_valid_heapgraph_context_resource(resource, __FUNCTION__);
  if (!hgptr || callback.isNull()) return;
  auto max = hgptr->hg.nodes.size();
  auto roots = toBoundIntVector(roots_arr, max);
  auto skips = toBoundIntVector(skips_arr, max);
  dfs_nodes(hgptr->hg, roots, skips, [&](int n) {
    auto phpnode = createPhpNode(hgptr, n);
    heapgraphCallback(phpnode, callback);
  });
}

void HHVM_FUNCTION(heapgraph_dfs_edges,
  const OptResource& resource,
  const Array& roots_arr,
  const Array& skips_arr,
  const Variant& callback
) {
  auto hgptr = get_valid_heapgraph_context_resource(resource, __FUNCTION__);
  if (!hgptr || callback.isNull()) return;
  auto max = hgptr->hg.ptrs.size();
  auto roots = toBoundIntVector(roots_arr, max);
  auto skips = toBoundIntVector(skips_arr, max);
  dfs_ptrs(hgptr->hg, roots, skips, [&](int n, int p) {
    auto phpnode = createPhpNode(hgptr, n);
    auto phpedge = createPhpEdge(hgptr, p);
    heapgraphCallback(phpedge, phpnode, callback);
  });
}

Array HHVM_FUNCTION(heapgraph_edge, const OptResource& resource, int64_t index) {
  auto hgptr = get_valid_heapgraph_context_resource(resource, __FUNCTION__);
  if (!hgptr) return Array::CreateDict();
  if (size_t(index) >= hgptr->hg.ptrs.size()) return Array::CreateDict();
  return createPhpEdge(hgptr, index);
}

Array HHVM_FUNCTION(heapgraph_node, const OptResource& resource, int64_t index) {
  auto hgptr = get_valid_heapgraph_context_resource(resource, __FUNCTION__);
  if (!hgptr) return Array::CreateDict();
  if (size_t(index) >= hgptr->hg.nodes.size()) return Array::CreateDict();
  return createPhpNode(hgptr, index);
}

Array HHVM_FUNCTION(heapgraph_node_out_edges,
  const OptResource& resource,
  int64_t index
) {
  auto hgptr = get_valid_heapgraph_context_resource(resource, __FUNCTION__);
  if (!hgptr) return Array::CreateVec();
  if (size_t(index) >= hgptr->hg.nodes.size()) return Array::CreateVec();
  size_t num_edges{0};
  hgptr->hg.eachOutPtr(index, [&](int) { num_edges++; });
  VecInit result(num_edges);
  hgptr->hg.eachOutPtr(index, [&](int ptr) {
    result.append(createPhpEdge(hgptr, ptr));
  });
  return result.toArray();
}

Array HHVM_FUNCTION(heapgraph_node_in_edges,
  const OptResource& resource,
  int64_t index
) {
  auto hgptr = get_valid_heapgraph_context_resource(resource, __FUNCTION__);
  if (!hgptr) return Array::CreateVec();
  if (size_t(index) >= hgptr->hg.nodes.size()) return Array::CreateVec();
  size_t num_edges{0};
  hgptr->hg.eachInPtr(index, [&](int) { num_edges++; });
  VecInit result(num_edges);
  hgptr->hg.eachInPtr(index, [&](int ptr) {
    result.append(createPhpEdge(hgptr, ptr));
  });
  return result.toArray();
}

Array HHVM_FUNCTION(heapgraph_stats, const OptResource& resource) {
  auto hgptr = get_valid_heapgraph_context_resource(resource, __FUNCTION__);
  if (!hgptr) return Array::CreateDict();
  auto result = make_dict_array(
    s_nodes, VarNR(int64_t(hgptr->hg.nodes.size())),
    s_edges, VarNR(int64_t(hgptr->hg.ptrs.size())),
    s_roots, VarNR(int64_t(hgptr->hg.root_ptrs.size())),
    s_root_nodes, VarNR(int64_t(hgptr->hg.root_nodes.size())),
    s_exact, VarNR(type_scan::hasNonConservative() ? 1 : 0)
  );
  return result;
}

///////////////////////////////////////////////////////////////////////////////
// Extension

}

struct heapgraphExtension final : Extension {
  heapgraphExtension() : Extension("heapgraph", "1.0", "hphp_hphpi") { }

  void moduleRegisterNative() override {
    HHVM_FALIAS(HH\\heapgraph_create, heapgraph_create);
    HHVM_FALIAS(HH\\heapgraph_stats, heapgraph_stats);
    HHVM_FALIAS(HH\\heapgraph_foreach_node, heapgraph_foreach_node);
    HHVM_FALIAS(HH\\heapgraph_foreach_edge, heapgraph_foreach_edge);
    HHVM_FALIAS(HH\\heapgraph_foreach_root, heapgraph_foreach_root);
    HHVM_FALIAS(HH\\heapgraph_foreach_root_node, heapgraph_foreach_root_node);
    HHVM_FALIAS(HH\\heapgraph_edge, heapgraph_edge);
    HHVM_FALIAS(HH\\heapgraph_node, heapgraph_node);
    HHVM_FALIAS(HH\\heapgraph_node_out_edges, heapgraph_node_out_edges);
    HHVM_FALIAS(HH\\heapgraph_node_in_edges, heapgraph_node_in_edges);
    HHVM_FALIAS(HH\\heapgraph_dfs_nodes, heapgraph_dfs_nodes);
    HHVM_FALIAS(HH\\heapgraph_dfs_edges, heapgraph_dfs_edges);
  }
} s_heapgraph_extension;


///////////////////////////////////////////////////////////////////////////////
}
