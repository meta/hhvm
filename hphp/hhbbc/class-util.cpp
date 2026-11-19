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
#include "hphp/hhbbc/class-util.h"

#include "hphp/hhbbc/analyze.h"
#include "hphp/hhbbc/index.h"
#include "hphp/hhbbc/representation.h"
#include "hphp/hhbbc/type-system.h"

#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/collections.h"

#include "hphp/runtime/vm/preclass-emitter.h"

#include <boost/algorithm/string/predicate.hpp>

namespace HPHP::HHBBC {

//////////////////////////////////////////////////////////////////////

namespace {

const StaticString
  s_SimpleXMLElement("SimpleXMLElement"),
  s_Closure("Closure"),
  s_MockClass("__MockClass"),
  s_NoFlatten("__NoFlatten"),
  s_invoke("__invoke"),
  s_debugInfo("__debugInfo"),
  s_construct("__construct");

}

//////////////////////////////////////////////////////////////////////

bool has_magic_bool_conversion(SString clsName) {
  return
    collections::isTypeName(clsName) ||
    clsName->tsame(s_SimpleXMLElement.get());
}

bool is_collection(res::Class cls) {
  auto const name = cls.name();
  return collections::isTypeName(name);
}

php::Func* find_method(const php::Class* cls, SString name) {
  for (auto& m : cls->methods) {
    if (m->name == name) {
      return m.get();
    }
  }
  return nullptr;
}

bool is_special_method_name(SString name) {
  auto const p = name->data();
  return p && p[0] == '8' && p[1] == '6';
}

bool has_name_only_func_family(SString name) {
  return
    name != s_construct.get() &&
    name != s_invoke.get() &&
    name != s_debugInfo.get() &&
    !is_special_method_name(name);
}

bool is_mock_class(const php::Class* cls) {
  return cls->userAttributes.contains(s_MockClass.get());
}

bool is_noflatten_trait(const php::Class* cls) {
  assertx(cls->attrs & AttrTrait);
  return cls->userAttributes.contains(s_NoFlatten.get());
}

bool is_closure_base(SString name) {
  return name->tsame(s_Closure.get());
}

bool is_closure_base(const php::Class& c) {
  return c.name->tsame(s_Closure.get());
}

bool is_closure(const php::Class& c) {
  return c.parentName && c.parentName->tsame(s_Closure.get());
}

bool is_closure_name(SString name) {
  return boost::starts_with(name->slice(), "Closure$");
}

bool is_unused_trait(const php::Class& c) {
  return
    (c.attrs & (AttrTrait | AttrNoOverride)) == (AttrTrait | AttrNoOverride);
}

bool is_used_trait(const php::Class& c) {
  return
    (c.attrs & (AttrTrait | AttrNoOverride)) == AttrTrait;
}

//////////////////////////////////////////////////////////////////////

Type get_type_of_reified_list(const UserAttributeMap& ua) {
  auto const it = ua.find(s___Reified.get());
  assertx(it != ua.end());
  auto const tv = it->second;
  assertx(tvIsVec(&tv));
  auto const numGenerics = extractSizeFromReifiedAttribute(tv.m_data.parr);
  assertx(numGenerics > 0);

  auto t = vec(std::vector<Type>(numGenerics, TDict));

  // If the type params are all soft, the reified list is allowed to
  // be empty.
  auto const allSoft = areAllGenericsSoft(tv.m_data.parr);
  if (allSoft) t |= TVecE;
  return t;
}

// The value returned from this must satisfy the type returned from
// get_type_of_reified_list.
TypedValue get_default_value_of_reified_list(const UserAttributeMap& ua) {
  auto const it = ua.find(s___Reified.get());
  assertx(it != ua.end());
  auto const tv = it->second;
  assertx(tvIsVec(&tv));
  auto const numGenerics = extractSizeFromReifiedAttribute(tv.m_data.parr);
  assertx(numGenerics > 0);

  // Empty vec is allowed for the "all soft" case, so use that if it
  // applies.
  auto const allSoft = areAllGenericsSoft(tv.m_data.parr);
  if (allSoft) return make_tv<KindOfVec>(staticEmptyVec());

  // Otherwise make a vec of the appropriate size filled with empty
  // dicts, which satisfies get_type_of_reified_list.
  VecInit init{numGenerics};
  for (size_t i = 0; i < numGenerics; ++i) {
    init.append(make_tv<KindOfDict>(staticEmptyDictArray()));
  }
  auto var = init.toVariant();
  var.setEvalScalar();
  return *var.asTypedValue();
}

Type loosen_this_prop_for_serialization(const php::Class& ctx,
                                        SString name,
                                        Type type) {
  // The 86reified_prop has special enforcement for serialization, so
  // we don't have to pessimize it as much.
  if (name == s_86reified_prop.get()) {
    return union_of(
      std::move(type),
      get_type_of_reified_list(ctx.userAttributes)
    );
  }
  return loosen_vec_or_dict(loosen_all(std::move(type)));
}

//////////////////////////////////////////////////////////////////////

namespace php {

//////////////////////////////////////////////////////////////////////

ClassBase::ClassBase(const ClassBase& other) {
  for (auto& m : other.methods) {
    if (!m) {
      methods.emplace_back();
    } else {
      methods.emplace_back(std::make_unique<php::Func>(*m));
    }
  }
  for (auto& c : other.closures) {
    closures.emplace_back(std::make_unique<php::Class>(*c));
  }
}

//////////////////////////////////////////////////////////////////////

}

//////////////////////////////////////////////////////////////////////

}
