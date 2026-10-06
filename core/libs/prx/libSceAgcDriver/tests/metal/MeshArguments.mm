#include "MetalTestSupport.hpp"
#include <array>
#include <cstring>

namespace {
struct MeshParameters {
    uint32_t indexCount;
    uint32_t inputSize;
    uint32_t step;
    uint32_t primitivesPerGroup;
    uint32_t maxGroups;
    uint32_t maxInstances;
    uint32_t maxTotal;
    uint32_t unused;
};

struct MeshCase {
    std::array<uint32_t, 3> record;
    MeshParameters parameters;
    std::array<uint32_t, 5> expected;
};
}

void RunMeshArgumentsTests(const MetalTests::Context& context) {
    const MeshCase cases[] = {
        {{23, 2, 5}, {100, 3, 2, 4, 3, 2, 6, 0}, {3, 2, 1, 23, 5}},
        {{23, 2, 5}, {100, 3, 2, 4, 2, 2, 6, 0}, {0, 0, 0, 23, 5}},
        {{23, 2, 5}, {100, 3, 2, 4, 3, 1, 6, 0}, {0, 0, 0, 23, 5}},
        {{23, 2, 5}, {100, 3, 2, 4, 3, 2, 5, 0}, {0, 0, 0, 23, 5}},
        {{23, 0, 5}, {100, 3, 2, 4, 3, 2, 6, 0}, {0, 0, 0, 23, 5}},
        {{2, 2, 5}, {100, 3, 2, 4, 3, 2, 6, 0}, {0, 0, 0, 2, 5}},
        {{3, 2, 5}, {100, 3, 2, 4, 3, 2, 6, 0}, {1, 2, 1, 3, 5}},
        {{10, 2, 5}, {100, 3, 2, 4, 3, 2, 6, 0}, {1, 2, 1, 10, 5}},
        {{11, 2, 5}, {100, 3, 2, 4, 3, 2, 6, 0}, {2, 2, 1, 11, 5}},
        {{1000, 2, 90}, {100, 3, 2, 4, 3, 2, 6, 0}, {1, 2, 1, 10, 90}},
        {{1000, 2, 100}, {100, 3, 2, 4, 3, 2, 6, 0}, {0, 0, 0, 0, 100}},
        {{1000, 2, 0xffffffffu}, {100, 3, 2, 4, 3, 2, 6, 0}, {0, 0, 0, 0, 0xffffffffu}},
        {{11, 0xffffffffu, 5}, {100, 3, 2, 4, 3, 0xffffffffu, 0xffffffffu, 0}, {0, 0, 0, 11, 5}},
    };
    for (const auto& test : cases) {
        std::array<uint32_t, 8> record = {test.record[0], test.record[1], test.record[2], 0x12345678u, 0x87654321u, 7, 8, 9};
        auto recordBuffer = context.Buffer(sizeof(record));
        auto argumentBuffer = context.Buffer(8 * sizeof(uint32_t), 0xa5);
        std::memcpy([recordBuffer contents], record.data(), sizeof(record));
        context.Dispatch(@"MeshArguments", @[recordBuffer, argumentBuffer], &test.parameters, sizeof(test.parameters), MTLSizeMake(1, 1, 1));
        const auto* output = static_cast<const uint32_t*>([argumentBuffer contents]);
        MetalTests::Require(std::memcmp(output, test.expected.data(), sizeof(test.expected)) == 0, "MeshArguments returned incorrect indirect draw arguments");
        for (size_t i = 5; i < 8; ++i) {
            MetalTests::Require(output[i] == 0xa5a5a5a5u, "MeshArguments overwrote words beyond the five argument words");
        }
        MetalTests::Require(std::memcmp([recordBuffer contents], record.data(), sizeof(record)) == 0, "MeshArguments modified its input record");
    }
}
