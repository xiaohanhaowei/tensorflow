/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "tensorflow/core/common_runtime/function.h"
#include "tensorflow/core/framework/dataset.h"
#include "tensorflow/core/framework/partial_tensor_shape.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/kernels/data/captured_function.h"
#include "tensorflow/core/kernels/data/dataset_utils.h"
#include "tensorflow/core/lib/random/random.h"
#include "tensorflow/core/util/ptr_util.h"

namespace tensorflow {
namespace data {
namespace {

// See documentation in ../../ops/dataset_ops.cc for a high-level
// description of the following op.

class MapDatasetOp : public UnaryDatasetOpKernel {
 public:
  using MapIteratorFunction = std::function<Status(
      IteratorContext*, std::vector<Tensor>, std::vector<Tensor>*)>;

  explicit MapDatasetOp(OpKernelConstruction* ctx) : UnaryDatasetOpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("f", &func_));
    OP_REQUIRES_OK(ctx, ctx->GetAttr("output_types", &output_types_));
    OP_REQUIRES_OK(ctx, ctx->GetAttr("output_shapes", &output_shapes_));
    OP_REQUIRES_OK(ctx, ctx->GetAttr("use_inter_op_parallelism",
                                     &use_inter_op_parallelism_));
  }

  void MakeDataset(OpKernelContext* ctx, DatasetBase* input,
                   DatasetBase** output) override {
    std::unique_ptr<CapturedFunction> captured_func;
    OP_REQUIRES_OK(ctx, CapturedFunction::Create(func_, ctx, "other_arguments",
                                                 use_inter_op_parallelism_,
                                                 &captured_func));

    std::vector<int> indices;
    OP_REQUIRES_OK(ctx, ComputeShortCircuitIndices(ctx, func_, &indices));

    MapIteratorFunction map_func;
    CapturedFunction* raw_captured_func = captured_func.get();
    if (indices.empty()) {
      map_func = [raw_captured_func](IteratorContext* ctx,
                                     std::vector<Tensor> args,
                                     std::vector<Tensor>* out_tensors) {
        return raw_captured_func->Run(ctx, std::move(args), out_tensors);
      };
    } else {
      std::vector<bool> can_move = ComputeMoveVector(indices);
      map_func = [raw_captured_func, indices, can_move](
                     IteratorContext* ctx, std::vector<Tensor> args,
                     std::vector<Tensor>* out_tensors) {
        const std::vector<Tensor>& captured_inputs =
            raw_captured_func->captured_inputs();
        size_t num_args = args.size();
        for (size_t i = 0; i < indices.size(); ++i) {
          if (indices[i] < num_args) {
            if (can_move[i]) {
              out_tensors->push_back(std::move(args[indices[i]]));
            } else {
              out_tensors->push_back(args[indices[i]]);
            }
          } else {
            out_tensors->push_back(captured_inputs[indices[i] - num_args]);
          }
        }
        return Status::OK();
      };
    }

    *output = new Dataset(ctx, input, func_, std::move(captured_func),
                          output_types_, output_shapes_,
                          use_inter_op_parallelism_, std::move(map_func));
  }

 private:
  class Dataset : public DatasetBase {
   public:
    Dataset(OpKernelContext* ctx, const DatasetBase* input,
            const NameAttrList& func,
            std::unique_ptr<CapturedFunction> captured_func,
            const DataTypeVector& output_types,
            const std::vector<PartialTensorShape>& output_shapes,
            bool use_inter_op_parallelism, MapIteratorFunction map_func)
        : DatasetBase(DatasetContext(ctx)),
          input_(input),
          func_(func),
          use_inter_op_parallelism_(use_inter_op_parallelism),
          captured_func_(std::move(captured_func)),
          output_types_(output_types),
          output_shapes_(output_shapes),
          map_func_(std::move(map_func)) {
      input_->Ref();
    }

    ~Dataset() override { input_->Unref(); }

    std::unique_ptr<IteratorBase> MakeIteratorInternal(
        const string& prefix) const override {
      return MakeUnique<Iterator>(
          Iterator::Params{this, strings::StrCat(prefix, "::Map")}, map_func_);
    }

    const DataTypeVector& output_dtypes() const override {
      return output_types_;
    }
    const std::vector<PartialTensorShape>& output_shapes() const override {
      return output_shapes_;
    }

    string DebugString() const override { return "MapDatasetOp::Dataset"; }

   protected:
    Status AsGraphDefInternal(SerializationContext* ctx,
                              DatasetGraphDefBuilder* b,
                              Node** output) const override {
      TF_RETURN_IF_ERROR(b->AddFunction(ctx, func_.name()));
      Node* input_graph_node = nullptr;
      TF_RETURN_IF_ERROR(b->AddInputDataset(ctx, input_, &input_graph_node));

      DataTypeVector other_arguments_types;
      other_arguments_types.reserve(captured_func_->captured_inputs().size());
      std::vector<Node*> other_arguments;
      other_arguments.reserve(captured_func_->captured_inputs().size());
      for (const Tensor& t : captured_func_->captured_inputs()) {
        Node* node;
        TF_RETURN_IF_ERROR(b->AddTensor(t, &node));
        other_arguments.emplace_back(node);
        other_arguments_types.emplace_back(t.dtype());
      }

      // Attr: f
      TF_RETURN_IF_ERROR(b->AddFunction(ctx, func_.name()));
      AttrValue f_attr;
      b->BuildAttrValue(func_, &f_attr);

      // Attr: Targuments
      AttrValue other_arguments_types_attr;
      b->BuildAttrValue(other_arguments_types, &other_arguments_types_attr);

      // Attr: use_inter_op_parallelism
      AttrValue use_inter_op_parallelism_attr;
      b->BuildAttrValue(use_inter_op_parallelism_,
                        &use_inter_op_parallelism_attr);

      TF_RETURN_IF_ERROR(b->AddDataset(
          this, {std::make_pair(0, input_graph_node)},  // Single tensor inputs.
          {std::make_pair(1, other_arguments)},         // Tensor list inputs.
          {std::make_pair("f", f_attr),
           std::make_pair("Targuments", other_arguments_types_attr),
           std::make_pair("use_inter_op_parallelism",
                          use_inter_op_parallelism_attr)},  // Attrs
          output));
      return Status::OK();
    }

   private:
    class Iterator : public DatasetIterator<Dataset> {
     public:
      explicit Iterator(const Params& params, MapIteratorFunction map_func)
          : DatasetIterator<Dataset>(params), map_func_(std::move(map_func)) {}

      Status Initialize(IteratorContext* ctx) override {
        TF_RETURN_IF_ERROR(
            dataset()->input_->MakeIterator(ctx, prefix(), &input_impl_));
        return dataset()->captured_func_->Instantiate(ctx);
      }

      Status GetNextInternal(IteratorContext* ctx,
                             std::vector<Tensor>* out_tensors,
                             bool* end_of_sequence) override {
        // NOTE(mrry): This method is thread-safe as long as
        // `input_impl_` and `f` are thread-safe. However, if multiple
        // threads enter this method, outputs may be observed in a
        // non-deterministic order.

        std::vector<Tensor> args;
        TF_RETURN_IF_ERROR(input_impl_->GetNext(ctx, &args, end_of_sequence));
        if (*end_of_sequence) {
          return Status::OK();
        }

        Status s = map_func_(ctx, args, out_tensors);
        if (errors::IsOutOfRange(s)) {
          // `f` may deliberately raise `errors::OutOfRange` to indicate
          // that we should terminate the iteration early.
          *end_of_sequence = true;
          return Status::OK();
        } else {
          return s;
        }
      }

     protected:
      Status SaveInternal(IteratorStateWriter* writer) override {
        TF_RETURN_IF_ERROR(SaveInput(writer, input_impl_));
        return Status::OK();
      }

      Status RestoreInternal(IteratorContext* ctx,
                             IteratorStateReader* reader) override {
        TF_RETURN_IF_ERROR(RestoreInput(ctx, reader, input_impl_));
        return Status::OK();
      }

     private:
      std::unique_ptr<IteratorBase> input_impl_;
      const MapIteratorFunction map_func_;
    };

    const DatasetBase* const input_;
    const NameAttrList func_;
    const bool use_inter_op_parallelism_;
    const std::unique_ptr<CapturedFunction> captured_func_;
    const DataTypeVector output_types_;
    const std::vector<PartialTensorShape> output_shapes_;
    const MapIteratorFunction map_func_;
  };

  DataTypeVector output_types_;
  std::vector<PartialTensorShape> output_shapes_;
  NameAttrList func_;
  bool use_inter_op_parallelism_;
};

REGISTER_KERNEL_BUILDER(Name("MapDataset").Device(DEVICE_CPU), MapDatasetOp);

}  // namespace
}  // namespace data
}  // namespace tensorflow
