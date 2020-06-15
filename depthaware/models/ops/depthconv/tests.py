import torch
import numpy as np
from depthaware.models.ops.depthconv.functional import DepthconvFunction

class TestLoss(torch.autograd.Function):
    @staticmethod
    def forward(ctx, input, target):
        return input - target

    @staticmethod
    def backward(ctx, grad_output):
        return grad_output, None


if __name__ == '__main__':

    #TODO check dilation and stride
    batch_size = 1
    w, h = 8, 8
    kernel_size = 3
    out_channels = 1
    stride = [1, 1]
    padding = [0, 0]
    dilation = [1, 1]

    if torch.cuda.is_available():
        device = torch.device('cuda')
    else:
        device = torch.device('cpu')

    input_size = (batch_size, 3, w, h)
    input =  0.01 * torch.FloatTensor(range(input_size[0]*input_size[1]*input_size[2]*input_size[3])).cuda().reshape(input_size)
    depth = torch.ones((batch_size, 1, w, h), device=device)
    weight_size = (out_channels, 3, kernel_size, kernel_size)
    weight = 0.01 * torch.FloatTensor(range(weight_size[0]*weight_size[1]*weight_size[2]*weight_size[3])).cuda().reshape(weight_size)
    bias = torch.ones((out_channels), device=device)
    outsize = DepthconvFunction.outputSize(input, weight, stride, padding, dilation)
    grad_output = torch.FloatTensor(range(outsize[0]*outsize[1]*outsize[2]*outsize[3])).cuda().reshape(outsize)
    alpha = 1.0

    print("Toy input:")
    print(input)

    print("Toy grad output:")
    print(grad_output)

    print("Toy weights output:")
    print(weight)

    x_test = DepthconvFunction.apply(input, depth, weight, bias, alpha, stride, padding, dilation)
    target = torch.zeros(x_test.shape, device=device)
    loss = TestLoss.apply(x_test, target)
    loss.backward(grad_output)

    print("DepthConv input gradient:")
    print(input.grad.cpu())

    conv_layer = torch.nn.Conv2d(out_channels, kernel_size, kernel_size, bias=True, stride=stride, padding=padding,
                                 dilation=dilation, groups=1)
    conv_layer.weight = torch.nn.Parameter(weight.clone(), requires_grad=True)
    conv_layer.bias = torch.nn.Parameter(bias.clone(), requires_grad=True)

    input = input.clone().detach().cuda().requires_grad_(True)
    x = conv_layer(input)
    target = torch.zeros(x.shape, device=device)
    loss = TestLoss.apply(x, target)
    loss.backward(grad_output)

    print("Pytorch input gradient:")
    print(input.grad.cpu())

    print("DepthConv weight gradient:")
    print(weight.grad.cpu())

    print("Pytorch weight gradient:")
    print(conv_layer.weight.grad.cpu())

    print("DepthConv bias gradient:")
    print(bias.grad.cpu())

    print("Pytorch bias gradient:")
    print(conv_layer.bias.grad.cpu())

    np.testing.assert_array_almost_equal(input.cpu().detach().numpy(), input.grad.cpu().detach().numpy())
    np.testing.assert_array_almost_equal(weight.grad.cpu().detach().numpy(), conv_layer.weight.grad.cpu().detach().numpy())
    np.testing.assert_array_almost_equal(bias.cpu().detach().numpy(), conv_layer.bias.grad.cpu().detach().numpy())