#include "ast/component/instance.h"
#include "ast/module.h"
#include "common/errcode.h"
#include "executor/executor.h"

#include "runtime/instance/module.h"

#include <string_view>
#include <variant>

namespace WasmEdge {
namespace Executor {

// Instantiate module instance. See "include/executor/Executor.h".
Expect<std::unique_ptr<Runtime::Instance::ComponentInstance>>
Executor::instantiate(Runtime::StoreManager &StoreMgr,
                      const AST::Component::Component &Comp,
                      std::optional<std::string_view> Name) {
  std::unique_ptr<Runtime::Instance::ComponentInstance> CompInst;
  if (Name.has_value()) {
    CompInst =
        std::make_unique<Runtime::Instance::ComponentInstance>(Name.value());
  } else {
    CompInst = std::make_unique<Runtime::Instance::ComponentInstance>("");
  }

  // TODO: figure out where will use these sections
  Comp.getCoreTypeSection();
  Comp.getAliasSection();
  Comp.getTypeSection();
  Comp.getCanonSection();
  Comp.getStartSection();
  Comp.getImportSection();
  Comp.getExportSection();

  spdlog::info("get definition list of core:module and component");
  // NOTE: get nested definition, so later instantiate expression can use.
  auto ModList = Comp.getCoreModuleSection().getContent();
  auto CompList = Comp.getComponentSection().getContent();
  spdlog::info("how many core:module there? {}", ModList.size());

  for (auto InstSec : Comp.getCoreInstanceSection()) {
    for (const AST::Component::CoreInstanceExpr &InstExpr :
         InstSec.getContent()) {
      if (std::holds_alternative<AST::Component::CoreInstantiate>(InstExpr)) {
        spdlog::info("instantiate module");
        auto Instantiate = std::get<AST::Component::CoreInstantiate>(InstExpr);

        auto Mod = ModList[Instantiate.getModuleIdx()];
        // TODO: arguments should lead some StoreMgr updation, but current
        // implementation didn't support stacking NamedXxx for now
        instantiate(StoreMgr, Mod, "");
      } else {
        spdlog::info("inline exports");
        std::get<AST::Component::CoreInlineExports>(InstExpr).getExports();
        // TODO:
      }
    }
  }

  for (auto InstSec : Comp.getInstanceSection()) {
    for (const AST::Component::InstanceExpr &InstExpr : InstSec.getContent()) {
      if (std::holds_alternative<AST::Component::Instantiate>(InstExpr)) {
        spdlog::info("instantiate component");
        auto Instantiate = std::get<AST::Component::Instantiate>(InstExpr);

        auto LocalComp = CompList[Instantiate.getComponentIdx()];
        // TODO: arguments should lead some StoreMgr updation, but current
        // implementation didn't support stacking NamedXxx for now
        instantiate(StoreMgr, *LocalComp, "");
      } else {
        spdlog::info("inline exports");
        std::get<AST::Component::CompInlineExports>(InstExpr).getExports();
        // TODO:
      }
    }
  }

  // TODO: ?

  spdlog::info("complete component instantiation");
  return CompInst;
}

} // namespace Executor
} // namespace WasmEdge
