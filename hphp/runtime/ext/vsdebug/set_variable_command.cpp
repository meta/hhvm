/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2017-present Facebook, Inc. (http://www.facebook.com)  |
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

#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/backtrace.h"
#include "hphp/runtime/base/static-string-table.h"
#include "hphp/runtime/base/string-util.h"
#include "hphp/runtime/ext/vsdebug/command.h"
#include "hphp/runtime/ext/vsdebug/debugger.h"
#include "hphp/runtime/vm/runtime.h"
#include "hphp/runtime/vm/vm-regs.h"

namespace HPHP {
namespace VSDEBUG {

SetVariableCommand::SetVariableCommand(
  Debugger* debugger,
  folly::dynamic message
) : VSCommand(debugger, message),
    m_objectId{0} {

  const folly::dynamic& args = tryGetObject(message, "arguments", s_emptyArgs);
  const int variablesReference = tryGetInt(args, "variablesReference", -1);
  if (variablesReference < 0) {
    throw DebuggerCommandException("Invalid variablesReference specified.");
  }

  m_objectId = variablesReference;
}

SetVariableCommand::~SetVariableCommand() {
}

request_id_t SetVariableCommand::targetThreadId(DebuggerSession* session) {
  ServerObject* obj = session->getServerObject(m_objectId);
  if (obj == nullptr) {
    throw DebuggerCommandException("Invalid variablesReference specified.");
  }

  if (obj->objectType() == ServerObjectType::Scope) {
    ScopeObject* scope = static_cast<ScopeObject*>(obj);
    return scope->m_requestId;
  } else if (obj->objectType() == ServerObjectType::Variable) {
    VariableObject* variable = static_cast<VariableObject*>(obj);
    return variable->m_requestId;
  }

  throw DebuggerCommandException("Unexpected server object type");
}

bool SetVariableCommand::executeImpl(
  DebuggerSession* session,
  folly::dynamic* responseMsg
) {
  // The request thread should not re-enter the debugger while
  // processing this command.
  DebuggerNoBreakContext noBreak(m_debugger);

  folly::dynamic body = folly::dynamic::object;
  folly::dynamic variables = folly::dynamic::array;
  auto& args = tryGetObject(getMessage(), "arguments", s_emptyArgs);

  ServerObject* obj = session->getServerObject(m_objectId);
  if (obj == nullptr) {
    throw DebuggerCommandException("Invalid variablesReference specified.");
  }

  const std::string& suppliedName = tryGetString(args, "name", "");
  if (suppliedName.empty()) {
    throw DebuggerCommandException("Invalid variable name specified.");
  }

  // Remove any prefixes that we would have added to the variable name
  // before presenting it to the front-end.
  std::string name = removeVariableNamePrefix(suppliedName);
  bool success = false;
  const std::string& strValue = tryGetString(args, "value", "");

  try {
    if (obj->objectType() == ServerObjectType::Scope) {
      ScopeObject* scope = static_cast<ScopeObject*>(obj);

      switch (scope->m_scopeType) {
        case ScopeType::Locals:
          success = setLocalVariable(
            session,
            name,
            strValue,
            scope,
            &body
          );
          break;

        case ScopeType::ServerConstants:
          success = setConstant(
            session,
            name,
            strValue,
            scope,
            &body
          );
          break;

        // Superglobals and core constants are provided by the current execution
        // context, rather than trying to overwrite them here, defer to the PHP
        // console, which will let the runtime enforce whatever policies are
        // appropriate.
        case ScopeType::Superglobals:
          m_debugger->sendUserMessage(
            "Could not directly set value of superglobal variable, you may "
              "be able to set this value by running a Hack/PHP command in the "
              " console.",
            DebugTransport::OutputLevelError
          );
          break;

        default:
          assertx(false);
      }
    } else if (obj->objectType() == ServerObjectType::Variable) {
      VariableObject* variable = static_cast<VariableObject*>(obj);
      Variant& variant = variable->m_variable;
      if (variant.isArray()) {
        success = setArrayVariable(session, name, strValue, variable, &body);
      } else if (variant.isObject()) {
        success = setObjectVariable(session, name, strValue, variable, &body);
      } else {
        throw DebuggerCommandException(
          "Failed to set variable: Unexpected variable type."
        );
      }
    }
  } catch (DebuggerCommandException &e) {
    m_debugger->sendUserMessage(
      e.what(),
      DebugTransport::OutputLevelError
    );
    throw e;
  }

  if (!success) {
    throw DebuggerCommandException(
      "Failed to set variable."
    );
  }

  (*responseMsg)["body"] = body;
  return false;
}

bool SetVariableCommand::setLocalVariable(
  DebuggerSession* session,
  const std::string& name,
  const std::string& value,
  ScopeObject* scope,
  folly::dynamic* result
) {
  VMRegAnchor _regAnchor;

  const auto fp =
    g_context->getFrameAtDepthForDebuggerUnsafe(scope->m_frameDepth);
  if (fp == nullptr ||
      fp->isInlined() ||
      fp->skipFrame()) {
    // Can't set variable in a frame with no context.
    return false;
  }

  const auto func = fp->func();
  if (func == nullptr) {
    return false;
  }

  const auto localCount = func->numNamedLocals();

  for (Id id = 0; id < localCount; id++) {
    // Check for unnamed local.
    auto const localNameSd = func->localVarName(id);
    if (!localNameSd) continue;

    auto const frameValue = frame_local(fp, id);
    const std::string localName = localNameSd->toCppString();
    if (localName == name) {
      setVariableValue(
        session,
        name,
        value,
        frameValue,
        scope->m_requestId,
        result
      );
      return true;
    }
  }

  return false;
}

const StaticString s_user("user");

bool SetVariableCommand::setConstant(
  DebuggerSession* session,
  const std::string& name,
  const std::string& value,
  ScopeObject* scope,
  folly::dynamic* result
) {
  const auto& constants = lookupDefinedConstants(false);
  for (ArrayIter iter(constants); iter; ++iter) {
    const std::string constantName = iter.first().toString().toCppString();
    if (constantName == name) {
      TypedValue* constantValue = iter.second().asTypedValue();
      setVariableValue(
        session,
        name,
        value,
        constantValue,
        scope->m_requestId,
        result
      );
      return true;
    }
  }
  return false;
}

bool SetVariableCommand::setArrayVariable(
  DebuggerSession* session,
  const std::string& name,
  const std::string& value,
  VariableObject* array,
  folly::dynamic* result
) {
  VMRegAnchor regAnchor;

  Variant& var = array->m_variable;
  assertx(var.isArray());

  Array arr = var.toArray();
  for (ArrayIter iter(arr); iter; ++iter) {
    std::string indexName = iter.first().toString().toCppString();
    if (indexName == name) {
      TypedValue* arrayValue = iter.second().asTypedValue();
      setVariableValue(
        session,
        name,
        value,
        arrayValue,
        array->m_requestId,
        result
      );

      auto keyVariant = iter.first();
      if (keyVariant.isString()) {
        HPHP::OptString key = keyVariant.toString();
        arr.set(key, tvToInit(*arrayValue));
      } else if (keyVariant.isInteger()) {
        int64_t key = keyVariant.toInt64();
        arr.set(key, tvToInit(*arrayValue));
      } else {
        throw DebuggerCommandException("Unsupported array key type.");
      }
      return true;
    }
  }

  return false;
}

bool SetVariableCommand::setObjectVariable(
  DebuggerSession* session,
  const std::string& name,
  const std::string& value,
  VariableObject* object,
  folly::dynamic* result
) {
  Variant& var = object->m_variable;
  assertx(var.isObject());

  HPHP::OptString key(name);
  ObjectData* obj = var.getObjectData();
  Variant currentValue = obj->o_get(key, false);
  if (!currentValue.isInitialized()) {
    throw DebuggerCommandException(
      "Failed to set variable: Property not found on object."
    );
  }

  TypedValue* propValue = currentValue.asTypedValue();

  setVariableValue(
    session,
    name,
    value,
    propValue,
    object->m_requestId,
    result
  );
  obj->o_set(key, currentValue);
  return true;
}

bool SetVariableCommand::getBooleanValue(const std::string& str) {
  // Trim leading and trailing whitespace.
  std::string trimmed = trimString(str);

  // Boolean values in PHP are not case sensitive.
  for (char& c : trimmed) {
    c = std::toupper(c);
  }

  if (trimmed == "TRUE") {
    return true;
  } else if (trimmed == "FALSE") {
    return false;
  }

  throw DebuggerCommandException(
    "The specified value was not a valid boolean value."
  );
}

void SetVariableCommand::setVariableValue(
  DebuggerSession* session,
  const std::string& name,
  const std::string& value,
  tv_lval typedVariable,
  request_id_t requestId,
  folly::dynamic* result
) {
  switch (type(typedVariable)) {
    case KindOfBoolean: {
        bool boolVal = getBooleanValue(value);
        val(typedVariable).num = boolVal ? 1 : 0;
      }
      break;

    case KindOfInt64:
      try {
        val(typedVariable).num = std::stoi(value, nullptr, 0);
      } catch (std::exception &) {
        throw DebuggerCommandException("Invalid value specified.");
      }
      break;

    case KindOfDouble:
      try {
        val(typedVariable).dbl = std::stod(value);
      } catch (std::exception &) {
        throw DebuggerCommandException("Invalid value specified.");
      }
      break;

    case KindOfString: {
      const auto newSd = StringData::Make(
        value.c_str(),
        CopyString
      );

      if (type(typedVariable) == KindOfString &&
          val(typedVariable).pstr != nullptr) {
        val(typedVariable).pstr->decRefCount();
      }

      val(typedVariable).pstr = newSd;
      type(typedVariable) = KindOfString;
      break;
    }

    case KindOfUninit:
    case KindOfNull:
      // In the case of uninit and null, we don't even know how to interpret
      // the value from the client, because the object has no known type.
      // Fallthrough.
    case KindOfResource:
    case KindOfVec:
    case KindOfDict:
    case KindOfKeyset:
    case KindOfObject:
      // For complex types, we need to run PHP code to create a new object
      // or determine what reference to assign, making direct assignment via
      // reflection impractical, so we defer to the console REPL, which will use
      // an evaluation command to run PHP.
      // NOTE: It would be nice in the future to just run an eval command here
      // on the user's behalf. At the moment, if we are setting a child prop
      // on an object or array, we don't know the fully qualified name string
      // to pass to PHP though, since we only have a reference to the container.
      throw DebuggerCommandException(
        "Failed to set object value. Please use the console to set the value "
        "of complex objects."
      );

    default:
      throw DebuggerCommandException("Unexpected variable type");
  }

  Variant variable{variant_ref{typedVariable}};
  *result = VariablesCommand::serializeVariable(
              session,
              m_debugger,
              requestId,
              name,
              variable
            );
}

}
}
