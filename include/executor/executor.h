// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

//===-- wasmedge/executor/executor.h - Executor class definition ----------===//
//
// Part of the WasmEdge Project.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains the declaration of the Executor class, which instantiate
/// and run Wasm modules.
///
//===----------------------------------------------------------------------===//
#pragma once

#include "ast/component/component.h"
#include "ast/module.h"
#include "common/async.h"
#include "common/configure.h"
#include "common/defines.h"
#include "common/errcode.h"
#include "common/statistics.h"
#include "runtime/callingframe.h"
#include "runtime/instance/component/component.h"
#include "runtime/instance/module.h"
#include "runtime/stackmgr.h"
#include "runtime/storemgr.h"

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace WasmEdge {
namespace Executor {

namespace {

// Template return type aliasing
/// Accept unsigned integer types. (uint32_t, uint64_t)
template <typename T>
using TypeU = typename std::enable_if_t<IsWasmUnsignV<T>, Expect<void>>;
/// Accept integer types. (uint32_t, int32_t, uint64_t, int64_t)
template <typename T>
using TypeI = typename std::enable_if_t<IsWasmIntV<T>, Expect<void>>;
/// Accept floating types. (float, double)
template <typename T>
using TypeF = typename std::enable_if_t<IsWasmFloatV<T>, Expect<void>>;
/// Accept all num types. (uint32_t, int32_t, uint64_t, int64_t, float, double)
template <typename T>
using TypeT = typename std::enable_if_t<IsWasmNumV<T>, Expect<void>>;
/// Accept Wasm built-in num types. (uint32_t, uint64_t, float, double)
template <typename T>
using TypeN = typename std::enable_if_t<IsWasmNativeNumV<T>, Expect<void>>;

/// Accept (unsigned integer types, unsigned integer types).
template <typename T1, typename T2>
using TypeUU = typename std::enable_if_t<IsWasmUnsignV<T1> && IsWasmUnsignV<T2>,
                                         Expect<void>>;
/// Accept (integer types, unsigned integer types).
template <typename T1, typename T2>
using TypeIU = typename std::enable_if_t<IsWasmIntV<T1> && IsWasmUnsignV<T2>,
                                         Expect<void>>;
/// Accept (floating types, floating types).
template <typename T1, typename T2>
using TypeFF = typename std::enable_if_t<IsWasmFloatV<T1> && IsWasmFloatV<T2>,
                                         Expect<void>>;
/// Accept (integer types, floating types).
template <typename T1, typename T2>
using TypeIF =
    typename std::enable_if_t<IsWasmIntV<T1> && IsWasmFloatV<T2>, Expect<void>>;
/// Accept (floating types, integer types).
template <typename T1, typename T2>
using TypeFI =
    typename std::enable_if_t<IsWasmFloatV<T1> && IsWasmIntV<T2>, Expect<void>>;
/// Accept (Wasm built-in num types, Wasm built-in num types).
template <typename T1, typename T2>
using TypeNN =
    typename std::enable_if_t<IsWasmNativeNumV<T1> && IsWasmNativeNumV<T2> &&
                                  sizeof(T1) == sizeof(T2),
                              Expect<void>>;

} // namespace

/// Helper class for handling the pre- and post- host functions
class HostFuncHandler {
public:
  void setPreHost(void *HostData, std::function<void(void *)> HostFunc) {
    std::unique_lock Lock(Mutex);
    PreHostData = HostData;
    PreHostFunc = HostFunc;
  }
  void setPostHost(void *HostData, std::function<void(void *)> HostFunc) {
    std::unique_lock Lock(Mutex);
    PostHostData = HostData;
    PostHostFunc = HostFunc;
  }
  void invokePreHostFunc() {
    if (PreHostFunc.operator bool()) {
      PreHostFunc(PreHostData);
    }
  }
  void invokePostHostFunc() {
    if (PostHostFunc.operator bool()) {
      PostHostFunc(PostHostData);
    }
  }

private:
  void *PreHostData = nullptr;
  void *PostHostData = nullptr;
  std::function<void(void *)> PreHostFunc = {};
  std::function<void(void *)> PostHostFunc = {};
  mutable std::shared_mutex Mutex;
};

/// Executor flow control class.
class Executor {
public:
  Executor(const Configure &Conf, Statistics::Statistics *S = nullptr) noexcept
      : Conf(Conf) {
    if (Conf.getStatisticsConfigure().isInstructionCounting() ||
        Conf.getStatisticsConfigure().isCostMeasuring() ||
        Conf.getStatisticsConfigure().isTimeMeasuring()) {
      Stat = S;
    } else {
      Stat = nullptr;
    }
    if (Stat) {
      Stat->setCostLimit(Conf.getStatisticsConfigure().getCostLimit());
    }
  }

  /// Getter of Configure
  const Configure &getConfigure() const { return Conf; }

  /// Instantiate a WASM Module into an anonymous module instance.
  Expect<std::unique_ptr<Runtime::Instance::ModuleInstance>>
  instantiateModule(Runtime::StoreManager &StoreMgr, const AST::Module &Mod);

  /// Instantiate and register a WASM module into a named module instance.
  Expect<std::unique_ptr<Runtime::Instance::ModuleInstance>>
  registerModule(Runtime::StoreManager &StoreMgr, const AST::Module &Mod,
                 std::string_view Name);

  /// Register an instantiated module into a named module instance.
  Expect<void> registerModule(Runtime::StoreManager &StoreMgr,
                              const Runtime::Instance::ModuleInstance &ModInst);

  /// Instantiate a Component into an anonymous component instance.
  Expect<std::unique_ptr<Runtime::Instance::ComponentInstance>>
  instantiateComponent(Runtime::StoreManager &StoreMgr,
                       const AST::Component::Component &Comp);

  /// Instantiate and register a Component into a named component instance.
  Expect<std::unique_ptr<Runtime::Instance::ComponentInstance>>
  registerComponent(Runtime::StoreManager &StoreMgr,
                    const AST::Component::Component &Comp,
                    std::string_view Name);

  /// Register an instantiated component into a named component instance.
  Expect<void>
  registerComponent(Runtime::StoreManager &StoreMgr,
                    const Runtime::Instance::ComponentInstance &CompInst);

  /// Register a host function which will be invoked before calling a
  /// host function.
  Expect<void> registerPreHostFunction(void *HostData,
                                       std::function<void(void *)> HostFunc);

  /// Register a host function which will be invoked after calling a
  /// host function.
  Expect<void> registerPostHostFunction(void *HostData,
                                        std::function<void(void *)> HostFunc);

  /// Invoke a WASM function by function instance.
  Expect<std::vector<std::pair<ValVariant, ValType>>>
  invoke(const Runtime::Instance::FunctionInstance *FuncInst,
         Span<const ValVariant> Params, Span<const ValType> ParamTypes);

  /// Invoke a Component function by function instance.
  Expect<std::vector<std::pair<ValInterface, ValType>>>
  invoke(const Runtime::Instance::Component::FunctionInstance *FuncInst,
         Span<const ValInterface> Params, Span<const ValType> ParamTypes);

  /// Asynchronous invoke a WASM function by function instance.
  Async<Expect<std::vector<std::pair<ValVariant, ValType>>>>
  asyncInvoke(const Runtime::Instance::FunctionInstance *FuncInst,
              Span<const ValVariant> Params, Span<const ValType> ParamTypes);

  /// Stop execution
  void stop() noexcept {
    StopToken.store(1, std::memory_order_relaxed);
    atomicNotifyAll();
  }

private:
  /// Run Wasm bytecode expression for initialization.
  Expect<void> runExpression(Runtime::StackManager &StackMgr,
                             AST::InstrView Instrs);

  /// Run Wasm function.
  Expect<void> runFunction(Runtime::StackManager &StackMgr,
                           const Runtime::Instance::FunctionInstance &Func,
                           Span<const ValVariant> Params);

  /// Execute instructions.
  Expect<void> execute(Runtime::StackManager &StackMgr,
                       const AST::InstrView::iterator Start,
                       const AST::InstrView::iterator End);

  /// \name Functions for instantiation.
  /// @{
  /// Instantiation of Module Instance.
  Expect<std::unique_ptr<Runtime::Instance::ModuleInstance>>
  instantiate(Runtime::StoreManager &StoreMgr, const AST::Module &Mod,
              std::optional<std::string_view> Name = std::nullopt);

  /// Instantiation of Imports.
  Expect<void> instantiate(Runtime::StoreManager &StoreMgr,
                           Runtime::Instance::ModuleInstance &ModInst,
                           const AST::ImportSection &ImportSec);

  /// Instantiation of Function Instances.
  Expect<void> instantiate(Runtime::Instance::ModuleInstance &ModInst,
                           const AST::FunctionSection &FuncSec,
                           const AST::CodeSection &CodeSec);

  /// Instantiation of Table Instances.
  Expect<void> instantiate(Runtime::StackManager &StackMgr,
                           Runtime::Instance::ModuleInstance &ModInst,
                           const AST::TableSection &TabSec);

  /// Instantiation of Memory Instances.
  Expect<void> instantiate(Runtime::Instance::ModuleInstance &ModInst,
                           const AST::MemorySection &MemSec);

  /// Instantiateion of Tag Instances.
  Expect<void> instantiate(Runtime::Instance::ModuleInstance &ModInst,
                           const AST::TagSection &TagSec);

  /// Instantiation of Global Instances.
  Expect<void> instantiate(Runtime::StackManager &StackMgr,
                           Runtime::Instance::ModuleInstance &ModInst,
                           const AST::GlobalSection &GlobSec);

  /// Instantiation of Element Instances.
  Expect<void> instantiate(Runtime::StackManager &StackMgr,
                           Runtime::Instance::ModuleInstance &ModInst,
                           const AST::ElementSection &ElemSec);

  /// Initialize table with Element Instances.
  Expect<void> initTable(Runtime::StackManager &StackMgr,
                         const AST::ElementSection &ElemSec);

  /// Instantiation of Data Instances.
  Expect<void> instantiate(Runtime::StackManager &StackMgr,
                           Runtime::Instance::ModuleInstance &ModInst,
                           const AST::DataSection &DataSec);

  /// Initialize memory with Data Instances.
  Expect<void> initMemory(Runtime::StackManager &StackMgr,
                          const AST::DataSection &DataSec);

  /// Instantiation of Exports.
  Expect<void> instantiate(Runtime::Instance::ModuleInstance &ModInst,
                           const AST::ExportSection &ExportSec);
  /// @}

  /// @{
  /// Instantiation of Component Instance.
  Expect<std::unique_ptr<Runtime::Instance::ComponentInstance>>
  instantiate(Runtime::StoreManager &StoreMgr,
              const AST::Component::Component &Comp,
              std::optional<std::string_view> Name = std::nullopt);
  Expect<void>
  instantiate(Runtime::StoreManager &StoreMgr,
              Runtime::Instance::ComponentInstance &CompInst,
              const AST::Component::CoreInstanceSection &CoreInstSec);
  Expect<void> instantiate(Runtime::StoreManager &StoreMgr,
                           Runtime::Instance::ComponentInstance &CompInst,
                           const AST::Component::CoreTypeSection &CoreTypeSec);
  Expect<void> instantiate(Runtime::StoreManager &StoreMgr,
                           Runtime::Instance::ComponentInstance &CompInst,
                           const AST::Component::InstanceSection &InstSec);
  Expect<void> instantiate(Runtime::StoreManager &StoreMgr,
                           Runtime::Instance::ComponentInstance &CompInst,
                           const AST::Component::AliasSection &AliasSec);
  Expect<void> instantiate(Runtime::StoreManager &StoreMgr,
                           Runtime::Instance::ComponentInstance &CompInst,
                           const AST::Component::TypeSection &TypeSec);
  Expect<void> instantiate(Runtime::StoreManager &StoreMgr,
                           Runtime::Instance::ComponentInstance &CompInst,
                           const AST::Component::CanonSection &CanonSec);
  Expect<void> instantiate(Runtime::StoreManager &StoreMgr,
                           Runtime::Instance::ComponentInstance &CompInst,
                           const AST::Component::StartSection &StartSec);
  Expect<void> instantiate(Runtime::StoreManager &StoreMgr,
                           Runtime::Instance::ComponentInstance &CompInst,
                           const AST::Component::ImportSection &ImportSec);
  Expect<void> instantiate(Runtime::StoreManager &StoreMgr,
                           Runtime::Instance::ComponentInstance &CompInst,
                           const AST::Component::ExportSection &ExportSec);
  /// @}

  /// \name Helper Functions for canonical ABI
  /// @{
  std::unique_ptr<Runtime::Instance::Component::FunctionInstance>
  lifting(Runtime::Instance::ComponentInstance &Comp,
          const WasmEdge::AST::Component::FuncType &FuncType,
          Runtime::Instance::FunctionInstance *F,
          Runtime::Instance::MemoryInstance *Memory,
          Runtime::Instance::FunctionInstance *Realloc);

  std::unique_ptr<Runtime::Instance::FunctionInstance>
  lowering(Runtime::Instance::Component::FunctionInstance *F,
           Runtime::Instance::MemoryInstance *Memory,
           Runtime::Instance::FunctionInstance *Realloc);
  /// @}

  /// \name Helper Functions for block controls.
  /// @{
  /// Helper function for calling functions. Return the continuation iterator.
  Expect<AST::InstrView::iterator>
  enterFunction(Runtime::StackManager &StackMgr,
                const Runtime::Instance::FunctionInstance &Func,
                const AST::InstrView::iterator RetIt, bool IsTailCall = false);

  /// Helper function for branching to label.
  Expect<void> branchToLabel(Runtime::StackManager &StackMgr,
                             const AST::Instruction::JumpDescriptor &JumpDesc,
                             AST::InstrView::iterator &PC) noexcept;

  /// Helper function for throwing an exception.
  Expect<void> throwException(Runtime::StackManager &StackMgr,
                              Runtime::Instance::TagInstance &TagInst,
                              AST::InstrView::iterator &PC) noexcept;
  /// @}

  /// \name Helper Functions for GC instructions.
  /// @{
  Expect<RefVariant> structNew(Runtime::StackManager &StackMgr,
                               const uint32_t TypeIdx,
                               Span<const ValVariant> Args = {}) const noexcept;
  Expect<ValVariant> structGet(Runtime::StackManager &StackMgr,
                               const RefVariant Ref, const uint32_t TypeIdx,
                               const uint32_t Off,
                               const bool IsSigned = false) const noexcept;
  Expect<void> structSet(Runtime::StackManager &StackMgr, const RefVariant Ref,
                         const ValVariant Val, const uint32_t TypeIdx,
                         const uint32_t Off) const noexcept;
  Expect<RefVariant> arrayNew(Runtime::StackManager &StackMgr,
                              const uint32_t TypeIdx, const uint32_t Length,
                              Span<const ValVariant> Args = {}) const noexcept;
  Expect<RefVariant> arrayNewData(Runtime::StackManager &StackMgr,
                                  const uint32_t TypeIdx,
                                  const uint32_t DataIdx, const uint32_t Start,
                                  const uint32_t Length) const noexcept;
  Expect<RefVariant> arrayNewElem(Runtime::StackManager &StackMgr,
                                  const uint32_t TypeIdx,
                                  const uint32_t ElemIdx, const uint32_t Start,
                                  const uint32_t Length) const noexcept;
  Expect<ValVariant> arrayGet(Runtime::StackManager &StackMgr,
                              const RefVariant &Ref, const uint32_t TypeIdx,
                              const uint32_t Idx,
                              const bool IsSigned = false) const noexcept;
  Expect<void> arraySet(Runtime::StackManager &StackMgr, const RefVariant &Ref,
                        const ValVariant &Val, const uint32_t TypeIdx,
                        const uint32_t Idx) const noexcept;
  Expect<void> arrayFill(Runtime::StackManager &StackMgr, const RefVariant &Ref,
                         const ValVariant &Val, const uint32_t TypeIdx,
                         const uint32_t Idx, const uint32_t Cnt) const noexcept;
  Expect<void> arrayInitData(Runtime::StackManager &StackMgr,
                             const RefVariant &Ref, const uint32_t TypeIdx,
                             const uint32_t DataIdx, const uint32_t DstIdx,
                             const uint32_t SrcIdx,
                             const uint32_t Cnt) const noexcept;
  Expect<void> arrayInitElem(Runtime::StackManager &StackMgr,
                             const RefVariant &Ref, const uint32_t TypeIdx,
                             const uint32_t ElemIdx, const uint32_t DstIdx,
                             const uint32_t SrcIdx,
                             const uint32_t Cnt) const noexcept;
  Expect<void> arrayCopy(Runtime::StackManager &StackMgr,
                         const RefVariant &DstRef, const uint32_t DstTypeIdx,
                         const uint32_t DstIdx, const RefVariant &SrcRef,
                         const uint32_t SrcTypeIdx, const uint32_t SrcIdx,
                         const uint32_t Cnt) const noexcept;
  /// @}

  /// \name Helper Functions for atomic operations.
  /// @{
  template <typename T>
  Expect<uint32_t> atomicWait(Runtime::Instance::MemoryInstance &MemInst,
                              uint32_t Address, T Expected,
                              int64_t Timeout) noexcept;
  Expect<uint32_t> atomicNotify(Runtime::Instance::MemoryInstance &MemInst,
                                uint32_t Address, uint32_t Count) noexcept;
  void atomicNotifyAll() noexcept;
  /// @}

  /// \name Helper Functions for getting instances or types.
  /// @{
  /// Helper function for getting defined type by index.
  const AST::SubType *getDefTypeByIdx(Runtime::StackManager &StackMgr,
                                      const uint32_t Idx) const;

  /// Helper function for getting composite type by index. Assuming validated.
  const WasmEdge::AST::CompositeType &
  getCompositeTypeByIdx(Runtime::StackManager &StackMgr,
                        const uint32_t Idx) const noexcept;

  /// Helper function for getting struct storage type by index.
  const ValType &getStructStorageTypeByIdx(Runtime::StackManager &StackMgr,
                                           const uint32_t Idx,
                                           const uint32_t Off) const noexcept;

  /// Helper function for getting array storage type by index.
  const ValType &getArrayStorageTypeByIdx(Runtime::StackManager &StackMgr,
                                          const uint32_t Idx) const noexcept;

  /// Helper function for getting function instance by index.
  Runtime::Instance::FunctionInstance *
  getFuncInstByIdx(Runtime::StackManager &StackMgr, const uint32_t Idx) const;

  /// Helper function for getting table instance by index.
  Runtime::Instance::TableInstance *
  getTabInstByIdx(Runtime::StackManager &StackMgr, const uint32_t Idx) const;

  /// Helper function for getting memory instance by index.
  Runtime::Instance::MemoryInstance *
  getMemInstByIdx(Runtime::StackManager &StackMgr, const uint32_t Idx) const;

  /// Helper function for getting tag instance by index.
  Runtime::Instance::TagInstance *
  getTagInstByIdx(Runtime::StackManager &StackMgr, const uint32_t Idx) const;

  /// Helper function for getting global instance by index.
  Runtime::Instance::GlobalInstance *
  getGlobInstByIdx(Runtime::StackManager &StackMgr, const uint32_t Idx) const;

  /// Helper function for getting element instance by index.
  Runtime::Instance::ElementInstance *
  getElemInstByIdx(Runtime::StackManager &StackMgr, const uint32_t Idx) const;

  /// Helper function for getting data instance by index.
  Runtime::Instance::DataInstance *
  getDataInstByIdx(Runtime::StackManager &StackMgr, const uint32_t Idx) const;

  /// Helper function for converting into bottom abstract heap type.
  TypeCode toBottomType(Runtime::StackManager &StackMgr,
                        const ValType &Type) const;

  /// Helper function for cleaning unused bits of numeric values in ValVariant.
  void cleanNumericVal(ValVariant &Val, const ValType &Type) const noexcept;

  /// Helper function for packing ValVariant for packed type.
  ValVariant packVal(const ValType &Type, const ValVariant &Val) const noexcept;

  /// Helper function for packing ValVariant vector for packed type.
  std::vector<ValVariant>
  packVals(const ValType &Type, std::vector<ValVariant> &&Vals) const noexcept;

  /// Helper function for unpacking ValVariant for packed type.
  ValVariant unpackVal(const ValType &Type, const ValVariant &Val,
                       bool IsSigned = false) const noexcept;
  /// @}

  /// \name Interpreter - Run instructions functions
  /// @{
  /// ======= Control instructions =======
  Expect<void> runIfElseOp(Runtime::StackManager &StackMgr,
                           const AST::Instruction &Instr,
                           AST::InstrView::iterator &PC) noexcept;
  Expect<void> runThrowOp(Runtime::StackManager &StackMgr,
                          const AST::Instruction &Instr,
                          AST::InstrView::iterator &PC) noexcept;
  Expect<void> runThrowRefOp(Runtime::StackManager &StackMgr,
                             const AST::Instruction &Instr,
                             AST::InstrView::iterator &PC) noexcept;
  Expect<void> runBrOp(Runtime::StackManager &StackMgr,
                       const AST::Instruction &Instr,
                       AST::InstrView::iterator &PC) noexcept;
  Expect<void> runBrIfOp(Runtime::StackManager &StackMgr,
                         const AST::Instruction &Instr,
                         AST::InstrView::iterator &PC) noexcept;
  Expect<void> runBrOnNullOp(Runtime::StackManager &StackMgr,
                             const AST::Instruction &Instr,
                             AST::InstrView::iterator &PC) noexcept;
  Expect<void> runBrOnNonNullOp(Runtime::StackManager &StackMgr,
                                const AST::Instruction &Instr,
                                AST::InstrView::iterator &PC) noexcept;
  Expect<void> runBrTableOp(Runtime::StackManager &StackMgr,
                            const AST::Instruction &Instr,
                            AST::InstrView::iterator &PC) noexcept;
  Expect<void> runBrOnCastOp(Runtime::StackManager &StackMgr,
                             const AST::Instruction &Instr,
                             AST::InstrView::iterator &PC,
                             bool IsReverse = false) noexcept;
  Expect<void> runReturnOp(Runtime::StackManager &StackMgr,
                           AST::InstrView::iterator &PC) noexcept;
  Expect<void> runCallOp(Runtime::StackManager &StackMgr,
                         const AST::Instruction &Instr,
                         AST::InstrView::iterator &PC,
                         bool IsTailCall = false) noexcept;
  Expect<void> runCallRefOp(Runtime::StackManager &StackMgr,
                            const AST::Instruction &Instr,
                            AST::InstrView::iterator &PC,
                            bool IsTailCall = false) noexcept;
  Expect<void> runCallIndirectOp(Runtime::StackManager &StackMgr,
                                 const AST::Instruction &Instr,
                                 AST::InstrView::iterator &PC,
                                 bool IsTailCall = false) noexcept;
  Expect<void> runTryTableOp(Runtime::StackManager &StackMgr,
                             const AST::Instruction &Instr,
                             AST::InstrView::iterator &PC) noexcept;
  /// ======= Variable instructions =======
  Expect<void> runLocalGetOp(Runtime::StackManager &StackMgr,
                             uint32_t StackOffset) const noexcept;
  Expect<void> runLocalSetOp(Runtime::StackManager &StackMgr,
                             uint32_t StackOffset) const noexcept;
  Expect<void> runLocalTeeOp(Runtime::StackManager &StackMgr,
                             uint32_t StackOffset) const noexcept;
  Expect<void> runGlobalGetOp(Runtime::StackManager &StackMgr,
                              uint32_t Idx) const noexcept;
  Expect<void> runGlobalSetOp(Runtime::StackManager &StackMgr,
                              uint32_t Idx) const noexcept;
  /// ======= Reference instructions =======
  Expect<void> runRefNullOp(Runtime::StackManager &StackMgr,
                            const ValType &Type) const noexcept;
  Expect<void> runRefIsNullOp(ValVariant &Val) const noexcept;
  Expect<void> runRefFuncOp(Runtime::StackManager &StackMgr,
                            uint32_t Idx) const noexcept;
  Expect<void> runRefEqOp(ValVariant &Val1,
                          const ValVariant &Val2) const noexcept;
  Expect<void> runRefAsNonNullOp(RefVariant &Val,
                                 const AST::Instruction &Instr) const noexcept;
  Expect<void> runStructNewOp(Runtime::StackManager &StackMgr,
                              const uint32_t TypeIdx,
                              const bool IsDefault = false) const noexcept;
  Expect<void> runStructGetOp(Runtime::StackManager &StackMgr,
                              const uint32_t TypeIdx, const uint32_t Off,
                              const AST::Instruction &Instr,
                              const bool IsSigned = false) const noexcept;
  Expect<void> runStructSetOp(Runtime::StackManager &StackMgr,
                              const ValVariant &Val, const uint32_t TypeIdx,
                              const uint32_t Off,
                              const AST::Instruction &Instr) const noexcept;
  Expect<void> runArrayNewOp(Runtime::StackManager &StackMgr,
                             const uint32_t TypeIdx, const uint32_t InitCnt,
                             uint32_t Length) const noexcept;
  Expect<void> runArrayNewDataOp(Runtime::StackManager &StackMgr,
                                 const uint32_t TypeIdx, const uint32_t DataIdx,
                                 const AST::Instruction &Instr) const noexcept;
  Expect<void> runArrayNewElemOp(Runtime::StackManager &StackMgr,
                                 const uint32_t TypeIdx, const uint32_t ElemIdx,
                                 const AST::Instruction &Instr) const noexcept;
  Expect<void> runArrayGetOp(Runtime::StackManager &StackMgr,
                             const uint32_t TypeIdx,
                             const AST::Instruction &Instr,
                             const bool IsSigned = false) const noexcept;
  Expect<void> runArraySetOp(Runtime::StackManager &StackMgr,
                             const ValVariant &Val, const uint32_t TypeIdx,
                             const AST::Instruction &Instr) const noexcept;
  Expect<void> runArrayLenOp(ValVariant &Val,
                             const AST::Instruction &Instr) const noexcept;
  Expect<void> runArrayFillOp(Runtime::StackManager &StackMgr,
                              const uint32_t Cnt, const ValVariant &Val,
                              const uint32_t TypeIdx,
                              const AST::Instruction &Instr) const noexcept;
  Expect<void> runArrayCopyOp(Runtime::StackManager &StackMgr,
                              const uint32_t Cnt, const uint32_t DstTypeIdx,
                              const uint32_t SrcTypeIdx,
                              const AST::Instruction &Instr) const noexcept;
  Expect<void> runArrayInitDataOp(Runtime::StackManager &StackMgr,
                                  const uint32_t Cnt, const uint32_t TypeIdx,
                                  const uint32_t DataIdx,
                                  const AST::Instruction &Instr) const noexcept;
  Expect<void> runArrayInitElemOp(Runtime::StackManager &StackMgr,
                                  const uint32_t Cnt, const uint32_t TypeIdx,
                                  const uint32_t ElemIdx,
                                  const AST::Instruction &Instr) const noexcept;
  Expect<void> runRefTestOp(const Runtime::Instance::ModuleInstance *ModInst,
                            ValVariant &Val, const AST::Instruction &Instr,
                            const bool IsCast = false) const noexcept;
  Expect<void> runRefConvOp(RefVariant &Val, TypeCode TCode) const noexcept;
  Expect<void> runRefI31Op(ValVariant &Val) const noexcept;
  Expect<void> runI31GetOp(ValVariant &Val, const AST::Instruction &Instr,
                           const bool IsSigned = false) const noexcept;
  /// ======= Table instructions =======
  Expect<void> runTableGetOp(Runtime::StackManager &StackMgr,
                             Runtime::Instance::TableInstance &TabInst,
                             const AST::Instruction &Instr);
  Expect<void> runTableSetOp(Runtime::StackManager &StackMgr,
                             Runtime::Instance::TableInstance &TabInst,
                             const AST::Instruction &Instr);
  Expect<void> runTableInitOp(Runtime::StackManager &StackMgr,
                              Runtime::Instance::TableInstance &TabInst,
                              Runtime::Instance::ElementInstance &ElemInst,
                              const AST::Instruction &Instr);
  Expect<void> runElemDropOp(Runtime::Instance::ElementInstance &ElemInst);
  Expect<void> runTableCopyOp(Runtime::StackManager &StackMgr,
                              Runtime::Instance::TableInstance &TabInstDst,
                              Runtime::Instance::TableInstance &TabInstSrc,
                              const AST::Instruction &Instr);
  Expect<void> runTableGrowOp(Runtime::StackManager &StackMgr,
                              Runtime::Instance::TableInstance &TabInst);
  Expect<void> runTableSizeOp(Runtime::StackManager &StackMgr,
                              Runtime::Instance::TableInstance &TabInst);
  Expect<void> runTableFillOp(Runtime::StackManager &StackMgr,
                              Runtime::Instance::TableInstance &TabInst,
                              const AST::Instruction &Instr);
  /// ======= Memory instructions =======
  template <typename T, uint32_t BitWidth = sizeof(T) * 8>
  TypeT<T> runLoadOp(Runtime::StackManager &StackMgr,
                     Runtime::Instance::MemoryInstance &MemInst,
                     const AST::Instruction &Instr);
  template <typename T, uint32_t BitWidth = sizeof(T) * 8>
  TypeN<T> runStoreOp(Runtime::StackManager &StackMgr,
                      Runtime::Instance::MemoryInstance &MemInst,
                      const AST::Instruction &Instr);
  Expect<void> runMemorySizeOp(Runtime::StackManager &StackMgr,
                               Runtime::Instance::MemoryInstance &MemInst);
  Expect<void> runMemoryGrowOp(Runtime::StackManager &StackMgr,
                               Runtime::Instance::MemoryInstance &MemInst);
  Expect<void> runMemoryInitOp(Runtime::StackManager &StackMgr,
                               Runtime::Instance::MemoryInstance &MemInst,
                               Runtime::Instance::DataInstance &DataInst,
                               const AST::Instruction &Instr);
  Expect<void> runDataDropOp(Runtime::Instance::DataInstance &DataInst);
  Expect<void> runMemoryCopyOp(Runtime::StackManager &StackMgr,
                               Runtime::Instance::MemoryInstance &MemInstDst,
                               Runtime::Instance::MemoryInstance &MemInstSrc,
                               const AST::Instruction &Instr);
  Expect<void> runMemoryFillOp(Runtime::StackManager &StackMgr,
                               Runtime::Instance::MemoryInstance &MemInst,
                               const AST::Instruction &Instr);
  /// ======= Test and Relation Numeric instructions =======
  template <typename T> TypeU<T> runEqzOp(ValVariant &Val) const;
  template <typename T>
  TypeT<T> runEqOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T>
  TypeT<T> runNeOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T>
  TypeT<T> runLtOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T>
  TypeT<T> runGtOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T>
  TypeT<T> runLeOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T>
  TypeT<T> runGeOp(ValVariant &Val1, const ValVariant &Val2) const;
  /// ======= Unary Numeric instructions =======
  template <typename T> TypeU<T> runClzOp(ValVariant &Val) const;
  template <typename T> TypeU<T> runCtzOp(ValVariant &Val) const;
  template <typename T> TypeU<T> runPopcntOp(ValVariant &Val) const;
  template <typename T> TypeF<T> runAbsOp(ValVariant &Val) const;
  template <typename T> TypeF<T> runNegOp(ValVariant &Val) const;
  template <typename T> TypeF<T> runCeilOp(ValVariant &Val) const;
  template <typename T> TypeF<T> runFloorOp(ValVariant &Val) const;
  template <typename T> TypeF<T> runTruncOp(ValVariant &Val) const;
  template <typename T> TypeF<T> runNearestOp(ValVariant &Val) const;
  template <typename T> TypeF<T> runSqrtOp(ValVariant &Val) const;
  /// ======= Binary Numeric instructions =======
  template <typename T>
  TypeN<T> runAddOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T>
  TypeN<T> runSubOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T>
  TypeN<T> runMulOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T>
  TypeT<T> runDivOp(const AST::Instruction &Instr, ValVariant &Val1,
                    const ValVariant &Val2) const;
  template <typename T>
  TypeI<T> runRemOp(const AST::Instruction &Instr, ValVariant &Val1,
                    const ValVariant &Val2) const;
  template <typename T>
  TypeU<T> runAndOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T>
  TypeU<T> runOrOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T>
  TypeU<T> runXorOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T>
  TypeU<T> runShlOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T>
  TypeI<T> runShrOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T>
  TypeU<T> runRotlOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T>
  TypeU<T> runRotrOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T>
  TypeF<T> runMinOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T>
  TypeF<T> runMaxOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T>
  TypeF<T> runCopysignOp(ValVariant &Val1, const ValVariant &Val2) const;
  /// ======= Cast Numeric instructions =======
  template <typename TIn, typename TOut>
  TypeUU<TIn, TOut> runWrapOp(ValVariant &Val) const;
  template <typename TIn, typename TOut>
  TypeFI<TIn, TOut> runTruncateOp(const AST::Instruction &Instr,
                                  ValVariant &Val) const;
  template <typename TIn, typename TOut>
  TypeFI<TIn, TOut> runTruncateSatOp(ValVariant &Val) const;
  template <typename TIn, typename TOut, size_t B = sizeof(TIn) * 8>
  TypeIU<TIn, TOut> runExtendOp(ValVariant &Val) const;
  template <typename TIn, typename TOut>
  TypeIF<TIn, TOut> runConvertOp(ValVariant &Val) const;
  template <typename TIn, typename TOut>
  TypeFF<TIn, TOut> runDemoteOp(ValVariant &Val) const;
  template <typename TIn, typename TOut>
  TypeFF<TIn, TOut> runPromoteOp(ValVariant &Val) const;
  template <typename TIn, typename TOut>
  TypeNN<TIn, TOut> runReinterpretOp(ValVariant &Val) const;
  /// ======= SIMD Memory instructions =======
  template <typename TIn, typename TOut>
  Expect<void> runLoadExpandOp(Runtime::StackManager &StackMgr,
                               Runtime::Instance::MemoryInstance &MemInst,
                               const AST::Instruction &Instr);
  template <typename T>
  Expect<void> runLoadSplatOp(Runtime::StackManager &StackMgr,
                              Runtime::Instance::MemoryInstance &MemInst,
                              const AST::Instruction &Instr);
  template <typename T>
  Expect<void> runLoadLaneOp(Runtime::StackManager &StackMgr,
                             Runtime::Instance::MemoryInstance &MemInst,
                             const AST::Instruction &Instr);
  template <typename T>
  Expect<void> runStoreLaneOp(Runtime::StackManager &StackMgr,
                              Runtime::Instance::MemoryInstance &MemInst,
                              const AST::Instruction &Instr);
  /// ======= SIMD Lane instructions =======
  template <typename TIn, typename TOut = TIn>
  Expect<void> runExtractLaneOp(ValVariant &Val, const uint8_t Index) const;
  template <typename TIn, typename TOut = TIn>
  Expect<void> runReplaceLaneOp(ValVariant &Val1, const ValVariant &Val2,
                                const uint8_t Index) const;
  /// ======= SIMD Numeric instructions =======
  template <typename TIn, typename TOut = TIn>
  Expect<void> runSplatOp(ValVariant &Val) const;
  template <typename T>
  Expect<void> runVectorEqOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T>
  Expect<void> runVectorNeOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T>
  Expect<void> runVectorLtOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T>
  Expect<void> runVectorGtOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T>
  Expect<void> runVectorLeOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T>
  Expect<void> runVectorGeOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T> Expect<void> runVectorAbsOp(ValVariant &Val) const;
  template <typename T> Expect<void> runVectorNegOp(ValVariant &Val) const;
  inline Expect<void> runVectorPopcntOp(ValVariant &Val) const;
  template <typename T> Expect<void> runVectorSqrtOp(ValVariant &Val) const;
  template <typename TIn, typename TOut>
  Expect<void> runVectorTruncSatOp(ValVariant &Val) const;
  template <typename TIn, typename TOut>
  Expect<void> runVectorConvertOp(ValVariant &Val) const;
  inline Expect<void> runVectorDemoteOp(ValVariant &Val) const;
  inline Expect<void> runVectorPromoteOp(ValVariant &Val) const;
  inline Expect<void> runVectorAnyTrueOp(ValVariant &Val) const;
  template <typename T> Expect<void> runVectorAllTrueOp(ValVariant &Val) const;
  template <typename T> Expect<void> runVectorBitMaskOp(ValVariant &Val) const;
  template <typename TIn, typename TOut>
  Expect<void> runVectorNarrowOp(ValVariant &Val1,
                                 const ValVariant &Val2) const;
  template <typename TIn, typename TOut>
  Expect<void> runVectorExtendLowOp(ValVariant &Val) const;
  template <typename TIn, typename TOut>
  Expect<void> runVectorExtendHighOp(ValVariant &Val) const;
  template <typename TIn, typename TOut>
  Expect<void> runVectorExtAddPairwiseOp(ValVariant &Val) const;
  template <typename TIn, typename TOut>
  Expect<void> runVectorExtMulLowOp(ValVariant &Val1,
                                    const ValVariant &Val2) const;
  template <typename TIn, typename TOut>
  Expect<void> runVectorExtMulHighOp(ValVariant &Val1,
                                     const ValVariant &Val2) const;
  inline Expect<void> runVectorQ15MulSatOp(ValVariant &Val1,
                                           const ValVariant &Val2) const;
  template <typename T>
  Expect<void> runVectorShlOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T>
  Expect<void> runVectorShrOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T>
  Expect<void> runVectorAddOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T>
  Expect<void> runVectorAddSatOp(ValVariant &Val1,
                                 const ValVariant &Val2) const;
  template <typename T>
  Expect<void> runVectorSubOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T>
  Expect<void> runVectorSubSatOp(ValVariant &Val1,
                                 const ValVariant &Val2) const;
  template <typename T>
  Expect<void> runVectorMulOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T>
  Expect<void> runVectorDivOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T>
  Expect<void> runVectorMinOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T>
  Expect<void> runVectorMaxOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T>
  Expect<void> runVectorFMinOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T>
  Expect<void> runVectorFMaxOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T, typename ET>
  Expect<void> runVectorAvgrOp(ValVariant &Val1, const ValVariant &Val2) const;
  template <typename T> Expect<void> runVectorCeilOp(ValVariant &Val) const;
  template <typename T> Expect<void> runVectorFloorOp(ValVariant &Val) const;
  template <typename T> Expect<void> runVectorTruncOp(ValVariant &Val) const;
  template <typename T> Expect<void> runVectorNearestOp(ValVariant &Val) const;
  /// ======= Relaxed SIMD instructions =======
  template <typename T>
  Expect<void> runVectorRelaxedLaneselectOp(ValVariant &Val1,
                                            const ValVariant &Val2,
                                            const ValVariant &Mask) const;
  inline Expect<void>
  runVectorRelaxedIntegerDotProductOp(ValVariant &Val1,
                                      const ValVariant &Val2) const;
  inline Expect<void> runVectorRelaxedIntegerDotProductOpAdd(
      ValVariant &Val1, const ValVariant &Val2, const ValVariant &C) const;
  /// ======= Atomic instructions =======
  Expect<void> runAtomicNotifyOp(Runtime::StackManager &StackMgr,
                                 Runtime::Instance::MemoryInstance &MemInst,
                                 const AST::Instruction &Instr);
  Expect<void> runMemoryFenceOp();
  template <typename T>
  TypeT<T> runAtomicWaitOp(Runtime::StackManager &StackMgr,
                           Runtime::Instance::MemoryInstance &MemInst,
                           const AST::Instruction &Instr);
  template <typename T, typename I>
  TypeT<T> runAtomicLoadOp(Runtime::StackManager &StackMgr,
                           Runtime::Instance::MemoryInstance &MemInst,
                           const AST::Instruction &Instr);
  template <typename T, typename I>
  TypeT<T> runAtomicStoreOp(Runtime::StackManager &StackMgr,
                            Runtime::Instance::MemoryInstance &MemInst,
                            const AST::Instruction &Instr);
  template <typename T, typename I>
  TypeT<T> runAtomicAddOp(Runtime::StackManager &StackMgr,
                          Runtime::Instance::MemoryInstance &MemInst,
                          const AST::Instruction &Instr);
  template <typename T, typename I>
  TypeT<T> runAtomicSubOp(Runtime::StackManager &StackMgr,
                          Runtime::Instance::MemoryInstance &MemInst,
                          const AST::Instruction &Instr);
  template <typename T, typename I>
  TypeT<T> runAtomicOrOp(Runtime::StackManager &StackMgr,
                         Runtime::Instance::MemoryInstance &MemInst,
                         const AST::Instruction &Instr);
  template <typename T, typename I>
  TypeT<T> runAtomicAndOp(Runtime::StackManager &StackMgr,
                          Runtime::Instance::MemoryInstance &MemInst,
                          const AST::Instruction &Instr);
  template <typename T, typename I>
  TypeT<T> runAtomicXorOp(Runtime::StackManager &StackMgr,
                          Runtime::Instance::MemoryInstance &MemInst,
                          const AST::Instruction &Instr);
  template <typename T, typename I>
  TypeT<T> runAtomicExchangeOp(Runtime::StackManager &StackMgr,
                               Runtime::Instance::MemoryInstance &MemInst,
                               const AST::Instruction &Instr);
  template <typename T, typename I>
  TypeT<T>
  runAtomicCompareExchangeOp(Runtime::StackManager &StackMgr,
                             Runtime::Instance::MemoryInstance &MemInst,
                             const AST::Instruction &Instr);
  /// @}

public:
  /// \name AOT/JIT - Run compiled functions
  /// @{
  Expect<void> proxyTrap(Runtime::StackManager &StackMgr,
                         const uint32_t Code) noexcept;
  Expect<void> proxyCall(Runtime::StackManager &StackMgr,
                         const uint32_t FuncIdx, const ValVariant *Args,
                         ValVariant *Rets) noexcept;
  Expect<void> proxyCallIndirect(Runtime::StackManager &StackMgr,
                                 const uint32_t TableIdx,
                                 const uint32_t FuncTypeIdx,
                                 const uint32_t FuncIdx, const ValVariant *Args,
                                 ValVariant *Rets) noexcept;
  Expect<void> proxyCallRef(Runtime::StackManager &StackMgr,
                            const RefVariant Ref, const ValVariant *Args,
                            ValVariant *Rets) noexcept;
  Expect<RefVariant> proxyRefFunc(Runtime::StackManager &StackMgr,
                                  const uint32_t FuncIdx) noexcept;
  Expect<RefVariant> proxyStructNew(Runtime::StackManager &StackMgr,
                                    const uint32_t TypeIdx,
                                    const ValVariant *Args,
                                    const uint32_t ArgSize) noexcept;
  Expect<void> proxyStructGet(Runtime::StackManager &StackMgr,
                              const RefVariant Ref, const uint32_t TypeIdx,
                              const uint32_t Off, const bool IsSigned,
                              ValVariant *Ret) noexcept;
  Expect<void> proxyStructSet(Runtime::StackManager &StackMgr,
                              const RefVariant Ref, const uint32_t TypeIdx,
                              const uint32_t Off,
                              const ValVariant *Val) noexcept;
  Expect<RefVariant> proxyArrayNew(Runtime::StackManager &StackMgr,
                                   const uint32_t TypeIdx,
                                   const uint32_t Length,
                                   const ValVariant *Args,
                                   const uint32_t ArgSize) noexcept;
  Expect<RefVariant> proxyArrayNewData(Runtime::StackManager &StackMgr,
                                       const uint32_t TypeIdx,
                                       const uint32_t DataIdx,
                                       const uint32_t Start,
                                       const uint32_t Length) noexcept;
  Expect<RefVariant> proxyArrayNewElem(Runtime::StackManager &StackMgr,
                                       const uint32_t TypeIdx,
                                       const uint32_t ElemIdx,
                                       const uint32_t Start,
                                       const uint32_t Length) noexcept;
  Expect<void> proxyArrayGet(Runtime::StackManager &StackMgr,
                             const RefVariant Ref, const uint32_t TypeIdx,
                             const uint32_t Idx, const bool IsSigned,
                             ValVariant *Ret) noexcept;
  Expect<void> proxyArraySet(Runtime::StackManager &StackMgr,
                             const RefVariant Ref, const uint32_t TypeIdx,
                             const uint32_t Idx,
                             const ValVariant *Val) noexcept;
  Expect<uint32_t> proxyArrayLen(Runtime::StackManager &StackMgr,
                                 const RefVariant Ref) noexcept;
  Expect<void> proxyArrayFill(Runtime::StackManager &StackMgr,
                              const RefVariant Ref, const uint32_t TypeIdx,
                              const uint32_t Idx, const uint32_t Cnt,
                              const ValVariant *Val) noexcept;
  Expect<void> proxyArrayCopy(Runtime::StackManager &StackMgr,
                              const RefVariant DstRef,
                              const uint32_t DstTypeIdx, const uint32_t DstIdx,
                              const RefVariant SrcRef,
                              const uint32_t SrcTypeIdx, const uint32_t SrcIdx,
                              const uint32_t Cnt) noexcept;
  Expect<void> proxyArrayInitData(Runtime::StackManager &StackMgr,
                                  const RefVariant Ref, const uint32_t TypeIdx,
                                  const uint32_t DataIdx, const uint32_t DstIdx,
                                  const uint32_t SrcIdx,
                                  const uint32_t Cnt) noexcept;
  Expect<void> proxyArrayInitElem(Runtime::StackManager &StackMgr,
                                  const RefVariant Ref, const uint32_t TypeIdx,
                                  const uint32_t ElemIdx, const uint32_t DstIdx,
                                  const uint32_t SrcIdx,
                                  const uint32_t Cnt) noexcept;
  Expect<uint32_t> proxyRefTest(Runtime::StackManager &StackMgr,
                                const RefVariant Ref, ValType VTTest) noexcept;
  Expect<RefVariant> proxyRefCast(Runtime::StackManager &StackMgr,
                                  const RefVariant Ref,
                                  ValType VTCast) noexcept;
  Expect<RefVariant> proxyTableGet(Runtime::StackManager &StackMgr,
                                   const uint32_t TableIdx,
                                   const uint32_t Off) noexcept;
  Expect<void> proxyTableSet(Runtime::StackManager &StackMgr,
                             const uint32_t TableIdx, const uint32_t Off,
                             const RefVariant Ref) noexcept;
  Expect<void> proxyTableInit(Runtime::StackManager &StackMgr,
                              const uint32_t TableIdx, const uint32_t ElemIdx,
                              const uint32_t DstOff, const uint32_t SrcOff,
                              const uint32_t Len) noexcept;
  Expect<void> proxyElemDrop(Runtime::StackManager &StackMgr,
                             const uint32_t ElemIdx) noexcept;
  Expect<void> proxyTableCopy(Runtime::StackManager &StackMgr,
                              const uint32_t TableIdxDst,
                              const uint32_t TableIdxSrc, const uint32_t DstOff,
                              const uint32_t SrcOff,
                              const uint32_t Len) noexcept;
  Expect<uint32_t> proxyTableGrow(Runtime::StackManager &StackMgr,
                                  const uint32_t TableIdx, const RefVariant Val,
                                  const uint32_t NewSize) noexcept;
  Expect<uint32_t> proxyTableSize(Runtime::StackManager &StackMgr,
                                  const uint32_t TableIdx) noexcept;
  Expect<void> proxyTableFill(Runtime::StackManager &StackMgr,
                              const uint32_t TableIdx, const uint32_t Off,
                              const RefVariant Ref,
                              const uint32_t Len) noexcept;
  Expect<uint32_t> proxyMemGrow(Runtime::StackManager &StackMgr,
                                const uint32_t MemIdx,
                                const uint32_t NewSize) noexcept;
  Expect<uint32_t> proxyMemSize(Runtime::StackManager &StackMgr,
                                const uint32_t MemIdx) noexcept;
  Expect<void> proxyMemInit(Runtime::StackManager &StackMgr,
                            const uint32_t MemIdx, const uint32_t DataIdx,
                            const uint32_t DstOff, const uint32_t SrcOff,
                            const uint32_t Len) noexcept;
  Expect<void> proxyDataDrop(Runtime::StackManager &StackMgr,
                             const uint32_t DataIdx) noexcept;
  Expect<void> proxyMemCopy(Runtime::StackManager &StackMgr,
                            const uint32_t DstMemIdx, const uint32_t SrcMemIdx,
                            const uint32_t DstOff, const uint32_t SrcOff,
                            const uint32_t Len) noexcept;
  Expect<void> proxyMemFill(Runtime::StackManager &StackMgr,
                            const uint32_t MemIdx, const uint32_t Off,
                            const uint8_t Val, const uint32_t Len) noexcept;
  Expect<uint32_t> proxyMemAtomicNotify(Runtime::StackManager &StackMgr,
                                        const uint32_t MemIdx,
                                        const uint32_t Offset,
                                        const uint32_t Count) noexcept;
  Expect<uint32_t>
  proxyMemAtomicWait(Runtime::StackManager &StackMgr, const uint32_t MemIdx,
                     const uint32_t Offset, const uint64_t Expected,
                     const int64_t Timeout, const uint32_t BitWidth) noexcept;
  Expect<void *> proxyTableGetFuncSymbol(Runtime::StackManager &StackMgr,
                                         const uint32_t TableIdx,
                                         const uint32_t FuncTypeIdx,
                                         const uint32_t FuncIdx) noexcept;
  Expect<void *> proxyRefGetFuncSymbol(Runtime::StackManager &StackMgr,
                                       const RefVariant Ref) noexcept;
  /// @}

  /// Callbacks for compiled modules
  static const Executable::IntrinsicsTable Intrinsics;
  /// Proxy helper template struct
  template <typename FuncPtr> struct ProxyHelper;

private:
  /// Execution context for compiled functions.
  struct ExecutionContextStruct {
#if WASMEDGE_ALLOCATOR_IS_STABLE
    uint8_t *const *Memories;
#else
    uint8_t **const *Memories;
#endif
    ValVariant *const *Globals;
    std::atomic_uint64_t *InstrCount;
    uint64_t *CostTable;
    std::atomic_uint64_t *Gas;
    uint64_t GasLimit;
    std::atomic_uint32_t *StopToken;
  };

  /// Restores thread local VM reference after overwriting it.
  struct SavedThreadLocal {
    SavedThreadLocal(Executor &Ex, Runtime::StackManager &StackMgr,
                     const Runtime::Instance::FunctionInstance &Func) noexcept;

    SavedThreadLocal(const SavedThreadLocal &) = delete;
    SavedThreadLocal(SavedThreadLocal &&) = delete;

    ~SavedThreadLocal() noexcept;

    Executor *SavedThis;
    Runtime::StackManager *SavedCurrentStack;
    ExecutionContextStruct SavedExecutionContext;
  };

  /// Pointer to current object.
  static thread_local Executor *This;
  /// Stack for passing into compiled functions
  static thread_local Runtime::StackManager *CurrentStack;
  /// Execution context for compiled functions
  static thread_local ExecutionContextStruct ExecutionContext;
  /// Record stack track on error
  static thread_local std::array<uint32_t, 256> StackTrace;
  static thread_local size_t StackTraceSize;

  /// Waiter struct for atomic instructions
  struct Waiter {
    std::mutex Mutex;
    std::condition_variable Cond;
    Runtime::Instance::MemoryInstance *MemInst;
    Waiter(Runtime::Instance::MemoryInstance *Inst) noexcept : MemInst(Inst) {}
  };
  /// Waiter map mutex
  std::mutex WaiterMapMutex;
  /// Waiter multimap
  std::unordered_multimap<uint32_t, Waiter> WaiterMap;

  /// WasmEdge configuration
  const Configure Conf;
  /// Executor statistics
  Statistics::Statistics *Stat;
  /// Stop Execution
  std::atomic_uint32_t StopToken = 0;
  /// Executor Host Function Handler
  HostFuncHandler HostFuncHelper = {};
};

} // namespace Executor
} // namespace WasmEdge

#include "engine/atomic.ipp"
#include "engine/binary_numeric.ipp"
#include "engine/cast_numeric.ipp"
#include "engine/memory.ipp"
#include "engine/relation_numeric.ipp"
#include "engine/unary_numeric.ipp"
