#pragma once

#include "ComputationalGraph/Operation.h"

namespace Neuro
{
    class LeakyReLUOp : public Operation
    {
    public:
        LeakyReLUOp(TensorLike* x, float alpha);

    protected:
        virtual void ComputeInternal() override;
        virtual void ComputeGradientInternal(const Tensor& grad) override;

    private:
        float m_Alpha;
    };

    static Operation* leaky_relu(TensorLike* x, float alpha)
    {
        return new LeakyReLUOp(x, alpha);
    }
}
