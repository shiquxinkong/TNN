// Tencent is pleased to support the open source community by making TNN available.
//
// Copyright (C) 2020 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include "tnn/device/opencl/acc/opencl_reduce_layer_acc.h"
#include "tnn/device/opencl/imagebuffer_convertor.h"

namespace TNN_NS {

#define LowOpParallelismThre 256
#define HighOpIntensityThre 128

DimsVector PadDims(DimsVector dims, std::vector<int> axis) {
    std::sort(axis.begin(), axis.end());
    for (const auto item : axis) {
        dims.insert(dims.begin() + item, 1);
    }

    return dims;
}

Status OpenCLReduceLayerAcc::Init(Context *context, LayerParam *param, LayerResource *resource,
                                  const std::vector<Blob *> &inputs, const std::vector<Blob *> &outputs) {
    LOGD("Init Reduce Acc\n");
    Status ret = OpenCLLayerAcc::Init(context, param, resource, inputs, outputs);
    CHECK_TNN_OK(ret)

    auto reduce_param = dynamic_cast<ReduceLayerParam *>(param);
    if (!reduce_param) {
        LOGE("Error: layer param is null\n");
        return Status(TNNERR_MODEL_ERR, "Error: layer param is null");
    }

    auto output_dims = outputs[0]->GetBlobDesc().dims;

    int hb   = DimsFunctionUtils::GetDim(output_dims, 0) * DimsFunctionUtils::GetDim(output_dims, 2);
    int cw   = DimsFunctionUtils::GetDim(output_dims, 3) * UP_DIV(DimsFunctionUtils::GetDim(output_dims, 1), 4);

    auto input_dims  = inputs[0]->GetBlobDesc().dims;

    if (reduce_param->axis.size() == 1) {
        int axis = reduce_param->axis[0];
        axis     = axis >= 0 ? axis : axis + (int)input_dims.size();

        int axis_n = DimsFunctionUtils::GetDim(input_dims, axis);

        run_local_work_ = cw * hb < LowOpParallelismThre && axis_n >= HighOpIntensityThre;

        run_3d_ndrange_         = false;
        std::string kernel_name;
        if (axis == 0) {
            kernel_name = "ReduceC0";
        } else if (axis == 1) {
            kernel_name = "ReduceC1";
        } else if (axis == 2) {
            kernel_name = "ReduceC2";
        } else {
            kernel_name = "ReduceC3";
        }

        if (run_local_work_) {
            kernel_name += "Local";
        }

        std::set<std::string> build_options = CreateBuildOptions();

        ret = CreateExecuteUnit(execute_units_[0], "reduce", kernel_name, build_options);
        if (ret != TNN_OK) {
            LOGE("create execute unit failed!\n");
            return ret;
        }
    } else {
        run_3d_ndrange_         = false;
        std::string kernel_name = "ReduceMultiAxis";

        std::set<std::string> build_options = CreateBuildOptions();

        ret = CreateExecuteUnit(execute_units_[0], "reduce", kernel_name, build_options);
        if (ret != TNN_OK) {
            LOGE("create execute unit failed!\n");
            return ret;
        }
    }

    return TNN_OK;
}

OpenCLReduceLayerAcc::~OpenCLReduceLayerAcc() {}

Status OpenCLReduceLayerAcc::Reshape(const std::vector<Blob *> &inputs, const std::vector<Blob *> &outputs) {
    LOGD("Reduce Layer Reshape\n");
    Status ret = OpenCLLayerAcc::Reshape(inputs, outputs);
    CHECK_TNN_OK(ret)

    auto reduce_param = dynamic_cast<ReduceLayerParam *>(param_);
    if (!reduce_param) {
        LOGE("Error: layer param is null\n");
        return Status(TNNERR_MODEL_ERR, "Error: layer param is null");
    }

    ASSERT(inputs.size() == 1);

    need_reshape_ = reduce_param->keep_dims == 0;
    // init
    if (need_reshape_) {
        ret = InitReshapeLayer(inputs, outputs);
        CHECK_TNN_OK(ret)
    }

    auto input_dims  = inputs[0]->GetBlobDesc().dims;
    auto output_dims = !need_reshape_ ? outputs[0]->GetBlobDesc().dims : reshape_inputs_[0]->GetBlobDesc().dims;

    auto *reduce_output_ptr =
        !need_reshape_ ? (cl::Image *)outputs[0]->GetHandle().base : (cl::Image *)reshape_inputs_[0]->GetHandle().base;

    int hb   = DimsFunctionUtils::GetDim(output_dims, 0) * DimsFunctionUtils::GetDim(output_dims, 2);
    int cw   = DimsFunctionUtils::GetDim(output_dims, 3) * UP_DIV(DimsFunctionUtils::GetDim(output_dims, 1), 4);
    int c4_n = DimsFunctionUtils::GetDim(input_dims, 1) / 4;
    int c4_r = DimsFunctionUtils::GetDim(input_dims, 1) % 4;
    int cw4  = DimsFunctionUtils::GetDim(input_dims, 3) * c4_n;

    if (reduce_param->axis.size() == 1) {
        int axis = reduce_param->axis[0];
        axis     = axis >= 0 ? axis : axis + (int)input_dims.size();

        int axis_n = DimsFunctionUtils::GetDim(input_dims, axis);

        auto &unit            = execute_units_[0];
        uint32_t workgroup_size = 0;

        OpenCLRuntime *opencl_runtime = OpenCLRuntime::GetInstance();
        int type_size = sizeof(float);
        if (opencl_runtime->GetPrecision() != PRECISION_HIGH) {
            type_size = 2;
        }

        if (run_local_work_) {
            workgroup_size = std::min(static_cast<uint32_t>(unit.local_mem_size / (4 * type_size)),
                                    unit.workgroupsize_max);
            workgroup_size = std::min(static_cast<uint32_t>(axis == 1 ? c4_n : axis_n), workgroup_size);
            int temp_size = 1;
            while ((temp_size <<= 1) <= workgroup_size);
            workgroup_size = temp_size >> 1;

            unit.global_work_size = {static_cast<uint32_t>(cw * workgroup_size), static_cast<uint32_t>(hb)};
            unit.local_work_size  = {workgroup_size, 1};
        } else {
            unit.global_work_size = {static_cast<uint32_t>(cw), static_cast<uint32_t>(hb)};
            unit.local_work_size  = LocalWS2DDefault(unit);
        }

        uint32_t idx = 0;
        execute_units_[0].ocl_kernel.setArg(idx++, execute_units_[0].global_work_size[0]);
        execute_units_[0].ocl_kernel.setArg(idx++, execute_units_[0].global_work_size[1]);

        execute_units_[0].ocl_kernel.setArg(idx++, *((cl::Image *)inputs[0]->GetHandle().base));
        execute_units_[0].ocl_kernel.setArg(idx++, *reduce_output_ptr);
        execute_units_[0].ocl_kernel.setArg(idx++, DimsFunctionUtils::GetDim(input_dims, 0));
        execute_units_[0].ocl_kernel.setArg(idx++, DimsFunctionUtils::GetDim(input_dims, 1));
        execute_units_[0].ocl_kernel.setArg(idx++, DimsFunctionUtils::GetDim(input_dims, 2));
        execute_units_[0].ocl_kernel.setArg(idx++, DimsFunctionUtils::GetDim(input_dims, 3));
        execute_units_[0].ocl_kernel.setArg(idx++, c4_n);
        execute_units_[0].ocl_kernel.setArg(idx++, c4_r);
        execute_units_[0].ocl_kernel.setArg(idx++, cw4);
        execute_units_[0].ocl_kernel.setArg(idx++, axis_n);

        if (run_local_work_) {
            if (axis == 1) {
                execute_units_[0].ocl_kernel.setArg(idx++, UP_DIV(c4_n, workgroup_size));
            } else {
                execute_units_[0].ocl_kernel.setArg(idx++, UP_DIV(axis_n, workgroup_size));
            }
            execute_units_[0].ocl_kernel.setArg(idx++, workgroup_size * 4 * type_size, nullptr);
        }
    } else {
        auto &unit              = execute_units_[0];
        uint32_t workgroup_size = 0;

        int axis_n = 1;
        std::vector<int> axis_nhwc = {0, 0, 0, 0};
        for (int i = 0; i < reduce_param->axis.size(); i++) {
            int axis = reduce_param->axis[i];
            axis     = axis >= 0 ? axis : axis + (int)input_dims.size();
            switch(axis) {
                case 0:
                    if (!axis_nhwc[0]) {
                        axis_n *= DimsFunctionUtils::GetDim(input_dims, axis);
                        axis_nhwc[0] = 1;
                    }
                    break;
                case 1:
                    if (!axis_nhwc[3]) {
                        axis_n *= DimsFunctionUtils::GetDim(input_dims, axis);
                        axis_nhwc[3] = 1;
                    }
                    break;
                case 2:
                    if (!axis_nhwc[1]) {
                        axis_n *= DimsFunctionUtils::GetDim(input_dims, axis);
                        axis_nhwc[1] = 1;
                    }
                    break;
                case 3:
                    if (!axis_nhwc[2]) {
                        axis_n *= DimsFunctionUtils::GetDim(input_dims, axis);
                        axis_nhwc[2] = 1;
                    }
                    break;
            }
        }

        unit.global_work_size = {static_cast<uint32_t>(cw), static_cast<uint32_t>(hb)};
        unit.local_work_size  = LocalWS2DDefault(unit);

        uint32_t idx = 0;
        execute_units_[0].ocl_kernel.setArg(idx++, execute_units_[0].global_work_size[0]);
        execute_units_[0].ocl_kernel.setArg(idx++, execute_units_[0].global_work_size[1]);

        execute_units_[0].ocl_kernel.setArg(idx++, *((cl::Image *)inputs[0]->GetHandle().base));
        execute_units_[0].ocl_kernel.setArg(idx++, *reduce_output_ptr);
        execute_units_[0].ocl_kernel.setArg(idx++, DimsFunctionUtils::GetDim(input_dims, 0));
        execute_units_[0].ocl_kernel.setArg(idx++, DimsFunctionUtils::GetDim(input_dims, 1));
        execute_units_[0].ocl_kernel.setArg(idx++, DimsFunctionUtils::GetDim(input_dims, 2));
        execute_units_[0].ocl_kernel.setArg(idx++, DimsFunctionUtils::GetDim(input_dims, 3));
        execute_units_[0].ocl_kernel.setArg(idx++, c4_n);
        execute_units_[0].ocl_kernel.setArg(idx++, c4_r);
        execute_units_[0].ocl_kernel.setArg(idx++, cw4);
        execute_units_[0].ocl_kernel.setArg(idx++, axis_n);
        execute_units_[0].ocl_kernel.setArg(idx++, 4 * sizeof(int), axis_nhwc.data());
    }

    // reshape
    if (need_reshape_) {
        if (reshape_layer_acc_ == nullptr) {
            return Status(TNNERR_OPENCL_ACC_RESHAPE_ERROR, "reshape layer acc in Reduce is null");
        }
        ret = reshape_layer_acc_->Reshape(reshape_inputs_, outputs);
        CHECK_TNN_OK(ret)
    }

    return TNN_OK;
}

Status OpenCLReduceLayerAcc::Forward(const std::vector<Blob *> &inputs, const std::vector<Blob *> &outputs) {
    Status ret = TNN_OK;
    ret        = OpenCLLayerAcc::Forward(inputs, outputs);
    if (need_reshape_) {
        // reshape first
        if (reshape_layer_acc_ == nullptr) {
            return Status(TNNERR_OPENCL_ACC_FORWARD_ERROR, "reshape layer acc in Reduce is null");
        }
        ret = reshape_layer_acc_->Forward(reshape_inputs_, outputs);
        CHECK_TNN_OK(ret)
    }

    return ret;
}

Status OpenCLReduceLayerAcc::InitReshapeLayer(const std::vector<Blob *> &inputs, const std::vector<Blob *> &outputs) {
    Status ret = TNN_OK;

    reshape_layer_acc_ = std::make_shared<OpenCLReshapeLayerAcc>();
    if (reshape_layer_acc_ == nullptr) {
        LOGE("Create Reshape Layer Acc in InnerProduct failed!\n");
        return Status(TNNERR_CREATE_LAYER, "Create Reshape Layer Acc in Reduce failed!");
    }

    auto reduce_param = dynamic_cast<ReduceLayerParam *>(param_);
    if (!reduce_param) {
        LOGE("Error: layer param is null\n");
        return Status(TNNERR_MODEL_ERR, "Error: layer param is null");
    }

    // create output_blob
    BlobDesc desc           = inputs[0]->GetBlobDesc();
    desc.data_format        = DATA_FORMAT_NHC4W4;
    auto input_dims         = inputs[0]->GetBlobDesc().dims;
    auto output_dims        = outputs[0]->GetBlobDesc().dims;
    auto reduce_axis        = reduce_param->axis;
    auto reshape_input_dims = PadDims(output_dims, reduce_axis);
    desc.dims               = reshape_input_dims;
    reshape_input_blob_     = std::make_shared<Blob>(desc);
    if (reshape_input_blob_ == nullptr) {
        LOGE("Create reshape output blob in Reduce failed!\n");
        return Status(TNNERR_CREATE_LAYER, "Create reshape output blob in Reduce failed!");
    }
    reshape_inputs_.clear();
    reshape_inputs_.push_back(reshape_input_blob_.get());

    // create output_image
    int climage_w =
        UP_DIV(DimsFunctionUtils::GetDim(reshape_input_dims, 1), 4) * DimsFunctionUtils::GetDim(reshape_input_dims, 3);
    int climage_h = DimsFunctionUtils::GetDim(reshape_input_dims, 0) * DimsFunctionUtils::GetDim(reshape_input_dims, 2);
    OpenCLRuntime *opencl_runtime = OpenCLRuntime::GetInstance();
    DimsVector imageshape{climage_w, climage_h};
    cl_channel_type data_type = CL_FLOAT;
    if (opencl_runtime->GetPrecision() != PRECISION_HIGH)
        data_type = CL_HALF_FLOAT;
    cl_int err           = CL_SUCCESS;
    reshape_input_image_ = std::make_shared<cl::Image2D>(*opencl_runtime->Context(), CL_MEM_READ_WRITE,
                                                         cl::ImageFormat(CL_RGBA, data_type), imageshape[0],
                                                         imageshape[1], 0, nullptr, &err);
    if (err != CL_SUCCESS) {
        CHECK_CL_SUCCESS(err)
        return Status(TNNERR_OPENCL_MEMALLOC_ERROR, "OpenCL malloc memory failed");
    }
    BlobHandle blob_handle;
    blob_handle.base = reshape_input_image_.get();
    reshape_input_blob_->SetHandle(blob_handle);

    // Init LayerAcc
    reshape_param_.name         = layer_name_ + "_Reshape";
    reshape_param_.reshape_type = 0;
    reshape_param_.axis         = 0;
    reshape_param_.num_axes     = output_dims.size();
    reshape_param_.shape        = output_dims;
    reshape_layer_acc_->Init(ocl_context_, &reshape_param_, nullptr, reshape_inputs_, outputs);

    return ret;
}

}  // namespace TNN_NS
