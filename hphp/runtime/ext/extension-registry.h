#pragma once

#include <string>
#include <vector>

#include "hphp/runtime/ext/extension.h"
#include "hphp/runtime/base/ini-setting.h"
#include "hphp/runtime/base/type-string.h"
#include "hphp/util/hdf.h"

namespace HPHP {

namespace jit {
struct ProfDataSerializer;
struct ProfDataDeserializer;
}

namespace ExtensionRegistry {
/////////////////////////////////////////////////////////////////////////////

void registerExtension(Extension* ext);

bool isLoaded(const char* name);
inline bool isLoaded(const OptString& name) {
  return isLoaded(name.data());
}

Extension* get(const char* name, bool ignoreDisabled = true);
inline Extension* get(const OptString& name) {
  return get(name.data());
}

Array getLoaded();

const std::vector<const Extension*> getExtensions();

// called by RuntimeOption to initialize all configurations of extension
void moduleLoad(const IniSetting::Map& ini, Hdf hdf);

// called by hphp_process_init/exit
void cliClientInit();
void moduleInit();
void moduleRegisterNative();
void moduleShutdown();
void threadInit();
void threadShutdown();
void requestInit();
void requestShutdown();

void moduleDeclInit();

bool modulesInitialised();

struct SerializedExtension {
  std::string name;
  std::vector<char> payload;

  bool operator==(const SerializedExtension&) const = default;
};
using SerializedExtensions = std::vector<SerializedExtension>;

// Shared in-memory SerDe for jumpstart and cprof, without profile-file framing.
// Serialization returns nonempty payloads in extension dependency order.
SerializedExtensions serialize(const char* onlyExtension = nullptr);
// False when the extension is missing or disabled; decoding errors propagate.
bool deserialize(const SerializedExtension& data);

// Jumpstart adapters: wrap the shared payload SerDe in ProfData file framing.
void serialize(jit::ProfDataSerializer& ser,
               const char* onlyExtension = nullptr);
void deserialize(jit::ProfDataDeserializer& des);

/////////////////////////////////////////////////////////////////////////////
} // namespace HPHP::ExtensionRegistry

}
