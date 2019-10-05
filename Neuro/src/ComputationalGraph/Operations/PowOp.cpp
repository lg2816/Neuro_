#include "ComputationalGraph/Operations/PowOp.h"

namespace Neuro
{
    //////////////////////////////////////////////////////////////////////////
    PowOp::PowOp(TensorLike* x, TensorLike* p, const string& name)
        : Operation({ x, p }, name.empty() ? "pow" : name)
    {
        assert(p->GetShape().Length == 1);
        m_Output.Resize(x->GetShape());
    }

    //////////////////////////////////////////////////////////////////////////
    void PowOp::ComputeInternal()
    {
        m_Output.ResizeBatch(m_Inputs[0]->Batch());

        float power = (*m_Inputs[1])(0);

        if (power == 2)
            m_Inputs[0]->Map([&](float x) {return x * x; }, m_Output);
        else
            m_Inputs[0]->Map([&](float x) {return ::pow(x, power); }, m_Output);
    }

    //////////////////////////////////////////////////////////////////////////
    void PowOp::ComputeGradientInternal(const Tensor& grad)
    {
        float power = (*m_Inputs[1])(0);

        //in_grad = grad * p * x^(p-1)
        if (power == 2)
            grad.Map([&](float g, float x) {return g * 2 * x; }, *m_Inputs[0], m_InputsGrads[0]);
        else
            grad.Map([&](float g, float x) {return g * power * ::pow(x, power - 1); }, *m_Inputs[0], m_InputsGrads[0]);
    }
}