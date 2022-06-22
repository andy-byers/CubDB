/*
 * Fuzz target that exercises in-memory database operations.
 */
#include "fuzzers.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    using Fuzzer = calico::fuzz::InMemoryOpsFuzzer;
    Fuzzer fuzzer {Fuzzer::Transformer {}};

    try {
        fuzzer(data, size);
    } catch (const std::invalid_argument&) {

    }
    return 0;
}
