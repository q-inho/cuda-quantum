/*************************************************************** -*- C++ -*- ***
 * Copyright (c) 2022 - 2023 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 *******************************************************************************/

#pragma once

#include "cudaq/builder/kernel_builder.h"
#include <fmt/core.h>
#include <functional>
#include <pybind11/pybind11.h>
#include <vector>

namespace py = pybind11;

namespace cudaq {

/// @brief The OpaqueArguments type wraps a vector
/// of function arguments represented as opaque
/// pointers. For each element in the vector of opaque
/// pointers, we also track the arguments corresponding
/// deletion function - a function invoked upon destruction
/// of this OpaqueArguments to clean up the memory.
class OpaqueArguments {
public:
  using OpaqueArgDeleter = std::function<void(void *)>;

private:
  /// @brief The opaque argument pointers
  std::vector<void *> args;

  /// @brief Deletion functions for the arguments.
  std::vector<OpaqueArgDeleter> deleters;

public:
  /// @brief Add an opaque argument and its deleter to this OpaqueArguments
  template <typename ArgPointer, typename Deleter>
  void emplace_back(ArgPointer &&pointer, Deleter &&deleter) {
    args.emplace_back(pointer);
    deleters.emplace_back(deleter);
  }

  /// @brief Return the args as a pointer to void*.
  void **data() { return args.data(); }

  /// @brief Return the number of arguments
  std::size_t size() { return args.size(); }

  /// Destructor, clean up the memory
  ~OpaqueArguments() {
    for (std::size_t counter = 0; auto &ptr : args)
      deleters[counter++](ptr);

    args.clear();
    deleters.clear();
  }
};

/// @brief Validate that the number of arguments provided is
/// correct for the given kernel_builder. Throw an exception if not.
inline py::args validateInputArguments(kernel_builder<> &kernel,
                                       py::args &args) {
  // Ensure the user input is correct.
  auto nInputArgs = args.size();
  auto nRequiredParams = kernel.getNumParams();
  if (nRequiredParams != nInputArgs)
    throw std::runtime_error(fmt::format(
        "Kernel requires {} input parameter{} but {} provided.",
        nRequiredParams, nRequiredParams, nRequiredParams > 1 ? "s" : "",
        nInputArgs == 0 ? "none" : std::to_string(nInputArgs)));

  // Look at the input arguments, validate they are ok
  // Specifically here we'll check if we've been given
  // other list-like types as input for a stdvec (like numpy array)
  py::args processed = py::tuple(args.size());
  for (std::size_t i = 0; i < args.size(); ++i) {
    auto arg = args[i];
    if (kernel.isArgStdVec(i)) {

      auto nRequiredElements = kernel.getArguments()[i].getRequiredElements();

      // Check if it has tolist, so it might be a 1d buffer (array / numpy
      // ndarray)
      if (py::hasattr(args[i], "tolist")) {
        // This is a valid ndarray if it has tolist and shape
        if (!py::hasattr(args[i], "shape"))
          throw std::runtime_error(
              "Invalid input argument type, could not get shape of array.");

        // This is an ndarray with tolist() and shape attributes
        // get the shape and check its size
        auto shape = args[i].attr("shape").cast<py::tuple>();
        if (shape.size() != 1)
          throw std::runtime_error("Cannot pass ndarray with shape != (N,).");

        arg = args[i].attr("tolist")();
      }

      // has to be a list if its not a nd array
      if (!py::isinstance<py::list>(arg))
        throw std::runtime_error(
            "Invalid list-like argument to Kernel.__call__()");

      auto nElements = arg.cast<py::list>().size();
      if (nRequiredElements != nElements)
        throw std::runtime_error("Kernel list argument requires " +
                                 std::to_string(nRequiredElements) + " but " +
                                 std::to_string(nElements) + " were provided.");
    }

    processed[i] = arg;
  }

  // TODO: Handle more type checking

  return processed;
}

/// @brief Convert py::args to an OpaqueArguments instance
inline void packArgs(OpaqueArguments &argData, py::args args) {
  for (auto &arg : args) {
    if (py::isinstance<py::float_>(arg)) {
      double *ourAllocatedArg = new double();
      *ourAllocatedArg = PyFloat_AsDouble(arg.ptr());
      argData.emplace_back(ourAllocatedArg, [](void *ptr) {
        delete static_cast<double *>(ptr);
      });
    }
    if (py::isinstance<py::int_>(arg)) {
      int *ourAllocatedArg = new int();
      *ourAllocatedArg = PyLong_AsLong(arg.ptr());
      argData.emplace_back(ourAllocatedArg,
                           [](void *ptr) { delete static_cast<int *>(ptr); });
    }
    if (py::isinstance<py::list>(arg)) {
      auto casted = py::cast<py::list>(arg);
      std::vector<double> *ourAllocatedArg =
          new std::vector<double>(casted.size());
      for (std::size_t counter = 0; auto el : casted) {
        (*ourAllocatedArg)[counter++] = PyFloat_AsDouble(el.ptr());
      }
      argData.emplace_back(ourAllocatedArg, [](void *ptr) {
        delete static_cast<std::vector<double> *>(ptr);
      });
    }
  }
}

} // namespace cudaq
