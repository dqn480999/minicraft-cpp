#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include "common/Asm.hpp"
#include "common/Crypto.hpp"
#include "common/Json.hpp"
#include "common/Uuid.hpp"
#include "lang/Compiler.hpp"
#include "lang/Disassembler.hpp"
#include "lang/Vm.hpp"
#include "protocol/Chat.hpp"
#include "protocol/ItemStack.hpp"
#include "net/ByteBuffer.hpp"
#include "world/Blocks.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

namespace {

int g_failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

void testAssembly() {
    const std::array<std::int32_t, 8> values = {0, 1, 127, 128, 255, 2147483647, -1, -2147483648};
    for (const std::int32_t value : values) {
        mc::u8 encoded[5] = {0, 0, 0, 0, 0};
        const int length = mc::varIntEncode(value, encoded);
        mc::i32 decoded = 0;
        const int consumed = mc::varIntDecode(encoded, static_cast<std::size_t>(length), decoded);
        expect(consumed == length && decoded == value, "assembly varint roundtrip");
    }

    std::array<mc::u8, 128> source{};
    std::array<mc::u8, 128> destination{};
    for (std::size_t i = 0; i < source.size(); ++i) {
        source[i] = static_cast<mc::u8>(i * 7 + 3);
    }
    mc::fastCopy(destination.data(), source.data(), source.size());
    expect(destination == source, "assembly fast copy");
    mc::fastZero(destination.data(), destination.size());
    expect(std::all_of(destination.begin(), destination.end(), [](mc::u8 byte) { return byte == 0; }), "assembly fast zero");
}

void testVarInt() {
    const std::vector<std::pair<std::int32_t, std::vector<mc::u8>>> cases = {
        {0, {0x00}},
        {1, {0x01}},
        {127, {0x7F}},
        {128, {0x80, 0x01}},
        {255, {0xFF, 0x01}},
        {2147483647, {0xFF, 0xFF, 0xFF, 0xFF, 0x07}},
        {-1, {0xFF, 0xFF, 0xFF, 0xFF, 0x0F}},
    };
    for (const auto& [value, encoded] : cases) {
        mc::ByteBuffer buffer;
        buffer.writeVarInt(value);
        expect(buffer.data() == encoded, "varint encoding " + std::to_string(value));
        const std::int32_t decoded = buffer.readVarInt();
        expect(decoded == value, "varint roundtrip " + std::to_string(value));
    }
}

void testPosition() {
    const std::array<std::array<std::int32_t, 3>, 4> cases = {{
        {{0, 0, 0}},
        {{8, 4, 8}},
        {{-1000, 255, 30000000}},
        {{-30000000, 0, -30000000}},
    }};
    for (const auto& point : cases) {
        mc::ByteBuffer buffer;
        buffer.writePosition(point[0], point[1], point[2]);
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::int32_t z = 0;
        buffer.readPosition(x, y, z);
        expect(x == point[0] && y == point[1] && z == point[2], "position roundtrip");
    }
}

void testUuid() {
    const mc::Uuid offline = mc::Uuid::offline("TestBot");
    expect(offline.toString() == "30fecbe1-2271-3418-8553-d3ded0e95f56", "offline uuid");
    const mc::Uuid parsed = mc::Uuid::fromString("30fecbe1-2271-3418-8553-d3ded0e95f56");
    expect(parsed == offline, "uuid parse");
}

void testHash() {
    const std::string hash = mc::minecraftServerHash("abc", {}, {});
    expect(hash == "-5666c1c9b8f97e9545c1da8e87af3d93632f2763", "minecraft server hash");
}

void testAes() {
    const std::array<mc::u8, 16> key = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    std::vector<mc::u8> data = {'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd'};
    const auto original = data;
    mc::AesCfb8 cipher(key);
    cipher.encrypt(data.data(), data.size());
    expect(data != original, "aes changed data");
    cipher.decrypt(data.data(), data.size());
    expect(data == original, "aes roundtrip");
}

void testRsa() {
    mc::RsaKey key;
    const mc::Bytes der = key.publicKeyDer();
    const unsigned char* cursor = der.data();
    EVP_PKEY* publicKey = d2i_PUBKEY(nullptr, &cursor, static_cast<long>(der.size()));
    expect(publicKey != nullptr, "rsa public key decode");
    if (publicKey == nullptr) {
        return;
    }

    const std::string message = "minicraft";
    EVP_PKEY_CTX* context = EVP_PKEY_CTX_new(publicKey, nullptr);
    std::size_t length = 0;
    bool ok = context != nullptr
        && EVP_PKEY_encrypt_init(context) == 1
        && EVP_PKEY_CTX_set_rsa_padding(context, RSA_PKCS1_PADDING) == 1
        && EVP_PKEY_encrypt(context, nullptr, &length,
                            reinterpret_cast<const unsigned char*>(message.data()), message.size()) == 1;
    mc::Bytes encrypted;
    if (ok) {
        encrypted.resize(length);
        ok = EVP_PKEY_encrypt(context, encrypted.data(), &length,
                              reinterpret_cast<const unsigned char*>(message.data()), message.size()) == 1;
        encrypted.resize(length);
    }
    if (context != nullptr) {
        EVP_PKEY_CTX_free(context);
    }
    EVP_PKEY_free(publicKey);
    expect(ok, "rsa encrypt");

    if (ok) {
        const mc::Bytes decrypted = key.decrypt(encrypted);
        expect(std::string(decrypted.begin(), decrypted.end()) == message, "rsa roundtrip");
    }
}

void testChunk() {
    mc::Chunk chunk(0, 0);
    const auto mesh = chunk.serialized();
    expect(mesh->mask == 0x1, "chunk mask");
    expect(mesh->data.size() == 12544, "chunk data size");

    const auto block = [&](std::size_t index) {
        return static_cast<mc::u16>(mesh->data[index] | (mesh->data[index + 1] << 8));
    };
    expect(block(0) == mc::kBedrock, "chunk bedrock");
    expect(block(3 * 256 * 2) == mc::kGrass, "chunk grass");
    expect(block(4 * 256 * 2) == mc::kAir, "chunk air");

    const std::size_t skyOffset = 8192 + 2048;
    expect((mesh->data[skyOffset] & 0x0F) == 0, "chunk sky light ground");
    expect((mesh->data[skyOffset + 4 * 256 / 2] & 0x0F) == 15, "chunk sky light air");
    expect(mesh->data[12288] == 1, "chunk biome");
}

void testItemStack() {
    mc::ByteBuffer buffer;
    mc::writeItemStack(buffer, mc::ItemStack{1, 64, 0});
    const mc::ItemStack stack = mc::readItemStack(buffer);
    expect(stack.id == 1 && stack.count == 64 && stack.damage == 0, "item stack roundtrip");
    expect(mc::maxStackSize(1) == 64, "block stack size");
    expect(mc::maxStackSize(274) == 1, "tool stack size");
    expect(mc::maxStackSize(323) == 16, "sign stack size");
    expect(mc::maxStackSize(325) == 16, "bucket stack size");
    expect(mc::maxStackSize(338) == 64, "sugar cane stack size");
    expect(mc::maxStackSize(283) == 1, "golden tool stack size");
    expect(mc::maxStackSize(355) == 1, "bed stack size");
    expect(mc::isValidItemId(50), "torch item id");
    expect(mc::isValidItemId(274), "pickaxe item id");
    expect(mc::isValidItemId(355), "bed item id");
    expect(mc::isValidItemId(404), "comparator item id");
    expect(!mc::isValidItemId(26), "block-only id is invalid item");
    expect(!mc::isValidItemId(64), "door block id is invalid item");
    const mc::ItemStack clamped = mc::sanitizeItemStack(mc::ItemStack{323, 64, 0});
    expect(clamped.count == 16, "sign stack clamp");

    mc::ByteBuffer withNbt;
    withNbt.writeI16(1);
    withNbt.writeI8(1);
    withNbt.writeI16(0);
    withNbt.writeU8(10);
    withNbt.writeU16(0);
    withNbt.writeU8(1);
    withNbt.writeU16(0);
    withNbt.writeU8(42);
    withNbt.writeU8(0);
    const mc::ItemStack nbtStack = mc::readItemStack(withNbt);
    expect(nbtStack.id == 1 && withNbt.remaining() == 0, "item stack nbt skip");
}

void testChatColors() {
    const mc::Json colored = mc::Json::parse(mc::chat::legacy("&aGreen &lbold"));
    expect(colored.contains("extra"), "chat legacy extra");
    const auto& parts = colored["extra"].asArray();
    expect(parts.size() == 2, "chat legacy parts");
    if (parts.size() == 2) {
        expect(parts[0]["color"].asString() == "green", "chat color green");
        expect(parts[1]["bold"].asBool(), "chat bold");
    }
    expect(mc::chat::plain("&cRed") == "Red", "chat plain");
    const mc::Json player = mc::Json::parse(mc::chat::playerMessage("Bob", "&bHi"));
    expect(player.contains("extra"), "player chat components");
}

void testBlockRoundtrip() {
    mc::Chunk chunk(0, 0);
    chunk.setBlock(1, 4, 1, mc::kStone);
    const mc::Bytes raw = chunk.rawBlocks();
    mc::Chunk restored(0, 0);
    restored.loadBlocks(raw);
    expect(restored.getBlock(1, 4, 1) == mc::kStone, "chunk raw roundtrip");
    expect(!restored.modified(), "restored chunk is clean");
}

void testTorchLight() {
    mc::Chunk chunk(0, 0);
    chunk.setBlock(5, 5, 5, mc::makeBlock(50, 5));
    const auto mesh = chunk.serialized();
    expect(mesh->mask == 0x1, "light chunk mask");
    const std::size_t blockLightOffset = 8192;
    const std::size_t index = 5 * 256 + 5 * 16 + 5;
    const mc::u8 torchNibble = (index & 1) == 0
        ? static_cast<mc::u8>(mesh->data[blockLightOffset + index / 2] & 0x0F)
        : static_cast<mc::u8>((mesh->data[blockLightOffset + index / 2] >> 4) & 0x0F);
    expect(torchNibble == 14, "torch light emission");
    const std::size_t neighbor = index + 1;
    const mc::u8 neighborNibble = (neighbor & 1) == 0
        ? static_cast<mc::u8>(mesh->data[blockLightOffset + neighbor / 2] & 0x0F)
        : static_cast<mc::u8>((mesh->data[blockLightOffset + neighbor / 2] >> 4) & 0x0F);
    expect(neighborNibble == 13, "torch light propagation");
}

void testTorchMigration() {
    mc::Bytes raw(16 * 16 * 256 * 2, 0);
    const std::size_t index = (4 * 256 + 0 * 16 + 0);
    raw[index * 2] = static_cast<mc::u8>((50 << 4) & 0xFF);
    raw[index * 2 + 1] = static_cast<mc::u8>(((50 << 4) >> 8) & 0xFF);
    mc::Chunk chunk(0, 0);
    chunk.loadBlocks(raw);
    expect(chunk.getBlock(0, 4, 0) == static_cast<mc::u16>((50 << 4) | 5), "torch meta migration");
    expect(chunk.modified(), "torch migration marks modified");
}

class ScriptProbe final : public mc::lang::ScriptHost {
public:
    void broadcastInteger(mc::lang::Word value) override {
        integers.push_back(value);
    }

    void broadcastText(const std::string& text) override {
        texts.push_back(text);
    }

    mc::lang::Word playerCount() override {
        return 7;
    }

    mc::lang::Word currentTimeMs() override {
        return 1000;
    }

    mc::lang::Word getBlock(mc::lang::Word, mc::lang::Word, mc::lang::Word) override {
        return 0;
    }

    void setBlock(mc::lang::Word, mc::lang::Word, mc::lang::Word, mc::lang::Word) override {
    }

    void logInteger(mc::lang::Word value) override {
        integers.push_back(value);
    }

    void traceLine(std::string_view) override {
    }

    std::vector<mc::lang::Word> integers;
    std::vector<std::string> texts;
};

void testLanguage() {
    const std::string source = R"OBL(
ARCANA "test" {
    ORACLE @main ~> {
        BIND($r0) ::= #6;
        BIND($r1) ::= #4;
        SCRIBE MEM[$r1 + #0] ::= $r0;
        REVEAL($r2) ::= MEM[$r1 + #0];
        BIND($a0) ::= $r2;
        SUMMON(#1);
        BIND($r0) ::= #0;
        BIND($r1) ::= #1;
        @loop::
        CALL @add;
        BIND($r1) ::= $r1 <+> #1;
        VERIFY($r1) <=> #11;
        WHEN::LESS => GATE @loop;
        BIND($a0) ::= $r0;
        SUMMON(#1);
        BIND($a0) ::= SIGIL("done");
        SUMMON(#2);
        HALT;
        @add::
        BIND($r0) ::= $r0 <+> $r1;
        RETURN;
    }

    ORACLE @persist ~> {
        BIND($r0) ::= #0;
        REVEAL($r1) ::= MEM[$r0 + #0];
        INCREMENT($r1);
        SCRIBE MEM[$r0 + #0] ::= $r1;
        BIND($a0) ::= $r1;
        SUMMON(#1);
        RETURN;
    }

    ORACLE @bytes ~> {
        BIND($r0) ::= #2;
        BIND($r3) ::= #0;
        BIND($r1) ::= #300;
        SCRIBE8 MEM8[$r0 + #0] ::= $r1;
        REVEAL8($r2) ::= MEM8[$r0 + $r3];
        BIND($a0) ::= $r2;
        SUMMON(#1);
        RETURN;
    }

    ORACLE @flags ~> {
        BIND($r0) ::= #9223372036854775807;
        INCREMENT($r0);
        WHEN::OVERFLOW => GATE @overflow;
        GATE @done;
        @overflow::
        BIND($a0) ::= #1;
        SUMMON(#1);
        @done::
        BIND($r1) ::= #5;
        BIND($r2) ::= #9;
        SWAP($r1, $r2);
        BIND($a0) ::= $r1;
        SUMMON(#1);
        NEGATE($r2);
        BIND($a0) ::= $r2;
        SUMMON(#1);
        COMPLEMENT($r2);
        BIND($a0) ::= $r2;
        SUMMON(#1);
        RETURN;
    }
}
)OBL";
    mc::lang::Program program;
    std::string error;
    expect(mc::lang::Compiler::compileInto(program, source, "inline.obl", error), "language compile " + error);
    for (const auto& patch : program.callPatches) {
        const auto target = program.functions.find(patch.name);
        expect(target != program.functions.end(), "language call target " + patch.name);
        if (target != program.functions.end()) {
            program.code[patch.instruction].imm = static_cast<mc::lang::Word>(target->second);
        }
    }
    expect(mc::lang::disassemble(program).find("MOV") != std::string::npos, "language disassembler");
    ScriptProbe host;
    std::vector<mc::lang::Word> memory(mc::lang::kMemoryCellCount, 0);
    mc::lang::VirtualMachine vm(program, host, memory);
    expect(vm.run("main", {}, {}, error), "language run " + error);
    expect(vm.run("persist", {}, {}, error), "language persist first " + error);
    expect(vm.run("persist", {}, {}, error), "language persist second " + error);
    expect(vm.run("bytes", {}, {}, error), "language byte memory " + error);
    expect(vm.run("flags", {}, {}, error), "language flags " + error);
    expect(host.integers.size() == 9, "language host count");
    if (host.integers.size() == 9) {
        expect(host.integers[0] == 6 && host.integers[1] == 55, "language arithmetic");
        expect(host.integers[2] == 1 && host.integers[3] == 2, "language persistent memory");
        expect(host.integers[4] == 44, "language byte memory");
        expect(host.integers[5] == 1, "language overflow flag");
        expect(host.integers[6] == 9 && host.integers[7] == -5, "language swap and negate");
        expect(host.integers[8] == 4, "language complement");
    }
    expect(host.texts.size() == 1 && host.texts[0] == "done", "language string table");
    expect(memory[0] == 2 && memory[2] == 44, "language memory state");
}

void testWorldPersistence() {
    const std::filesystem::path directory = "/tmp/minicraft_world_test";
    std::filesystem::remove_all(directory);
    {
        mc::World world(0, 1, 64, directory.string(), true, 1.0);
        world.setBlock(3, 5, 3, mc::kStone);
        world.setBlock(-2, 6, 4, mc::kDirt);
        world.save();
    }
    {
        mc::World world(0, 1, 64, directory.string(), true, 1.0);
        expect(world.getBlock(3, 5, 3) == mc::kStone, "world persistence positive x");
        expect(world.getBlock(-2, 6, 4) == mc::kDirt, "world persistence negative x");
    }
    std::filesystem::remove_all(directory);
}

}

int main() {
    testVarInt();
    testAssembly();
    testPosition();
    testUuid();
    testHash();
    testAes();
    testRsa();
    testChunk();
    testItemStack();
    testTorchLight();
    testTorchMigration();
    testChatColors();
    testBlockRoundtrip();
    testWorldPersistence();
    testLanguage();

    if (g_failures == 0) {
        std::cout << "core_test PASS\n";
        return 0;
    }
    std::cerr << "core_test FAIL " << g_failures << '\n';
    return 1;
}
