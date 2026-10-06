#include <metal_stdlib>
using namespace metal;

struct MeshArgumentsParameters {
    uint indexCount;
    uint inputSize;
    uint step;
    uint primitivesPerGroup;
    uint maxGroups;
    uint maxInstances;
    uint maxTotal;
    uint unused;
};

kernel void MeshArguments(device const uint* record [[buffer(0)]], device uint* arguments [[buffer(1)]], constant MeshArgumentsParameters& params [[buffer(2)]]) {
    uint count = record[0];
    uint instances = record[1];
    uint first = record[2];
    uint effective = first < params.indexCount ? min(count, params.indexCount - first) : 0u;
    uint groups = 0u;
    if (effective >= params.inputSize && instances != 0u) {
        groups = (effective - params.inputSize) / params.step / params.primitivesPerGroup + 1u;
        if (groups > params.maxGroups || instances > params.maxInstances || groups > params.maxTotal / instances) groups = 0u;
    }
    arguments[0] = groups;
    arguments[1] = groups != 0u ? instances : 0u;
    arguments[2] = groups != 0u ? 1u : 0u;
    arguments[3] = effective;
    arguments[4] = first;
}
