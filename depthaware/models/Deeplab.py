
import torch.nn as nn
from torch.autograd import Variable
from collections import OrderedDict
from tensorboardX import SummaryWriter

import depthaware.models.VGG_Deeplab as VGG_Deeplab
from depthaware.models.base_model import BaseModel
from depthaware.utils.util import *

class Deeplab_VGG(nn.Module):
    def __init__(self, num_classes, depthconv=False):
        super(Deeplab_VGG,self).__init__()
        self.Scale = VGG_Deeplab.vgg16(num_classes=num_classes,depthconv=depthconv)

    def forward(self,x, depth=None):
        output = self.Scale(x,depth) # for original scale
        return output

#------------------------------------------------------#

class Deeplab_Solver(BaseModel):
    def __init__(self, opt, dataset=None, encoder='VGG', useCuda = torch.cuda.is_available()):
        BaseModel.initialize(self, opt)

        self.useCuda = useCuda
        self.encoder = encoder
        if encoder == 'VGG':
            self.model = Deeplab_VGG(self.opt.label_nc, self.opt.depthconv)

        if self.opt.isTrain:
            if self.useCuda:
                self.criterionSeg = torch.nn.CrossEntropyLoss(ignore_index=255).cuda()
            else:
                self.criterionSeg = torch.nn.CrossEntropyLoss(ignore_index=255)

            if encoder == 'VGG':
                self.optimizer = torch.optim.SGD([{'params': self.model.Scale.get_1x_lr_params_NOscale(), 'lr': self.opt.lr},
                                                 {'params': self.model.Scale.get_10x_lr_params(), 'lr': self.opt.lr},
                                                 {'params': self.model.Scale.get_2x_lr_params_NOscale(), 'lr': self.opt.lr, 'weight_decay': 0.},
                                                 {'params': self.model.Scale.get_20x_lr_params(), 'lr': self.opt.lr, 'weight_decay': 0.}
                                                  ],
                                                 lr=self.opt.lr, momentum=self.opt.momentum, weight_decay=self.opt.wd)

            self.old_lr = self.opt.lr
            self.averageloss = []

            self.writer = SummaryWriter(self.tensorborad_dir)
            self.counter = 0

        if not self.isTrain or self.opt.continue_train:
            if self.opt.pretrained_model!='':
                self.load_pretrained_network(self.model, self.opt.pretrained_model, self.opt.which_epoch, strict=False)
                print("Successfully loaded from pretrained model with given path!")
            else:
                self.load()
                print("Successfully loaded model, continue training....!")

        if self.useCuda:
            self.model.cuda()
        self.normweightgrad=0.
        # if len(opt.gpu_ids):#opt.isTrain and
        #     self.model = torch.nn.DataParallel(self.model, device_ids=opt.gpu_ids)

    def forward(self, data, isTrain=True):
        self.model.zero_grad()

        self.image = Variable(data['image'], requires_grad=not isTrain)
        if 'depth' in data.keys():
            self.depth = Variable(data['depth'], requires_grad=not isTrain)
        else:
            self.depth = None
        if data['seg'] is not None:
            self.seggt = Variable(data['seg'])
        else:
            self.seggt = None

        if self.useCuda:
            self.depth = self.depth.cuda()
            self.image = self.image.cuda()
            if self.seggt is not None:
                self.seggt = self.seggt.cuda()

        input_size = self.image.size()

        self.segpred = self.model(self.image,self.depth)
        self.segpred = nn.functional.interpolate(self.segpred, size=(input_size[2], input_size[3]), mode='bilinear', align_corners=True)

        if self.opt.isTrain:
            self.loss = self.criterionSeg(self.segpred, torch.squeeze(self.seggt,1).long())
            self.averageloss += [self.loss.item()]

        segpred = self.segpred.max(1, keepdim=True)[1]
        return self.seggt, segpred


    def backward(self, step, total_step):
        #print(self.loss)
        self.loss.backward()
        self.optimizer.step()
        # print self.model.Scale.classifier.fc6_2.weight.grad.mean().data.cpu().numpy()
        # self.normweightgrad +=self.model.Scale.classifier.norm.scale.grad.mean().data.cpu().numpy()
        # print self.normweightgrad#self.model.Scale.classifier.norm.scale.grad.mean().data.cpu().numpy()
        if step % self.opt.iterSize  == 0:
            self.update_learning_rate(step, total_step)
            trainingavgloss = np.mean(self.averageloss)
            if self.opt.verbose:
                print ('  Iter: %d, Loss: %f' % (step, trainingavgloss) )

    def get_visuals(self, step):
        ############## Display results and errors ############
        if self.opt.isTrain:
            self.trainingavgloss = np.mean(self.averageloss)
            if self.opt.verbose:
                print ('  Iter: %d, Loss: %f' % (step, self.trainingavgloss) )
            self.writer.add_scalar(self.opt.name+'/trainingloss/', self.trainingavgloss, step)
            self.averageloss = []

        if self.depth is not None:
            return OrderedDict([('image', tensor2im(self.image.data[0], inputmode=self.opt.inputmode)),
                                ('depth', tensor2im(self.depth.data[0], inputmode='divstd-mean')),
                                ('segpred', tensor2label(self.segpred.data[0], self.opt.label_nc)),
                                ('seggt', tensor2label(self.seggt.data[0], self.opt.label_nc))])
        else:
            return OrderedDict([('image', tensor2im(self.image.data[0], inputmode=self.opt.inputmode)),
                                ('segpred', tensor2label(self.segpred.data[0], self.opt.label_nc)),
                                ('seggt', tensor2label(self.seggt.data[0], self.opt.label_nc))])

    def update_tensorboard(self, data, step):
        if self.opt.isTrain:
            self.writer.add_scalar(self.opt.name+'/Accuracy/', data[0], step)
            self.writer.add_scalar(self.opt.name+'/Accuracy_Class/', data[1], step)
            self.writer.add_scalar(self.opt.name+'/Mean_IoU/', data[2], step)
            self.writer.add_scalar(self.opt.name+'/FWAV_Accuracy/', data[3], step)

            self.trainingavgloss = np.mean(self.averageloss)
            self.writer.add_scalars(self.opt.name+'/loss', {"train": self.trainingavgloss,
                                                             "val": np.mean(self.averageloss)}, step)

            self.writer.add_scalars('trainingavgloss/', {self.opt.name: self.trainingavgloss}, step)
            self.writer.add_scalars('valloss/', {self.opt.name: np.mean(self.averageloss)}, step)
            self.writer.add_scalars('val_MeanIoU/', {self.opt.name: data[2]}, step)

            file_name = os.path.join(self.save_dir, 'MIoU.txt')
            with open(file_name, 'wt') as opt_file:
                opt_file.write('%f\n' % (data[2]))
            # self.writer.add_scalars('losses/'+self.opt.name, {"train": self.trainingavgloss,
            #                                                  "val": np.mean(self.averageloss)}, step)
            self.averageloss = []

    def save(self, which_epoch):
        # self.save_network(self.netG, 'G', which_epoch, self.gpu_ids)
        self.save_network(self.model, 'net', which_epoch, self.gpu_ids)

    def load(self):
        self.load_network(self.model, 'net',self.opt.which_epoch)

    def update_learning_rate(self, step, total_step):

        lr = max(self.opt.lr * ((1 - float(step) / total_step) ** (self.opt.lr_power)), 1e-6)

        # drop_ratio = (1. * float(total_step - step) / (total_step - step + 1)) ** self.opt.lr_power
        # lr = self.old_lr * drop_ratio

        self.writer.add_scalar(self.opt.name+'/Learning_Rate/', lr, step)

        self.optimizer.param_groups[0]['lr'] = lr
        self.optimizer.param_groups[1]['lr'] = lr
        self.optimizer.param_groups[2]['lr'] = lr
        self.optimizer.param_groups[3]['lr'] = lr

        # torch.nn.utils.clip_grad_norm(self.model.Scale.get_1x_lr_params_NOscale(), 1.)
        # torch.nn.utils.clip_grad_norm(self.model.Scale.get_10x_lr_params(), 1.)

        if self.opt.verbose:
            print('     update learning rate: %f -> %f' % (self.old_lr, lr))
        self.old_lr = lr
