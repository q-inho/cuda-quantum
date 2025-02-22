/*************************************************************** -*- C++ -*- ***
 * Copyright (c) 2022 - 2023 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 *******************************************************************************/

// RUN: cudaq-quake %s | FileCheck %s

#include <cudaq.h>

struct k {
  void operator()(cudaq::qspan<> q) __qpu__ {
    h(q[0]);
    ry(3.14, q[1]);
    t(q[2]);
  }
};

// CHECK-LABEL:   func.func @__nvqpp__mlirgen__k
// CHECK-SAME: (%[[VAL_0:.*]]: !quake.qvec<?>)
// CHECK:           quake.h (%{{.*}})
// CHECK:           quake.ry |%{{.*}} : f64|(%{{.*}})
// CHECK:           quake.t (%{{.*}})
// CHECK:           return

struct ep {
  void operator()() __qpu__ {
    cudaq::qreg<3> q;
    cudaq::adjoint(k{}, q);
  }
};

// CHECK-LABEL:   func.func @__nvqpp__mlirgen__ep()
// CHECK:           %[[VAL_2:.*]] = arith.constant 3 : i64
// CHECK:           %[[VAL_3:.*]] = quake.alloca(%[[VAL_2]] : i64) : !quake.qvec<3>
// CHECK:           %[[VAL_4:.*]] = quake.relax_size %[[VAL_3]] : (!quake.qvec<3>) -> !quake.qvec<?>
// CHECK:           quake.apply<adj> @__nvqpp__mlirgen__k %[[VAL_4]] : (!quake.qvec<?>) -> ()
// CHECK:           return

