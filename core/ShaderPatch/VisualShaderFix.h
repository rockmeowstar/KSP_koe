// SPDX-License-Identifier: MIT
#pragma once
#include "ShaderPatch.h"
#include "DxilHash.h"
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace KoeVisualFix {
struct Anchor {
    uint32_t offset;
    std::array<unsigned char, 32> tokens;
};
struct Program {
    std::vector<Anchor> anchors;
    std::vector<unsigned char> recipe;
};
static std::array<std::vector<Program>, 2> programs;
static std::atomic<bool> ready{};
static std::atomic<unsigned long long> replacements[2]{}, rejected[2]{};
static std::mutex mutex;

static uint32_t Word(const unsigned char *bytes) {
    uint32_t value;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

static bool Read(const wchar_t *path, std::vector<Program> &entries) {
    if (!path || !*path) return true;
    std::ifstream file(std::filesystem::path(path), std::ios::binary | std::ios::ate);
    if (!file) return false;
    auto length = file.tellg();
    if (length < 12 || length > 1024 * 1024) return false;
    std::vector<unsigned char> bank(static_cast<size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char *>(bank.data()), length) ||
        std::memcmp(bank.data(), "KOEVIS02", 8)) return false;
    const uint32_t count = Word(bank.data() + 8);
    size_t cursor = 12;
    if (!count || count > 64) return false;
    for (uint32_t i = 0; i < count; ++i) {
        if (bank.size() - cursor < 8) return false;
        const uint32_t anchors = Word(bank.data() + cursor);
        const uint32_t size = Word(bank.data() + cursor + 4);
        cursor += 8;
        if (anchors < 2 || anchors > 32 || anchors * 36 > bank.size() - cursor) return false;
        Program program;
        for (uint32_t j = 0; j < anchors; ++j) {
            Anchor anchor{};
            anchor.offset = Word(bank.data() + cursor);
            std::memcpy(anchor.tokens.data(), bank.data() + cursor + 4, anchor.tokens.size());
            program.anchors.push_back(anchor);
            cursor += 36;
        }
        if (size < 88 || size > bank.size() - cursor ||
            std::memcmp(bank.data() + cursor, "KOEPATCH", 8)) return false;
        program.recipe.assign(bank.data() + cursor, bank.data() + cursor + size);
        entries.push_back(std::move(program));
        cursor += size;
    }
    return cursor == bank.size();
}

static int Configure(const wchar_t *wet, const wchar_t *horizon) {
    std::lock_guard<std::mutex> lock(mutex);
    if (ready.load()) return -1;
    try {
        decltype(programs) parsed;
        if (!Read(wet, parsed[0]) || !Read(horizon, parsed[1])) return -2;
        programs = std::move(parsed);
        ready.store(true, std::memory_order_release);
        return 0;
    } catch (...) { return -3; }
}

static bool ApplyBank(const decltype(programs) &bank, const void *source, size_t length,
                      std::vector<unsigned char> &replacement, unsigned &selected,
                      std::atomic<unsigned long long> *rejections = nullptr) {
    if (!source || length < 36 || length > 1024 * 1024 || (bank[0].empty() && bank[1].empty())) return false;
    const auto *input = static_cast<const unsigned char *>(source);
    if (std::memcmp(input, "DXBC", 4) || Word(input + 24) != length) return false;
    const uint32_t chunks = Word(input + 28);
    if (!chunks || chunks > (length - 32) / 4) return false;
    const size_t headerLength = 32 + 4 * chunks;
    const uint32_t offset = Word(input + headerLength - 4);
    if (offset < headerLength || offset > length - 16) return false;
    if (std::memcmp(input + offset, "SHDR", 4) && std::memcmp(input + offset, "SHEX", 4)) return false;
    const uint32_t size = Word(input + offset + 4);
    if (size != length - offset - 8) return false;
    const auto *code = input + offset + 8;
    if ((Word(code) >> 16) != 0 || Word(code + 4) != size / 4) return false;
    for (unsigned i = 0; i < bank.size(); ++i) {
        for (const auto &program : bank[i]) {
            if (Word(program.recipe.data() + 12) != size) continue;
            bool matches = true;
            for (const auto &anchor : program.anchors) {
                if (anchor.offset > size || anchor.tokens.size() > size - anchor.offset ||
                    std::memcmp(code + anchor.offset, anchor.tokens.data(), anchor.tokens.size())) {
                    matches = false;
                    break;
                }
            }
            if (!matches) continue;
            std::vector<unsigned char> patched;
            if (!KoeShaderPatch::Apply(program.recipe, code, size, patched)) {
                if (rejections) rejections[i]++;
                continue;
            }
            replacement.assign(input, code);
            replacement.insert(replacement.end(), patched.begin(), patched.end());
            const auto shaderSize = static_cast<uint32_t>(patched.size());
            const auto containerSize = static_cast<uint32_t>(replacement.size());
            std::memcpy(replacement.data() + offset + 4, &shaderSize, 4);
            std::memcpy(replacement.data() + 24, &containerSize, 4);
            ComputeHashRetail(replacement.data() + 20, containerSize - 20, replacement.data() + 4);
            selected = i;
            return true;
        }
    }
    return false;
}

static bool Resolve(const void *source, size_t length, std::vector<unsigned char> &replacement,
                    unsigned &selected) {
    return ready.load(std::memory_order_acquire) && ApplyBank(programs, source, length, replacement, selected, rejected);
}

static void Created(unsigned selected, bool success) {
    if (success) replacements[selected]++; else rejected[selected]++;
}
}
