// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2022 Second State INC

//===-- wasmedge/runtime/instance/array.h - Array Instance definition -----===//
//
// Part of the WasmEdge Project.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains the array instance definition in store manager.
///
//===----------------------------------------------------------------------===//
#pragma once

#include "ast/type.h"
#include "common/span.h"
#include "common/types.h"
#include "runtime/instance/composite.h"

#include <vector>

namespace WasmEdge {
namespace Runtime {
namespace Instance {

class ArrayInstance : public CompositeBase {
public:
  ArrayInstance() = delete;
  ArrayInstance(const ModuleInstance *Mod, const uint32_t Idx,
                const uint32_t Size) noexcept
      : CompositeBase(Mod, Idx), RefCount(1), Data(Size) {
    assuming(ModInst);
  }
  ArrayInstance(const ModuleInstance *Mod, const uint32_t Idx,
                const uint32_t Size, const ValVariant &Init) noexcept
      : CompositeBase(Mod, Idx), RefCount(1), Data(Size, Init) {
    assuming(ModInst);
  }
  ArrayInstance(const ModuleInstance *Mod, const uint32_t Idx,
                std::vector<ValVariant> &&Init) noexcept
      : CompositeBase(Mod, Idx), RefCount(1), Data(std::move(Init)) {
    assuming(ModInst);
  }

  /// Get field data in array instance.
  ValVariant &getData(uint32_t Idx) noexcept { return Data[Idx]; }
  const ValVariant &getData(uint32_t Idx) const noexcept { return Data[Idx]; }

  /// Get array length.
  uint32_t getLength() const noexcept { return Data.size(); }

  /// Get reference count.
  uint32_t getRefCount() const noexcept { return RefCount; }

private:
  /// \name Data of array instance.
  /// @{
  uint32_t RefCount;
  std::vector<ValVariant> Data;
  /// @}
};

} // namespace Instance
} // namespace Runtime
} // namespace WasmEdge
