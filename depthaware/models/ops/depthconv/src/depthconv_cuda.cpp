#include "depthconv_cuda_kernel.h"

#include <torch/extension.h>
#include <THC/THC.h>
#include <stdexcept>
#include <memory>
#include <string>

extern THCState *state;

// C++ interface

#define CHECK_CUDA(x) TORCH_CHECK(x.device().type() == torch::kCUDA, #x " must be a CUDA tensor")
#define CHECK_CONTIGUOUS(x) TORCH_CHECK(x.is_contiguous(), #x " must be contiguous")
#define CHECK_INPUT(x) CHECK_CUDA(x); CHECK_CONTIGUOUS(x)

template<typename ... Args>
std::string string_format( const std::string& format, Args ... args )
{
    size_t size = snprintf( nullptr, 0, format.c_str(), args ... ) + 1; // Extra space for '\0'
    if( size <= 0 ){ throw std::runtime_error( "Error during formatting." ); }
    std::unique_ptr<char[]> buf( new char[ size ] );
    snprintf( buf.get(), size, format.c_str(), args ... );
    return std::string( buf.get(), buf.get() + size - 1 ); // We don't want the '\0' inside
}

torch::Tensor pad_within(torch::Tensor x, int strideX, int strideY){
    namespace F = torch::nn::functional;
    torch::Tensor w = torch::zeros({strideX, strideY}, torch::kCUDA);
    w.index_put_({0, 0}, 1);
    return F::conv_transpose2d(x, w.expand({x.size(1), 1, strideX, strideY}),
                               F::ConvTranspose2dFuncOptions().stride({strideX, strideY}).groups(x.size(1)));
}

void shape_check_forward(torch::Tensor input, torch::Tensor input_depth, torch::Tensor weight,
    int kH, int kW, int dH, int dW, int padH, int padW, int dilationH, int dilationW) {

    if(weight.ndimension() != 4){
        throw std::invalid_argument(string_format("4D weight tensor (nOutputPlane,nInputPlane,kH,kW) expected, "
            "but got: %s", weight.ndimension()));
    }

    if(kW <= 0 || kH <= 0){
        throw std::invalid_argument(string_format("kernel size should be greater than zero, but got kH: %d kW: %d",
            kH, kW));
    }

    if(!(weight.size(2) == kH && weight.size(3) == kW)){
        throw std::invalid_argument(string_format("kernel size should be consistent with weight, but got kH: %d kW: %d weight.size(2): %d, weight.size(3): %d", kH,
            kW, weight.size(2), weight.size(3)));
    }

    if(dW <= 0 || dH <= 0){
        throw std::invalid_argument(string_format("stride should be greater than zero, but got dH: %d dW: %d", dH, dW));
    }

    if(dilationW <= 0 || dilationH <= 0){
        throw std::invalid_argument(string_format("dilation should be greater than 0, but got dilationH: %d dilationW: %d",
            dilationH, dilationW));
    }

    //////////////////////////////////////////

    int ndim = input.ndimension();
    int dimf = 0;
    int dimh = 1;
    int dimw = 2;

    if (ndim == 4) {
        dimf++;
        dimh++;
        dimw++;
    }

    if(ndim != 3 && ndim != 4){
        throw std::invalid_argument(string_format("3D or 4D input tensor expected but got: %s", ndim));
    }

    long nInputPlane = weight.size(1);
    long inputHeight = input.size(dimh);
    long inputWidth = input.size(dimw);
    long nOutputPlane = weight.size(0);

    long outputHeight = (inputHeight + 2 * padH - (dilationH * (kH - 1) + 1)) / dH + 1;
    long outputWidth = (inputWidth + 2 * padW - (dilationW * (kW - 1) + 1)) / dW + 1;

    if (outputWidth < 1 || outputHeight < 1){
        throw std::invalid_argument(string_format(
            "Given input size: (%ld x %ld x %ld). "
            "Calculated output size: (%ld x %ld x %ld). Output size is too small",
            nInputPlane, inputHeight, inputWidth, nOutputPlane, outputHeight,
            outputWidth));
    }

    if(!(inputHeight >= kH && inputWidth >= kW)){
        throw std::invalid_argument("input image is smaller than kernel");
    }

/////////check depth map shape /////////

    int ndim_depth = input.ndimension();
    int dimf_depth = 0;
    int dimh_depth = 1;
    int dimw_depth = 2;

    if (ndim_depth == 4) {
        dimf_depth++;
        dimh_depth++;
        dimw_depth++;
    }

    if(ndim_depth != 3 && ndim_depth != 4){
        throw std::invalid_argument(string_format("3D input depth tensor expected but got: %s", ndim));
    }

    //long inputHeight_depth = input_depth->size[dimh_depth];
    //long inputWidth_depth = input_depth->size[dimw_depth];
    long inputHeight_depth = input_depth.size(dimh_depth);
    long inputWidth_depth = input_depth.size(dimw_depth);

    if(input_depth.size(1) != 1){
        throw std::invalid_argument("input depth should have only 1 channel");
    }

    if(!(inputHeight == inputHeight_depth && inputWidth == inputWidth_depth)){
        throw std::invalid_argument("input image and input depth should be the same size");
    }
}

void shape_check_bias(torch::Tensor weight, torch::Tensor bias){
    //////////// check bias //////////////////
    if(bias.ndimension() != 1){
        throw std::invalid_argument(string_format("Need bias of dimension %d but got %d", 1, bias.ndimension()));
    }

    if(bias.size(0) != weight.size(0)){
        throw std::invalid_argument(string_format("Need bias of size %d but got %d",
            weight.size(0), bias.size(0)));
    }
}

void shape_check_gradOutput(torch::Tensor input, torch::Tensor weight, torch::Tensor gradOutput,
    int kH, int kW, int strideH, int strideW, int padH, int padW, int dilationH, int dilationW){

    int ndim = input.ndimension();
    int dimf = 0;
    int dimh = 1;
    int dimw = 2;

    if (ndim == 4) {
        dimf++;
        dimh++;
        dimw++;
    }

    long inputHeight = input.size(dimh);
    long inputWidth = input.size(dimw);
    long nOutputPlane = weight.size(0);

    long outputHeight = (inputHeight + 2 * padH - (dilationH * (kH - 1) + 1)) / strideH + 1;
    long outputWidth = (inputWidth + 2 * padW - (dilationW * (kW - 1) + 1)) / strideW + 1;

//////////////////////////////////////////
    if(gradOutput.size(dimf) != nOutputPlane){
        throw std::invalid_argument(string_format("invalid number of gradOutput planes, expected: %d, but got: %d",
            nOutputPlane, gradOutput.size(dimf)));
    }

    if(!(gradOutput.size(dimh) == outputHeight && gradOutput.size(dimw) == outputWidth)){
        throw std::invalid_argument(string_format("invalid size of gradOutput, expected height: %d width: %d , but got height: %d width: %d",
            outputHeight, outputWidth, gradOutput.size(dimh), gradOutput.size(dimw)));
    }
}

torch::Tensor depthconv_forward_cuda(torch::Tensor input, torch::Tensor input_depth,
                             torch::Tensor weight, torch::Tensor bias, int alpha,
                             int kW, int kH, int strideW, int strideH, int padW, int padH,
                             int dilationH, int dilationW) {

    CHECK_INPUT(input);
    CHECK_INPUT(input_depth);
    CHECK_INPUT(weight);
    CHECK_INPUT(bias);

    shape_check_forward(input, input_depth, weight, kH, kW, strideH, strideW, padH, padW,
              dilationH, dilationW);
    shape_check_bias(weight, bias);

    int batch = 1;
    if (input.ndimension() == 3) {
        // Force batch
        batch = 0;
        input = input.reshape({1, input.size(0), input.size(1), input.size(2)});
        input_depth = input_depth.reshape({1, input_depth.size(0), input_depth.size(1), input_depth.size(2)});
    }

    int batchSize = input.size(0);
    int nInputPlane = input.size(1);
    int inputHeight = input.size(2);
    int inputWidth = input.size(3);

    int nOutputPlane = weight.size(0);

    int outputWidth =
        (inputWidth + 2 * padW - (dilationW * (kW - 1) + 1)) / strideW + 1;
    int outputHeight =
        (inputHeight + 2 * padH - (dilationH * (kH - 1) + 1)) / strideH + 1;

    // Allocate memory to build up output representation
    torch::Tensor output = torch::zeros({batchSize, nOutputPlane, outputHeight, outputWidth}, torch::kCUDA);

    torch::Tensor input_n;
    torch::Tensor depth_n;
    torch::Tensor output_n;

    //Repeat bias to match output size
    //Without the extra singleton dimensions the repeat function has the wrong dimensionality
    bias = bias.reshape({bias.size(0), 1}).repeat({1, outputHeight*outputWidth});

    //Calculate output for each input element in batch
    for (int elt = 0; elt < batchSize; elt++) {

        input_n = input.select(0, elt);
        depth_n = input_depth.select(0, elt);
        output_n = output.select(0, elt);

        //Reshape input and weight with depth difference
        torch::Tensor columns = depthconv_im2col(input_n, depth_n, alpha,
            nInputPlane, inputHeight, inputWidth,
            kH, kW,
            padH, padW,
            strideH, strideW,
            dilationH, dilationW);

        torch::Tensor weight_slice = weight.reshape({weight.size(0), weight.size(1)*weight.size(2)*weight.size(3)});
        torch::Tensor output_slice = output_n.reshape({nOutputPlane, outputWidth*outputHeight});

        //Multiplication with reshaped input is equivalent to 2d convolution
        {
        using namespace torch::indexing;
        output_slice.index_put_({Ellipsis}, torch::addmm(bias, weight_slice, columns));
        }

       //Original code for reference
//        long m = weight.size(0);
//        long n = columns.size(1);
//        long k = input.size(1) * kH * kW;
//
//        THCudaBlas_Sgemm(state, 'n', 'n', n, m, k, 1.0f,
//                     columns.data(), n,
//                     weight.data(), k, 1.0f,
//                     output_n.data(), n);
    }

    if (batch == 0) {
        output = output.reshape({nOutputPlane, outputHeight, outputWidth});
    }

    return output;
}

//Compute input gradient as a full convolution between grad_output and dialated_weight transposed
torch::Tensor depthconv_input_grad(torch::Tensor input_depth, torch::Tensor gradOutput,
    torch::Tensor weight, double alpha,
    int nInputPlane, int inputWidth, int inputHeight,
    int kW, int kH, int strideW, int strideH,
    int dilationW, int dilationH){

    //Transpose weight
    torch::Tensor weight_t = weight.permute({1, 0, 3, 2});

    int batchSize = gradOutput.size(0);
    int nOutputPlane = gradOutput.size(1);

    //This is a full convolution, so we need extra padding based on kernel size
    int padW = ((weight.size(2) - 1)*dilationW + 1) / 2;
    int padH = ((weight.size(3) - 1)*dilationH + 1) / 2;
    namespace F = torch::nn::functional;
    torch::Tensor gradOutput_padded = F::pad(gradOutput, F::PadFuncOptions({padW, padW, padH, padH}));

    //The depth also needs padding
    int depth_padW = (gradOutput_padded.size(2) - input_depth.size(2)) / 2;
    int depth_padH = (gradOutput_padded.size(3) - input_depth.size(3)) / 2;
    torch::Tensor depth_padded = F::pad(input_depth, F::PadFuncOptions({depth_padW, depth_padW, depth_padH, depth_padH}));

    std::cout << string_format("weight_t dim: %i", weight_t.ndimension()) << std::endl;
    std::cout << weight_t << std::endl;

    //Stride and dialation are added with padding between matrix elements
    weight_t = pad_within(weight_t, dilationW, dilationH);

    std::cout << string_format("weight_t dim: %i", weight_t.ndimension()) << std::endl;
    std::cout << weight_t << std::endl;
    int kt_W = weight_t.size(2);
    int kt_H = weight_t.size(3);
    weight_t = weight_t.reshape({weight_t.size(1), weight_t.size(0), weight_t.size(2)*weight_t.size(3)});

    std::cout << string_format("gradOutput_padded dim: %i", gradOutput_padded.ndimension()) << std::endl;
    std::cout << gradOutput_padded << std::endl;

    gradOutput_padded = pad_within(gradOutput_padded, strideW, strideH);

    std::cout << string_format("gradOutput_padded dim: %i", gradOutput_padded.ndimension()) << std::endl;
    std::cout << gradOutput_padded << std::endl;

    std::cout << string_format("depth_padded dim: %i", depth_padded.ndimension()) << std::endl;
    std::cout << depth_padded << std::endl;

    depth_padded = pad_within(depth_padded, strideW, strideH);

    //Stride and dialation are added with padding between matrix elements
//    weight_t = pad_within(weight_t, dilationW, dilationH);
//    gradOutput_padded = pad_within(gradOutput_padded, strideW, strideH);
//    torch::Tensor depth_padded = pad_within(input_depth, strideW, strideH);

    std::cout << string_format("depth_padded dim: %i", depth_padded.ndimension()) << std::endl;
    std::cout << depth_padded << std::endl;

    // Allocate memory to build up output representation
    torch::Tensor gradInput = torch::zeros({batchSize, nInputPlane, inputWidth, inputHeight}, torch::kCUDA);

    for(int elt=0; elt<batchSize; elt++){
        torch::Tensor gradOutput_n = gradOutput_padded.select(0, elt);
        torch::Tensor depth_n = depth_padded.select(0, elt);
        torch::Tensor gradInput_n = gradInput.select(0, elt);

        std::cout << string_format("gradOutput_n dim: %i", gradOutput_n.ndimension()) << std::endl;
        std::cout << gradOutput_n << std::endl;

        //Reshape input and weight with depth difference
        torch::Tensor columns = depthconv_im2col(gradOutput_n, depth_n, alpha,
                nOutputPlane, gradOutput_padded.size(2), gradOutput_padded.size(3),
                kt_W, kt_H,
                0, 0, 1, 1, 1, 1);

        std::cout << string_format("columns dim: %i", columns.ndimension()) << std::endl;
        std::cout << columns << std::endl;

        std::cout << string_format("weight_t dim: %i", weight_t.ndimension()) << std::endl;
        std::cout << weight_t << std::endl;

        //Multiplication with reshaped input is equivalent to 2d convolution
        {
        using namespace torch::indexing;
        columns = torch::matmul(weight_t, columns).reshape({weight.size(0), nInputPlane, inputWidth, inputHeight});
        gradInput_n.index_put_({Ellipsis}, columns); //.index({Ellipsis, Slice(padW, -padW), Slice(padH, -padH)})
        }
    }

    std::cout << string_format("gradInput dim: %i", gradInput.ndimension()) << std::endl;
    std::cout << string_format("gradInput: %i x %i x %i", gradInput.size(0), gradInput.size(1), gradInput.size(2)) << std::endl;
    std::cout << gradInput << std::endl;

    return gradInput;
}

//Compute weight gradient as covolution of input and gradOutput
torch::Tensor depthconv_weight_grad(torch::Tensor input, torch::Tensor input_depth, torch::Tensor gradOutput,
    double alpha, int kW, int kH, int strideW, int strideH, int padW, int padH, int dilationH, int dilationW){

    int batchSize = input.size(0);
    int nInputPlane = input.size(1);
    int inputWidth = input.size(2);
    int inputHeight = input.size(3);
    int nOutputPlane = gradOutput.size(1);

    // Allocate memory to build up output representation
    torch::Tensor gradWeight = torch::zeros({nOutputPlane, nInputPlane, kW, kH}, torch::kCUDA);

    for(int elt=0; elt<batchSize; elt++){
        torch::Tensor gradOutput_n = gradOutput.select(0, elt).reshape({nOutputPlane, gradOutput.size(2)*gradOutput.size(3)});
        torch::Tensor depth_n = input_depth.select(0, elt);
        torch::Tensor input_n = input.select(0, elt);

        //Reshape input and gradOutput with depth difference
        //In backward pass of convolution, stride and dilation switch roles
        torch::Tensor columns = depthconv_im2col(input_n, depth_n, alpha,
                nInputPlane, inputHeight, inputWidth,
                kH, kW,
                padH, padW,
                dilationH, dilationW,
                strideH, strideW);

        std::cout << string_format("columns dim: %i", columns.ndimension()) << std::endl;
        std::cout << columns << std::endl;

        std::cout << string_format("gradOutput_n dim: %i", gradOutput_n.ndimension()) << std::endl;
        std::cout << gradOutput_n << std::endl;

        //Multiplication with reshaped input is equivalent to 2d convolution
        torch::Tensor product = torch::matmul(gradOutput_n, columns.transpose(1,0));
        gradWeight.add_(product.reshape({nOutputPlane, nInputPlane, kW,  kH}));
    }

    std::cout << string_format("gradWeight dim: %i", gradWeight.ndimension()) << std::endl;
    std::cout << gradWeight << std::endl;

    return gradWeight;
}

torch::Tensor depthconv_bias_grad(torch::Tensor gradOutput, double scale){
    // Do Bias:
    // Sum of gradOutput across width, height, batchsize,
    return gradOutput.mul(scale).sum(/*dim=*/{0, 2, 3});
}

std::vector<torch::Tensor> depthconv_backward_cuda(
    torch::Tensor input, torch::Tensor input_depth, torch::Tensor gradOutput,
    torch::Tensor weight, double alpha, int kW, int kH, int strideW, int strideH,
    int padW, int padH, int dilationH, int dilationW, double scale) {

    CHECK_INPUT(input);
    CHECK_INPUT(input_depth);
    CHECK_INPUT(gradOutput);
    CHECK_INPUT(weight);

    shape_check_forward(input, input_depth, weight, kH, kW, strideH, strideW, padH,
              padW, dilationH, dilationW);
    shape_check_gradOutput(input, weight, gradOutput, kH, kW, strideH, strideW, padH,
              padW, dilationH, dilationW);

    int batch = 1;
    if (input.ndimension() == 3) {
        // Force batch
        batch = 0;
        input = input.view({1, input.size(0), input.size(1), input.size(2)});
        gradOutput = gradOutput.view({1, gradOutput.size(0), gradOutput.size(1), gradOutput.size(2)});
    }

    int batchSize = input.size(0);
    int nInputPlane = input.size(1);
    int inputHeight = input.size(2);
    int inputWidth = input.size(3);

    int nOutputPlane = weight.size(0);

    int outputWidth =
      (inputWidth + 2 * padW - (dilationW * (kW - 1) + 1)) / strideW + 1;
    int outputHeight =
      (inputHeight + 2 * padH - (dilationH * (kH - 1) + 1)) / strideH + 1;

    if(input_depth.size(0) != batchSize){
        throw std::invalid_argument("invalid batch size of input depth");
    }

    torch::Tensor gradInput = depthconv_input_grad(input_depth, gradOutput, weight, alpha,
                                                   nInputPlane, inputWidth, inputHeight,
                                                   kW, kH, strideW, strideH,
                                                   dilationW, dilationH);

    torch::Tensor gradWeight = depthconv_weight_grad(input, input_depth, gradOutput, alpha,
                                                    kW, kH, strideW, strideH,
                                                    padW, padH, dilationH, dilationW);

    // Do Bias:
    torch::Tensor gradBias = depthconv_bias_grad(gradOutput, scale);

    if (batch == 0) {
        gradOutput = gradOutput.view({nOutputPlane, outputHeight, outputWidth});
        input = input.view({nInputPlane, inputHeight, inputWidth});
        input_depth = input_depth.view({1, inputHeight, inputWidth});
        gradInput = gradInput.view({nInputPlane, inputHeight, inputWidth});
    }

    return {gradInput, gradWeight, gradBias};
}

PYBIND11_MODULE(TORCH_EXTENSION_NAME, m) {
  m.def("forward", &depthconv_forward_cuda, "Depth Aware Convolution forward (CUDA)");
  m.def("backward", &depthconv_backward_cuda, "Depth Aware Convolution backward pass for input (CUDA)");
}
