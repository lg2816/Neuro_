﻿#include "ComputationalGraph/Session.h"
#include "ComputationalGraph/Graph.h"
#include "ComputationalGraph/Operation.h"
#include "ComputationalGraph/Placeholder.h"
#include "ComputationalGraph/Variable.h"
#include "Tensors/Tensor.h"
#include "Tools.h"
#include "Debug.h"

//#define ENABLE_SESSION_LOGS

#ifdef ENABLE_SESSION_LOGS
#include <windows.h>
#include <debugapi.h>
#define SESSION_DEBUG_INFO(...) do { OutputDebugString(StringFormat(__VA_ARGS__).c_str()); } while(0)
#else
#define SESSION_DEBUG_INFO(...) {}
#endif

namespace Neuro
{
    Session* Session::s_Default = nullptr;

    //////////////////////////////////////////////////////////////////////////
    Session::Session(Graph* graph)
    {
        if (!graph)
            graph = Graph::Default();

        m_Graph = graph;
    }

    //////////////////////////////////////////////////////////////////////////
    Session* Session::Default()
    {
        if (!s_Default)
            s_Default = new Session();

        return s_Default;
    }

    //////////////////////////////////////////////////////////////////////////
    size_t Session::GetFetchesHash(const vector<TensorLike*>& fetches)
    {
        size_t fetchesHash = 0;
        std::hash<TensorLike*> hasher;
        for (size_t i = 0; i < fetches.size(); ++i)
            fetchesHash = fetchesHash * 31 + hasher(fetches[i]);
        return fetchesHash;
    }

    //////////////////////////////////////////////////////////////////////////
    vector<Tensor*> Session::Run(const vector<TensorLike*>& fetches, const map<Placeholder*, const Tensor*>& feeds)
    {
        size_t fetchesHash = GetFetchesHash(fetches);
        auto orderIt = m_OrderCache.find(fetchesHash);
        if (orderIt == m_OrderCache.end())
        {
            OrderCacheData data;
            data.is_training = m_Graph->BuildForwardOrder(fetches, data.order);
            m_OrderCache[fetchesHash] = data;
            orderIt = m_OrderCache.find(fetchesHash);
        }

        return RunInOrder(orderIt->second.order, fetches, feeds, orderIt->second.is_training);
    }

    //////////////////////////////////////////////////////////////////////////
    vector<Tensor*> Session::RunInOrder(const vector<TensorLike*>& order, const vector<TensorLike*>& fetches, const map<Placeholder*, const Tensor*>& feeds, bool training)
    {
        m_Graph->InitVariables();
        m_Graph->IncrementStep();

        for (auto feed : feeds)
        {
            SESSION_DEBUG_INFO("##Session: Feeding '%s'...\n", feed.first->Name().c_str());
            feed.first->m_Output.ResizeBatch(feed.second->Batch());
            NEURO_ASSERT(feed.second->GetShape() == feed.first->m_Output.GetShape(), "Mismatched feed shape. Expected: " << feed.first->m_Output.GetShape().ToString() << " received: " << feed.second->GetShape().ToString());
            feed.second->CopyTo(feed.first->m_Output);
        }

        for (size_t n = 0; n < order.size(); ++n)
        {
            // as of right now there is no functionality using that feature
            /*if (n + 1 < order.size())
            {
                auto node = order[n + 1];
                SESSION_DEBUG_INFO("##Session: Preloading '%s'...\n", node->Name().c_str());
                node->Prefetch();
            }*/

            auto node = order[n];

            NVTXProfile p(node->Name().c_str(), 0xFFD67FFF);

            bool isFetched = find(fetches.begin(), fetches.end(), node) != fetches.end();
            node->SetFetched(isFetched);
            node->Output().ResetRef(isFetched ? 1 : 0); // lock fetches outputs so they don't get completely released 
            
            if (node->IsOp())
            {
                SESSION_DEBUG_INFO("##Session: Computing '%s'...\n", node->Name().c_str());
                Operation* op = static_cast<Operation*>(node);
                op->Compute(training);

                if (Debug::ShouldLogOutput(node->Name()))
                {
                    for (size_t i = 0; i < op->Inputs().size(); ++i)
                    {
                        //op->Inputs()[i]->Validate();
                        op->Inputs()[i]->DebugDumpValues(node->Name() + "_input" + to_string(i) + "_step" + to_string(Debug::GetStep()) + ".log");
                    }
                }
            }

            if (Debug::ShouldLogOutput(node->Name()))
            {
                //node->Output().Validate();
                node->Output().DebugDumpValues(node->Name() + "_output0_step" + to_string(Debug::GetStep()) + ".log");
            }
        }

        Debug::Step();

        vector<Tensor*> result(fetches.size());
        for (size_t i = 0; i < fetches.size(); ++i)
            result[i] = fetches[i]->OutputPtr();
        return result;
    }

    //////////////////////////////////////////////////////////////////////////
    void Session::Clear()
    {
        m_OrderCache.clear();
        m_Graph->Clear();
    }
}
