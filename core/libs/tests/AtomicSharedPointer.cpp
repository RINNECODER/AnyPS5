#include "prx/libc/include/general/AtomicSharedPtr.hpp"
#include <array>
#include <cassert>
#include <vector>
#include <thread>
#include <cstdio>
int main() {
    AtomicSharedPtr<int> slot;
    std::shared_ptr<int> expected;
    auto first = std::make_shared<int>(1);
    assert(slot.compare_exchange_strong(expected, first));
    assert(!expected);
    auto second = std::make_shared<int>(2);
    assert(!slot.compare_exchange_strong(expected, second));
    assert(expected == first);
    assert(slot.compare_exchange_strong(expected, second));
    assert(expected == first && slot.load() == second);
    std::shared_ptr<int> unrelated(second.get(), [](int*) {});
    expected = unrelated;
    assert(!slot.compare_exchange_strong(expected, {}));
    assert(!expected.owner_before(second) && !second.owner_before(expected));
    auto array = std::make_shared<std::array<int, 2>>();
    std::shared_ptr<int> aliasA(array, &(*array)[0]), aliasB(array, &(*array)[1]);
    slot.store(aliasA); expected = aliasB;
    assert(!slot.compare_exchange_strong(expected, {}));
    assert(expected == aliasA);
    std::shared_ptr<int> nullAlias(array, nullptr);
    slot.store(nullAlias); expected = {};
    assert(!slot.compare_exchange_strong(expected, {}));
    assert(!expected && !expected.owner_before(nullAlias) && !nullAlias.owner_before(expected));
    slot.store(std::make_shared<int>(0));
    std::vector<std::thread> workers;
    for (int thread = 0; thread < 8; ++thread) workers.emplace_back([&] {
        for (int i = 0; i < 10000; ++i) {
            auto old = slot.load();
            while (!slot.compare_exchange_strong(old, std::make_shared<int>(*old + 1))) {}
        }
    });
    for (auto& worker : workers) worker.join();
    assert(*slot.load() == 80000);
    int deleted = 0;
    expected = std::shared_ptr<int>(new int(5), [&](int* p) { assert(slot.load()); ++deleted; delete p; });
    assert(!slot.compare_exchange_strong(expected, {}));
    assert(deleted == 1);
    puts("shared ownership, aliasing, failed-CAS deletion and 80000 concurrent updates passed");
}
