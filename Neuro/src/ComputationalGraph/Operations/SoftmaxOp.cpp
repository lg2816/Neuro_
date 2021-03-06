#include "ComputationalGraph/Operations/SoftmaxOp.h"

namespace Neuro
{
    //////////////////////////////////////////////////////////////////////////
    SoftmaxOp::SoftmaxOp(TensorLike* x, const string& name)
        : Operation({ x }, name.empty() ? "softmax" : name)
    {
        UpdateOutputShape();
    }

    //////////////////////////////////////////////////////////////////////////
    void SoftmaxOp::ComputeInternal()
    {
        m_Output.ResizeBatch(m_Inputs[0]->Batch());
        m_Inputs[0]->Softmax(m_Output);
    }

    //////////////////////////////////////////////////////////////////////////
    void SoftmaxOp::ComputeGradientInternal(const Tensor& grad)
    {
        if (m_InputNodes[0]->CareAboutGradient())
            m_Output.SoftmaxGradient(m_Output, grad, m_InputsGrads[0]);
    }
}