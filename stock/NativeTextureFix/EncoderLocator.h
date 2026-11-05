#pragma once
#include <cstddef>
#include <cstring>
namespace EncoderLocator
{
static const unsigned char Signature[] = {0x85, 0xd2, 0x0f, 0x8e, 0xc2, 0x02, 0x00, 0x00, 0x48, 0x8b, 0xc4};
struct Match
{
    std::size_t offset = 0;
    unsigned count = 0;
};
inline Match Find(const unsigned char *bytes, std::size_t length, const unsigned char *signature = Signature,
                  std::size_t size = sizeof(Signature))
{
    Match result;
    if (!bytes || !signature || !size || size > length)
        return result;
    for (std::size_t i = 0; i <= length - size; ++i)
        if (bytes[i] == signature[0] && !std::memcmp(bytes + i, signature, size))
        {
            result.offset = i;
            if (++result.count > 1)
                break;
        }
    return result;
}
}
