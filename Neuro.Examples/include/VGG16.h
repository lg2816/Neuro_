#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <numeric>

#include "Neuro.h"

using namespace std;
using namespace Neuro;

//https://github.com/keras-team/keras-applications/blob/master/keras_applications/vgg16.py
// weights can be downloaded from: https://github.com/fchollet/deep-learning-models/releases/download/v0.1/vgg16_weights_tf_dim_ordering_tf_kernels.h5
class VGG16
{
public:
    void Run()
    {
        Tensor::SetForcedOpMode(GPU);

        Tensor image = LoadImage("data/mug.jpg", 224, 224);
        PreprocessImage(image, NHWC);

        auto model = CreateModel(NHWC);

        cout << model->Summary();

        model->LoadWeights("data/vgg16_weights_tf_dim_ordering_tf_kernels.h5");

        auto prediction = model->Predict(image)[0];

        cout << prediction->ArgMax(WidthAxis)(0) << " " << prediction->Max(WidthAxis)(0) * 100 << "%" <<  endl;
    }

    //////////////////////////////////////////////////////////////////////////
    static void PreprocessImage(Tensor& image, EDataFormat dataFormat)
    {
        image.Sub(Tensor({ 103.939f, 116.779f, 123.68f }, dataFormat == NHWC ? Shape(3) : Shape(1,1,3)), image);
    }

    static void UnprocessImage(Tensor& image, EDataFormat dataFormat)
    {
        image.Add(Tensor({ 103.939f, 116.779f, 123.68f }, dataFormat == NHWC ? Shape(3) : Shape(1,1,3)), image);
        image.Clipped(0, 255, image);
    }

    //////////////////////////////////////////////////////////////////////////
    static ModelBase* CreateModel(EDataFormat dataFormat)
    {
        Shape inputShape(224, 224, 3);
        if (dataFormat == NHWC)
            inputShape = Shape(3, 224, 224);

        auto model = new Sequential("vgg16");
        model->AddLayer(new Conv2D(inputShape, 64, 3, 1, 1, new ReLU(), dataFormat, "block1_conv1"));
        model->AddLayer(new Conv2D(64, 3, 1, 1, new ReLU(), dataFormat, "block1_conv2"));
        model->AddLayer(new MaxPooling2D(2, 2, 0, dataFormat, "block1_pool"));
        model->AddLayer(new Conv2D(128, 3, 1, 1, new ReLU(), dataFormat, "block2_conv1"));
        model->AddLayer(new Conv2D(128, 3, 1, 1, new ReLU(), dataFormat, "block2_conv2"));
        model->AddLayer(new MaxPooling2D(2, 2, 0, dataFormat, "block2_pool"));
        model->AddLayer(new Conv2D(256, 3, 1, 1, new ReLU(), dataFormat, "block3_conv1"));
        model->AddLayer(new Conv2D(256, 3, 1, 1, new ReLU(), dataFormat, "block3_conv2"));
        model->AddLayer(new Conv2D(256, 3, 1, 1, new ReLU(), dataFormat, "block3_conv3"));
        model->AddLayer(new MaxPooling2D(2, 2, 0, dataFormat, "block3_pool"));
        model->AddLayer(new Conv2D(512, 3, 1, 1, new ReLU(), dataFormat, "block4_conv1"));
        model->AddLayer(new Conv2D(512, 3, 1, 1, new ReLU(), dataFormat, "block4_conv2"));
        model->AddLayer(new Conv2D(512, 3, 1, 1, new ReLU(), dataFormat, "block4_conv3"));
        model->AddLayer(new MaxPooling2D(2, 2, 0, dataFormat, "block4_pool"));
        model->AddLayer(new Conv2D(512, 3, 1, 1, new ReLU(), dataFormat, "block5_conv1"));
        model->AddLayer(new Conv2D(512, 3, 1, 1, new ReLU(), dataFormat, "block5_conv2"));
        model->AddLayer(new Conv2D(512, 3, 1, 1, new ReLU(), dataFormat, "block5_conv3"));
        model->AddLayer(new MaxPooling2D(2, 2, 0, dataFormat, "block5_pool"));
        model->AddLayer(new Flatten("flatten"));
        model->AddLayer(new Dense(4096, new ReLU(), "fc1"));
        model->AddLayer(new Dense(4096, new ReLU(), "fc2"));
        model->AddLayer(new Dense(1000, new Softmax(), "predictions"));
        return model;
    }
};
