#include "fuzzers.h"
#include "validators.h"

using namespace cub;

//auto f(const uint8_t *data, Size size)
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, Size size)
{
    fuzz::OperationFuzzer fuzzer;
    fuzzer.fuzzer_action(data, size);
    fuzzer.fuzzer_validation();
    return 0;
}
//
//int main()
//{
//    const auto raw =
//        "\x00\x02\x00\x00\x00\x00\x00\x00\x00\x00\x00\x04\x00\x00\x00\x00"
//        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
//        "\x00\x00\x00\x00\x3a\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
//        "\xdf\x00\x3b\x02\x00\x02\x00\x00\x00\x00\xff\x00\x00\x01\x00\xff"
//        "\x01\x01\x01\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
//        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
//        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
//        "\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00\x00\x00\x09\x09\x09\x09"
//        "\x09\x09\x09\x09\x09\x09\x09\x09\x09\x25\x09\x09\x09\x09\x09\x09"
//        "\x09\x09\x09\x09\x09\x09\x09\x09\x09\x09\x09\x09\x09\x09\xf6\xf6"
//        "\xf6\xf6\xf6\xf6\x09\x09\x09\x09\x09\x09\x09\x09\x09\x09\x09\x09"
//        "\x35\x09\x35\x35\x35\x35\x35\x35\x09\x35\x09\x09\x09\x09\x13\x13"
//        "\x13\x13\x13\x13\x13\x13\x13\x13\x13\x13\x13\x13\x13\x13\x09\x13"
//        "\xf7\x09\xf6\xf6\xf6\xf6\xf6\xf6\x09\xf6\x09\x09\x09\x09\x09\x09"
//        "\x09\xd4\x09\x09\x09\x09\x75\x09\x35\x35\xcc\x35\x35\x35\x35\x35"
//        "\x35\x35\x02\x02\x02\x02\x02\x00\x02\x02\xcc\x00\x00\x00\x7a\x00"
//        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xf9\x00\x00\x03\x00\x00"
//        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x06\x00\x58\x02\x00\x00"
//        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x0e\x00"
//        "\xc0\x06\xff\x04\x00\x01\x5e\x00\x00\xf8\x00\x00\x00\x00\x00\x00"
//        "\x00\x00\x00\x00\x02\x02\x00\x00\x00\x00\x00\x00\x06\x0e\x04\xc0"
//        "\x00\x00\x00\x00\x00\x00\x00\x00\xdf\x02\x00\x02\x00\x00\x28\x10"
//        "\x01\xf3\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x08\x00\x00\x00"
//        "\x00\x00\x00\x00\x00\x01\x00\x00\x3b\x3b\x00\x01\x00\x00\x80\x00"
//        "\x02\x00\x06\x8e\x02\x3b\xd7\xdf\x00\x01\x01\x01\x00\x01\x01\x01"
//        "\x01\x01\x01\x01\x01\x01\xdf\x00\x00\x00\x01\x00\x01\x00\x01\x01"
//        "\x01\x01\x01\x06\xdf\x00\x00\x02\x00\x00\x00\x35\x00\x00\x35\x35"
//        "\x09\x09\x09\x09\x09\x09\x10\x09\x00\x00\x00\x00\x00\x00\x00\x00"
//        "\x00\x00\x00\x00\x00\x00\x06\x00\x00\x00\x00\xd7\x00\x00\x00\x00"
//        "\x00\x00\x08\x00\x00\x00\x00\x00\x00\x00\x01\x01\x02\x00\x00\x00"
//        "\x00\x00\x00\x00\x00\x00\x04\x00\x00\x00\x00\x00\x04\x00\x00\x00"
//        "\x00\x00\x7f\x00\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01"
//        "\x06\x01\x00\x01\x00\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01"
//        "\x01\x01\x01\x01\x01\x01\x06\x01\x00\x01\x01\x00\x00\x01\xe0\x00"
//        "\x00\x01\x00\x01\x00\x0a\x00\x00\x09\x00\x09\x09\x09\x09\x09\x09"
//        "\x09\x09\x09\x25\x09\x09\x09\x09\x09\x09\x09\x09\x09\x09\x09\x09"
//        "\x00\x09\x00\x00\x00\x00\x06\x00\x00\x00\x00\x00\x00\x00\x06\x00"
//        "\x00\x00\x00\x00\x00\x00\x00\x00\x04\x00\x00\x00\x00\x00\x00\x00"
//        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x06\x00"
//        "\x00\x02\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01\xff\x00\x00"
//        "\xf8\x00\x00\x00\x2e\x00\x00\x00\x00\x00\x00\xf8\x00\x00\x00\x2e"
//        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x7f\x00\x00\x01"
//        "\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01"
//        "\x00\x00\x00\x00\x00\x00\x00\x00\x01\x01\x00\x01\x00\x00\x00\x00"
//        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
//        "\x00\x00\x00\x00\x00\x00\x00\x00\xff\x00\x00\x01\x00\x00\x00\x00"
//        "\xff\x00\x00\x01\x00\x00\x00\x00\x01\x00\xc5\x01\x01\x00\x00\xc5"
//        "\x35\x02\x01\x83\x00\x00\x00\x00\x00\x00\xff\x00\x00\x01\x00\x00"
//        "\x00\x00\x00\x00\x00\x2e\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00"
//        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xc6\xc5\x00\x00\x00\x00"
//        "\x00\x00\x00\x06\x00\x00\x00\x00\xc0\x00\x00\x00\x00\x00\x00\x01"
//        "\x00\x00\x00\x06\x00\xc0\x86\x00\xff\xf7\x01\x01\x00\x00\x00\x00"
//        "\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
//        "\x00\x00";
//    std::string data {raw, 850};
//    f(reinterpret_cast<const uint8_t*>(data.data()), data.size());
//    return 0;
//}