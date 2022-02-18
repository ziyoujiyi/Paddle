// Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "paddle/infrt/kernel/pten/infershaped/infershaped_kernel_launchers.h"
#include "paddle/infrt/kernel/pten/infershaped/elementwise_add.h"

namespace infrt {
namespace kernel {

void RegisterInferShapeLaunchers(host_context::KernelRegistry* registry) {
  registry->AddKernel(
      "elementwise_add",
      std::bind(&KernelLauncherFunc<decltype(&ElementwiseAdd),
                                    &ElementwiseAdd,
                                    decltype(&ElementwiseAddInferShape),
                                    &ElementwiseAddInferShape>,
                KernelLauncher<decltype(&ElementwiseAdd),
                               &ElementwiseAdd,
                               decltype(&ElementwiseAddInferShape),
                               &ElementwiseAddInferShape>(),
                std::placeholders::_1));
}

}  // namespace kernel
}  // namespace infrt