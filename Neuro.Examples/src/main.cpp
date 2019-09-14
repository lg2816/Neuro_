#include "AutoencoderNetwork.h"
#include "ConvAutoencoderNetwork.h"
#include "ConvNetwork.h"
#include "FlowNetwork.h"
#include "IrisNetwork.h"
#include "MnistConvNetwork.h"
#include "MnistNetwork.h"
#include "GAN.h"
#include "DeepConvGAN.h"

int main()
{
    // tensor image-related operations
    /*Tensor t(Shape(50, 50, 1));
    t.FillWithRand(-1, 0, 1);
    t.SaveAsImage("test.bmp", true);

    t.UpSample2D(2).SaveAsImage("test_x2.bmp", true);
    t.Conv2D(Tensor({1,1,1,1,-8,1,1,1,1,}, Shape(3,3,1)), 1, 0).NormalizedMinMax(EAxis::Global).SaveAsImage("test_conv.bmp", true);
    t.Conv2DTransposed(Tensor({ 1,1,1,1,-8,1,1,1,1, }, Shape(3, 3, 1)), 1, 1, 0).NormalizedMinMax(EAxis::Global).SaveAsImage("test_deconv.bmp", true);
    t.Pool2D(2, 2, EPoolingMode::Max, 0).SaveAsImage("test_maxpool.bmp", true);*/

    //IrisNetwork().Run();
    //ConvNetwork().Run();
    //FlowNetwork().Run();
    //MnistConvNetwork().Run();
    //MnistNetwork().Run();
    //AutoencoderNetwork::Run();
    //ConvAutoencoderNetwork::Run();
    //GAN().Run();
    DeepConvGAN().Run();

    return 0;
}