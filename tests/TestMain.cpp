#include "emugbds/core/Types.h"
#include "emugbds/cores/InHouseDsCoreAdapter.h"
#include "emugbds/cores/InHouseGbaCoreAdapter.h"
#include "emugbds/input/GbaKeys.h"
#include "emugbds/inds/Core.h"
#include "emugbds/inds/MemoryBus.h"
#include "emugbds/ingba/Core.h"
#include "emugbds/ingba/MemoryBus.h"
#include "emugbds/runtime/RuntimeKernel.h"
#include "emugbds/runtime/Types.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr uint32_t kGbaRomBase = 0x08000000u;
constexpr uint32_t kDsArm9BiosBase = 0xFFFF0000u;
constexpr uint32_t kDsMainRamBase = 0x02000000u;
constexpr uint32_t kDsSharedWramBase = 0x03000000u;
constexpr uint32_t kDsSlot2Base = 0x08000000u;
constexpr std::size_t kGbaBiosSize = 0x4000u;
constexpr std::size_t kGbaHeaderSize = 0xC0u;
constexpr std::size_t kNdsHeaderReadSize = 0x200u;

struct TestContext {
    int failures = 0;

    void Expect(bool condition, std::string_view message) {
        if (!condition) {
            ++failures;
            std::cerr << "  FAIL: " << message << "\n";
        }
    }
};

void WriteFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void WriteLe32(std::vector<uint8_t>& bytes, std::size_t offset, uint32_t value) {
    bytes[offset + 0] = static_cast<uint8_t>(value & 0xFFu);
    bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    bytes[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    bytes[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

void WriteLe16(std::vector<uint8_t>& bytes, std::size_t offset, uint16_t value) {
    bytes[offset + 0] = static_cast<uint8_t>(value & 0xFFu);
    bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}

uint8_t ComputeGbaComplement(const std::vector<uint8_t>& rom) {
    unsigned sum = 0;
    for (std::size_t i = 0xA0; i <= 0xBC; ++i) {
        sum = (sum + rom[i]) & 0xFFu;
    }
    return static_cast<uint8_t>((0xE7u - sum) & 0xFFu);
}

std::vector<uint8_t> BuildSyntheticGbaRom() {
    std::vector<uint8_t> rom(0x10000u, 0);
    for (std::size_t i = 0; i + 3 < rom.size(); i += 4) {
        rom[i + 0] = 0x00;
        rom[i + 1] = 0x00;
        rom[i + 2] = 0xA0;
        rom[i + 3] = 0xE1;
    }

    const std::string title = "TESTGBADEMO ";
    const std::string game_code = "EGBD";
    const std::string maker = "01";
    std::copy(title.begin(), title.end(), rom.begin() + 0xA0);
    std::copy(game_code.begin(), game_code.end(), rom.begin() + 0xAC);
    std::copy(maker.begin(), maker.end(), rom.begin() + 0xB0);
    rom[0xB2] = 0x96;
    rom[0xB3] = 0x00;
    rom[0xB4] = 0x00;
    rom[0xBC] = 0x00;
    rom[0xBD] = ComputeGbaComplement(rom);
    return rom;
}

std::vector<uint8_t> BuildSyntheticNdsRom() {
    std::vector<uint8_t> rom(0x400u, 0);

    const std::string title = "EMUGBDS NDS ";
    const std::string game_code = "NGBD";
    const std::string maker = "01";
    std::copy(title.begin(), title.end(), rom.begin() + 0x00);
    std::copy(game_code.begin(), game_code.end(), rom.begin() + 0x0C);
    std::copy(maker.begin(), maker.end(), rom.begin() + 0x10);

    rom[0x12] = 0x00;
    rom[0x13] = 0x00;
    rom[0x14] = 0x07;
    rom[0x1E] = 0x00;
    rom[0x1F] = 0x00;

    WriteLe32(rom, 0x20, 0x00000200u);
    WriteLe32(rom, 0x24, 0x02000000u);
    WriteLe32(rom, 0x28, 0x02000000u);
    WriteLe32(rom, 0x2C, 0x00000004u);

    WriteLe32(rom, 0x30, 0x00000204u);
    WriteLe32(rom, 0x34, 0x02380000u);
    WriteLe32(rom, 0x38, 0x02380000u);
    WriteLe32(rom, 0x3C, 0x00000004u);

    rom[0x200] = 0x00;
    rom[0x201] = 0x00;
    rom[0x202] = 0xA0;
    rom[0x203] = 0xE1;
    rom[0x204] = 0x00;
    rom[0x205] = 0x00;
    rom[0x206] = 0xA0;
    rom[0x207] = 0xE1;
    return rom;
}

std::vector<uint8_t> BuildSyntheticGbaBios() {
    std::vector<uint8_t> bios(kGbaBiosSize, 0);
    for (std::size_t i = 0; i < bios.size(); ++i) {
        bios[i] = static_cast<uint8_t>(i & 0xFFu);
    }
    return bios;
}

void WriteWordLe(std::vector<uint8_t>& bytes, std::size_t offset, uint32_t value) {
    WriteLe32(bytes, offset, value);
}

uint32_t EncodeMovImm(uint32_t rd, uint32_t imm8) {
    return 0xE3A00000u | ((rd & 0xFu) << 12u) | (imm8 & 0xFFu);
}

uint32_t EncodeMul(uint32_t rd, uint32_t rm, uint32_t rs, bool accumulate = false, uint32_t rn = 0, bool set_flags = false) {
    return 0xE0000090u |
        (accumulate ? (1u << 21) : 0u) |
        (set_flags ? (1u << 20) : 0u) |
        ((rd & 0xFu) << 16u) |
        ((rn & 0xFu) << 12u) |
        ((rs & 0xFu) << 8u) |
        (rm & 0xFu);
}

uint32_t EncodeLdrLiteral(uint32_t rd, uint32_t imm12) {
    return 0xE59F0000u | ((rd & 0xFu) << 12u) | (imm12 & 0xFFFu);
}

uint32_t EncodeLdrLiteralAt(uint32_t rd, uint32_t instruction_address, uint32_t literal_address) {
    return EncodeLdrLiteral(rd, literal_address - (instruction_address + 8u));
}

uint32_t EncodeMsrCpsrControl(uint32_t rm) {
    return 0xE121F000u | (rm & 0xFu);
}

uint32_t EncodeSubImmSetFlags(uint32_t rd, uint32_t rn, uint32_t imm12) {
    return 0xE2500000u |
        ((rn & 0xFu) << 16u) |
        ((rd & 0xFu) << 12u) |
        (imm12 & 0xFFFu);
}

uint32_t EncodeLdrReg(uint32_t rd, uint32_t rn, uint32_t imm12) {
    return 0xE5900000u | ((rn & 0xFu) << 16u) | ((rd & 0xFu) << 12u) | (imm12 & 0xFFFu);
}

uint32_t EncodeStrReg(uint32_t rd, uint32_t rn, uint32_t imm12) {
    return 0xE5800000u | ((rn & 0xFu) << 16u) | ((rd & 0xFu) << 12u) | (imm12 & 0xFFFu);
}

uint32_t EncodeSubImm(uint32_t rd, uint32_t rn, uint32_t imm12) {
    return 0xE2400000u | ((rn & 0xFu) << 16u) | ((rd & 0xFu) << 12u) | (imm12 & 0xFFFu);
}

uint32_t EncodeStrhImm(uint32_t rd, uint32_t rn, uint32_t offset) {
    return 0xE1C000B0u |
        ((rn & 0xFu) << 16u) |
        ((rd & 0xFu) << 12u) |
        (((offset >> 4u) & 0xFu) << 8u) |
        (offset & 0xFu);
}

uint32_t EncodeLdrhImm(uint32_t rd, uint32_t rn, uint32_t offset) {
    return 0xE1D000B0u |
        ((rn & 0xFu) << 16u) |
        ((rd & 0xFu) << 12u) |
        (((offset >> 4u) & 0xFu) << 8u) |
        (offset & 0xFu);
}

uint32_t EncodeLdrsbImm(uint32_t rd, uint32_t rn, uint32_t offset) {
    return 0xE1D000D0u |
        ((rn & 0xFu) << 16u) |
        ((rd & 0xFu) << 12u) |
        (((offset >> 4u) & 0xFu) << 8u) |
        (offset & 0xFu);
}

uint32_t EncodeLdrshImm(uint32_t rd, uint32_t rn, uint32_t offset) {
    return 0xE1D000F0u |
        ((rn & 0xFu) << 16u) |
        ((rd & 0xFu) << 12u) |
        (((offset >> 4u) & 0xFu) << 8u) |
        (offset & 0xFu);
}

uint32_t EncodeStmiaWriteback(uint32_t rn, uint32_t reg_list) {
    return 0xE8A00000u | ((rn & 0xFu) << 16u) | (reg_list & 0xFFFFu);
}

uint32_t EncodeLdmiaWriteback(uint32_t rn, uint32_t reg_list) {
    return 0xE8B00000u | ((rn & 0xFu) << 16u) | (reg_list & 0xFFFFu);
}

uint32_t EncodeStmiaNoWriteback(uint32_t rn, uint32_t reg_list) {
    return 0xE8800000u | ((rn & 0xFu) << 16u) | (reg_list & 0xFFFFu);
}

uint32_t EncodeStmiaUser(uint32_t rn, uint32_t reg_list) {
    return 0xE8C00000u | ((rn & 0xFu) << 16u) | (reg_list & 0xFFFFu);
}

uint32_t EncodeLdmiaNoWriteback(uint32_t rn, uint32_t reg_list) {
    return 0xE8900000u | ((rn & 0xFu) << 16u) | (reg_list & 0xFFFFu);
}

uint32_t EncodeLdmiaUser(uint32_t rn, uint32_t reg_list) {
    return 0xE8D00000u | ((rn & 0xFu) << 16u) | (reg_list & 0xFFFFu);
}

uint32_t EncodeSwi(uint8_t imm8) {
    return 0xEF000000u | static_cast<uint32_t>(imm8);
}

uint32_t EncodeBranchRelativeWords(int32_t relative_words) {
    return 0xEA000000u | (static_cast<uint32_t>(relative_words) & 0x00FFFFFFu);
}

uint32_t EncodeArmBx(uint32_t rn) {
    return 0xE12FFF10u | (rn & 0xFu);
}

uint16_t EncodeThumbMovImm(uint32_t rd, uint32_t imm8) {
    return static_cast<uint16_t>(0x2000u | ((rd & 0x7u) << 8u) | (imm8 & 0xFFu));
}

uint16_t EncodeThumbSwi(uint8_t imm8) {
    return static_cast<uint16_t>(0xDF00u | imm8);
}

uint16_t EncodeThumbAddReg(uint32_t rd, uint32_t rs, uint32_t rn) {
    return static_cast<uint16_t>(0x1800u | ((rn & 0x7u) << 6u) | ((rs & 0x7u) << 3u) | (rd & 0x7u));
}

uint16_t EncodeThumbSubImm(uint32_t rd, uint32_t imm8) {
    return static_cast<uint16_t>(0x3800u | ((rd & 0x7u) << 8u) | (imm8 & 0xFFu));
}

uint16_t EncodeThumbAddImm(uint32_t rd, uint32_t imm8) {
    return static_cast<uint16_t>(0x3000u | ((rd & 0x7u) << 8u) | (imm8 & 0xFFu));
}

uint16_t EncodeThumbCmpImm(uint32_t rd, uint32_t imm8) {
    return static_cast<uint16_t>(0x2800u | ((rd & 0x7u) << 8u) | (imm8 & 0xFFu));
}

uint16_t EncodeThumbLdrLiteral(uint32_t rd, uint32_t imm8) {
    return static_cast<uint16_t>(0x4800u | ((rd & 0x7u) << 8u) | (imm8 & 0xFFu));
}

uint16_t EncodeThumbStrbImm(uint32_t rd, uint32_t rn, uint32_t imm5) {
    return static_cast<uint16_t>(0x7000u | ((imm5 & 0x1Fu) << 6u) | ((rn & 0x7u) << 3u) | (rd & 0x7u));
}

uint16_t EncodeThumbLdrbImm(uint32_t rd, uint32_t rn, uint32_t imm5) {
    return static_cast<uint16_t>(0x7800u | ((imm5 & 0x1Fu) << 6u) | ((rn & 0x7u) << 3u) | (rd & 0x7u));
}

uint16_t EncodeThumbBRelative(int32_t relative_words) {
    return static_cast<uint16_t>(0xE000u | (static_cast<uint32_t>(relative_words) & 0x07FFu));
}

uint16_t EncodeThumbBxReg(uint32_t rs) {
    return static_cast<uint16_t>(0x4700u | ((rs & 0xFu) << 3u));
}

uint16_t EncodeThumbBlPrefix(uint32_t imm11) {
    return static_cast<uint16_t>(0xF000u | (imm11 & 0x07FFu));
}

uint16_t EncodeThumbBlSuffix(uint32_t imm11) {
    return static_cast<uint16_t>(0xF800u | (imm11 & 0x07FFu));
}

std::vector<uint8_t> BuildProgramGbaRom(const std::vector<uint32_t>& words) {
    std::vector<uint8_t> rom = BuildSyntheticGbaRom();
    for (std::size_t i = 0; i < words.size(); ++i) {
        WriteWordLe(rom, i * 4u, words[i]);
    }
    return rom;
}

std::vector<uint8_t> BuildBytes(std::size_t size, uint8_t seed) {
    std::vector<uint8_t> bytes(size, 0);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<uint8_t>(seed + static_cast<uint8_t>(i & 0x3Fu));
    }
    return bytes;
}

std::filesystem::path MakeTempRoot() {
    const auto root = std::filesystem::temp_directory_path() / "emugbds-tests";
    std::filesystem::create_directories(root);
    return root;
}

void EmitFixtures(const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir);
    WriteFile(dir / "test.gba", BuildSyntheticGbaRom());
    WriteFile(dir / "test.nds", BuildSyntheticNdsRom());
    WriteFile(dir / "gba_bios.bin", BuildSyntheticGbaBios());
    WriteFile(dir / "nds7.bin", BuildBytes(0x100u, 0x70));
    WriteFile(dir / "nds9.bin", BuildBytes(0x1000u, 0x90));
    WriteFile(dir / "firmware.bin", BuildBytes(0x20000u, 0xF0));
}

void TestIngbaMemoryBus(TestContext& t) {
    const auto dir = MakeTempRoot() / "ingba-memory-bus";
    std::filesystem::create_directories(dir);

    const auto invalid_bios = dir / "invalid_gba_bios.bin";
    WriteFile(invalid_bios, {0x01});

    emugbds::ingba::MemoryBus bus;
    std::string error;
    t.Expect(!bus.LoadBios(invalid_bios, error), "ingba bus rejects non-16KiB BIOS");

    const auto gba_bios = dir / "gba_bios.bin";
    const auto bios_bytes = BuildSyntheticGbaBios();
    WriteFile(gba_bios, bios_bytes);
    error.clear();
    t.Expect(bus.LoadBios(gba_bios, error), "ingba bus loads 16KiB BIOS");
    t.Expect(bus.HasBios(), "ingba bus reports BIOS loaded");
    t.Expect(bus.Read8(0x00000010u) == bios_bytes[0x10], "ingba BIOS read returns expected value");

    bus.Write8(0x02000020u, 0xAA);
    t.Expect(bus.Read8(0x02000020u) == 0xAA, "ingba EWRAM read/write works");
    bus.Write8(0x03000008u, 0x55);
    t.Expect(bus.Read8(0x03000008u) == 0x55, "ingba IWRAM read/write works");
    t.Expect(bus.Read8(0x03FFFFF8u) == 0x00u, "ingba IWRAM mirror initially matches base storage");
    bus.Write32(0x03007FFCu, 0x08000101u);
    t.Expect(bus.Read32(0x03FFFFFCu) == 0x08000101u, "ingba IWRAM callback mirror reads the base callback slot");
    bus.Write8(0x03FFFFF8u, 0xA5u);
    t.Expect(bus.Read8(0x03007FF8u) == 0xA5u, "ingba IWRAM mirror writes the base BIOS flag slot");

    const auto gba_rom = dir / "test.gba";
    const auto rom_bytes = BuildSyntheticGbaRom();
    WriteFile(gba_rom, rom_bytes);
    error.clear();
    t.Expect(bus.LoadRom(gba_rom, error), "ingba bus loads ROM");
    t.Expect(bus.Read8(kGbaRomBase) == rom_bytes[0], "ingba ROM read at base works");
    t.Expect(bus.Read8(kGbaRomBase + static_cast<uint32_t>(rom_bytes.size()) + 5u) == rom_bytes[5], "ingba ROM wrap read works");

    t.Expect(bus.Read8(0x0E000000u) == 0xFFu, "ingba save window starts erased and does not alias ROM");
    bus.Write8(0x0E005555u, 0xAAu);
    bus.Write8(0x0E002AAAu, 0x55u);
    bus.Write8(0x0E005555u, 0x90u);
    t.Expect(bus.Read8(0x0E000000u) == 0xC2u, "ingba flash exposes manufacturer ID mode");
    t.Expect(bus.Read8(0x0E000001u) == 0x09u, "ingba flash exposes device ID mode");
    bus.Write8(0x0E000000u, 0xF0u);
    bus.Write8(0x0E005555u, 0xAAu);
    bus.Write8(0x0E002AAAu, 0x55u);
    bus.Write8(0x0E005555u, 0xA0u);
    bus.Write8(0x0E000123u, 0x5Au);
    t.Expect(bus.Read8(0x0E000123u) == 0x5Au, "ingba flash programs a byte");
    bus.Write8(0x0E005555u, 0xAAu);
    bus.Write8(0x0E002AAAu, 0x55u);
    bus.Write8(0x0E005555u, 0xB0u);
    bus.Write8(0x0E000000u, 0x01u);
    bus.Write8(0x0E005555u, 0xAAu);
    bus.Write8(0x0E002AAAu, 0x55u);
    bus.Write8(0x0E005555u, 0xA0u);
    bus.Write8(0x0E000123u, 0xA5u);
    t.Expect(bus.Read8(0x0E000123u) == 0xA5u, "ingba flash bank switch selects the second bank");
    bus.Write8(0x0E005555u, 0xAAu);
    bus.Write8(0x0E002AAAu, 0x55u);
    bus.Write8(0x0E005555u, 0xB0u);
    bus.Write8(0x0E000000u, 0x00u);
    t.Expect(bus.Read8(0x0E000123u) == 0x5Au, "ingba flash bank switch restores the first bank");
    bus.Write8(0x0E005555u, 0xAAu);
    bus.Write8(0x0E002AAAu, 0x55u);
    bus.Write8(0x0E005555u, 0x80u);
    bus.Write8(0x0E005555u, 0xAAu);
    bus.Write8(0x0E002AAAu, 0x55u);
    bus.Write8(0x0E000123u, 0x30u);
    t.Expect(bus.Read8(0x0E000123u) == 0xFFu, "ingba flash sector erase restores erased bytes");

    bus.Write16(0x04000000u, 0x0403u);
    t.Expect(bus.Read16(0x04000000u) == 0x0403u, "ingba IO register write/read works");
    bus.Write16(0x04000100u, 0xFFFEu);
    bus.Write16(0x04000102u, 0x00C0u);
    bus.AdvanceTimers(1u);
    t.Expect(bus.Read16(0x04000100u) == 0xFFFFu, "ingba timer advances at its configured rate");
    bus.AdvanceTimers(1u);
    t.Expect(bus.Read16(0x04000100u) == 0xFFFEu, "ingba timer reloads after overflow");
    t.Expect((bus.Read16(0x04000202u) & 0x0008u) != 0u, "ingba timer overflow requests its IRQ");
    bus.Write8(0x04000301u, 0x00u);
    t.Expect(bus.ConsumeHaltRequest(), "ingba HALTCNT write requests a CPU halt");
    t.Expect(!bus.ConsumeHaltRequest(), "ingba HALTCNT request is consumed once");
    bus.Write16(0x05000002u, 0x7C00u);
    t.Expect(bus.Read16(0x05000002u) == 0x7C00u, "ingba palette halfword write preserves distinct bytes");
    bus.Write8(0x05000003u, 0x5Au);
    t.Expect(bus.Read16(0x05000002u) == 0x5A5Au, "ingba palette byte write duplicates across its halfword");

    bus.Write16(0x04000000u, 0x0000u); // Tile mode: OBJ VRAM begins at 0x06010000.
    bus.Write16(0x06000000u, 0x03E0u);
    t.Expect(bus.Read16(0x06000000u) == 0x03E0u, "ingba VRAM halfword write preserves distinct bytes");
    bus.Write8(0x06000000u, 0x11u);
    bus.Write8(0x06008000u, 0x33u);
    bus.Write16(0x06010000u, 0x2222u);
    bus.Write8(0x06010001u, 0x44u);
    t.Expect(bus.Read16(0x06000000u) == 0x1111u, "ingba BG VRAM byte write duplicates across its halfword");
    t.Expect(bus.Read16(0x06008000u) == 0x3333u, "ingba VRAM middle bank remains distinct");
    t.Expect(bus.Read16(0x06010000u) == 0x2222u, "ingba OBJ VRAM ignores byte writes in tile modes");
    t.Expect(bus.Read16(0x06018000u) == 0x2222u, "ingba VRAM upper mirror maps to the upper physical bank");

    bus.Write16(0x07000000u, 0x1234u);
    bus.Write8(0x07000001u, 0xAAu);
    t.Expect(bus.Read16(0x07000000u) == 0x1234u, "ingba OAM ignores byte writes");

    bus.Write32(0x02000000u, 0x12345678u);
    bus.Write32(0x040000D4u, 0x02000000u); // DMA3 source
    bus.Write32(0x040000D8u, 0x06000000u); // DMA3 destination
    bus.Write16(0x040000DCu, 1u);          // word count
    bus.Write16(0x040000DEu, 0xC400u);     // immediate + 32-bit + IRQ + enable
    t.Expect(bus.Read32(0x06000000u) == 0x12345678u, "ingba immediate DMA3 copies source word into VRAM");
    t.Expect((bus.Read16(0x04000202u) & 0x0800u) != 0u, "ingba DMA3 completion requests its IRQ");
    t.Expect((bus.Read16(0x040000DEu) & 0x8000u) == 0u, "ingba immediate DMA3 clears enable bit after transfer");
}

void TestIngbaCoreRunFrame(TestContext& t) {
    const auto dir = MakeTempRoot() / "ingba-core-run";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "test.gba";
    WriteFile(gba_rom, BuildSyntheticGbaRom());

    emugbds::ingba::Core core;
    std::string error;
    t.Expect(core.Initialize(error), "ingba core initializes");
    error.clear();
    t.Expect(!core.RunFrame(error), "ingba core rejects run without ROM");

    error.clear();
    t.Expect(core.LoadRom(gba_rom, error), "ingba core loads ROM");
    t.Expect(core.cpu().r[15] == kGbaRomBase, "ingba core reset PC points to ROM base without BIOS");
    const auto frames_before = core.frame_counter();
    error.clear();
    t.Expect(core.RunFrame(error), "ingba core advances one frame");
    t.Expect(core.frame_counter() == frames_before + 1, "ingba frame counter increments");
    t.Expect(core.IsRunning(), "ingba core remains running after smoke frame");
}

void TestIngbaCpuFiqBanking(TestContext& t) {
    const auto dir = MakeTempRoot() / "ingba-cpu-fiq-banking";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "fiq_banking.gba";

    constexpr uint32_t kSystemMode = 0x1Fu;
    constexpr uint32_t kFiqMode = 0x11u;
    constexpr uint32_t kLiteralBase = 0x60u;

    std::vector<uint8_t> rom = BuildSyntheticGbaRom();
    auto write_ldr = [&](uint32_t address, uint32_t rd, uint32_t literal_address) {
        WriteWordLe(rom, address, EncodeLdrLiteralAt(rd, address, literal_address));
    };

    write_ldr(0x00u, 0u, kLiteralBase + 0x00u);
    WriteWordLe(rom, 0x04u, EncodeMsrCpsrControl(0u));
    write_ldr(0x08u, 8u, kLiteralBase + 0x04u);
    write_ldr(0x0Cu, 9u, kLiteralBase + 0x08u);
    write_ldr(0x10u, 10u, kLiteralBase + 0x0Cu);
    write_ldr(0x14u, 11u, kLiteralBase + 0x10u);
    write_ldr(0x18u, 12u, kLiteralBase + 0x14u);
    write_ldr(0x1Cu, 13u, kLiteralBase + 0x18u);
    write_ldr(0x20u, 14u, kLiteralBase + 0x1Cu);
    write_ldr(0x24u, 0u, kLiteralBase + 0x20u);
    WriteWordLe(rom, 0x28u, EncodeMsrCpsrControl(0u));
    write_ldr(0x2Cu, 8u, kLiteralBase + 0x24u);
    write_ldr(0x30u, 9u, kLiteralBase + 0x28u);
    write_ldr(0x34u, 10u, kLiteralBase + 0x2Cu);
    write_ldr(0x38u, 11u, kLiteralBase + 0x30u);
    write_ldr(0x3Cu, 12u, kLiteralBase + 0x34u);
    write_ldr(0x40u, 13u, kLiteralBase + 0x38u);
    write_ldr(0x44u, 14u, kLiteralBase + 0x3Cu);
    write_ldr(0x48u, 0u, kLiteralBase + 0x40u);
    WriteWordLe(rom, 0x4Cu, EncodeMsrCpsrControl(0u));
    WriteWordLe(rom, 0x50u, EncodeBranchRelativeWords(-2));

    WriteWordLe(rom, kLiteralBase + 0x00u, kSystemMode);
    WriteWordLe(rom, kLiteralBase + 0x04u, 0x11111111u);
    WriteWordLe(rom, kLiteralBase + 0x08u, 0x22222222u);
    WriteWordLe(rom, kLiteralBase + 0x0Cu, 0x33333333u);
    WriteWordLe(rom, kLiteralBase + 0x10u, 0x44444444u);
    WriteWordLe(rom, kLiteralBase + 0x14u, 0x55555555u);
    WriteWordLe(rom, kLiteralBase + 0x18u, 0x02001000u);
    WriteWordLe(rom, kLiteralBase + 0x1Cu, 0x08000100u);
    WriteWordLe(rom, kLiteralBase + 0x20u, kFiqMode);
    WriteWordLe(rom, kLiteralBase + 0x24u, 0xAAAAAAAAu);
    WriteWordLe(rom, kLiteralBase + 0x28u, 0xBBBBBBBBu);
    WriteWordLe(rom, kLiteralBase + 0x2Cu, 0xCCCCCCCCu);
    WriteWordLe(rom, kLiteralBase + 0x30u, 0xDDDDDDDDu);
    WriteWordLe(rom, kLiteralBase + 0x34u, 0xEEEEEEEEu);
    WriteWordLe(rom, kLiteralBase + 0x38u, 0x02002000u);
    WriteWordLe(rom, kLiteralBase + 0x3Cu, 0x08000200u);
    WriteWordLe(rom, kLiteralBase + 0x40u, kSystemMode);
    WriteFile(gba_rom, rom);

    emugbds::ingba::Core core;
    std::string error;
    t.Expect(core.Initialize(error), "ingba FIQ banking core initializes");
    error.clear();
    t.Expect(core.LoadRom(gba_rom, error), "ingba FIQ banking core loads ROM");
    error.clear();
    t.Expect(core.RunFrame(error), "ingba FIQ banking core runs frame");

    t.Expect((core.cpu().cpsr & 0x1Fu) == kSystemMode, "FIQ banking program returns to System mode");
    t.Expect(core.cpu().r[8] == 0x11111111u, "FIQ preserves shared r8");
    t.Expect(core.cpu().r[9] == 0x22222222u, "FIQ preserves shared r9");
    t.Expect(core.cpu().r[10] == 0x33333333u, "FIQ preserves shared r10");
    t.Expect(core.cpu().r[11] == 0x44444444u, "FIQ preserves shared r11");
    t.Expect(core.cpu().r[12] == 0x55555555u, "FIQ preserves shared r12");
    t.Expect(core.cpu().r[13] == 0x02001000u, "FIQ preserves System SP");
    t.Expect(core.cpu().r[14] == 0x08000100u, "FIQ preserves System LR");
    t.Expect(core.cpu().banked_r8_r14_fiq[0] == 0xAAAAAAAAu, "FIQ keeps banked r8");
    t.Expect(core.cpu().banked_r8_r14_fiq[6] == 0x08000200u, "FIQ keeps banked LR");
}

void TestIngbaFiqUserBankTransfers(TestContext& t) {
    const auto dir = MakeTempRoot() / "ingba-fiq-user-transfers";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "fiq_user_transfers.gba";

    constexpr uint32_t kLiteralBase = 0x100u;
    constexpr uint32_t kUserValuesOffset = 0x10u;
    constexpr uint32_t kFiqValuesOffset = 0x2Cu;
    constexpr uint32_t kNewValuesOffset = 0x48u;
    constexpr uint32_t kUserRegisterList = 0x7F00u; // r8-r14
    constexpr uint32_t kLowRegisterList = 0x00FEu;  // r1-r7

    std::vector<uint8_t> rom = BuildSyntheticGbaRom();
    auto write_ldr = [&](uint32_t address, uint32_t rd, uint32_t literal_address) {
        WriteWordLe(rom, address, EncodeLdrLiteralAt(rd, address, literal_address));
    };

    write_ldr(0x00u, 1u, kLiteralBase + 0x00u);
    WriteWordLe(rom, 0x04u, EncodeMsrCpsrControl(1u));
    write_ldr(0x08u, 0u, kLiteralBase + 0x04u);
    write_ldr(0x0Cu, 8u, kLiteralBase + kUserValuesOffset + 0x00u);
    write_ldr(0x10u, 9u, kLiteralBase + kUserValuesOffset + 0x04u);
    write_ldr(0x14u, 10u, kLiteralBase + kUserValuesOffset + 0x08u);
    write_ldr(0x18u, 11u, kLiteralBase + kUserValuesOffset + 0x0Cu);
    write_ldr(0x1Cu, 12u, kLiteralBase + kUserValuesOffset + 0x10u);
    write_ldr(0x20u, 13u, kLiteralBase + kUserValuesOffset + 0x14u);
    write_ldr(0x24u, 14u, kLiteralBase + kUserValuesOffset + 0x18u);
    write_ldr(0x28u, 1u, kLiteralBase + 0x08u);
    WriteWordLe(rom, 0x2Cu, EncodeMsrCpsrControl(1u));
    write_ldr(0x30u, 8u, kLiteralBase + kFiqValuesOffset + 0x00u);
    write_ldr(0x34u, 9u, kLiteralBase + kFiqValuesOffset + 0x04u);
    write_ldr(0x38u, 10u, kLiteralBase + kFiqValuesOffset + 0x08u);
    write_ldr(0x3Cu, 11u, kLiteralBase + kFiqValuesOffset + 0x0Cu);
    write_ldr(0x40u, 12u, kLiteralBase + kFiqValuesOffset + 0x10u);
    write_ldr(0x44u, 13u, kLiteralBase + kFiqValuesOffset + 0x14u);
    write_ldr(0x48u, 14u, kLiteralBase + kFiqValuesOffset + 0x18u);
    WriteWordLe(rom, 0x4Cu, EncodeStmiaUser(0u, kUserRegisterList));
    WriteWordLe(rom, 0x50u, EncodeLdmiaNoWriteback(0u, kLowRegisterList));
    WriteWordLe(rom, 0x54u, EncodeStrReg(7u, 0u, 0x1Cu)); // Preserve STM^'s observed r14.
    write_ldr(0x58u, 1u, kLiteralBase + kNewValuesOffset + 0x00u);
    write_ldr(0x5Cu, 2u, kLiteralBase + kNewValuesOffset + 0x04u);
    write_ldr(0x60u, 3u, kLiteralBase + kNewValuesOffset + 0x08u);
    write_ldr(0x64u, 4u, kLiteralBase + kNewValuesOffset + 0x0Cu);
    write_ldr(0x68u, 5u, kLiteralBase + kNewValuesOffset + 0x10u);
    write_ldr(0x6Cu, 6u, kLiteralBase + kNewValuesOffset + 0x14u);
    write_ldr(0x70u, 7u, kLiteralBase + kNewValuesOffset + 0x18u);
    WriteWordLe(rom, 0x74u, EncodeStmiaNoWriteback(0u, kLowRegisterList));
    WriteWordLe(rom, 0x78u, EncodeLdmiaUser(0u, kUserRegisterList));
    write_ldr(0x7Cu, 1u, kLiteralBase + 0x00u);
    WriteWordLe(rom, 0x80u, EncodeMsrCpsrControl(1u));
    write_ldr(0x84u, 1u, kLiteralBase + 0x0Cu);
    WriteWordLe(rom, 0x88u, EncodeLdrReg(0u, 1u, 0u));
    WriteWordLe(rom, 0x8Cu, EncodeBranchRelativeWords(-2));

    WriteWordLe(rom, kLiteralBase + 0x00u, 0x0000001Fu); // System mode.
    WriteWordLe(rom, kLiteralBase + 0x04u, 0x02000020u); // Transfer base.
    WriteWordLe(rom, kLiteralBase + 0x08u, 0x00000011u); // FIQ mode.
    WriteWordLe(rom, kLiteralBase + 0x0Cu, 0x0200003Cu); // STM^ observation marker.
    WriteWordLe(rom, kLiteralBase + kUserValuesOffset + 0x00u, 0x11111111u);
    WriteWordLe(rom, kLiteralBase + kUserValuesOffset + 0x04u, 0x22222222u);
    WriteWordLe(rom, kLiteralBase + kUserValuesOffset + 0x08u, 0x33333333u);
    WriteWordLe(rom, kLiteralBase + kUserValuesOffset + 0x0Cu, 0x44444444u);
    WriteWordLe(rom, kLiteralBase + kUserValuesOffset + 0x10u, 0x55555555u);
    WriteWordLe(rom, kLiteralBase + kUserValuesOffset + 0x14u, 0x02001000u);
    WriteWordLe(rom, kLiteralBase + kUserValuesOffset + 0x18u, 0x08000100u);
    WriteWordLe(rom, kLiteralBase + kFiqValuesOffset + 0x00u, 0xAAAAAAAAu);
    WriteWordLe(rom, kLiteralBase + kFiqValuesOffset + 0x04u, 0xBBBBBBBBu);
    WriteWordLe(rom, kLiteralBase + kFiqValuesOffset + 0x08u, 0xCCCCCCCCu);
    WriteWordLe(rom, kLiteralBase + kFiqValuesOffset + 0x0Cu, 0xDDDDDDDDu);
    WriteWordLe(rom, kLiteralBase + kFiqValuesOffset + 0x10u, 0xEEEEEEEEu);
    WriteWordLe(rom, kLiteralBase + kFiqValuesOffset + 0x14u, 0x02002000u);
    WriteWordLe(rom, kLiteralBase + kFiqValuesOffset + 0x18u, 0x08000200u);
    WriteWordLe(rom, kLiteralBase + kNewValuesOffset + 0x00u, 0x12121212u);
    WriteWordLe(rom, kLiteralBase + kNewValuesOffset + 0x04u, 0x23232323u);
    WriteWordLe(rom, kLiteralBase + kNewValuesOffset + 0x08u, 0x34343434u);
    WriteWordLe(rom, kLiteralBase + kNewValuesOffset + 0x0Cu, 0x45454545u);
    WriteWordLe(rom, kLiteralBase + kNewValuesOffset + 0x10u, 0x56565656u);
    WriteWordLe(rom, kLiteralBase + kNewValuesOffset + 0x14u, 0x02003000u);
    WriteWordLe(rom, kLiteralBase + kNewValuesOffset + 0x18u, 0x08000300u);
    WriteFile(gba_rom, rom);

    emugbds::ingba::Core core;
    std::string error;
    t.Expect(core.Initialize(error), "ingba FIQ user-transfer core initializes");
    error.clear();
    t.Expect(core.LoadRom(gba_rom, error), "ingba FIQ user-transfer core loads ROM");
    error.clear();
    t.Expect(core.RunFrame(error), "ingba FIQ user-transfer core runs frame");

    t.Expect((core.cpu().cpsr & 0x1Fu) == 0x1Fu, "FIQ user-transfer program returns to System mode");
    t.Expect(core.cpu().r[0] == 0x08000100u, "STM^ reads the User-bank LR while in FIQ");
    t.Expect(core.cpu().r[8] == 0x12121212u, "LDM^ writes the shared/User r8");
    t.Expect(core.cpu().r[9] == 0x23232323u, "LDM^ writes the shared/User r9");
    t.Expect(core.cpu().r[10] == 0x34343434u, "LDM^ writes the shared/User r10");
    t.Expect(core.cpu().r[11] == 0x45454545u, "LDM^ writes the shared/User r11");
    t.Expect(core.cpu().r[12] == 0x56565656u, "LDM^ writes the shared/User r12");
    t.Expect(core.cpu().r[13] == 0x02003000u, "LDM^ writes the User-bank SP");
    t.Expect(core.cpu().r[14] == 0x08000300u, "LDM^ writes the User-bank LR");
    t.Expect(core.cpu().banked_r8_r14_fiq[0] == 0xAAAAAAAAu, "LDM^ leaves active FIQ r8 untouched");
}

void TestIngbaIrqWakesHalt(TestContext& t) {
    const auto dir = MakeTempRoot() / "ingba-irq-halt";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "irq_halt.gba";

    constexpr uint32_t kLiteralBase = 0xA0u;
    std::vector<uint8_t> rom = BuildSyntheticGbaRom();
    auto write_ldr = [&](uint32_t address, uint32_t rd, uint32_t literal_address) {
        WriteWordLe(rom, address, EncodeLdrLiteralAt(rd, address, literal_address));
    };

    write_ldr(0x00u, 0u, kLiteralBase + 0x00u);
    WriteWordLe(rom, 0x04u, EncodeMsrCpsrControl(0u)); // Enable IRQs and use System mode.
    write_ldr(0x08u, 0u, kLiteralBase + 0x04u);
    write_ldr(0x0Cu, 1u, kLiteralBase + 0x08u);
    WriteWordLe(rom, 0x10u, EncodeStrReg(0u, 1u, 0u));
    write_ldr(0x14u, 1u, kLiteralBase + 0x0Cu);
    WriteWordLe(rom, 0x18u, EncodeMovImm(0u, 1u));
    WriteWordLe(rom, 0x1Cu, EncodeStrReg(0u, 1u, 0u));
    write_ldr(0x20u, 1u, kLiteralBase + 0x10u);
    WriteWordLe(rom, 0x24u, EncodeStrReg(0u, 1u, 0u));
    write_ldr(0x28u, 1u, kLiteralBase + 0x14u);
    WriteWordLe(rom, 0x2Cu, EncodeMovImm(0u, 8u));
    WriteWordLe(rom, 0x30u, EncodeStrReg(0u, 1u, 0u));
    WriteWordLe(rom, 0x34u, EncodeSwi(0x02u)); // HALT until the VBlank IRQ arrives.
    WriteWordLe(rom, 0x38u, EncodeBranchRelativeWords(-2));

    write_ldr(0x40u, 1u, kLiteralBase + 0x18u);
    WriteWordLe(rom, 0x44u, EncodeLdrhImm(2u, 1u, 2u)); // Observe IF before acknowledging it.
    WriteWordLe(rom, 0x48u, EncodeMovImm(0u, 1u));
    WriteWordLe(rom, 0x4Cu, EncodeStrhImm(0u, 1u, 2u)); // IF is write-one-to-clear.
    WriteWordLe(rom, 0x50u, EncodeMovImm(0u, 0x77u));
    WriteWordLe(rom, 0x54u, EncodeSubImmSetFlags(15u, 14u, 4u)); // SUBS pc, lr, #4

    WriteWordLe(rom, kLiteralBase + 0x00u, 0x0000001Fu); // CPSR: System mode, IRQ enabled.
    WriteWordLe(rom, kLiteralBase + 0x04u, 0x08000040u); // IRQ callback in ROM.
    WriteWordLe(rom, kLiteralBase + 0x08u, 0x03007FFCu); // IRQ callback slot.
    WriteWordLe(rom, kLiteralBase + 0x0Cu, 0x04000200u); // IE.
    WriteWordLe(rom, kLiteralBase + 0x10u, 0x04000208u); // IME.
    WriteWordLe(rom, kLiteralBase + 0x14u, 0x04000004u); // DISPSTAT.
    WriteWordLe(rom, kLiteralBase + 0x18u, 0x04000200u); // IE/IF base.
    WriteFile(gba_rom, rom);

    emugbds::ingba::Core core;
    std::string error;
    t.Expect(core.Initialize(error), "ingba IRQ/HALT core initializes");
    error.clear();
    t.Expect(core.LoadRom(gba_rom, error), "ingba IRQ/HALT core loads ROM");
    error.clear();
    t.Expect(core.RunFrame(error), "ingba IRQ/HALT core runs frame");

    t.Expect(core.cpu().r[0] == 0x77u, "VBlank IRQ handler executes after HALT");
    t.Expect(core.cpu().r[2] == 0x0001u, "IRQ handler observes IF before acknowledging it");
    t.Expect((core.cpu().cpsr & 0x1Fu) == 0x1Fu, "IRQ return restores System mode");
    t.Expect((core.cpu().cpsr & (1u << 7u)) == 0u, "IRQ return restores IRQ enable state");
    t.Expect(core.iflags() == 0u, "serviced VBlank IRQ is acknowledged");
    t.Expect(core.ie() == 0x0001u, "VBlank IRQ remains enabled in IE");
    t.Expect(core.ime() == 0x0001u, "interrupt master enable remains active");
    t.Expect(core.cpu().r[15] == 0x08000038u, "IRQ return resumes at the HALT wait loop");
}

void TestIngbaHaltWakesWithMaskedIrq(TestContext& t) {
    const auto dir = MakeTempRoot() / "ingba-halt-masked-irq";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "halt_masked_irq.gba";

    constexpr uint32_t kLiteralBase = 0x60u;
    std::vector<uint8_t> rom = BuildSyntheticGbaRom();
    auto write_ldr = [&](uint32_t address, uint32_t rd, uint32_t literal_address) {
        WriteWordLe(rom, address, EncodeLdrLiteralAt(rd, address, literal_address));
    };

    write_ldr(0x00u, 1u, kLiteralBase + 0x00u);
    WriteWordLe(rom, 0x04u, EncodeMsrCpsrControl(1u));
    write_ldr(0x08u, 1u, kLiteralBase + 0x04u);
    WriteWordLe(rom, 0x0Cu, EncodeMovImm(0u, 1u));
    WriteWordLe(rom, 0x10u, EncodeStrReg(0u, 1u, 0u)); // IE = VBlank.
    write_ldr(0x14u, 1u, kLiteralBase + 0x08u);
    WriteWordLe(rom, 0x18u, EncodeMovImm(0u, 8u));
    WriteWordLe(rom, 0x1Cu, EncodeStrReg(0u, 1u, 0u)); // DISPSTAT VBlank IRQ enable.
    WriteWordLe(rom, 0x20u, EncodeSwi(0x02u));
    WriteWordLe(rom, 0x24u, EncodeMovImm(0u, 0x66u));
    WriteWordLe(rom, 0x28u, EncodeBranchRelativeWords(-2));

    WriteWordLe(rom, kLiteralBase + 0x00u, 0x0000001Fu); // System mode, IRQ enabled.
    WriteWordLe(rom, kLiteralBase + 0x04u, 0x04000200u); // IE.
    WriteWordLe(rom, kLiteralBase + 0x08u, 0x04000004u); // DISPSTAT.
    WriteFile(gba_rom, rom);

    emugbds::ingba::Core core;
    std::string error;
    t.Expect(core.Initialize(error), "ingba masked-IRQ HALT core initializes");
    error.clear();
    t.Expect(core.LoadRom(gba_rom, error), "ingba masked-IRQ HALT core loads ROM");
    error.clear();
    t.Expect(core.RunFrame(error), "ingba masked-IRQ HALT core runs frame");

    t.Expect(core.cpu().r[0] == 0x66u, "HALT wakes when IME masks IRQ delivery");
    t.Expect((core.cpu().cpsr & 0x1Fu) == 0x1Fu, "masked IRQ wake leaves CPU in System mode");
    t.Expect(core.iflags() == 0x0001u, "masked VBlank request remains pending");
    t.Expect(core.IsRunning(), "HALT does not make the emulation core unavailable");
}

void TestIngbaIrqReturnsToThumb(TestContext& t) {
    const auto dir = MakeTempRoot() / "ingba-irq-thumb-return";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "irq_thumb_return.gba";

    constexpr uint32_t kLiteralBase = 0xC0u;
    std::vector<uint8_t> rom = BuildSyntheticGbaRom();
    auto write_ldr = [&](uint32_t address, uint32_t rd, uint32_t literal_address) {
        WriteWordLe(rom, address, EncodeLdrLiteralAt(rd, address, literal_address));
    };

    write_ldr(0x00u, 0u, kLiteralBase + 0x00u);
    WriteWordLe(rom, 0x04u, EncodeMsrCpsrControl(0u));
    write_ldr(0x08u, 0u, kLiteralBase + 0x04u);
    write_ldr(0x0Cu, 1u, kLiteralBase + 0x08u);
    WriteWordLe(rom, 0x10u, EncodeStrReg(0u, 1u, 0u));
    write_ldr(0x14u, 1u, kLiteralBase + 0x0Cu);
    WriteWordLe(rom, 0x18u, EncodeMovImm(0u, 1u));
    WriteWordLe(rom, 0x1Cu, EncodeStrReg(0u, 1u, 0u));
    write_ldr(0x20u, 1u, kLiteralBase + 0x10u);
    WriteWordLe(rom, 0x24u, EncodeStrReg(0u, 1u, 0u));
    write_ldr(0x28u, 1u, kLiteralBase + 0x14u);
    WriteWordLe(rom, 0x2Cu, EncodeMovImm(0u, 8u));
    WriteWordLe(rom, 0x30u, EncodeStrReg(0u, 1u, 0u));
    write_ldr(0x34u, 0u, kLiteralBase + 0x1Cu);
    WriteWordLe(rom, 0x38u, EncodeArmBx(0u));

    write_ldr(0x40u, 1u, kLiteralBase + 0x18u);
    WriteWordLe(rom, 0x44u, EncodeLdrhImm(2u, 1u, 2u));
    WriteWordLe(rom, 0x48u, EncodeMovImm(0u, 1u));
    WriteWordLe(rom, 0x4Cu, EncodeStrhImm(0u, 1u, 2u));
    WriteWordLe(rom, 0x50u, EncodeMovImm(0u, 0x77u));
    WriteWordLe(rom, 0x54u, EncodeSubImmSetFlags(15u, 14u, 4u)); // SUBS pc, lr, #4

    WriteLe16(rom, 0x80u, EncodeThumbSwi(0x02u));
    WriteLe16(rom, 0x82u, EncodeThumbBRelative(-2));

    WriteWordLe(rom, kLiteralBase + 0x00u, 0x0000001Fu); // System mode, IRQ enabled.
    WriteWordLe(rom, kLiteralBase + 0x04u, 0x08000040u); // ARM IRQ callback in ROM.
    WriteWordLe(rom, kLiteralBase + 0x08u, 0x03007FFCu); // IRQ callback slot.
    WriteWordLe(rom, kLiteralBase + 0x0Cu, 0x04000200u); // IE.
    WriteWordLe(rom, kLiteralBase + 0x10u, 0x04000208u); // IME.
    WriteWordLe(rom, kLiteralBase + 0x14u, 0x04000004u); // DISPSTAT.
    WriteWordLe(rom, kLiteralBase + 0x18u, 0x04000200u); // IE/IF base for handler.
    WriteWordLe(rom, kLiteralBase + 0x1Cu, 0x08000081u); // Thumb entry in ROM.
    WriteFile(gba_rom, rom);

    emugbds::ingba::Core core;
    std::string error;
    t.Expect(core.Initialize(error), "ingba Thumb IRQ return core initializes");
    error.clear();
    t.Expect(core.LoadRom(gba_rom, error), "ingba Thumb IRQ return core loads ROM");
    error.clear();
    t.Expect(core.RunFrame(error), "ingba Thumb IRQ return core runs frame");

    t.Expect(core.cpu().r[0] == 0x77u, "IRQ handler runs while interrupted code was Thumb");
    t.Expect(core.cpu().r[2] == 0x0001u, "Thumb-origin IRQ handler observes IF");
    t.Expect((core.cpu().cpsr & (1u << 5u)) != 0u, "IRQ return restores Thumb state");
    t.Expect(core.cpu().r[15] == 0x08000082u, "IRQ return preserves Thumb PC bit alignment");
    t.Expect(core.iflags() == 0u, "Thumb-origin IRQ is acknowledged by the handler");
}

void TestIngbaMultiplyInstruction(TestContext& t) {
    const auto dir = MakeTempRoot() / "ingba-multiply";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "mul.gba";

    std::vector<uint32_t> words = {
        EncodeMovImm(1, 6),
        EncodeMovImm(2, 7),
        EncodeMul(0, 1, 2),
        EncodeBranchRelativeWords(-2),
    };
    WriteFile(gba_rom, BuildProgramGbaRom(words));

    emugbds::ingba::Core core;
    std::string error;
    t.Expect(core.Initialize(error), "ingba multiply core initializes");
    error.clear();
    t.Expect(core.LoadRom(gba_rom, error), "ingba multiply core loads ROM");
    error.clear();
    t.Expect(core.RunFrame(error), "ingba multiply core runs frame");
    t.Expect(core.cpu().r[0] == 42u, "ingba multiply writes expected result");
    t.Expect(core.unsupported_arm_stats().multiply == 0u, "ingba multiply handled without unsupported multiply class");
}

void TestIngbaBlockTransferInstruction(TestContext& t) {
    const auto dir = MakeTempRoot() / "ingba-block-transfer";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "block.gba";

    std::vector<uint32_t> words = {
        EncodeLdrLiteral(4, 0x24),      // r4 = [pc + 0x24] -> 0x2C literal
        EncodeMovImm(0, 1),
        EncodeMovImm(1, 2),
        EncodeMovImm(2, 3),
        EncodeStmiaWriteback(4, 0x0007), // store r0-r2
        EncodeMovImm(0, 0),
        EncodeMovImm(1, 0),
        EncodeMovImm(2, 0),
        EncodeSubImm(4, 4, 12),
        EncodeLdmiaWriteback(4, 0x0007), // load r0-r2
        EncodeBranchRelativeWords(-2),
    };

    std::vector<uint8_t> rom = BuildProgramGbaRom(words);
    WriteWordLe(rom, 0x2Cu, 0x02000020u);
    WriteFile(gba_rom, rom);

    emugbds::ingba::Core core;
    std::string error;
    t.Expect(core.Initialize(error), "ingba block-transfer core initializes");
    error.clear();
    t.Expect(core.LoadRom(gba_rom, error), "ingba block-transfer core loads ROM");
    error.clear();
    t.Expect(core.RunFrame(error), "ingba block-transfer core runs frame");
    t.Expect(core.cpu().r[0] == 1u, "ingba LDM restores r0");
    t.Expect(core.cpu().r[1] == 2u, "ingba LDM restores r1");
    t.Expect(core.cpu().r[2] == 3u, "ingba LDM restores r2");
    t.Expect(core.unsupported_arm_stats().block_transfer == 0u, "ingba block transfer handled without unsupported class");
}

void TestIngbaArmHalfwordTransfer(TestContext& t) {
    const auto dir = MakeTempRoot() / "ingba-arm-halfword";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "arm_halfword.gba";

    std::vector<uint32_t> words = {
        EncodeLdrLiteral(4, 0x18), // 0x20: base pointer
        EncodeLdrLiteral(0, 0x18), // 0x24: value 0x1234
        EncodeStrhImm(0, 4, 0),
        EncodeLdrhImm(1, 4, 0),
        EncodeLdrsbImm(2, 4, 1),
        EncodeLdrshImm(3, 4, 0),
        EncodeBranchRelativeWords(-2),
    };

    std::vector<uint8_t> rom = BuildProgramGbaRom(words);
    WriteWordLe(rom, 0x20u, 0x02000020u);
    WriteWordLe(rom, 0x24u, 0x00001234u);
    WriteFile(gba_rom, rom);

    emugbds::ingba::Core core;
    std::string error;
    t.Expect(core.Initialize(error), "ingba halfword core initializes");
    error.clear();
    t.Expect(core.LoadRom(gba_rom, error), "ingba halfword core loads ROM");
    error.clear();
    t.Expect(core.RunFrame(error), "ingba halfword core runs frame");
    t.Expect(core.cpu().r[1] == 0x00001234u, "ingba LDRH loads expected halfword");
    t.Expect(core.cpu().r[2] == 0x00000012u, "ingba LDRSB sign-extends expected byte");
    t.Expect(core.cpu().r[3] == 0x00001234u, "ingba LDRSH sign-extends expected halfword");
}

void TestIngbaSwiCpuSetInstruction(TestContext& t) {
    const auto dir = MakeTempRoot() / "ingba-swi-cpuset";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "swi_cpuset.gba";

    std::vector<uint32_t> words = {
        EncodeLdrLiteral(0, 0x18), // src ptr at 0x20
        EncodeLdrLiteral(1, 0x18), // dst ptr at 0x24
        EncodeLdrLiteral(2, 0x18), // control at 0x28
        EncodeSwi(0x0B),           // CpuSet
        EncodeLdrReg(3, 1, 0),
        EncodeBranchRelativeWords(-2),
    };

    std::vector<uint8_t> rom = BuildProgramGbaRom(words);
    WriteWordLe(rom, 0x20u, 0x08000040u); // source address (ROM)
    WriteWordLe(rom, 0x24u, 0x02000020u); // destination address (EWRAM)
    WriteWordLe(rom, 0x28u, 0x04000001u); // 32-bit copy count=1
    WriteWordLe(rom, 0x40u, 0x12345678u); // source word
    WriteFile(gba_rom, rom);

    emugbds::ingba::Core core;
    std::string error;
    t.Expect(core.Initialize(error), "ingba SWI core initializes");
    error.clear();
    t.Expect(core.LoadRom(gba_rom, error), "ingba SWI core loads ROM");
    error.clear();
    t.Expect(core.RunFrame(error), "ingba SWI core runs frame");
    t.Expect(core.cpu().r[3] == 0x12345678u, "ingba CpuSet SWI copied expected value");
    t.Expect(core.unsupported_arm_stats().software_interrupt == 0u, "ingba SWI class handled for CpuSet");
}

void TestIngbaUnsupportedSwiClassCounter(TestContext& t) {
    const auto dir = MakeTempRoot() / "ingba-swi-unsupported";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "swi_bad.gba";

    std::vector<uint32_t> words = {
        EncodeSwi(0xFE),
        EncodeBranchRelativeWords(-3), // branch back to SWI instruction at 0x00
    };
    WriteFile(gba_rom, BuildProgramGbaRom(words));

    emugbds::ingba::Core core;
    std::string error;
    t.Expect(core.Initialize(error), "ingba unsupported SWI core initializes");
    error.clear();
    t.Expect(core.LoadRom(gba_rom, error), "ingba unsupported SWI core loads ROM");
    error.clear();
    t.Expect(core.RunFrame(error), "ingba unsupported SWI core still runs one frame below halt threshold");
    t.Expect(core.unsupported_arm_stats().software_interrupt > 0u, "ingba unsupported SWI increments software interrupt class counter");
    t.Expect(core.unsupported_arm_stats().generic == 0u, "ingba unsupported SWI does not pollute generic unsupported class");
}

void TestIngbaSwiLz77UnCompWram(TestContext& t) {
    const auto dir = MakeTempRoot() / "ingba-swi-lz77";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "swi_lz77.gba";

    std::vector<uint32_t> words = {
        EncodeLdrLiteral(0, 0x18), // src @0x20
        EncodeLdrLiteral(1, 0x18), // dst @0x24
        EncodeSwi(0x11),           // LZ77UnCompWram
        EncodeLdrReg(2, 1, 0),
        EncodeBranchRelativeWords(-2),
    };

    std::vector<uint8_t> rom = BuildProgramGbaRom(words);
    WriteWordLe(rom, 0x20u, 0x08000040u);
    WriteWordLe(rom, 0x24u, 0x02000020u);
    WriteWordLe(rom, 0x40u, 0x00000410u); // type=0x10, size=4
    rom[0x44u] = 0x00u;                   // 8 literal bytes flag
    rom[0x45u] = 0x12u;
    rom[0x46u] = 0x34u;
    rom[0x47u] = 0x56u;
    rom[0x48u] = 0x78u;
    WriteFile(gba_rom, rom);

    emugbds::ingba::Core core;
    std::string error;
    t.Expect(core.Initialize(error), "ingba lz77 SWI core initializes");
    error.clear();
    t.Expect(core.LoadRom(gba_rom, error), "ingba lz77 SWI core loads ROM");
    error.clear();
    t.Expect(core.RunFrame(error), "ingba lz77 SWI core runs frame");
    t.Expect(core.cpu().r[2] == 0x78563412u, "ingba LZ77UnCompWram SWI decompresses expected bytes");
}

void TestIngbaSwiRlUnCompWram(TestContext& t) {
    const auto dir = MakeTempRoot() / "ingba-swi-rl";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "swi_rl.gba";

    std::vector<uint32_t> words = {
        EncodeLdrLiteral(0, 0x18), // src @0x20
        EncodeLdrLiteral(1, 0x18), // dst @0x24
        EncodeSwi(0x14),           // RLUnCompWram
        EncodeLdrReg(2, 1, 0),
        EncodeBranchRelativeWords(-2),
    };

    std::vector<uint8_t> rom = BuildProgramGbaRom(words);
    WriteWordLe(rom, 0x20u, 0x08000040u);
    WriteWordLe(rom, 0x24u, 0x02000020u);
    WriteWordLe(rom, 0x40u, 0x00000430u); // type=0x30, size=4
    rom[0x44u] = 0x03u;                   // raw run, count=(3+1)=4
    rom[0x45u] = 0x9Au;
    rom[0x46u] = 0xBCu;
    rom[0x47u] = 0xDEu;
    rom[0x48u] = 0xF0u;
    WriteFile(gba_rom, rom);

    emugbds::ingba::Core core;
    std::string error;
    t.Expect(core.Initialize(error), "ingba rl SWI core initializes");
    error.clear();
    t.Expect(core.LoadRom(gba_rom, error), "ingba rl SWI core loads ROM");
    error.clear();
    t.Expect(core.RunFrame(error), "ingba rl SWI core runs frame");
    t.Expect(core.cpu().r[2] == 0xF0DEBC9Au, "ingba RLUnCompWram SWI decompresses expected bytes");
}

void TestIngbaSwiBgAffineSet(TestContext& t) {
    const auto dir = MakeTempRoot() / "ingba-swi-bgaffine";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "swi_bgaffine.gba";

    std::vector<uint32_t> words = {
        EncodeLdrLiteral(0, 0x18), // src @0x20
        EncodeLdrLiteral(1, 0x18), // dst @0x24
        EncodeMovImm(2, 1),        // count=1
        EncodeSwi(0x0E),           // BgAffineSet
        EncodeLdrReg(3, 1, 0),     // read pa/pb
        EncodeBranchRelativeWords(-2),
    };

    std::vector<uint8_t> rom = BuildProgramGbaRom(words);
    WriteWordLe(rom, 0x20u, 0x08000040u);
    WriteWordLe(rom, 0x24u, 0x02000020u);
    WriteWordLe(rom, 0x40u, 0x00000000u); // tex_x
    WriteWordLe(rom, 0x44u, 0x00000000u); // tex_y
    WriteLe16(rom, 0x48u, 0x0000u);       // disp_x
    WriteLe16(rom, 0x4Au, 0x0000u);       // disp_y
    WriteLe16(rom, 0x4Cu, 0x0100u);       // scale_x = 1.0
    WriteLe16(rom, 0x4Eu, 0x0100u);       // scale_y = 1.0
    WriteLe16(rom, 0x50u, 0x0000u);       // angle
    WriteLe16(rom, 0x52u, 0x0000u);       // padding
    WriteFile(gba_rom, rom);

    emugbds::ingba::Core core;
    std::string error;
    t.Expect(core.Initialize(error), "ingba BgAffineSet SWI core initializes");
    error.clear();
    t.Expect(core.LoadRom(gba_rom, error), "ingba BgAffineSet SWI core loads ROM");
    error.clear();
    t.Expect(core.RunFrame(error), "ingba BgAffineSet SWI core runs frame");
    t.Expect(core.cpu().r[3] == 0x00000100u, "ingba BgAffineSet writes identity matrix pa/pb");
}

void TestIngbaInputMasks(TestContext& t) {
    const auto dir = MakeTempRoot() / "ingba-input";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "input.gba";
    WriteFile(gba_rom, BuildSyntheticGbaRom());

    emugbds::ingba::Core core;
    std::string error;
    t.Expect(core.Initialize(error), "ingba input core initializes");
    error.clear();
    t.Expect(core.LoadRom(gba_rom, error), "ingba input core loads ROM");
    t.Expect(core.keyinput_state() == 0x03FFu, "ingba keyinput defaults to all released");

    const uint32_t press = emugbds::input::GbaKeyMask(emugbds::input::GbaKey::A) |
        emugbds::input::GbaKeyMask(emugbds::input::GbaKey::Start);
    core.ApplyInputMasks(press, 0u);
    t.Expect(core.keyinput_state() == 0x03F6u, "ingba press mask clears active key bits");

    const uint32_t release_a = emugbds::input::GbaKeyMask(emugbds::input::GbaKey::A);
    core.ApplyInputMasks(0u, release_a);
    t.Expect(core.keyinput_state() == 0x03F7u, "ingba release mask restores released key bit");
}

void TestIngbaThumbAluAndBranch(TestContext& t) {
    const auto dir = MakeTempRoot() / "ingba-thumb-alu";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "thumb_alu.gba";

    std::vector<uint8_t> rom = BuildSyntheticGbaRom();

    WriteWordLe(rom, 0x00u, EncodeLdrLiteral(0, 0x18)); // loads 0x08000041 from 0x20
    WriteWordLe(rom, 0x04u, EncodeArmBx(0));
    WriteWordLe(rom, 0x20u, 0x08000041u);

    WriteLe16(rom, 0x40u, EncodeThumbMovImm(1, 5));
    WriteLe16(rom, 0x42u, EncodeThumbMovImm(2, 7));
    WriteLe16(rom, 0x44u, EncodeThumbAddReg(0, 1, 2));
    WriteLe16(rom, 0x46u, EncodeThumbSubImm(0, 2));
    WriteLe16(rom, 0x48u, EncodeThumbCmpImm(0, 10));
    WriteLe16(rom, 0x4Au, EncodeThumbBRelative(-2)); // loop on the compare

    WriteFile(gba_rom, rom);

    emugbds::ingba::Core core;
    std::string error;
    t.Expect(core.Initialize(error), "ingba thumb ALU core initializes");
    error.clear();
    t.Expect(core.LoadRom(gba_rom, error), "ingba thumb ALU core loads ROM");
    error.clear();
    t.Expect(core.RunFrame(error), "ingba thumb ALU core runs frame");
    t.Expect(core.cpu().r[0] == 10u, "ingba thumb ALU program reaches expected value");
    t.Expect(core.unsupported_arm_stats().thumb == 0u, "ingba thumb ALU/branch path has no unsupported thumb class");
}

void TestIngbaThumbLoadStore(TestContext& t) {
    const auto dir = MakeTempRoot() / "ingba-thumb-loadstore";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "thumb_loadstore.gba";

    std::vector<uint8_t> rom = BuildSyntheticGbaRom();

    WriteWordLe(rom, 0x00u, EncodeLdrLiteral(0, 0x18)); // loads 0x08000041 from 0x20
    WriteWordLe(rom, 0x04u, EncodeArmBx(0));
    WriteWordLe(rom, 0x20u, 0x08000041u);

    WriteLe16(rom, 0x40u, EncodeThumbLdrLiteral(4, 0x0B)); // (0x44 + 0x0B*4)=0x70
    WriteLe16(rom, 0x42u, EncodeThumbMovImm(0, 0x5A));
    WriteLe16(rom, 0x44u, EncodeThumbStrbImm(0, 4, 0));
    WriteLe16(rom, 0x46u, EncodeThumbMovImm(0, 0));
    WriteLe16(rom, 0x48u, EncodeThumbLdrbImm(0, 4, 0));
    WriteLe16(rom, 0x4Au, EncodeThumbBRelative(-2)); // loop
    WriteWordLe(rom, 0x70u, 0x02000020u);

    WriteFile(gba_rom, rom);

    emugbds::ingba::Core core;
    std::string error;
    t.Expect(core.Initialize(error), "ingba thumb load/store core initializes");
    error.clear();
    t.Expect(core.LoadRom(gba_rom, error), "ingba thumb load/store core loads ROM");
    error.clear();
    t.Expect(core.RunFrame(error), "ingba thumb load/store core runs frame");
    t.Expect(core.cpu().r[0] == 0x5Au, "ingba thumb load/store program loads stored byte");
    t.Expect(core.unsupported_arm_stats().thumb == 0u, "ingba thumb load/store path has no unsupported thumb class");
}

void TestIngbaThumbBlInterworking(TestContext& t) {
    const auto dir = MakeTempRoot() / "ingba-thumb-bl";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "thumb_bl.gba";

    std::vector<uint8_t> rom = BuildSyntheticGbaRom();

    WriteWordLe(rom, 0x00u, EncodeLdrLiteral(0, 0x18)); // loads 0x08000041 from 0x20
    WriteWordLe(rom, 0x04u, EncodeArmBx(0));
    WriteWordLe(rom, 0x20u, 0x08000041u);

    WriteLe16(rom, 0x40u, EncodeThumbMovImm(0, 1));
    WriteLe16(rom, 0x42u, EncodeThumbBlPrefix(0x000u));
    WriteLe16(rom, 0x44u, EncodeThumbBlSuffix(0x003u)); // target = 0x4C
    WriteLe16(rom, 0x46u, EncodeThumbBRelative(-2));    // main loop
    WriteLe16(rom, 0x4Cu, EncodeThumbAddImm(0, 2));
    WriteLe16(rom, 0x4Eu, EncodeThumbBxReg(14));

    WriteFile(gba_rom, rom);

    emugbds::ingba::Core core;
    std::string error;
    t.Expect(core.Initialize(error), "ingba thumb BL core initializes");
    error.clear();
    t.Expect(core.LoadRom(gba_rom, error), "ingba thumb BL core loads ROM");
    error.clear();
    t.Expect(core.RunFrame(error), "ingba thumb BL core runs frame");
    t.Expect(core.cpu().r[0] == 3u, "ingba thumb BL/BX interworking returns expected result");
    t.Expect(core.unsupported_arm_stats().thumb == 0u, "ingba thumb BL path has no unsupported thumb class");
}

void TestIngbaVideoMode3Render(TestContext& t) {
    const auto dir = MakeTempRoot() / "ingba-video-mode3";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "video_mode3.gba";

    std::vector<uint32_t> words = {
        EncodeLdrLiteral(4, 0x24), // -> 0x2C: DISPCNT
        EncodeLdrLiteral(0, 0x24), // -> 0x30: mode3 + BG2
        EncodeStrReg(0, 4, 0),
        EncodeLdrLiteral(4, 0x20), // -> 0x34: VRAM
        EncodeLdrLiteral(0, 0x20), // -> 0x38: two mode3 pixels
        EncodeStrReg(0, 4, 0),
        EncodeBranchRelativeWords(-2),
    };

    std::vector<uint8_t> rom = BuildProgramGbaRom(words);
    WriteWordLe(rom, 0x2Cu, 0x04000000u);
    WriteWordLe(rom, 0x30u, 0x00000403u);
    WriteWordLe(rom, 0x34u, 0x06000000u);
    WriteWordLe(rom, 0x38u, 0x03E0001Fu); // pixel0 red, pixel1 green
    WriteFile(gba_rom, rom);

    emugbds::ingba::Core core;
    std::string error;
    t.Expect(core.Initialize(error), "ingba mode3 core initializes");
    error.clear();
    t.Expect(core.LoadRom(gba_rom, error), "ingba mode3 core loads ROM");
    error.clear();
    t.Expect(core.RunFrame(error), "ingba mode3 core runs frame");

    const uint32_t* pixels = core.video_pixels();
    t.Expect(pixels != nullptr, "ingba mode3 core exposes video pixels");
    if (pixels != nullptr) {
        t.Expect(pixels[0] == 0xFF0000FFu, "ingba mode3 pixel0 renders red");
        t.Expect(pixels[1] == 0xFF00FF00u, "ingba mode3 pixel1 renders green");
    }
}

void TestIngbaVideoMode4Render(TestContext& t) {
    const auto dir = MakeTempRoot() / "ingba-video-mode4";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "video_mode4.gba";

    std::vector<uint32_t> words = {
        EncodeLdrLiteral(4, 0x30), // -> 0x38: DISPCNT
        EncodeLdrLiteral(0, 0x30), // -> 0x3C: mode4 + BG2
        EncodeStrReg(0, 4, 0),
        EncodeLdrLiteral(4, 0x2C), // -> 0x40: PAL RAM base
        EncodeLdrLiteral(0, 0x2C), // -> 0x44: palette[1] = red
        EncodeStrReg(0, 4, 0),
        EncodeLdrLiteral(4, 0x28), // -> 0x48: VRAM base
        EncodeLdrLiteral(0, 0x28), // -> 0x4C: index 1 in first pixel
        EncodeStrReg(0, 4, 0),
        EncodeBranchRelativeWords(-2),
    };

    std::vector<uint8_t> rom = BuildProgramGbaRom(words);
    WriteWordLe(rom, 0x38u, 0x04000000u);
    WriteWordLe(rom, 0x3Cu, 0x00000404u);
    WriteWordLe(rom, 0x40u, 0x05000000u);
    WriteWordLe(rom, 0x44u, 0x001F0000u);
    WriteWordLe(rom, 0x48u, 0x06000000u);
    WriteWordLe(rom, 0x4Cu, 0x00000001u);
    WriteFile(gba_rom, rom);

    emugbds::ingba::Core core;
    std::string error;
    t.Expect(core.Initialize(error), "ingba mode4 core initializes");
    error.clear();
    t.Expect(core.LoadRom(gba_rom, error), "ingba mode4 core loads ROM");
    error.clear();
    t.Expect(core.RunFrame(error), "ingba mode4 core runs frame");

    const uint32_t* pixels = core.video_pixels();
    t.Expect(pixels != nullptr, "ingba mode4 core exposes video pixels");
    if (pixels != nullptr) {
        t.Expect(pixels[0] == 0xFF0000FFu, "ingba mode4 pixel0 resolves palette index");
    }
}

void TestIngbaVideoObjRender(TestContext& t) {
    const auto dir = MakeTempRoot() / "ingba-video-obj";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "video_obj.gba";

    constexpr uint32_t kLiteralBase = 0x100u;
    std::vector<uint32_t> words;
    const auto emit_ldr = [&](uint32_t rd, uint32_t literal_address) {
        const uint32_t instruction_address = static_cast<uint32_t>(words.size() * 4u);
        words.push_back(EncodeLdrLiteralAt(rd, instruction_address, literal_address));
    };

    emit_ldr(4u, kLiteralBase + 0x00u); // DISPCNT
    emit_ldr(0u, kLiteralBase + 0x04u); // OBJ enabled
    words.push_back(EncodeStrReg(0u, 4u, 0u));
    emit_ldr(4u, kLiteralBase + 0x08u); // OBJ palette entry 1
    emit_ldr(0u, kLiteralBase + 0x0Cu); // red
    words.push_back(EncodeStrhImm(0u, 4u, 0u));
    emit_ldr(4u, kLiteralBase + 0x28u); // OBJ palette entry 3
    emit_ldr(0u, kLiteralBase + 0x2Cu); // red
    words.push_back(EncodeStrhImm(0u, 4u, 0u));
    emit_ldr(4u, kLiteralBase + 0x30u); // OBJ palette entry 2
    emit_ldr(0u, kLiteralBase + 0x34u); // green
    words.push_back(EncodeStrhImm(0u, 4u, 0u));
    emit_ldr(4u, kLiteralBase + 0x10u); // 4bpp OBJ tile memory
    emit_ldr(0u, kLiteralBase + 0x14u); // first OBJ texel is color 1
    words.push_back(EncodeStrReg(0u, 4u, 0u));
    emit_ldr(4u, kLiteralBase + 0x18u); // OAM
    emit_ldr(0u, kLiteralBase + 0x1Cu); // attr0
    words.push_back(EncodeStrhImm(0u, 4u, 0u));
    emit_ldr(0u, kLiteralBase + 0x20u); // attr1
    words.push_back(EncodeStrhImm(0u, 4u, 2u));
    emit_ldr(0u, kLiteralBase + 0x24u); // attr2
    words.push_back(EncodeStrhImm(0u, 4u, 4u));
    emit_ldr(4u, kLiteralBase + 0x38u); // 8bpp OBJ tile memory
    emit_ldr(0u, kLiteralBase + 0x3Cu); // color indices 1 and 2
    words.push_back(EncodeStrReg(0u, 4u, 0u));
    emit_ldr(4u, kLiteralBase + 0x40u); // second OAM entry
    emit_ldr(0u, kLiteralBase + 0x44u); // 8bpp attr0 at y=8
    words.push_back(EncodeStrhImm(0u, 4u, 0u));
    emit_ldr(0u, kLiteralBase + 0x48u); // attr1
    words.push_back(EncodeStrhImm(0u, 4u, 2u));
    emit_ldr(0u, kLiteralBase + 0x4Cu); // attr2, tile 2
    words.push_back(EncodeStrhImm(0u, 4u, 4u));
    emit_ldr(4u, kLiteralBase + 0x50u); // 16x16 tile 0
    emit_ldr(0u, kLiteralBase + 0x54u);
    words.push_back(EncodeStrReg(0u, 4u, 0u));
    emit_ldr(4u, kLiteralBase + 0x58u); // 16x16 tile 1
    emit_ldr(0u, kLiteralBase + 0x5Cu);
    words.push_back(EncodeStrReg(0u, 4u, 0u));
    emit_ldr(4u, kLiteralBase + 0x60u); // 16x16 tile 2
    emit_ldr(0u, kLiteralBase + 0x64u);
    words.push_back(EncodeStrReg(0u, 4u, 0u));
    emit_ldr(4u, kLiteralBase + 0x68u); // 16x16 tile 3
    emit_ldr(0u, kLiteralBase + 0x6Cu);
    words.push_back(EncodeStrReg(0u, 4u, 0u));
    emit_ldr(4u, kLiteralBase + 0x70u); // OAM entry 2
    emit_ldr(0u, kLiteralBase + 0x74u); // y=16
    words.push_back(EncodeStrhImm(0u, 4u, 0u));
    emit_ldr(0u, kLiteralBase + 0x78u); // 16x16 square
    words.push_back(EncodeStrhImm(0u, 4u, 2u));
    emit_ldr(0u, kLiteralBase + 0x7Cu); // tile 4
    words.push_back(EncodeStrhImm(0u, 4u, 4u));
    emit_ldr(4u, kLiteralBase + 0x80u); // BG0CNT
    emit_ldr(0u, kLiteralBase + 0x84u); // priority 0, screen base 1
    words.push_back(EncodeStrhImm(0u, 4u, 0u));
    emit_ldr(4u, kLiteralBase + 0x88u); // BG palette entry 1
    emit_ldr(0u, kLiteralBase + 0x8Cu); // blue
    words.push_back(EncodeStrhImm(0u, 4u, 0u));
    emit_ldr(4u, kLiteralBase + 0x90u); // BG tile 0
    emit_ldr(0u, kLiteralBase + 0x94u); // first texel uses palette entry 1
    words.push_back(EncodeStrReg(0u, 4u, 0u));
    words.push_back(EncodeBranchRelativeWords(-static_cast<int32_t>(words.size()) - 2));

    std::vector<uint8_t> rom = BuildProgramGbaRom(words);
    WriteWordLe(rom, kLiteralBase + 0x00u, 0x04000000u);
    WriteWordLe(rom, kLiteralBase + 0x04u, 0x00001140u);
    WriteWordLe(rom, kLiteralBase + 0x08u, 0x05000202u);
    WriteWordLe(rom, kLiteralBase + 0x0Cu, 0x0000001Fu);
    WriteWordLe(rom, kLiteralBase + 0x10u, 0x06010000u);
    WriteWordLe(rom, kLiteralBase + 0x14u, 0x00000301u);
    WriteWordLe(rom, kLiteralBase + 0x18u, 0x07000000u);
    WriteWordLe(rom, kLiteralBase + 0x1Cu, 0x00000000u);
    WriteWordLe(rom, kLiteralBase + 0x20u, 0x00000000u);
    WriteWordLe(rom, kLiteralBase + 0x24u, 0x00000000u);
    WriteWordLe(rom, kLiteralBase + 0x28u, 0x05000206u);
    WriteWordLe(rom, kLiteralBase + 0x2Cu, 0x0000001Fu);
    WriteWordLe(rom, kLiteralBase + 0x30u, 0x05000204u);
    WriteWordLe(rom, kLiteralBase + 0x34u, 0x000003E0u);
    WriteWordLe(rom, kLiteralBase + 0x38u, 0x06010040u);
    WriteWordLe(rom, kLiteralBase + 0x3Cu, 0x00000201u);
    WriteWordLe(rom, kLiteralBase + 0x40u, 0x07000008u);
    WriteWordLe(rom, kLiteralBase + 0x44u, 0x00002008u);
    WriteWordLe(rom, kLiteralBase + 0x48u, 0x00000000u);
    WriteWordLe(rom, kLiteralBase + 0x4Cu, 0x00000002u);
    WriteWordLe(rom, kLiteralBase + 0x50u, 0x06010080u);
    WriteWordLe(rom, kLiteralBase + 0x54u, 0x00000001u);
    WriteWordLe(rom, kLiteralBase + 0x58u, 0x060100A0u);
    WriteWordLe(rom, kLiteralBase + 0x5Cu, 0x00000002u);
    WriteWordLe(rom, kLiteralBase + 0x60u, 0x060100C0u);
    WriteWordLe(rom, kLiteralBase + 0x64u, 0x00000003u);
    WriteWordLe(rom, kLiteralBase + 0x68u, 0x060100E0u);
    WriteWordLe(rom, kLiteralBase + 0x6Cu, 0x00000002u);
    WriteWordLe(rom, kLiteralBase + 0x70u, 0x07000010u);
    WriteWordLe(rom, kLiteralBase + 0x74u, 0x00000010u);
    WriteWordLe(rom, kLiteralBase + 0x78u, 0x00004000u);
    WriteWordLe(rom, kLiteralBase + 0x7Cu, 0x00000004u);
    WriteWordLe(rom, kLiteralBase + 0x80u, 0x04000008u);
    WriteWordLe(rom, kLiteralBase + 0x84u, 0x00000100u);
    WriteWordLe(rom, kLiteralBase + 0x88u, 0x05000002u);
    WriteWordLe(rom, kLiteralBase + 0x8Cu, 0x00007C00u);
    WriteWordLe(rom, kLiteralBase + 0x90u, 0x06000000u);
    WriteWordLe(rom, kLiteralBase + 0x94u, 0x00000001u);
    WriteFile(gba_rom, rom);

    emugbds::ingba::Core core;
    std::string error;
    t.Expect(core.Initialize(error), "ingba OBJ core initializes");
    error.clear();
    t.Expect(core.LoadRom(gba_rom, error), "ingba OBJ core loads ROM");
    error.clear();
    t.Expect(core.RunFrame(error), "ingba OBJ core runs frame");

    const uint32_t* pixels = core.video_pixels();
    t.Expect(pixels != nullptr, "ingba OBJ core exposes video pixels");
    if (pixels != nullptr) {
        t.Expect(pixels[0] == 0xFF0000FFu, "ingba OBJ wins ties against a same-priority background");
        t.Expect(pixels[1] == 0xFF000000u, "ingba OBJ transparent pixels preserve backdrop");
        t.Expect(pixels[2] == 0xFF0000FFu, "ingba OBJ reads the second packed tile byte");
        t.Expect(pixels[7u * 240u + 7u] == 0xFF000000u, "ingba OBJ tile does not read outside its tile data");
        t.Expect(pixels[8u * 240u] == 0xFF0000FFu, "ingba 8bpp OBJ resolves its first byte");
        t.Expect(pixels[8u * 240u + 1u] == 0xFF00FF00u, "ingba 8bpp OBJ advances to its second byte");
        t.Expect(pixels[16u * 240u] == 0xFF0000FFu, "ingba 1D OBJ maps its top-left tile");
        t.Expect(pixels[16u * 240u + 8u] == 0xFF00FF00u, "ingba 1D OBJ maps its top-right tile");
        t.Expect(pixels[24u * 240u] == 0xFF0000FFu, "ingba 1D OBJ maps its bottom-left tile");
        t.Expect(pixels[24u * 240u + 8u] == 0xFF00FF00u, "ingba 1D OBJ maps its bottom-right tile");
    }
}

void TestIngbaVideoMode2Render(TestContext& t) {
    const auto dir = MakeTempRoot() / "ingba-video-mode2";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "video_mode2.gba";

    constexpr uint32_t kLiteralBase = 0x200u;
    std::vector<uint32_t> words;
    const auto emit_ldr = [&](uint32_t rd, uint32_t literal_address) {
        const uint32_t instruction_address = static_cast<uint32_t>(words.size() * 4u);
        words.push_back(EncodeLdrLiteralAt(rd, instruction_address, literal_address));
    };
    const auto emit_store_half = [&](uint32_t address_literal, uint32_t value_literal) {
        emit_ldr(4u, address_literal);
        emit_ldr(0u, value_literal);
        words.push_back(EncodeStrhImm(0u, 4u, 0u));
    };
    const auto emit_store_word = [&](uint32_t address_literal, uint32_t value_literal) {
        emit_ldr(4u, address_literal);
        emit_ldr(0u, value_literal);
        words.push_back(EncodeStrReg(0u, 4u, 0u));
    };

    emit_store_half(kLiteralBase + 0x00u, kLiteralBase + 0x04u); // DISPCNT: mode 2 + BG2
    emit_store_half(kLiteralBase + 0x08u, kLiteralBase + 0x0Cu); // BG2CNT: 256-color, char base 1
    emit_store_half(kLiteralBase + 0x10u, kLiteralBase + 0x14u); // BG2PA
    emit_store_half(kLiteralBase + 0x18u, kLiteralBase + 0x1Cu); // BG2PD
    emit_store_half(kLiteralBase + 0x20u, kLiteralBase + 0x24u); // BG2X
    emit_store_half(kLiteralBase + 0x28u, kLiteralBase + 0x2Cu); // BG2Y
    emit_store_half(kLiteralBase + 0x30u, kLiteralBase + 0x34u); // palette entry 1
    emit_store_word(kLiteralBase + 0x38u, kLiteralBase + 0x3Cu); // first affine tile row
    words.push_back(EncodeBranchRelativeWords(-static_cast<int32_t>(words.size()) - 2));

    std::vector<uint8_t> rom = BuildProgramGbaRom(words);
    WriteWordLe(rom, kLiteralBase + 0x00u, 0x04000000u);
    WriteWordLe(rom, kLiteralBase + 0x04u, 0x00000402u);
    WriteWordLe(rom, kLiteralBase + 0x08u, 0x0400000Cu);
    WriteWordLe(rom, kLiteralBase + 0x0Cu, 0x00000004u);
    WriteWordLe(rom, kLiteralBase + 0x10u, 0x04000020u);
    WriteWordLe(rom, kLiteralBase + 0x14u, 0x00000100u);
    WriteWordLe(rom, kLiteralBase + 0x18u, 0x04000026u);
    WriteWordLe(rom, kLiteralBase + 0x1Cu, 0x00000100u);
    WriteWordLe(rom, kLiteralBase + 0x20u, 0x04000028u);
    WriteWordLe(rom, kLiteralBase + 0x24u, 0x00000000u);
    WriteWordLe(rom, kLiteralBase + 0x28u, 0x0400002Cu);
    WriteWordLe(rom, kLiteralBase + 0x2Cu, 0x00000000u);
    WriteWordLe(rom, kLiteralBase + 0x30u, 0x05000002u);
    WriteWordLe(rom, kLiteralBase + 0x34u, 0x0000001Fu);
    WriteWordLe(rom, kLiteralBase + 0x38u, 0x06004000u);
    WriteWordLe(rom, kLiteralBase + 0x3Cu, 0x01010101u);
    WriteFile(gba_rom, rom);

    emugbds::ingba::Core core;
    std::string error;
    t.Expect(core.Initialize(error), "ingba mode2 core initializes");
    error.clear();
    t.Expect(core.LoadRom(gba_rom, error), "ingba mode2 core loads ROM");
    error.clear();
    t.Expect(core.RunFrame(error), "ingba mode2 core runs frame");

    const uint32_t* pixels = core.video_pixels();
    t.Expect(pixels != nullptr, "ingba mode2 core exposes video pixels");
    if (pixels != nullptr) {
        t.Expect(pixels[0] == 0xFF0000FFu, "ingba mode2 affine BG2 renders a tile pixel");
        t.Expect(pixels[3] == 0xFF0000FFu, "ingba mode2 affine BG2 advances through tile pixels");
        t.Expect(pixels[4] == 0xFF000000u, "ingba mode2 affine BG2 transparent tile pixels preserve backdrop");
    }
}

void TestIndsMemoryBus(TestContext& t) {
    const auto dir = MakeTempRoot() / "inds-memory-bus";
    std::filesystem::create_directories(dir);

    const auto bios7 = dir / "nds7.bin";
    const auto bios9 = dir / "nds9.bin";
    const auto slot2 = dir / "test.gba";
    const auto bios7_bytes = BuildBytes(0x100u, 0x10);
    const auto bios9_bytes = BuildBytes(0x200u, 0x20);
    const auto slot2_bytes = BuildSyntheticGbaRom();
    WriteFile(bios7, bios7_bytes);
    WriteFile(bios9, bios9_bytes);
    WriteFile(slot2, slot2_bytes);

    emugbds::inds::MemoryBus bus;
    std::string error;
    t.Expect(bus.LoadBios7(bios7, error), "inds bus loads ARM7 BIOS");
    error.clear();
    t.Expect(bus.LoadBios9(bios9, error), "inds bus loads ARM9 BIOS");
    error.clear();
    t.Expect(bus.LoadSlot2Rom(slot2, error), "inds bus loads Slot-2 ROM");

    t.Expect(bus.Read8(0x00000000u) == bios7_bytes[0], "inds ARM7 BIOS mapping works");
    t.Expect(bus.Read8(kDsArm9BiosBase) == bios9_bytes[0], "inds ARM9 BIOS mapping works");
    t.Expect(bus.Read8(kDsSlot2Base) == slot2_bytes[0], "inds Slot-2 mapping works");
    t.Expect(
        bus.Read8(kDsSlot2Base + static_cast<uint32_t>(slot2_bytes.size()) + 7u) == slot2_bytes[7],
        "inds Slot-2 wrap mapping works");

    bus.Write8(kDsMainRamBase + 0x24u, 0xA5);
    bus.Write8(kDsSharedWramBase + 0x42u, 0x5A);
    t.Expect(bus.Read8(kDsMainRamBase + 0x24u) == 0xA5, "inds main RAM read/write works");
    t.Expect(bus.Read8(kDsSharedWramBase + 0x42u) == 0x5A, "inds shared WRAM read/write works");

    bus.ClearSlot2Rom();
    t.Expect(bus.Read8(kDsSlot2Base) == 0x00, "inds clear slot2 ROM removes mapping");
}

void TestIndsCoreFirmwareSlot2Transition(TestContext& t) {
    const auto dir = MakeTempRoot() / "inds-core-slot2";
    std::filesystem::create_directories(dir);
    const auto slot2 = dir / "test.gba";
    WriteFile(slot2, BuildSyntheticGbaRom());

    emugbds::inds::Core core;
    std::string error;
    t.Expect(
        core.Configure(
            std::nullopt,
            std::nullopt,
            std::nullopt,
            emugbds::core::DsBootMode::DsFirmware,
            false,
            error),
        "inds core configures for DS firmware boot");

    error.clear();
    t.Expect(core.Initialize(error), "inds core initializes");
    error.clear();
    t.Expect(core.LoadSlot2Rom(slot2, error), "inds core loads Slot-2 ROM");
    error.clear();
    t.Expect(core.LoadPrimaryRom({}, error), "inds core allows firmware boot with no NDS ROM");

    error.clear();
    t.Expect(core.RunFrame(error), "inds core runs frame 1");
    t.Expect(!core.PollTransition().has_value(), "inds core has no transition on first frame");

    error.clear();
    t.Expect(core.RunFrame(error), "inds core runs frame 2");
    const auto transition = core.PollTransition();
    t.Expect(transition.has_value(), "inds core emits DS->GBA transition");
    if (transition.has_value()) {
        t.Expect(transition->from_mode == emugbds::core::EmuMode::DS, "inds transition origin mode is DS");
        t.Expect(transition->to_mode == emugbds::core::EmuMode::GBA, "inds transition destination mode is GBA");
    }
}

void TestIndsCoreDirectNdsBoot(TestContext& t) {
    const auto dir = MakeTempRoot() / "inds-core-nds";
    std::filesystem::create_directories(dir);
    const auto nds = dir / "test.nds";
    WriteFile(nds, BuildSyntheticNdsRom());

    emugbds::inds::Core core;
    std::string error;
    t.Expect(
        core.Configure(
            std::nullopt,
            std::nullopt,
            std::nullopt,
            emugbds::core::DsBootMode::Direct,
            false,
            error),
        "inds core configures for direct boot");
    error.clear();
    t.Expect(core.Initialize(error), "inds core initializes in direct mode");
    error.clear();
    t.Expect(core.LoadPrimaryRom(nds, error), "inds core loads NDS ROM");
    t.Expect(core.nds_header().has_value(), "inds core exposes parsed NDS header");
    t.Expect(core.arm9().r[15] == 0x02000000u, "inds ARM9 PC starts at NDS header entry");
    t.Expect(core.arm7().r[15] == 0x02380000u, "inds ARM7 PC starts at NDS header entry");

    error.clear();
    t.Expect(core.RunFrame(error), "inds core runs with NDS ROM");
    t.Expect(!core.PollTransition().has_value(), "inds direct NDS boot does not emit slot2 transition");
}

void TestRuntimeKernelSlot2Switch(TestContext& t) {
    const auto dir = MakeTempRoot() / "runtime-kernel";
    std::filesystem::create_directories(dir);
    const auto slot2 = dir / "test.gba";
    WriteFile(slot2, BuildSyntheticGbaRom());

    auto ds = std::make_unique<emugbds::cores::InHouseDsCoreAdapter>();
    auto gba = std::make_unique<emugbds::cores::InHouseGbaCoreAdapter>();
    emugbds::runtime::RuntimeKernel kernel(std::move(ds), std::move(gba));

    emugbds::runtime::BootConfig config;
    config.mode_hint = emugbds::core::ModeHint::DS;
    config.slot2_rom_path = slot2;
    config.ds_boot_mode = emugbds::core::DsBootMode::DsFirmware;
    config.trace_transition = false;

    std::string error;
    t.Expect(kernel.Boot(config, error), "runtime kernel boots in DS mode with Slot-2 ROM");
    t.Expect(kernel.active_mode() == emugbds::core::EmuMode::DS, "runtime starts in DS mode");

    emugbds::runtime::TickResult tick{};
    error.clear();
    t.Expect(kernel.Tick(tick, error), "runtime tick 1 succeeds");
    t.Expect(tick.status == emugbds::runtime::TickStatus::FrameAdvanced, "runtime tick 1 is frame advance");

    error.clear();
    t.Expect(kernel.Tick(tick, error), "runtime tick 2 succeeds");
    t.Expect(tick.status == emugbds::runtime::TickStatus::Transitioned, "runtime tick 2 reports transition");
    t.Expect(kernel.active_mode() == emugbds::core::EmuMode::GBA, "runtime switches to GBA mode");
    t.Expect(kernel.last_transition().has_value(), "runtime stores last transition");
}

void TestRuntimeKernelRejectsMissingGbaRom(TestContext& t) {
    auto ds = std::make_unique<emugbds::cores::InHouseDsCoreAdapter>();
    auto gba = std::make_unique<emugbds::cores::InHouseGbaCoreAdapter>();
    emugbds::runtime::RuntimeKernel kernel(std::move(ds), std::move(gba));

    emugbds::runtime::BootConfig config;
    config.mode_hint = emugbds::core::ModeHint::GBA;

    std::string error;
    t.Expect(!kernel.Boot(config, error), "runtime rejects GBA mode without ROM path");
    t.Expect(!error.empty(), "runtime missing GBA ROM reports error string");
}

void TestRuntimeKernelInjectInputToGba(TestContext& t) {
    const auto dir = MakeTempRoot() / "runtime-input";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "input.gba";
    WriteFile(gba_rom, BuildSyntheticGbaRom());

    auto ds = std::make_unique<emugbds::cores::InHouseDsCoreAdapter>();
    auto gba = std::make_unique<emugbds::cores::InHouseGbaCoreAdapter>();
    emugbds::cores::InHouseGbaCoreAdapter* gba_raw = gba.get();
    emugbds::runtime::RuntimeKernel kernel(std::move(ds), std::move(gba));

    emugbds::runtime::BootConfig config;
    config.mode_hint = emugbds::core::ModeHint::GBA;
    config.gba_rom_path = gba_rom;

    std::string error;
    t.Expect(kernel.Boot(config, error), "runtime input kernel boots in GBA mode");
    t.Expect(gba_raw->keyinput_state() == 0x03FFu, "runtime input initial key state is released");

    emugbds::runtime::InputSnapshot input{};
    input.gba_press_mask = emugbds::input::GbaKeyMask(emugbds::input::GbaKey::A);
    error.clear();
    t.Expect(kernel.InjectInput(input, error), "runtime inject input accepts GBA press");
    t.Expect(gba_raw->keyinput_state() == 0x03FEu, "runtime inject input propagates press mask");

    input = {};
    error.clear();
    t.Expect(kernel.InjectInput(input, error), "runtime inject input accepts empty snapshot");
    t.Expect(gba_raw->keyinput_state() == 0x03FEu, "runtime empty input leaves key state unchanged");

    input = {};
    input.gba_release_mask = emugbds::input::GbaKeyMask(emugbds::input::GbaKey::A);
    error.clear();
    t.Expect(kernel.InjectInput(input, error), "runtime inject input accepts release mask");
    t.Expect(gba_raw->keyinput_state() == 0x03FFu, "runtime inject input propagates release mask");
}

void TestRuntimeKernelProvidesGbaVideo(TestContext& t) {
    const auto dir = MakeTempRoot() / "runtime-video";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "video.gba";
    WriteFile(gba_rom, BuildSyntheticGbaRom());

    auto ds = std::make_unique<emugbds::cores::InHouseDsCoreAdapter>();
    auto gba = std::make_unique<emugbds::cores::InHouseGbaCoreAdapter>();
    emugbds::runtime::RuntimeKernel kernel(std::move(ds), std::move(gba));

    emugbds::runtime::BootConfig config;
    config.mode_hint = emugbds::core::ModeHint::GBA;
    config.gba_rom_path = gba_rom;

    std::string error;
    t.Expect(kernel.Boot(config, error), "runtime video kernel boots in GBA mode");

    emugbds::runtime::TickResult tick{};
    error.clear();
    t.Expect(kernel.Tick(tick, error), "runtime video tick succeeds");

    emugbds::runtime::OutputBundle output{};
    t.Expect(kernel.PollOutputs(output), "runtime video poll outputs succeeds");
    t.Expect(output.active_mode == emugbds::core::EmuMode::GBA, "runtime video output mode is GBA");
    t.Expect(output.video.has_value(), "runtime video frame is present");
    if (output.video.has_value()) {
        t.Expect(output.video->pixels != nullptr, "runtime video frame pointer is non-null");
        t.Expect(output.video->width == 240u, "runtime video width matches expected GBA width");
        t.Expect(output.video->height == 160u, "runtime video height matches expected GBA height");
    }
}

int RunUnitTests() {
    struct NamedTest {
        const char* name;
        void (*fn)(TestContext&);
    };

    const std::array<NamedTest, 30> tests = {{
        {"ingba_memory_bus", &TestIngbaMemoryBus},
        {"ingba_core_run_frame", &TestIngbaCoreRunFrame},
        {"ingba_cpu_fiq_banking", &TestIngbaCpuFiqBanking},
        {"ingba_fiq_user_bank_transfers", &TestIngbaFiqUserBankTransfers},
        {"ingba_irq_wakes_halt", &TestIngbaIrqWakesHalt},
        {"ingba_halt_wakes_with_masked_irq", &TestIngbaHaltWakesWithMaskedIrq},
        {"ingba_irq_returns_to_thumb", &TestIngbaIrqReturnsToThumb},
        {"ingba_multiply_instruction", &TestIngbaMultiplyInstruction},
        {"ingba_block_transfer_instruction", &TestIngbaBlockTransferInstruction},
        {"ingba_arm_halfword_transfer", &TestIngbaArmHalfwordTransfer},
        {"ingba_swi_cpuset_instruction", &TestIngbaSwiCpuSetInstruction},
        {"ingba_unsupported_swi_class_counter", &TestIngbaUnsupportedSwiClassCounter},
        {"ingba_swi_lz77_uncomp_wram", &TestIngbaSwiLz77UnCompWram},
        {"ingba_swi_rl_uncomp_wram", &TestIngbaSwiRlUnCompWram},
        {"ingba_swi_bgaffineset", &TestIngbaSwiBgAffineSet},
        {"ingba_input_masks", &TestIngbaInputMasks},
        {"ingba_thumb_alu_and_branch", &TestIngbaThumbAluAndBranch},
        {"ingba_thumb_load_store", &TestIngbaThumbLoadStore},
        {"ingba_thumb_bl_interworking", &TestIngbaThumbBlInterworking},
        {"ingba_video_mode3_render", &TestIngbaVideoMode3Render},
        {"ingba_video_mode4_render", &TestIngbaVideoMode4Render},
        {"ingba_video_obj_render", &TestIngbaVideoObjRender},
        {"ingba_video_mode2_render", &TestIngbaVideoMode2Render},
        {"inds_memory_bus", &TestIndsMemoryBus},
        {"inds_core_firmware_slot2_transition", &TestIndsCoreFirmwareSlot2Transition},
        {"inds_core_direct_nds_boot", &TestIndsCoreDirectNdsBoot},
        {"runtime_kernel_inject_input_to_gba", &TestRuntimeKernelInjectInputToGba},
        {"runtime_kernel_provides_gba_video", &TestRuntimeKernelProvidesGbaVideo},
        {"runtime_kernel_slot2_switch", &TestRuntimeKernelSlot2Switch},
        {"runtime_kernel_reject_missing_gba_rom", &TestRuntimeKernelRejectsMissingGbaRom},
    }};

    int total_failures = 0;
    for (const auto& test : tests) {
        std::cout << "[TEST] " << test.name << "\n";
        TestContext context;
        test.fn(context);
        if (context.failures == 0) {
            std::cout << "  PASS\n";
        } else {
            std::cout << "  FAIL (" << context.failures << ")\n";
            total_failures += context.failures;
        }
    }

    if (total_failures == 0) {
        std::cout << "All tests passed.\n";
        return 0;
    }

    std::cerr << "Test failures: " << total_failures << "\n";
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--emit-fixtures") {
        EmitFixtures(std::filesystem::path(argv[2]));
        std::cout << "Wrote fixtures to " << argv[2] << "\n";
        return 0;
    }
    return RunUnitTests();
}
