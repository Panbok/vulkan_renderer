#include "ssr_stability_generated.cpp"
#include <cstdio>
int main()
{
    float results[36] = {};
    GlobalParams_0 globals = {};
    globals.g_results_0.data = results;
    globals.g_results_0.count = 36;
    ComputeVaryingInput input = {};
    input.startGroupID = uint3(0u, 0u, 0u);
    input.endGroupID = uint3(1u, 1u, 1u);
    ssr_stability_execute(&input, nullptr, &globals);
    for (float value : results) std::printf("%.9g\n", value);
}
