#include "oneflow/core/kernel/kernel.h"
#include "oneflow/core/kernel/kernel_context.h"
#include "oneflow/core/kernel/indexed_slices_reduce_sum_kernel_util.h"

namespace oneflow {

template<DeviceType device_type, typename T, typename K>
class IndexedSlicesLazyAdamOptimizerKernel final : public KernelIf<device_type> {
 public:
  OF_DISALLOW_COPY_AND_MOVE(IndexedSlicesLazyAdamOptimizerKernel);
  IndexedSlicesLazyAdamOptimizerKernel() = default;
  ~IndexedSlicesLazyAdamOptimizerKernel() override = default;

 private:
  using ReduceSumUtilT = IndexedSlicesReduceSumKernelUtil<device_type, K, T>;
  void ForwardDataContent(const KernelCtx&,
                          std::function<Blob*(const std::string&)>) const override;
};

template<DeviceType device_type, typename T, typename K>
void IndexedSlicesLazyAdamOptimizerKernel<device_type, T, K>::ForwardDataContent(
    const KernelCtx& ctx, std::function<Blob*(const std::string&)> BnInOp2Blob) const {
  const Blob* diff_indices = BnInOp2Blob("model_diff_indices");
  const Blob* diff_values = BnInOp2Blob("model_diff_values");
  const int64_t num_indices = diff_indices->shape().elem_cnt();
  const int64_t num_values = diff_values->shape().elem_cnt();
  CHECK_EQ(num_values % num_indices, 0);
  const int64_t m = num_values / num_indices;
  Blob* unique_diff_indices = BnInOp2Blob("unique_diff_indices");
  Blob* unique_diff_values = BnInOp2Blob("unique_diff_values");
  Blob* num_unique_diff_indices = BnInOp2Blob("num_unique_diff_indices");
  Blob* unique_workspace = BnInOp2Blob("unique_workspace");
  ReduceSumUtilT::ReduceSum(ctx.device_ctx, num_indices, m, diff_indices->dptr<K>(),
                            diff_values->dptr<T>(), num_unique_diff_indices->mut_dptr<int64_t>(),
                            unique_diff_indices->mut_dptr<K>(), unique_diff_values->mut_dptr<T>(),
                            unique_workspace->mut_dptr(),
                            unique_workspace->ByteSizeOfDataContentField());
}

namespace {

using namespace kernel_registration;
using namespace constraint;

class IndexedSlicesLazyAdamOptimizerKernelConstraint final : public KernelConstraint {
 public:
  IndexedSlicesLazyAdamOptimizerKernelConstraint(DeviceType dev, DataType data_type,
                                                 DataType indices_data_type)
      : device_and_dtype_constraint_(dev, data_type), indices_data_type_(indices_data_type) {}
  ~IndexedSlicesLazyAdamOptimizerKernelConstraint() override = default;

  bool IsMatched(const KernelConf& kernel_conf) override {
    return kernel_conf.indexed_slices_lazy_adam_optimizer_conf().indices_data_type()
               == indices_data_type_
           && device_and_dtype_constraint_.IsMatched(kernel_conf);
  }

 private:
  DeviceAndDTypeConstraint device_and_dtype_constraint_;
  DataType indices_data_type_;
};

#define REGISTER_INDEXED_SLICES_LAZY_ADAM_OPTIMIZER_KERNEL(op_type, device, data_type,          \
                                                           indices_data_type, ...)              \
  static KernelRegistrar OF_PP_CAT(registrar, __LINE__)(                                        \
      OperatorConf::kIndexedSlicesLazyAdamOptimizerConf,                                        \
      new IndexedSlicesLazyAdamOptimizerKernelConstraint(device, data_type, indices_data_type), \
      []() { return new __VA_ARGS__(); });

REGISTER_INDEXED_SLICES_LAZY_ADAM_OPTIMIZER_KERNEL(
    OperatorConf::kIndexedSlicesLazyAdamOptimizerConf, DeviceType::kGPU, kFloat, kInt32,
    IndexedSlicesLazyAdamOptimizerKernel<DeviceType::kGPU, float, int32_t>);

#undef REGISTER_INDEXED_SLICES_LAZY_ADAM_OPTIMIZER_KERNEL

}  // namespace

}  // namespace oneflow