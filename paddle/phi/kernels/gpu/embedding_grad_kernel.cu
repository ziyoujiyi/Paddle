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

#include "paddle/phi/kernels/embedding_grad_kernel.h"
#include "paddle/phi/kernels/funcs/embedding_util.h"

#include "paddle/fluid/framework/convert_utils.h"
#include "paddle/fluid/framework/data_type.h"
#include "paddle/phi/backends/gpu/gpu_context.h"
#include "paddle/phi/core/kernel_registry.h"
#include "paddle/phi/kernels/funcs/eigen/common.h"

#include "paddle/fluid/framework/mixed_vector.h"
#include "paddle/fluid/platform/device/gpu/gpu_primitives.h"

namespace phi {

template <typename InT, typename OutT>
__global__ void InputTypeConvert(const InT* in_ids,
                                 const int64_t K,
                                 OutT* out_ids) {
  for (int i = 0; i < K; i++) {
    out_ids[i] = static_cast<OutT>(in_ids[i]);
  }
}

template <typename T, typename IdT, int BlockDimX, int BlockDimY, int GridDimX>
__global__ void LookupTableV2Grad(T* table,
                                  const T* output,
                                  const IdT* ids,
                                  const int64_t N,
                                  const int64_t K,
                                  const int64_t D) {
  int idx = threadIdx.x;
  int idy = blockIdx.x + threadIdx.y * GridDimX;

  while (idy < K) {
    auto id = static_cast<int64_t>(ids[idy]);
    const T* out = output + idy * D;
    T* tab = table + id * D;
    for (int i = idx; i < D; i += BlockDimX) {
      paddle::platform::CudaAtomicAdd(&tab[i], out[i]);
    }
    idy += BlockDimY * GridDimX;
  }
}

template <typename T, typename Context>
struct LookupTableV2GradCUDAFunctor {
  LookupTableV2GradCUDAFunctor(const Context& dev_ctx,
                               const DenseTensor& input,
                               const DenseTensor& weight,
                               const DenseTensor& out_grad,
                               int64_t padding_idx,
                               DenseTensor* weight_grad)
      : dev_ctx_(dev_ctx),
        input_(input),
        weight_(weight),
        out_grad_(out_grad),
        padding_idx_(padding_idx),
        weight_grad_(weight_grad) {}

  template <typename IdT>
  void apply() {
    // Since paddings are not trainable and fixed in forward, the gradient of
    // paddings makes no sense and we don't deal with it in backward.
    {
      auto d_output_t = out_grad_;
      auto d_table_t = weight_grad_;

      int N = weight_grad_->dims()[0];
      int D = weight_grad_->dims()[1];
      int K = input_.numel();

      dim3 threads(128, 8);
      dim3 grids(8, 1);
      const T* d_output = d_output_t.template data<T>();
      const auto* ids = input_.template data<IdT>();
      T* d_table = d_table_t->mutable_data<T>(dev_ctx_.GetPlace());

      auto t = EigenVector<T>::Flatten(*d_table_t);
      t.device(*dev_ctx_.eigen_device()) = t.constant(static_cast<T>(0));

      LookupTableV2Grad<T,
                        IdT,
                        128,
                        8,
                        8><<<grids, threads, 0, dev_ctx_.stream()>>>(
          d_table, d_output, ids, N, K, D);
    }
  }

 private:
  const phi::GPUContext& dev_ctx_;
  const DenseTensor& input_;
  const DenseTensor& weight_;
  const DenseTensor& out_grad_;
  int64_t padding_idx_;
  DenseTensor* weight_grad_;
};

template <typename T, typename Context>
void EmbeddingGradKernel(const Context& ctx,
                         const DenseTensor& input,
                         const DenseTensor& weight,
                         const DenseTensor& out_grad,
                         int64_t padding_idx,
                         DenseTensor* weight_grad) {
  LookupTableV2GradCUDAFunctor<T, Context> functor(
      ctx, input, weight, out_grad, padding_idx, weight_grad);
  paddle::framework::VisitIntDataType(
      paddle::framework::TransToProtoVarType(input.dtype()), functor);
}

template <typename T, typename Context>
struct LookupTableV2SparseGradCUDAFunctor {
  LookupTableV2SparseGradCUDAFunctor(const Context& dev_ctx,
                                     const DenseTensor& input,
                                     const DenseTensor& weight,
                                     const DenseTensor& out_grad,
                                     int64_t padding_idx,
                                     SelectedRows* weight_grad)
      : dev_ctx_(dev_ctx),
        input_(input),
        weight_(weight),
        out_grad_(out_grad),
        padding_idx_(padding_idx),
        weight_grad_(weight_grad) {}

  template <typename IdT>
  void apply() {
    // Since paddings are not trainable and fixed in forward, the gradient of
    // paddings makes no sense and we don't deal with it in backward.

    const auto* ids_data = input_.template data<IdT>();
    auto* d_table = weight_grad_;
    auto* table = &weight_;
    auto* d_output = &out_grad_;
    int64_t ids_num = input_.numel();
    dim3 threads(128, 8);
    dim3 grids(8, 1);
    auto stream = dev_ctx_.stream();
    paddle::framework::Vector<int64_t> new_rows;
    new_rows.resize(ids_num);
    auto gpu_place = dev_ctx_.GetPlace();

    paddle::framework::MixVector<int64_t> mixv_new_rows(&new_rows);
    if (!std::is_same<IdT, int64_t>::value) {
      InputTypeConvert<<<grids, threads, 0, stream>>>(
          ids_data, ids_num, mixv_new_rows.MutableData(gpu_place));
    } else {
      paddle::memory::Copy(gpu_place,
                           mixv_new_rows.CUDAMutableData(gpu_place),
                           gpu_place,
                           ids_data,
                           ids_num * sizeof(int64_t),
                           stream);
    }

    mixv_new_rows.CopyToCPU();
    d_table->set_rows(new_rows);

    auto* d_table_value = d_table->mutable_value();
    d_table_value->Resize({ids_num, table->dims()[1]});
    d_table_value->template mutable_data<T>(gpu_place);

    auto* d_table_data = d_table_value->template data<T>();
    auto* d_output_data = d_output->template data<T>();
    auto d_output_dims = d_output->dims();
    auto d_output_dims_2d =
        phi::flatten_to_2d(d_output_dims, d_output_dims.size() - 1);
    PADDLE_ENFORCE_EQ(d_table_value->dims(),
                      d_output_dims_2d,
                      phi::errors::InvalidArgument(
                          "ShapeError: The shape of lookup_table@Grad and "
                          "output@Grad should be same. "
                          "But received lookup_table@Grad's shape = [%s], "
                          "output@Grad's shape = [%s].",
                          d_table_value->dims(),
                          d_output_dims_2d));
    paddle::memory::Copy(gpu_place,
                         d_table_data,
                         gpu_place,
                         d_output_data,
                         d_output->numel() * sizeof(T),
                         stream);
  }

 private:
  const phi::GPUContext& dev_ctx_;
  const DenseTensor& input_;
  const DenseTensor& weight_;
  const DenseTensor& out_grad_;
  int64_t padding_idx_;
  SelectedRows* weight_grad_;
};

template <typename T, typename Context>
void EmbeddingSparseGradKernel(const Context& ctx,
                               const DenseTensor& input,
                               const DenseTensor& weight,
                               const DenseTensor& out_grad,
                               int64_t padding_idx,
                               SelectedRows* weight_grad) {
  LookupTableV2SparseGradCUDAFunctor<T, Context> functor(
      ctx, input, weight, out_grad, padding_idx, weight_grad);
  paddle::framework::VisitIntDataType(
      paddle::framework::TransToProtoVarType(input.dtype()), functor);
}

}  // namespace phi

PD_REGISTER_KERNEL(embedding_grad,
                   GPU,
                   ALL_LAYOUT,
                   phi::EmbeddingGradKernel,
                   float,
                   double,
                   phi::dtype::float16) {}

PD_REGISTER_KERNEL(embedding_sparse_grad,
                   GPU,
                   ALL_LAYOUT,
                   phi::EmbeddingSparseGradKernel,
                   float,
                   double,
                   phi::dtype::float16) {}
