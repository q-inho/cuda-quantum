/*************************************************************** -*- C++ -*- ***
 * Copyright (c) 2022 - 2023 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 *******************************************************************************/

#include <pybind11/stl.h>

#include "py_observe.h"
#include "utils/OpaqueArguments.h"

#include "cudaq/platform/quantum_platform.h"

#include "common/Logger.h"

namespace cudaq {

/// @brief Default shots value set to -1
constexpr int defaultShotsValue = -1;

/// @brief Default qpu id value set to 0
constexpr int defaultQpuIdValue = 0;

/// @brief Run `cudaq::observe` on the provided kernel and spin operator.
observe_result pyObserve(kernel_builder<> &kernel, spin_op &spin_operator,
                         py::args args = {}, int shots = defaultShotsValue) {
  // Ensure the user input is correct.
  auto validatedArgs = validateInputArguments(kernel, args);
  auto &platform = cudaq::get_platform();

  // TODO: would like to handle errors in the case that
  // `kernel.num_qubits() >= spin_operator.num_qubits()`
  kernel.jitCode();

  // Does this platform expose more than 1 QPU
  // If so, let's distribute the work amongst the QPUs
  if (auto nQpus = platform.num_qpus(); nQpus > 1)
    return details::distributeComputations(
        [&](std::size_t i, spin_op &op) {
          return pyObserveAsync(kernel, op, args, i, shots);
        },
        spin_operator, nQpus);

  // Launch the observation task
  return details::runObservation(
             [&]() mutable {
               OpaqueArguments argData;
               packArgs(argData, validatedArgs);
               kernel.jitAndInvoke(argData.data());
             },
             spin_operator, platform, shots)
      .value();
}

/// @brief Asynchronously run `cudaq::observe` on the provided kernel and
/// spin operator.
async_observe_result pyObserveAsync(kernel_builder<> &kernel,
                                    spin_op &spin_operator, py::args args = {},
                                    std::size_t qpu_id = defaultQpuIdValue,
                                    int shots = defaultShotsValue) {

  // Ensure the user input is correct.
  auto validatedArgs = validateInputArguments(kernel, args);

  // TODO: would like to handle errors in the case that
  // `kernel.num_qubits() >= spin_operator.num_qubits()`
  kernel.jitCode();

  // Get the platform, first check that the given qpu_id is valid
  auto &platform = cudaq::get_platform();

  // Launch the asynchronous execution.
  return details::runObservationAsync(
      [&, a = std::move(validatedArgs)]() mutable {
        OpaqueArguments argData;
        packArgs(argData, a);
        kernel.jitAndInvoke(argData.data());
      },
      spin_operator, platform, shots, qpu_id);
}

void bindObserve(py::module &mod) {

  // FIXME provide ability to inject noise model here
  mod.def(
      "observe",
      [&](kernel_builder<> &kernel, spin_op &spin_operator, py::args arguments,
          int shots) {
        return pyObserve(kernel, spin_operator, arguments, shots);
      },
      py::arg("kernel"), py::arg("spin_operator"), py::kw_only(),
      py::arg("shots_count") = defaultShotsValue,
      "Compute the expected value of the `spin_operator` with respect to "
      "the `kernel`. If the kernel accepts arguments, it will be evaluated "
      "with respect to `kernel(*arguments)`.\n"
      "\nArgs:\n"
      "  kernel (:class:`Kernel`): The :class:`Kernel` to evaluate the "
      "expectation "
      "value with respect to.\n"
      "  spin_operator (:class:`SpinOperator`): The Hermitian spin operator to "
      "calculate "
      "the expectation of.\n"
      "  *arguments (Optional[Any]): The concrete values to evaluate the "
      "kernel "
      "function at. Leave empty if the kernel doesn't accept any arguments.\n"
      "  shots_count (Optional[int]): The number of shots to use for QPU "
      "execution. "
      "Defaults to 1 shot. Key-word only.\n"
      "\nReturns:\n"
      "  :class:`ObserveResult` : A data-type containing the expectation value "
      "of the "
      "`spin_operator` with respect to the `kernel(*arguments)`. If "
      "`shots_count` was "
      "provided, the :class:`ObserveResult` will also contain a "
      ":class:`SampleResult` "
      "dictionary.\n");

  /// Expose observe_async, can optionally take the qpu_id to target.
  mod.def(
      "observe_async",
      [&](kernel_builder<> &kernel, spin_op &spin_operator, py::args arguments,
          std::size_t qpu_id, int shots) {
        return pyObserveAsync(kernel, spin_operator, arguments, qpu_id, shots);
      },
      py::arg("kernel"), py::arg("spin_operator"), py::kw_only(),
      py::arg("qpu_id") = defaultQpuIdValue,
      py::arg("shots_count") = defaultShotsValue,
      "Compute the expected value of the `spin_operator` with respect to "
      "the `kernel` asynchronously. If the kernel accepts arguments, it will "
      "be evaluated with respect to `kernel(*arguments)`.\n"
      "When targeting a quantum platform "
      "with more than one QPU, the optional `qpu_id` allows for control over "
      "which QPU to enable. Will return a future whose results can be "
      "retrieved via "
      "`future.get()`.\n"
      "\nArgs:\n"
      "  kernel (:class:`Kernel`): The :class:`Kernel` to evaluate the "
      "expectation "
      "value with respect to.\n"
      "  spin_operator (:class:`SpinOperator`): The Hermitian spin operator to "
      "calculate "
      "the expectation of.\n"
      "  *arguments (Optional[Any]): The concrete values to evaluate the "
      "kernel "
      "function at. Leave empty if the kernel doesn't accept any arguments.\n"
      "  qpu_id (Optional[int]): The optional identification for which QPU on "
      "the platform to target. Defaults to zero. Key-word only.\n"
      "  shots_count (Optional[int]): The number of shots to use for QPU "
      "execution. "
      "Defaults to 1 shot. Key-word only.\n"
      "\nReturns:\n"
      "  :class:`AsyncObserveResult` : A future containing the result of the "
      "call to observe.\n");
}

} // namespace cudaq
