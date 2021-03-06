#pragma once

#include <random>

namespace Neuro
{
	using namespace std;

	class Random
	{
	public:
		Random(unsigned int seed = 0);

		int Next(int max);
		int Next(int min, int max);
		float NextFloat();
		float NextFloat(float max);
		float NextFloat(float min, float max);

	private:
		mt19937 m_Engine;
        int m_GeneratedNumbersCount = 0;
	};
}