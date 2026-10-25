#include "emugbds/arm/ArmCpu.h"
#include "emugbds/core/Types.h"
#include "emugbds/cores/InHouseDsCoreAdapter.h"
#include "emugbds/cores/InHouseGbaCoreAdapter.h"
#include "emugbds/frontend/Settings.h"
#include "emugbds/input/GbaKeys.h"
#include "emugbds/inds/Backup.h"
#include "emugbds/inds/Core.h"
#include "emugbds/inds/MemoryBus.h"
#include "emugbds/ingba/Core.h"
#include "emugbds/ingba/MemoryBus.h"
#include "emugbds/runtime/RuntimeKernel.h"
#include "emugbds/runtime/Types.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
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

void TestIngbaBackup(TestContext& t) {
    using emugbds::ingba::Backup;
    using emugbds::ingba::BackupType;
    const auto dir = MakeTempRoot() / "ingba-backup";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::string error;

    const std::vector<uint8_t> rom = BuildSyntheticGbaRom();
    const auto with_id = [&](const char* id, size_t offset = 0x400u) {
        std::vector<uint8_t> bytes = rom;
        std::memcpy(bytes.data() + offset, id, std::strlen(id));
        return bytes;
    };
    t.Expect(Backup::Detect(rom) == BackupType::None, "a ROM without an ID string has no save chip");
    t.Expect(Backup::Detect(with_id("SRAM_V113")) == BackupType::Sram &&
        Backup::Detect(with_id("SRAM_F_V102")) == BackupType::Sram &&
        Backup::Detect(with_id("EEPROM_V124")) == BackupType::Eeprom &&
        Backup::Detect(with_id("FLASH_V126")) == BackupType::Flash64K &&
        Backup::Detect(with_id("FLASH512_V131")) == BackupType::Flash64K &&
        Backup::Detect(with_id("FLASH1M_V103")) == BackupType::Flash128K,
        "GBATEK's backup ID strings select the save chip");
    t.Expect(Backup::Detect(with_id("SRAM_V113", 0x401u)) == BackupType::None, "ID strings must be word aligned");

    // SRAM through the bus: an 8-bit bus mirrored every 32 KiB, saved to <rom>.sav.
    WriteFile(dir / "sram.gba", with_id("SRAM_V113"));
    {
        emugbds::ingba::MemoryBus bus;
        t.Expect(bus.LoadRom(dir / "sram.gba", error), "an SRAM game loads");
        bus.Write8(0x0E000010u, 0x42u);
        t.Expect(bus.Read8(0x0E008010u) == 0x42u && bus.Read8(0x0FFF8010u) == 0x42u, "SRAM mirrors every 32 KiB up to FFFFFFFh");
        t.Expect(bus.Read16(0x0E000010u) == 0x4242u && bus.Read32(0x0E000010u) == 0x42424242u,
            "wider SRAM reads repeat the byte (8-bit bus)");
        bus.Write16(0x0E000020u, 0x1234u);
        t.Expect(bus.Read8(0x0E000020u) == 0x34u && bus.Read8(0x0E000021u) == 0xFFu, "a halfword store reaches SRAM as one byte");
        t.Expect(bus.backup().dirty() && bus.backup().Flush(error) && std::filesystem::file_size(dir / "sram.sav") == 0x8000u &&
            !std::filesystem::exists(dir / "sram.sav.tmp"), "SRAM is written to a 32 KiB save file");
    }
    {
        emugbds::ingba::MemoryBus bus;
        t.Expect(bus.LoadRom(dir / "sram.gba", error) && bus.Read8(0x0E000010u) == 0x42u && !bus.backup().dirty(),
            "the save file loads back with the ROM");
    }

    // FLASH 64K: Macronix ID 1CC2h and a single bank.
    const auto command = [](Backup& chip, uint8_t value) {
        chip.Write8(0x5555u, 0xAAu);
        chip.Write8(0x2AAAu, 0x55u);
        chip.Write8(0x5555u, value);
    };
    Backup flash;
    t.Expect(flash.Load(BackupType::Flash64K, dir / "flash64.sav", error), "a blank 64K FLASH starts");
    command(flash, 0x90u);
    t.Expect(flash.Read8(0u) == 0xC2u && flash.Read8(1u) == 0x1Cu, "64K FLASH identifies as Macronix 1CC2h");
    flash.Write8(0u, 0xF0u);
    command(flash, 0xA0u);
    flash.Write8(0x0123u, 0x5Au);
    command(flash, 0xB0u);
    flash.Write8(0u, 1u);
    t.Expect(flash.Read8(0x0123u) == 0x5Au, "64K FLASH ignores bank switching");
    t.Expect(flash.Flush(error) && std::filesystem::file_size(dir / "flash64.sav") == 0x10000u, "64K FLASH saves 64 KiB");
    Backup larger;
    t.Expect(larger.Load(BackupType::Flash128K, dir / "flash64.sav", error) && larger.Read8(0x0123u) == 0x5Au &&
        larger.data().size() == 0x20000u, "a 64 KiB save loads into the first bank of a 128K chip");
    Backup smaller;
    error.clear();
    t.Expect(!smaller.Load(BackupType::Sram, dir / "flash64.sav", error) && !error.empty(),
        "a save file larger than the chip is refused, not truncated");

    // EEPROM, driven by DMA3 bit streams the way games do it (one bit per halfword).
    WriteFile(dir / "eeprom.gba", with_id("EEPROM_V124"));
    emugbds::ingba::MemoryBus bus;
    error.clear();
    t.Expect(bus.LoadRom(dir / "eeprom.gba", error), "an EEPROM game loads");
    const auto dma3 = [&](uint32_t source, uint32_t destination, uint32_t count) {
        bus.Write32(0x040000D4u, source);
        bus.Write32(0x040000D8u, destination);
        bus.Write32(0x040000DCu, count | (0x8000u << 16u)); // enable, immediate, 16-bit, incrementing
    };
    const auto send = [&](const std::vector<uint8_t>& bits) {
        for (size_t i = 0u; i < bits.size(); ++i) {
            bus.Write16(0x02000000u + static_cast<uint32_t>(i) * 2u, bits[i]);
        }
        dma3(0x02000000u, 0x0D000000u, static_cast<uint32_t>(bits.size()));
    };
    const auto append = [](std::vector<uint8_t>& bits, uint64_t value, uint32_t count) {
        for (uint32_t i = count; i-- > 0u;) {
            bits.push_back(static_cast<uint8_t>((value >> i) & 1u));
        }
    };
    std::vector<uint8_t> write = {1u, 0u};
    append(write, 3u, 6u);                     // block 3 (6-bit address: a 512-byte chip)
    append(write, 0x0123456789ABCDEFull, 64u);
    write.push_back(0u);
    send(write);
    t.Expect(bus.Read16(0x0D000000u) == 1u, "the EEPROM reports ready after a write");
    std::vector<uint8_t> read = {1u, 1u};
    append(read, 3u, 6u);
    read.push_back(0u);
    send(read);
    dma3(0x0D000000u, 0x02001000u, 68u);
    uint64_t value = 0u;
    for (uint32_t i = 4u; i < 68u; ++i) {
        value = (value << 1u) | (bus.Read16(0x02001000u + i * 2u) & 1u);
    }
    t.Expect(value == 0x0123456789ABCDEFull, "the EEPROM reads back what was written, after 4 dummy bits");
    t.Expect(bus.backup().data().size() == 0x200u && bus.backup().Flush(error) &&
        std::filesystem::file_size(dir / "eeprom.sav") == 0x200u, "a 6-bit address means a 512-byte EEPROM save");
    t.Expect(bus.Read8(0x08000000u) == rom[0], "ROM below the EEPROM window still reads normally");

    // The same 512-byte save used with 14-bit addresses grows to 8K; block 3 keeps its place.
    write = {1u, 0u};
    append(write, 64u, 14u);
    append(write, 0xFEDCBA9876543210ull, 64u);
    write.push_back(0u);
    send(write);
    t.Expect(bus.backup().data().size() == 0x2000u && bus.backup().data()[3u * 8u] == 0x01u &&
        bus.backup().data()[64u * 8u] == 0xFEu, "a 14-bit EEPROM address grows a 512-byte save to 8K without aliasing");

    // A ROM whose save is refused leaves the running game's save chip in place.
    emugbds::ingba::MemoryBus keep;
    t.Expect(keep.LoadRom(dir / "sram.gba", error), "an SRAM game loads before a failing one");
    WriteFile(dir / "oversize.gba", with_id("SRAM_V113"));
    WriteFile(dir / "oversize.sav", std::vector<uint8_t>(0x10000u, 0u));
    error.clear();
    t.Expect(!keep.LoadRom(dir / "oversize.gba", error) && keep.backup().type() == BackupType::Sram &&
        keep.Read8(0x0E000010u) == 0x42u, "a refused save file does not replace the running game's save");
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
    auto rom_bytes = BuildSyntheticGbaRom();
    std::memcpy(rom_bytes.data() + 0x200u, "FLASH1M_V103", 12u); // ask for the 128K FLASH chip
    WriteFile(gba_rom, rom_bytes);
    std::filesystem::remove(dir / "test.sav");
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
    bus.SetLcdStatus(0u, true, true, false);
    bus.Write16(0x04000004u, 0x0028u);
    t.Expect(bus.Read16(0x04000004u) == 0x002Bu, "ingba DISPSTAT write keeps read-only LCD status bits");
    bus.Write16(0x04000100u, 0xFFFEu);
    bus.Write16(0x04000102u, 0x00C0u);
    bus.AdvanceTimers(1u);
    t.Expect(bus.Read16(0x04000100u) == 0xFFFFu, "ingba timer advances at its configured rate");
    bus.AdvanceTimers(1u);
    t.Expect(bus.Read16(0x04000100u) == 0xFFFEu, "ingba timer reloads after overflow");
    t.Expect((bus.Read16(0x04000202u) & 0x0008u) != 0u, "ingba timer overflow requests its IRQ");
    emugbds::ingba::MemoryBus timer_bus;
    t.Expect(timer_bus.CyclesUntilTimerOverflow() == 0xFFFFFFFFu, "ingba stopped timers never overflow");
    timer_bus.Write16(0x04000100u, 0xFFF0u);
    timer_bus.Write16(0x04000102u, 0x0081u); // Enabled, prescaler 64.
    t.Expect(timer_bus.CyclesUntilTimerOverflow() == 1024u, "ingba timer overflow bound covers the prescaler");
    timer_bus.AdvanceTimers(24u);
    t.Expect(timer_bus.CyclesUntilTimerOverflow() == 1000u, "ingba timer overflow bound includes prescaler phase");
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

// Runs the shared ARM CPU directly on 64 KiB of flat RAM, with a CP15 stand-in.
struct ArmCpuHarness final : emugbds::arm::ICpuHost {
    struct RamBus final : emugbds::arm::IBus {
        std::vector<uint8_t> ram = std::vector<uint8_t>(0x10000u, 0u);
        uint8_t Read8(uint32_t a) const override { return ram[a & 0xFFFFu]; }
        uint16_t Read16(uint32_t a) const override { return static_cast<uint16_t>(Read8(a) | (Read8(a + 1u) << 8u)); }
        uint32_t Read32(uint32_t a) const override { return Read16(a) | (static_cast<uint32_t>(Read16(a + 2u)) << 16u); }
        void Write8(uint32_t a, uint8_t v) override { ram[a & 0xFFFFu] = v; }
        void Write16(uint32_t a, uint16_t v) override { Write8(a, static_cast<uint8_t>(v)); Write8(a + 1u, static_cast<uint8_t>(v >> 8u)); }
        void Write32(uint32_t a, uint32_t v) override { Write16(a, static_cast<uint16_t>(v)); Write16(a + 2u, static_cast<uint16_t>(v >> 16u)); }
    };

    explicit ArmCpuHarness(emugbds::arm::ArmArchitecture architecture) : cpu(bus, *this, architecture) {
        cpu.Reset(); // System mode, ARM state, PC = 0.
    }
    bool OnSoftwareInterrupt(uint8_t, uint32_t, bool, std::string&) override { return false; }
    bool OnCoprocessorRead(uint32_t cp, uint32_t, uint32_t crn, uint32_t, uint32_t, uint32_t& value) override {
        value = cp15_read_value + crn;
        return cp == 15u;
    }
    bool OnCoprocessorWrite(uint32_t cp, uint32_t, uint32_t crn, uint32_t crm, uint32_t, uint32_t value) override {
        cp15_written = (crn << 28u) | (crm << 24u) | (value & 0x00FFFFFFu);
        return cp == 15u;
    }
    void Arm(std::initializer_list<uint32_t> words) {
        uint32_t address = 0u;
        for (uint32_t word : words) {
            bus.Write32(address, word);
            address += 4u;
        }
    }
    void Run(uint32_t steps) {
        std::string error;
        for (uint32_t i = 0u; i < steps; ++i) {
            cpu.Step(error);
        }
    }
    emugbds::arm::CpuState& s() { return cpu.state(); }

    RamBus bus;
    emugbds::arm::ArmCpu cpu;
    uint32_t cp15_read_value = 0x00012000u;
    uint32_t cp15_written = 0u;
};

constexpr uint32_t kQFlag = 1u << 27u;

void TestArmV5ClzAndSaturation(TestContext& t) {
    using emugbds::arm::ArmArchitecture;
    const uint32_t inputs[] = {0u, 1u, 0x80000000u, 0x00010000u};
    const uint32_t expected[] = {32u, 31u, 0u, 15u};
    for (size_t i = 0; i < 4u; ++i) {
        ArmCpuHarness h(ArmArchitecture::V5TE);
        h.Arm({0xE16F0F11u}); // CLZ r0, r1
        h.s().r[1] = inputs[i];
        h.Run(1u);
        t.Expect(h.s().r[0] == expected[i], "ARMv5 CLZ counts leading zeros");
    }

    ArmCpuHarness q(ArmArchitecture::V5TE);
    q.Arm({0xE1012050u, 0xE1213050u, 0xE1414050u}); // QADD r2,r0,r1 ; QSUB r3,r0,r1 ; QDADD r4,r0,r1
    q.s().r[0] = 0x7FFFFFF0u;
    q.s().r[1] = 0x00000100u;
    q.Run(1u);
    t.Expect(q.s().r[2] == 0x7FFFFFFFu, "ARMv5 QADD saturates at INT32_MAX");
    t.Expect((q.s().cpsr & kQFlag) != 0u, "ARMv5 QADD sets Q on saturation");
    q.s().cpsr &= ~kQFlag;
    q.Run(1u);
    t.Expect(q.s().r[3] == 0x7FFFFEF0u, "ARMv5 QSUB subtracts without saturating");
    t.Expect((q.s().cpsr & kQFlag) == 0u, "ARMv5 QSUB leaves Q clear without saturation");
    q.s().r[0] = 0u;
    q.s().r[1] = 0xC0000000u; // Doubling -2^30 reaches INT32_MIN exactly.
    q.Run(1u);
    t.Expect(q.s().r[4] == 0x80000000u, "ARMv5 QDADD doubles the second operand");

    ArmCpuHarness v4(ArmArchitecture::V4T);
    v4.Arm({0xE16F0F11u});
    v4.s().r[0] = 0x55u;
    v4.s().r[1] = 1u;
    v4.Run(1u);
    t.Expect(v4.s().r[0] == 0x55u, "ARMv4 does not decode CLZ");
}

void TestArmV5HalfwordMultiplies(TestContext& t) {
    using emugbds::arm::ArmArchitecture;
    ArmCpuHarness h(ArmArchitecture::V5TE);
    h.Arm({
        0xE1600281u, // SMULBB r0, r1, r2
        0xE16005A1u, // SMULTB r0, r1, r5
        0xE1003281u, // SMLABB r0, r1, r2, r3
        0xE12002A1u, // SMULWB r0, r1, r2
        0xE1454281u, // SMLALBB r4, r5, r1, r2
    });
    h.s().r[1] = 0x0003FFFEu; // top = 3, bottom = -2
    h.s().r[2] = 0x00000005u;
    h.Run(1u);
    t.Expect(h.s().r[0] == static_cast<uint32_t>(-10), "ARMv5 SMULBB multiplies signed bottom halves");
    h.s().r[5] = 7u;
    h.Run(1u);
    t.Expect(h.s().r[0] == 21u, "ARMv5 SMULTB uses the top half of Rm");
    h.s().r[3] = 0x7FFFFFFFu;
    h.s().r[2] = 1u;
    h.s().cpsr &= ~kQFlag;
    h.Run(1u);
    t.Expect(h.s().r[0] == 0x7FFFFFFDu, "ARMv5 SMLABB accumulates");
    h.s().r[2] = 0x00000003u;
    h.s().r[1] = 0x00020000u;
    h.Run(1u);
    t.Expect(h.s().r[0] == 6u, "ARMv5 SMULWB keeps the top 32 bits of the 48-bit product");
    h.s().r[4] = 0xFFFFFFFFu;
    h.s().r[5] = 0u;
    h.s().r[1] = 0x00000002u;
    h.s().r[2] = 0x00000003u;
    h.Run(1u);
    t.Expect(h.s().r[4] == 5u && h.s().r[5] == 1u, "ARMv5 SMLALBB carries into RdHi");

    ArmCpuHarness overflow(ArmArchitecture::V5TE);
    overflow.Arm({0xE1003281u}); // SMLABB r0, r1, r2, r3
    overflow.s().r[1] = 2u;
    overflow.s().r[2] = 1u;
    overflow.s().r[3] = 0x7FFFFFFFu;
    overflow.Run(1u);
    t.Expect(overflow.s().r[0] == 0x80000001u, "ARMv5 SMLAxy wraps instead of saturating");
    t.Expect((overflow.s().cpsr & kQFlag) != 0u, "ARMv5 SMLAxy sets Q on accumulate overflow");
}

void TestArmV5BranchesAndInterworking(TestContext& t) {
    using emugbds::arm::ArmArchitecture;
    ArmCpuHarness blx(ArmArchitecture::V5TE);
    blx.Arm({0xFB000000u}); // BLX label with H=1: target = 0 + 8 + 2
    blx.Run(1u);
    t.Expect(blx.s().r[15] == 0x0Au && blx.s().thumb, "ARMv5 BLX label switches to Thumb at a halfword target");
    t.Expect(blx.s().r[14] == 4u, "ARMv5 BLX label sets LR to the next ARM instruction");

    ArmCpuHarness blx_reg(ArmArchitecture::V5TE);
    blx_reg.Arm({0xE12FFF33u}); // BLX r3
    blx_reg.s().r[3] = 0x101u;
    blx_reg.Run(1u);
    t.Expect(blx_reg.s().r[15] == 0x100u && blx_reg.s().thumb && blx_reg.s().r[14] == 4u, "ARMv5 BLX Rm calls with interworking");

    ArmCpuHarness thumb_blx(ArmArchitecture::V5TE);
    thumb_blx.bus.Write16(0x40u, 0xF000u); // BL prefix: LR = 0x44
    thumb_blx.bus.Write16(0x42u, 0xE802u); // BLX suffix: target = (LR + 4) & ~3
    thumb_blx.bus.Write16(0x44u, 0x4798u); // BLX r3 (hi-register form)
    thumb_blx.s().r[15] = 0x40u;
    thumb_blx.cpu.SetThumbState(true);
    thumb_blx.Run(2u);
    t.Expect(thumb_blx.s().r[15] == 0x48u && !thumb_blx.s().thumb, "ARMv5 Thumb BLX suffix switches to ARM");
    t.Expect(thumb_blx.s().r[14] == 0x45u, "ARMv5 Thumb BLX suffix returns to Thumb");
    thumb_blx.s().r[15] = 0x44u;
    thumb_blx.cpu.SetThumbState(true);
    thumb_blx.s().r[3] = 0x200u;
    thumb_blx.Run(1u);
    t.Expect(thumb_blx.s().r[15] == 0x200u && !thumb_blx.s().thumb && thumb_blx.s().r[14] == 0x47u, "ARMv5 Thumb BLX Rs calls ARM code");

    for (const auto architecture : {ArmArchitecture::V4T, ArmArchitecture::V5TE}) {
        const bool v5 = architecture == ArmArchitecture::V5TE;
        ArmCpuHarness ldr(architecture);
        ldr.Arm({0xE590F000u}); // LDR pc, [r0]
        ldr.s().r[0] = 0x100u;
        ldr.bus.Write32(0x100u, 0x301u);
        ldr.Run(1u);
        t.Expect(ldr.s().thumb == v5 && ldr.s().r[15] == 0x300u, "LDR pc interworks only on ARMv5");

        ArmCpuHarness ldm(architecture);
        ldm.Arm({0xE8908000u}); // LDMIA r0, {pc}
        ldm.s().r[0] = 0x100u;
        ldm.bus.Write32(0x100u, 0x301u);
        ldm.Run(1u);
        t.Expect(ldm.s().thumb == v5, "LDM {pc} interworks only on ARMv5");

        ArmCpuHarness pop(architecture);
        pop.bus.Write16(0x40u, 0xBD00u); // POP {pc}
        pop.s().r[15] = 0x40u;
        pop.s().r[13] = 0x100u;
        pop.cpu.SetThumbState(true);
        pop.bus.Write32(0x100u, 0x300u); // Bit 0 clear: ARMv5 returns to ARM, ARMv4 stays in Thumb.
        pop.Run(1u);
        t.Expect(pop.s().thumb == !v5 && pop.s().r[15] == 0x300u, "Thumb POP {pc} interworks only on ARMv5");
    }

    ArmCpuHarness nv(ArmArchitecture::V4T);
    nv.Arm({0xFB000000u});
    nv.Run(1u);
    t.Expect(nv.s().r[15] == 4u && !nv.s().thumb, "ARMv4 treats cond=1111 as never");
}

void TestArmMemoryAdditions(TestContext& t) {
    using emugbds::arm::ArmArchitecture;
    ArmCpuHarness d(ArmArchitecture::V5TE);
    d.Arm({0xE1C020D0u, 0xE1C120F0u}); // LDRD r2, [r0] ; STRD r2, [r1]
    d.s().r[0] = 0x100u;
    d.s().r[1] = 0x200u;
    d.bus.Write32(0x100u, 0x11111111u);
    d.bus.Write32(0x104u, 0x22222222u);
    d.Run(2u);
    t.Expect(d.s().r[2] == 0x11111111u && d.s().r[3] == 0x22222222u, "ARMv5 LDRD loads a register pair");
    t.Expect(d.bus.Read32(0x200u) == 0x11111111u && d.bus.Read32(0x204u) == 0x22222222u, "ARMv5 STRD stores a register pair");

    for (const auto architecture : {ArmArchitecture::V4T, ArmArchitecture::V5TE}) {
        ArmCpuHarness swap(architecture);
        swap.Arm({0xE1002091u, 0xE1403091u}); // SWP r2, r1, [r0] ; SWPB r3, r1, [r0]
        swap.s().r[0] = 0x100u;
        swap.s().r[1] = 0xAABBCCDDu;
        swap.bus.Write32(0x100u, 0x12345678u);
        swap.Run(1u);
        t.Expect(swap.s().r[2] == 0x12345678u && swap.bus.Read32(0x100u) == 0xAABBCCDDu, "SWP exchanges a word");
        swap.Run(1u);
        t.Expect(swap.s().r[3] == 0xDDu && swap.bus.Read8(0x100u) == 0xDDu, "SWPB exchanges a byte");
    }

    ArmCpuHarness pld(ArmArchitecture::V5TE);
    pld.Arm({0xF5D0F000u}); // PLD [r0]
    pld.Run(1u);
    t.Expect(pld.s().r[15] == 4u && pld.cpu.unsupported_arm_stats().generic == 0u, "ARMv5 PLD is a no-op");

    // Base register inside the list.
    for (const auto architecture : {ArmArchitecture::V4T, ArmArchitecture::V5TE}) {
        const bool v5 = architecture == ArmArchitecture::V5TE;
        ArmCpuHarness ldm_first(architecture);
        ldm_first.Arm({0xE8B00003u}); // LDMIA r0!, {r0, r1}: base is not the last register
        ldm_first.s().r[0] = 0x100u;
        ldm_first.bus.Write32(0x100u, 0x55u);
        ldm_first.Run(1u);
        t.Expect(ldm_first.s().r[0] == (v5 ? 0x108u : 0x55u), "LDM base-in-list writeback follows the architecture");

        ArmCpuHarness ldm_last(architecture);
        ldm_last.Arm({0xE8B10003u}); // LDMIA r1!, {r0, r1}: base is the last register
        ldm_last.s().r[1] = 0x100u;
        ldm_last.bus.Write32(0x104u, 0x66u);
        ldm_last.Run(1u);
        t.Expect(ldm_last.s().r[1] == 0x66u, "LDM keeps a loaded base that is the last register");

        ArmCpuHarness stm(architecture);
        stm.Arm({0xE8A10003u}); // STMIA r1!, {r0, r1}: base is not the first register
        stm.s().r[1] = 0x100u;
        stm.Run(1u);
        t.Expect(stm.bus.Read32(0x104u) == (v5 ? 0x100u : 0x108u), "STM stores the old base on ARMv5, the new one on ARMv4");

        ArmCpuHarness thumb_stm(architecture);
        thumb_stm.bus.Write16(0x40u, 0xC103u); // STMIA r1!, {r0, r1}
        thumb_stm.s().r[15] = 0x40u;
        thumb_stm.s().r[1] = 0x100u;
        thumb_stm.cpu.SetThumbState(true);
        thumb_stm.Run(1u);
        t.Expect(thumb_stm.bus.Read32(0x104u) == (v5 ? 0x100u : 0x108u), "Thumb STMIA base-in-list follows the architecture");
    }
}

void TestArmV5CoprocessorTransfers(TestContext& t) {
    using emugbds::arm::ArmArchitecture;
    ArmCpuHarness h(ArmArchitecture::V5TE);
    h.Arm({
        0xEE110F10u, // MRC p15, 0, r0, c1, c0, 0
        0xEE091F11u, // MCR p15, 0, r1, c9, c1, 0
        0xEE11FF10u, // MRC p15, 0, r15, c1, c0, 0: flags only
        0xEE110E10u, // MRC p14, ...: absent coprocessor
    });
    h.s().r[1] = 0x00ABCDEFu;
    h.cp15_read_value = 0xA0000000u;
    h.Run(1u);
    t.Expect(h.s().r[0] == 0xA0000001u, "ARMv5 MRC reads a coprocessor register through the host");
    h.Run(1u);
    t.Expect(h.cp15_written == 0x91ABCDEFu, "ARMv5 MCR writes a coprocessor register through the host");
    h.Run(1u);
    t.Expect((h.s().cpsr >> 28u) == 0xAu && h.s().r[15] == 12u, "ARMv5 MRC to r15 copies only NZCV");
    h.Run(1u);
    t.Expect(h.cpu.unsupported_arm_stats().generic == 1u, "ARMv5 MRC to an absent coprocessor is unsupported");
}

void TestIngbaApuFifoMixing(TestContext& t) {
    emugbds::ingba::Apu apu;
    constexpr uint16_t kAOnTimer0BOnTimer1 = 0x4304u; // A: 100%, left+right, timer 0. B: timer 1, muted.
    apu.WriteFifo(0u, 0x40u);
    t.Expect(apu.OnTimerOverflow(0u, kAOnTimer0BOnTimer1) == 0x1u, "ingba APU requests a FIFO A refill when low");
    apu.Advance(512u, kAOnTimer0BOnTimer1, 0x0080u);
    t.Expect(apu.samples().size() == 2u, "ingba APU emits one stereo sample per 512 cycles");
    t.Expect(apu.samples()[0] == 64 * 4 * 64 && apu.samples()[1] == 64 * 4 * 64, "ingba APU scales a 100% FIFO sample");
    apu.ClearSamples();
    apu.Advance(512u, 0x4300u, 0x0080u);
    t.Expect(apu.samples()[0] == 64 * 2 * 64, "ingba APU halves a 50% FIFO sample");
    apu.ClearSamples();
    apu.Advance(512u, kAOnTimer0BOnTimer1, 0x0000u);
    t.Expect(apu.samples()[0] == 0, "ingba APU is silent when the master enable is off");
    apu.ClearSamples();
    apu.WriteFifo(0u, 0x7Fu);
    apu.WriteFifo(1u, 0x7Fu);
    apu.OnTimerOverflow(0u, 0x330Cu); // A and B on timer 0, both 100%, both sides.
    apu.Advance(512u, 0x330Cu, 0x0080u);
    t.Expect(apu.samples()[0] == 0x1FF * 64, "ingba APU clips the mixed output to the 10-bit range");
}

void TestIngbaSoundDmaRefillsFifo(TestContext& t) {
    const auto dir = MakeTempRoot() / "ingba-sound-dma";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "sound_dma.gba";
    WriteFile(gba_rom, BuildSyntheticGbaRom());

    emugbds::ingba::MemoryBus bus;
    std::string error;
    t.Expect(bus.LoadRom(gba_rom, error), "ingba sound DMA bus loads ROM");
    for (uint32_t i = 0u; i < 64u; ++i) {
        bus.Write8(0x02000000u + i, static_cast<uint8_t>(i + 1u));
    }
    bus.Write32(0x040000BCu, 0x02000000u);   // DMA1 source.
    bus.Write32(0x040000C0u, 0x040000A0u);   // DMA1 destination: FIFO A.
    bus.Write16(0x040000C6u, 0xB600u);       // Enable, sound FIFO timing, repeat, 32-bit.
    bus.Write16(0x04000084u, 0x0080u);       // Master sound enable.
    bus.Write16(0x04000082u, 0x4304u);       // FIFO A on timer 0 at 100%, both sides.
    bus.Write16(0x04000100u, 0xFFFFu);       // Timer 0 overflows every cycle.
    bus.Write16(0x04000102u, 0x0080u);

    const auto play_first_sample = [&]() {
        bus.apu().ClearSamples();
        bus.AdvanceTimers(1u);   // Empty FIFO asks for data.
        bus.AdvanceTimers(1u);   // First byte plays; 15 left, so DMA refills again.
        bus.AdvanceTimers(510u); // Sample point: output still holds the first byte.
        return bus.apu().samples().empty() ? -1 : bus.apu().samples()[0];
    };
    t.Expect(play_first_sample() == 1 * 4 * 64, "ingba sound DMA feeds the FIFO and the sample holds its first byte");
    t.Expect(bus.Read32(0x040000BCu) == 0x02000000u, "ingba DMA leaves the programmed source register unchanged");
    t.Expect((bus.Read16(0x040000C6u) & 0x8000u) != 0u, "ingba sound DMA stays enabled in repeat mode");

    // Sound engines restart their buffer by toggling enable without rewriting SAD.
    bus.Write16(0x040000C6u, 0x3600u);
    bus.Write16(0x04000082u, 0x4304u | 0x0800u); // Also reset FIFO A.
    t.Expect(bus.Read16(0x04000082u) == 0x4304u, "ingba SOUNDCNT_H FIFO reset bit is not stored");
    bus.Write16(0x040000C6u, 0xB600u);
    t.Expect(play_first_sample() == 1 * 4 * 64, "ingba re-enabling sound DMA restarts from the programmed source");
}

void TestIngbaDmaLatchesCountOnEnable(TestContext& t) {
    const auto dir = MakeTempRoot() / "ingba-dma-latch";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "dma_latch.gba";
    WriteFile(gba_rom, BuildSyntheticGbaRom());

    emugbds::ingba::MemoryBus bus;
    std::string error;
    t.Expect(bus.LoadRom(gba_rom, error), "ingba DMA latch bus loads ROM");
    bus.Write32(0x02000000u, 0x11111111u);
    bus.Write32(0x02000004u, 0x22222222u);
    bus.Write32(0x040000D4u, 0x02000000u); // DMA3 source.
    bus.Write32(0x040000D8u, 0x02000100u); // DMA3 destination.
    bus.Write16(0x040000DCu, 1u);
    bus.Write16(0x040000DEu, 0x9400u);     // Enable, VBlank timing, 32-bit.
    bus.Write16(0x040000DCu, 2u);          // Rewritten while armed: must not affect this run.
    bus.TriggerDmaTiming(0x1u);
    t.Expect(bus.Read32(0x02000100u) == 0x11111111u, "ingba VBlank DMA copies its first unit");
    t.Expect(bus.Read32(0x02000104u) == 0u, "ingba DMA uses the word count latched on enable");
}

void TestIngbaTimerWakesHaltPromptly(TestContext& t) {
    const auto dir = MakeTempRoot() / "ingba-timer-halt";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "timer_halt.gba";

    constexpr uint32_t kLiteralBase = 0x40u;
    std::vector<uint8_t> rom = BuildSyntheticGbaRom();
    auto write_ldr = [&](uint32_t address, uint32_t rd, uint32_t literal_address) {
        WriteWordLe(rom, address, EncodeLdrLiteralAt(rd, address, literal_address));
    };

    write_ldr(0x00u, 1u, kLiteralBase + 0x00u);
    WriteWordLe(rom, 0x04u, EncodeMovImm(0u, 8u));
    WriteWordLe(rom, 0x08u, EncodeStrhImm(0u, 1u, 0u));       // IE = timer 0.
    write_ldr(0x0Cu, 1u, kLiteralBase + 0x04u);
    write_ldr(0x10u, 0u, kLiteralBase + 0x08u);
    WriteWordLe(rom, 0x14u, EncodeStrReg(0u, 1u, 0u));        // Timer 0: reload 0xFFF0, IRQ, start.
    WriteWordLe(rom, 0x18u, EncodeSwi(0x02u));                // HALT until the timer overflows.
    write_ldr(0x1Cu, 2u, kLiteralBase + 0x0Cu);
    WriteWordLe(rom, 0x20u, EncodeLdrhImm(3u, 2u, 0u));       // Sample DISPSTAT on wake.
    WriteWordLe(rom, 0x24u, EncodeBranchRelativeWords(-2));

    WriteWordLe(rom, kLiteralBase + 0x00u, 0x04000200u);
    WriteWordLe(rom, kLiteralBase + 0x04u, 0x04000100u);
    WriteWordLe(rom, kLiteralBase + 0x08u, 0x00C0FFF0u);
    WriteWordLe(rom, kLiteralBase + 0x0Cu, 0x04000004u);
    WriteFile(gba_rom, rom);

    emugbds::ingba::Core core;
    std::string error;
    t.Expect(core.Initialize(error), "ingba timer HALT core initializes");
    error.clear();
    t.Expect(core.LoadRom(gba_rom, error), "ingba timer HALT core loads ROM");
    error.clear();
    t.Expect(core.RunFrame(error), "ingba timer HALT core runs frame");
    t.Expect(core.cpu().r[15] == 0x08000024u, "ingba timer IRQ wakes HALT");
    t.Expect((core.cpu().r[3] & 0x0002u) == 0u, "ingba timer IRQ wakes HALT before the line's HBlank");
}

void TestIngbaVCountTargetWriteMidLine(TestContext& t) {
    const auto dir = MakeTempRoot() / "ingba-vcount-target";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "vcount_target.gba";

    constexpr uint32_t kLiteralBase = 0x40u;
    std::vector<uint8_t> rom = BuildSyntheticGbaRom();
    auto write_ldr = [&](uint32_t address, uint32_t rd, uint32_t literal_address) {
        WriteWordLe(rom, address, EncodeLdrLiteralAt(rd, address, literal_address));
    };

    write_ldr(0x00u, 1u, kLiteralBase + 0x00u);
    write_ldr(0x04u, 0u, kLiteralBase + 0x04u);
    WriteWordLe(rom, 0x08u, EncodeStrhImm(0u, 1u, 0u));       // DISPSTAT target = line 5.
    WriteWordLe(rom, 0x0Cu, EncodeMovImm(0u, 0x20u));
    WriteWordLe(rom, 0x10u, EncodeStrhImm(0u, 1u, 0u));       // Target = current line 0, VCount IRQ on.
    write_ldr(0x14u, 2u, kLiteralBase + 0x08u);
    WriteWordLe(rom, 0x18u, EncodeLdrhImm(3u, 2u, 0u));       // Sample IF right away.
    WriteWordLe(rom, 0x1Cu, EncodeBranchRelativeWords(-2));

    WriteWordLe(rom, kLiteralBase + 0x00u, 0x04000004u);
    WriteWordLe(rom, kLiteralBase + 0x04u, 0x00000500u);
    WriteWordLe(rom, kLiteralBase + 0x08u, 0x04000202u);
    WriteFile(gba_rom, rom);

    emugbds::ingba::Core core;
    std::string error;
    t.Expect(core.Initialize(error), "ingba VCOUNT target core initializes");
    error.clear();
    t.Expect(core.LoadRom(gba_rom, error), "ingba VCOUNT target core loads ROM");
    error.clear();
    t.Expect(core.RunFrame(error), "ingba VCOUNT target core runs frame");
    t.Expect((core.cpu().r[3] & 0x0004u) != 0u, "ingba mid-line VCOUNT target write requests the VCount IRQ at once");
}

void TestIngbaBlendChangesPerScanline(TestContext& t) {
    const auto dir = MakeTempRoot() / "ingba-blend-scanline";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "blend_scanline.gba";

    auto encode_bne = [](int32_t relative_words) {
        return (EncodeBranchRelativeWords(relative_words) & 0x0FFFFFFFu) | 0x10000000u;
    };
    std::vector<uint8_t> rom = BuildSyntheticGbaRom();
    WriteWordLe(rom, 0x00u, EncodeLdrLiteralAt(1u, 0x00u, 0x40u));
    WriteWordLe(rom, 0x04u, EncodeMovImm(0u, 0xA0u));
    WriteWordLe(rom, 0x08u, EncodeStrhImm(0u, 1u, 0x50u));     // BLDCNT: brighten the backdrop.
    WriteWordLe(rom, 0x0Cu, EncodeLdrhImm(2u, 1u, 0x06u));     // Wait for VCOUNT 80.
    WriteWordLe(rom, 0x10u, EncodeSubImmSetFlags(3u, 2u, 80u));
    WriteWordLe(rom, 0x14u, encode_bne(-4));
    WriteWordLe(rom, 0x18u, EncodeMovImm(0u, 16u));
    WriteWordLe(rom, 0x1Cu, EncodeStrhImm(0u, 1u, 0x54u));     // BLDY = 16 from here down.
    WriteWordLe(rom, 0x20u, EncodeLdrhImm(2u, 1u, 0x06u));     // Wait for VBlank.
    WriteWordLe(rom, 0x24u, EncodeSubImmSetFlags(3u, 2u, 160u));
    WriteWordLe(rom, 0x28u, encode_bne(-4));
    WriteWordLe(rom, 0x2Cu, EncodeMovImm(0u, 0u));
    WriteWordLe(rom, 0x30u, EncodeStrhImm(0u, 1u, 0x54u));     // BLDY = 0 for the next frame's top.
    WriteWordLe(rom, 0x34u, EncodeBranchRelativeWords(-12));
    WriteWordLe(rom, 0x40u, 0x04000000u);
    WriteFile(gba_rom, rom);

    emugbds::ingba::Core core;
    std::string error;
    t.Expect(core.Initialize(error), "ingba scanline blend core initializes");
    error.clear();
    t.Expect(core.LoadRom(gba_rom, error), "ingba scanline blend core loads ROM");
    error.clear();
    t.Expect(core.RunFrame(error), "ingba scanline blend core runs frame");
    const uint32_t* pixels = core.video_pixels();
    t.Expect(pixels[10u * 240u + 5u] == 0xFF000000u, "ingba lines before the BLDY write keep the plain backdrop");
    t.Expect(pixels[120u * 240u + 5u] == 0xFFFFFFFFu, "ingba lines after the BLDY write are brightened");
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

void TestIngbaThumbBlSuffixUsesLr(TestContext& t) {
    const auto dir = MakeTempRoot() / "ingba-thumb-bl-suffix";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "thumb_bl_suffix.gba";

    std::vector<uint8_t> rom = BuildSyntheticGbaRom();
    WriteWordLe(rom, 0x00u, EncodeLdrLiteralAt(0u, 0x00u, 0x20u));
    WriteWordLe(rom, 0x04u, EncodeLdrLiteralAt(14u, 0x04u, 0x24u)); // LR holds a BL prefix result.
    WriteWordLe(rom, 0x08u, EncodeArmBx(0u));
    WriteWordLe(rom, 0x20u, 0x08000041u);
    WriteWordLe(rom, 0x24u, 0x08000048u);

    WriteLe16(rom, 0x40u, EncodeThumbMovImm(0, 1));
    WriteLe16(rom, 0x42u, EncodeThumbBlSuffix(0x002u)); // target = LR + 4 = 0x4C
    WriteLe16(rom, 0x44u, EncodeThumbBRelative(-2));
    WriteLe16(rom, 0x4Cu, EncodeThumbAddImm(0, 2));
    WriteLe16(rom, 0x4Eu, EncodeThumbBxReg(14));
    WriteFile(gba_rom, rom);

    emugbds::ingba::Core core;
    std::string error;
    t.Expect(core.Initialize(error), "ingba thumb BL suffix core initializes");
    error.clear();
    t.Expect(core.LoadRom(gba_rom, error), "ingba thumb BL suffix core loads ROM");
    error.clear();
    t.Expect(core.RunFrame(error), "ingba thumb BL suffix core runs frame");
    t.Expect(core.cpu().r[0] == 3u, "ingba thumb BL suffix branches relative to LR");
    t.Expect(core.cpu().r[14] == 0x08000045u, "ingba thumb BL suffix sets LR to the Thumb return address");
    t.Expect(core.unsupported_arm_stats().thumb == 0u, "ingba thumb BL suffix alone is supported");
}

void TestIngbaThumbRegisterShiftByZero(TestContext& t) {
    const auto dir = MakeTempRoot() / "ingba-thumb-shift-zero";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "thumb_shift_zero.gba";

    std::vector<uint8_t> rom = BuildSyntheticGbaRom();
    WriteWordLe(rom, 0x00u, EncodeLdrLiteralAt(0u, 0x00u, 0x20u));
    WriteWordLe(rom, 0x04u, EncodeArmBx(0u));
    WriteWordLe(rom, 0x20u, 0x08000041u);

    WriteLe16(rom, 0x40u, EncodeThumbMovImm(0, 0x80));
    WriteLe16(rom, 0x42u, EncodeThumbMovImm(1, 0));
    WriteLe16(rom, 0x44u, EncodeThumbMovImm(2, 0x40));
    WriteLe16(rom, 0x46u, 0x40C8u); // LSR r0, r1
    WriteLe16(rom, 0x48u, 0x410Au); // ASR r2, r1
    WriteLe16(rom, 0x4Au, EncodeThumbBRelative(-2));
    WriteFile(gba_rom, rom);

    emugbds::ingba::Core core;
    std::string error;
    t.Expect(core.Initialize(error), "ingba thumb shift-by-zero core initializes");
    error.clear();
    t.Expect(core.LoadRom(gba_rom, error), "ingba thumb shift-by-zero core loads ROM");
    error.clear();
    t.Expect(core.RunFrame(error), "ingba thumb shift-by-zero core runs frame");
    t.Expect(core.cpu().r[0] == 0x80u, "ingba Thumb LSR by a zero register leaves the value unchanged");
    t.Expect(core.cpu().r[2] == 0x40u, "ingba Thumb ASR by a zero register leaves the value unchanged");
}

void TestIngbaLoadRotationAndLdmBase(TestContext& t) {
    const auto dir = MakeTempRoot() / "ingba-load-rotation";
    std::filesystem::create_directories(dir);
    const auto gba_rom = dir / "load_rotation.gba";

    constexpr uint32_t kLiteralBase = 0x40u;
    std::vector<uint8_t> rom = BuildSyntheticGbaRom();
    auto write_ldr = [&](uint32_t address, uint32_t rd, uint32_t literal_address) {
        WriteWordLe(rom, address, EncodeLdrLiteralAt(rd, address, literal_address));
    };

    write_ldr(0x00u, 1u, kLiteralBase + 0x00u);
    write_ldr(0x04u, 2u, kLiteralBase + 0x04u);
    WriteWordLe(rom, 0x08u, EncodeStrReg(2u, 1u, 0u));
    write_ldr(0x0Cu, 2u, kLiteralBase + 0x08u);
    WriteWordLe(rom, 0x10u, EncodeStrReg(2u, 1u, 4u));
    WriteWordLe(rom, 0x14u, EncodeLdrReg(3u, 1u, 1u));            // Misaligned LDR.
    write_ldr(0x18u, 5u, kLiteralBase + 0x00u);
    WriteWordLe(rom, 0x1Cu, EncodeLdmiaWriteback(1u, 0x0003u));   // LDMIA r1!, {r0, r1}
    write_ldr(0x20u, 7u, kLiteralBase + 0x0Cu);
    WriteWordLe(rom, 0x24u, EncodeArmBx(7u));

    WriteWordLe(rom, kLiteralBase + 0x00u, 0x03000000u);
    WriteWordLe(rom, kLiteralBase + 0x04u, 0x11223344u);
    WriteWordLe(rom, kLiteralBase + 0x08u, 0x0BADF00Du);
    WriteWordLe(rom, kLiteralBase + 0x0Cu, 0x08000081u);

    WriteLe16(rom, 0x80u, 0xCD60u); // Thumb LDMIA r5!, {r5, r6}
    WriteLe16(rom, 0x82u, EncodeThumbBRelative(-2));
    WriteFile(gba_rom, rom);

    emugbds::ingba::Core core;
    std::string error;
    t.Expect(core.Initialize(error), "ingba load rotation core initializes");
    error.clear();
    t.Expect(core.LoadRom(gba_rom, error), "ingba load rotation core loads ROM");
    error.clear();
    t.Expect(core.RunFrame(error), "ingba load rotation core runs frame");
    t.Expect(core.cpu().r[3] == 0x44112233u, "ingba misaligned LDR rotates the loaded word");
    t.Expect(core.cpu().r[0] == 0x11223344u, "ingba LDM loads the first register");
    t.Expect(core.cpu().r[1] == 0x0BADF00Du, "ingba LDM keeps a loaded base instead of writeback");
    t.Expect(core.cpu().r[5] == 0x11223344u, "ingba Thumb LDMIA keeps a loaded base instead of writeback");
    t.Expect(core.cpu().r[6] == 0x0BADF00Du, "ingba Thumb LDMIA loads the following register");
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

    core.set_hidden_layers(0x04u); // debug: hide BG2
    error.clear();
    t.Expect(core.RunFrame(error), "ingba mode3 core runs a frame with BG2 hidden");
    t.Expect(core.video_pixels()[0] == 0xFF000000u, "a hidden BG2 shows the backdrop instead");
    core.set_hidden_layers(0u);
    error.clear();
    t.Expect(core.RunFrame(error) && core.video_pixels()[0] == 0xFF0000FFu, "showing BG2 again restores it");
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

    auto& arm9 = bus.arm9();
    auto& arm7 = bus.arm7();
    t.Expect(arm7.Read8(0x00000000u) == bios7_bytes[0], "inds ARM7 BIOS mapping works");
    t.Expect(arm9.Read8(0x00000000u) == 0u, "inds ARM9 does not see the ARM7 BIOS");
    t.Expect(arm9.Read8(kDsArm9BiosBase) == bios9_bytes[0], "inds ARM9 BIOS mapping works");
    t.Expect(arm9.Read8(kDsArm9BiosBase + 0x8000u) == 0u, "inds ARM9 BIOS is not mirrored past its 32 KiB region");
    t.Expect(arm9.Read8(kDsSlot2Base) == slot2_bytes[0] && arm7.Read8(kDsSlot2Base) == slot2_bytes[0], "inds Slot-2 mapping works");
    t.Expect(
        arm9.Read8(kDsSlot2Base + static_cast<uint32_t>(slot2_bytes.size()) + 7u) == slot2_bytes[7],
        "inds Slot-2 wrap mapping works");

    arm9.Write8(kDsMainRamBase + 0x24u, 0xA5);
    t.Expect(arm7.Read8(kDsMainRamBase + 0x24u) == 0xA5, "inds main RAM is shared by both CPUs");
    t.Expect(arm9.Read8(kDsMainRamBase + 0x00400024u) == 0xA5, "inds main RAM mirrors every 4 MiB");
    arm7.Write8(kDsSharedWramBase + 0x42u, 0x5A);
    t.Expect(arm7.Read8(kDsSharedWramBase + 0x42u) == 0x5A, "inds shared WRAM read/write works");
    arm7.Write32(0x03800010u, 0x12345678u);
    t.Expect(arm7.Read32(0x0380FFF0u + 0x20u) == 0x12345678u, "inds ARM7 WRAM mirrors every 64 KiB");

    t.Expect(arm9.Read32(0x04000700u) == 0u, "inds unemulated I/O reads as zero");
    t.Expect(bus.arm9_unmapped().reads.count(0x04000700u) == 1u, "inds logs unemulated I/O reads");
    arm9.Write16(0x0B000000u, 0x7FFFu);
    t.Expect(bus.arm9_unmapped().writes.count(0x0B000000u) == 1u, "inds logs writes to unemulated memory by block");

    bus.ClearSlot2Rom();
    t.Expect(arm9.Read8(kDsSlot2Base) == 0x00, "inds clear slot2 ROM removes mapping");
}

void TestIndsCp15AndTcm(TestContext& t) {
    emugbds::inds::MemoryBus bus;
    auto& cp15 = bus.cp15();
    auto& arm9 = bus.arm9();
    uint32_t value = 0u;
    t.Expect(cp15.Read(0u, 0u, 0u, 0u, value) && value == 0x41059461u, "CP15 main ID is the ARM946 rev 1");
    t.Expect(cp15.Read(0u, 0u, 0u, 2u, value) && value == 0x00140180u, "CP15 reports 32K ITCM and 16K DTCM");
    t.Expect(cp15.exception_base() == 0xFFFF0000u, "CP15 resets with high exception vectors");
    t.Expect(!cp15.Write(1u, 1u, 0u, 0u, 0u), "CP15 rejects opcode1 != 0");

    cp15.Write(0u, 1u, 0u, 0u, 0xFFFFFFFFu);
    cp15.Read(0u, 1u, 0u, 0u, value);
    t.Expect(value == 0x000FF0FDu, "CP15 control keeps only the DS-writable bits");
    cp15.Write(0u, 1u, 0u, 0u, 0x00000000u);
    cp15.Read(0u, 1u, 0u, 0u, value);
    t.Expect(value == 0x00000078u && cp15.exception_base() == 0u, "CP15 control bits 3-6 stay set");

    arm9.Write32(0x00000010u, 0xAAAAAAAAu);
    t.Expect(arm9.Read32(0x00000010u) == 0u, "ITCM is not mapped while disabled");
    cp15.Write(0u, 9u, 1u, 1u, 0x0000000Eu); // ITCM virtual size 64 KiB from 0
    cp15.Write(0u, 9u, 1u, 0u, 0x0080000Au); // DTCM 16 KiB at 00800000h, outside the ITCM window
    cp15.Write(0u, 1u, 0u, 0u, (1u << 16) | (1u << 18));
    arm9.Write32(0x00000010u, 0x11223344u);
    t.Expect(arm9.Read32(0x00008010u) == 0x11223344u, "ITCM mirrors its 32 KiB across the virtual size");
    t.Expect(arm9.Read32(0x00010010u) == 0u, "ITCM ends at its virtual size");
    arm9.Write32(0x00800020u, 0x55667788u);
    t.Expect(arm9.Read32(0x00800020u) == 0x55667788u && arm9.Read32(0x00000020u) == 0u, "DTCM maps at its programmed base");
    t.Expect(arm9.Read32(0x00804020u) == 0u, "DTCM ends at its virtual size");
    t.Expect(bus.arm7().Read32(0x00800020u) == 0u, "the ARM7 has no TCM");

    cp15.Write(0u, 9u, 1u, 0u, 0x0000000Au); // Move DTCM under ITCM: ITCM wins.
    t.Expect(arm9.Read32(0x00000010u) == 0x11223344u, "ITCM has priority over an overlapping DTCM");

    // Load mode: writes land in the TCM, reads see the memory underneath.
    cp15.Write(0u, 9u, 1u, 0u, 0x0200000Au); // DTCM 16 KiB over main RAM
    arm9.Write32(0x02000040u, 0x0000AAAAu);
    cp15.Write(0u, 1u, 0u, 0u, (1u << 16) | (1u << 17) | (1u << 18));
    arm9.Write32(0x02000040u, 0x0000BBBBu);
    t.Expect(arm9.Read32(0x02000040u) == 0u, "DTCM load mode reads the memory underneath");
    cp15.Write(0u, 1u, 0u, 0u, (1u << 16) | (1u << 18));
    t.Expect(arm9.Read32(0x02000040u) == 0xBBBBu, "DTCM load mode still writes into the DTCM");

    cp15.Write(0u, 9u, 1u, 1u, 0x0000002Eu); // N=23: ITCM covers the whole 4 GiB
    arm9.Write32(0x00007FFCu, 0xCD000000u);
    t.Expect(arm9.Read8(0xFFFFFFFFu) == 0xCDu, "a 4 GiB ITCM window includes the last byte");

    t.Expect(!cp15.ConsumeHaltRequest(), "CP15 has no pending halt");
    cp15.Write(0u, 7u, 0u, 4u, 0u);
    t.Expect(cp15.ConsumeHaltRequest() && !cp15.ConsumeHaltRequest(), "CP15 wait-for-interrupt requests one halt");
}

void TestIndsWramcnt(TestContext& t) {
    emugbds::inds::MemoryBus bus;
    auto& arm9 = bus.arm9();
    auto& arm7 = bus.arm7();
    t.Expect(arm7.Read8(0x04000241u) == 3u, "WRAMSTAT starts with all shared WRAM on the ARM7");
    arm7.Write32(0x03000000u, 0x0000AAAAu); // First 16K
    arm7.Write32(0x03004000u, 0x0000BBBBu); // Second 16K
    arm7.Write32(0x03800000u, 0x0000CCCCu); // ARM7 WRAM
    t.Expect(arm9.Read32(0x03000000u) == 0u, "WRAMCNT 3: the ARM9 has no shared WRAM");

    arm9.Write8(0x04000247u, 0u);
    t.Expect(arm9.Read32(0x03004000u) == 0xBBBBu && arm9.Read8(0x04000247u) == 0u, "WRAMCNT 0: the ARM9 sees all 32K");
    t.Expect(arm7.Read32(0x03000000u) == 0xCCCCu, "WRAMCNT 0: the ARM7 sees its own WRAM there");
    arm9.Write8(0x04000247u, 1u);
    t.Expect(arm9.Read32(0x03000000u) == 0xBBBBu && arm7.Read32(0x03000000u) == 0xAAAAu, "WRAMCNT 1: ARM9 2nd 16K, ARM7 1st 16K");
    arm9.Write8(0x04000247u, 2u);
    t.Expect(arm9.Read32(0x03000000u) == 0xAAAAu && arm7.Read32(0x03000000u) == 0xBBBBu, "WRAMCNT 2: ARM9 1st 16K, ARM7 2nd 16K");
    t.Expect(arm7.Read8(0x04000241u) == 2u, "WRAMSTAT reports the ARM9's WRAMCNT");
    t.Expect(arm9.Read32(0x04000244u) == 0x02000000u, "a wide I/O read keeps the implemented byte lanes");
    t.Expect(arm7.Read32(0x04000240u) == 0x00000200u, "WRAMSTAT keeps its lane in a wide ARM7 read");
}

void TestIndsTimers(TestContext& t) {
    emugbds::inds::Timers timers;
    timers.Reset();
    timers.WriteWord(0x0u, 0x00C0FFFEu, 0xFFFFFFFFu); // TM0: reload FFFEh, IRQ, start, F/1
    t.Expect((timers.ReadWord(0x0u) & 0xFFFFu) == 0xFFFEu, "DS timer start loads the reload value");
    t.Expect(timers.Advance(1u) == 0u && (timers.ReadWord(0x0u) & 0xFFFFu) == 0xFFFFu, "DS timer counts at F/1");
    t.Expect(timers.Advance(1u) == (1u << 3), "DS timer overflow requests IRQ 3");
    timers.WriteWord(0x4u, 0x00C4FFFFu, 0xFFFFFFFFu); // TM1: count-up, IRQ, start, reload FFFFh
    t.Expect(timers.Advance(2u) == ((1u << 3) | (1u << 4)), "DS count-up timer ticks on the lower timer's overflow");
    timers.WriteWord(0x8u, 0x00810000u, 0xFFFFFFFFu); // TM2: F/64, no IRQ
    timers.Advance(63u);
    t.Expect((timers.ReadWord(0x8u) & 0xFFFFu) == 0u, "DS F/64 timer waits for 64 cycles");
    timers.Advance(1u);
    t.Expect((timers.ReadWord(0x8u) & 0xFFFFu) == 1u, "DS F/64 timer ticks after 64 cycles");
}

void TestIndsIpc(TestContext& t) {
    emugbds::inds::Ipc ipc;
    ipc.Reset();
    ipc.WriteSync(0, 0x0500u, 0xFFFFu);
    t.Expect((ipc.ReadSync(1) & 0xFu) == 5u && (ipc.ReadSync(0) & 0xF00u) == 0x500u, "IPCSYNC output reaches the other CPU");
    ipc.WriteSync(0, 0x2000u, 0xFFFFu);
    t.Expect(ipc.TakeIrqs(1) == 0u, "IPCSYNC IRQ needs the receiver's enable bit");
    ipc.WriteSync(1, 0x4000u, 0xFFFFu);
    ipc.WriteSync(0, 0x2000u, 0xFFFFu);
    t.Expect(ipc.TakeIrqs(1) == emugbds::inds::Ipc::kSyncIrq, "IPCSYNC bit 13 raises the remote IRQ");

    ipc.WriteFifoControl(0, 0x8000u, 0xFFFFu);          // ARM9: enable
    ipc.WriteFifoControl(1, 0x8000u | 0x0400u, 0xFFFFu); // ARM7: enable + recv-not-empty IRQ
    t.Expect((ipc.ReadFifoControl(1) & 0x0100u) != 0u, "ARM7 receive FIFO starts empty");
    ipc.Send(0, 0x11111111u);
    ipc.Send(0, 0x22222222u);
    t.Expect(ipc.TakeIrqs(1) == emugbds::inds::Ipc::kRecvNotEmptyIrq, "receive-not-empty IRQ fires once on the edge");
    t.Expect(ipc.Receive(1) == 0x11111111u && ipc.Receive(1) == 0x22222222u, "IPC FIFO is first in, first out");
    t.Expect(ipc.Receive(1) == 0x22222222u && (ipc.ReadFifoControl(1) & 0x4000u) != 0u, "empty FIFO read repeats the last word and flags an error");
    for (int i = 0; i < 17; ++i) {
        ipc.Send(0, static_cast<uint32_t>(i));
    }
    t.Expect((ipc.ReadFifoControl(0) & 0x4002u) == 0x4002u, "sending to a full FIFO flags an error");
    ipc.WriteFifoControl(0, 0x8008u | 0x4000u, 0xFFFFu); // clear send FIFO, acknowledge error
    t.Expect((ipc.ReadFifoControl(0) & 0x4001u) == 0x0001u, "FIFO clear empties it and the error acknowledges");
}

void TestIndsIoInterruptsAndDma(TestContext& t) {
    emugbds::inds::MemoryBus bus;
    auto& arm9 = bus.arm9();
    auto& io = bus.io();
    arm9.Write32(0x04000210u, 0x00000005u); // IE: VBlank + VCount
    arm9.Write16(0x04000004u, 0x0A28u);     // DISPSTAT: VBlank + VCount IRQ, LYC 10
    arm9.Write32(0x04000208u, 1u);          // IME
    io.StartLine(10u);
    t.Expect((arm9.Read16(0x04000004u) & 0x4u) != 0u && arm9.Read16(0x04000006u) == 10u, "VCOUNT match sets the flag");
    t.Expect(arm9.Read32(0x04000214u) == 0x4u && io.IrqAsserted(0), "VCOUNT match requests its IRQ");
    io.StartLine(192u);
    t.Expect((arm9.Read16(0x04000004u) & 0x1u) != 0u && arm9.Read32(0x04000214u) == 0x5u, "line 192 starts V-Blank with its IRQ");
    arm9.Write32(0x04000214u, 0x4u);
    t.Expect(arm9.Read32(0x04000214u) == 0x1u, "IF is acknowledged by writing 1");
    t.Expect(!io.IrqAsserted(1), "the ARM7 has its own interrupt controller");

    // Immediate DMA copies main RAM; DMA never sees the DTCM.
    arm9.Write32(0x02000100u, 0xCAFEF00Du);
    bus.cp15().Write(0u, 9u, 1u, 0u, 0x0200000Au); // DTCM over 02000000h
    bus.cp15().Write(0u, 1u, 0u, 0u, 1u << 16);
    arm9.Write32(0x02000100u, 0x0BADBEEFu);       // Lands in DTCM only
    arm9.Write32(0x040000D4u, 0x02000100u);       // DMA3 SAD
    arm9.Write32(0x040000D8u, 0x02100000u);       // DMA3 DAD
    arm9.Write32(0x040000DCu, 0x84000001u);       // enable, 32-bit, immediate, 1 word
    t.Expect(arm9.Read32(0x02100000u) == 0xCAFEF00Du, "ARM9 DMA reads main RAM underneath the DTCM");
    t.Expect((arm9.Read32(0x040000DCu) & 0x80000000u) == 0u, "immediate DMA clears its enable bit");

    arm9.Write32(0x040000C8u, 0x02100000u);       // DMA2 SAD
    arm9.Write32(0x040000CCu, 0x02100010u);       // DMA2 DAD
    arm9.Write32(0x040000D0u, 0x8C000001u);       // enable, 32-bit, V-Blank start (mode 1)
    t.Expect(arm9.Read32(0x02100010u) == 0u, "V-Blank DMA waits for V-Blank");
    io.StartLine(192u);
    t.Expect(arm9.Read32(0x02100010u) == 0xCAFEF00Du, "V-Blank DMA runs at line 192");

    arm9.Write16(0x04000004u, 0xC008u);           // LYC = 192, V-Blank IRQ enable
    arm9.Write32(0x040000BCu, 0x04000004u);       // DMA1 SAD: DISPSTAT
    arm9.Write32(0x040000C0u, 0x02100020u);       // DMA1 DAD
    arm9.Write32(0x040000C4u, 0x88000001u);       // enable, 16-bit, V-Blank start, 1 unit
    io.StartLine(191u);
    io.StartLine(192u);
    t.Expect((arm9.Read16(0x02100020u) & 0x4u) != 0u, "V-Blank DMA sees the VCOUNT match of line 192");
}

void TestIndsEmptyCardSlot(TestContext& t) {
    emugbds::inds::MemoryBus bus;
    auto& arm7 = bus.arm7();
    auto& arm9 = bus.arm9();
    arm9.Write16(0x04000204u, 0x0800u);          // EXMEMCNT: NDS slot to the ARM7
    arm7.Write16(0x040001A0u, 0xC000u);          // AUXSPICNT: slot enable, transfer IRQ
    arm7.Write32(0x04000210u, 1u << 19);         // IE: card transfer complete
    arm7.Write32(0x040001A8u, 0x00000090u);      // Command 90h: chip ID
    arm7.Write32(0x040001A4u, 0xA7000000u);      // Start, release reset, 4-byte block
    t.Expect((arm7.Read32(0x040001A4u) & 0x80800000u) == 0x80800000u, "card transfer is busy with data ready");
    t.Expect(arm7.Read32(0x04100010u) == 0xFFFFFFFFu, "an empty NDS slot reads all ones");
    t.Expect((arm7.Read32(0x040001A4u) & 0x80800000u) == 0u, "the transfer ends after its last word");
    t.Expect((arm7.Read32(0x04000214u) & (1u << 19)) != 0u, "transfer completion raises the card IRQ on the slot owner");
    t.Expect((arm9.Read32(0x04000214u) & (1u << 19)) == 0u, "the other CPU gets no card IRQ");
    t.Expect(arm9.Read32(0x040001A4u) == 0u, "the CPU without slot rights reads card registers as zero");
    arm9.Write32(0x040001A4u, 0x87000000u);
    t.Expect((arm7.Read32(0x040001A4u) & 0x80000000u) == 0u, "the CPU without slot rights cannot start a transfer");
    arm7.Write32(0x040001A4u, 0x80000000u);      // Reset bit 29 cannot be cleared; zero-length block
    t.Expect((arm7.Read32(0x040001A4u) & 0xA0800000u) == 0x20000000u, "a zero-length transfer completes at once; RESB sticks");
}

void TestIndsSpiDevices(TestContext& t) {
    const auto dir = MakeTempRoot() / "inds-spi";
    std::filesystem::create_directories(dir);
    const auto firmware = dir / "firmware.bin";
    std::vector<uint8_t> image(0x40000u, 0u);
    image[0x20u] = 0xA1u;
    image[0x21u] = 0xB2u;
    WriteFile(firmware, image);

    emugbds::inds::MemoryBus bus;
    std::string error;
    t.Expect(bus.LoadFirmware(firmware, error), "SPI firmware image loads");
    auto& arm7 = bus.arm7();
    auto exchange = [&](uint16_t spicnt, uint8_t out) {
        arm7.Write16(0x040001C0u, spicnt);
        arm7.Write16(0x040001C2u, out);
        return static_cast<uint8_t>(arm7.Read16(0x040001C2u));
    };
    constexpr uint16_t kFlashHold = 0x8000u | 0x0100u | 0x0800u; // enable, firmware, hold
    constexpr uint16_t kFlashLast = 0x8000u | 0x0100u;
    exchange(kFlashHold, 0x03u); // READ
    exchange(kFlashHold, 0x00u);
    exchange(kFlashHold, 0x00u);
    exchange(kFlashHold, 0x20u);
    t.Expect(exchange(kFlashHold, 0u) == 0xA1u && exchange(kFlashLast, 0u) == 0xB2u, "SPI flash READ streams the firmware image");
    exchange(kFlashHold, 0x9Fu); // RDID
    t.Expect(exchange(kFlashHold, 0u) == 0x20u && exchange(kFlashHold, 0u) == 0x40u && exchange(kFlashLast, 0u) == 0x12u,
        "SPI flash reports a 256 KiB ST M45PE20-style ID");
    exchange(kFlashLast, 0x06u); // WREN
    exchange(kFlashHold, 0x05u); // RDSR
    t.Expect(exchange(kFlashLast, 0u) == 0x02u, "SPI flash status shows write enable");
    exchange(kFlashHold, 0x0Au); // PW at 000100h
    exchange(kFlashHold, 0x00u);
    exchange(kFlashHold, 0x01u);
    exchange(kFlashHold, 0x00u);
    exchange(kFlashLast, 0x5Au);
    t.Expect(bus.firmware_image()[0x100u] == 0x5Au, "SPI flash page write updates the in-memory image");
    exchange(kFlashHold, 0x05u);
    t.Expect(exchange(kFlashLast, 0u) == 0x00u, "a completed page write clears write enable");
    exchange(kFlashHold, 0x03u); // READ left held, then the device switches
    exchange(0x8000u | 0x0800u, 0x80u); // power management, hold: must start a new command
    t.Expect(exchange(0x8000u, 0u) == 0x0Du, "switching SPI device ends the held flash command");

    constexpr uint16_t kPowerHold = 0x8000u | 0x0800u; // enable, power management, hold
    constexpr uint16_t kPowerLast = 0x8000u;
    exchange(kPowerHold, 0x00u); // write register 0
    exchange(kPowerLast, 0x05u);
    exchange(kPowerHold, 0x80u); // read register 0
    t.Expect(exchange(kPowerLast, 0u) == 0x05u, "SPI power management registers read back");

    arm7.Write32(0x04000210u, 1u << 23);
    exchange(0x8000u | 0x4000u | 0x0200u, 0x90u); // touchscreen, IRQ enabled
    t.Expect((arm7.Read32(0x04000214u) & (1u << 23)) != 0u, "an SPI transfer with IRQ enabled raises IF bit 23");

    arm7.Write16(0x04000504u, 0x0200u);
    t.Expect(arm7.Read16(0x04000504u) == 0x0200u, "SOUNDBIAS keeps the value the BIOS ramps");
}

void TestIndsVramBanking(TestContext& t) {
    emugbds::inds::MemoryBus bus;
    auto& arm9 = bus.arm9();
    auto& arm7 = bus.arm7();
    arm9.Write8(0x04000240u, 0x80u); // A: LCDC
    arm9.Write16(0x06800010u, 0x1234u);
    t.Expect(arm9.Read16(0x06800010u) == 0x1234u, "VRAM A in LCDC mode is read/write at 6800000h");
    arm9.Write8(0x06800010u, 0xFFu);
    t.Expect(arm9.Read16(0x06800010u) == 0x1234u, "ARM9 byte writes to VRAM are ignored");
    t.Expect(bus.video().LcdcBank16(0u, 8u) == 0x1234u, "VRAM display reads an LCDC bank by pixel");
    arm9.Write8(0x04000240u, 0x89u); // A: engine A BG, offset 1
    t.Expect(arm9.Read16(0x06800010u) == 0u, "a bank leaves LCDC when remapped");
    t.Expect(arm9.Read16(0x06020010u) == 0x1234u, "engine A BG offset 1 maps the bank at 6020000h");
    t.Expect(arm9.Read16(0x06020010u + 0x80000u) == 0x1234u, "engine A BG VRAM mirrors every 512 KiB");
    t.Expect(bus.video().Read16(emugbds::inds::VideoMemory::Area::EngineABg, 0x20010u) == 0x1234u,
        "the display reads the same BG VRAM");
    arm9.Write8(0x04000242u, 0x82u); // C: ARM7
    arm7.Write8(0x06000003u, 0x5Au);
    t.Expect(arm7.Read8(0x06000003u) == 0x5Au, "VRAM C in ARM7 mode takes ARM7 byte writes");
    t.Expect((arm7.Read8(0x04000240u) & 3u) == 1u, "VRAMSTAT shows bank C allocated to the ARM7");
    arm9.Write8(0x04000249u, 0x81u); // I: engine B BG at 6208000h
    arm9.Write16(0x06208000u, 0xBEEFu);
    t.Expect(bus.video().Read16(emugbds::inds::VideoMemory::Area::EngineBBg, 0x8000u) == 0xBEEFu,
        "VRAMCNT_I at 4000249h maps bank I to engine B BG at 6208000h");
    arm9.Write16(0x05000402u, 0x7FFFu);
    t.Expect(arm9.Read16(0x05000C02u) == 0x7FFFu && bus.video().Palette(1, false, 1u) == 0x7FFFu,
        "palette RAM mirrors every 2 KiB; 5000400h is engine B's BG palette");
}

// Card commands as the CPU issues them: 8 command bytes at 40001A8h, then a 4-byte or 200h block.
void TestIndsCartridgeReads(TestContext& t) {
    const auto dir = MakeTempRoot() / "inds-cart";
    std::filesystem::create_directories(dir);
    std::vector<uint8_t> image(0x20000u, 0u);
    for (size_t i = 0u; i < image.size(); ++i) {
        image[i] = static_cast<uint8_t>(i ^ (i >> 8u));
    }
    WriteFile(dir / "cart.nds", image);
    emugbds::inds::MemoryBus bus;
    std::string error;
    t.Expect(bus.LoadNdsRom(dir / "cart.nds", error), "cartridge image loads");
    auto& arm9 = bus.arm9();
    arm9.Write16(0x040001A0u, 0x8000u); // slot enabled, ROM mode
    auto command = [&](uint32_t low, uint32_t high, uint32_t block) {
        arm9.Write32(0x040001A8u, low);
        arm9.Write32(0x040001ACu, high);
        arm9.Write32(0x040001A4u, 0xA0000000u | (block << 24u));
    };
    command(0x000000B8u, 0u, 7u); // B8h: chip ID
    t.Expect(arm9.Read32(0x04100010u) == 0x000000C2u, "chip ID: Macronix, 1 MiB for a 128 KiB image");
    command(0x1F0000B7u, 0xF0u, 7u); // B7h 00001FF0h: inside the redirect range
    const uint32_t redirected = arm9.Read32(0x04100010u);
    t.Expect(redirected == (image[0x81F0u] | (image[0x81F1u] << 8u) | (image[0x81F2u] << 16u) | (image[0x81F3u] << 24u)),
        "B7h below 8000h reads 8000h + (addr & 1FFh)");
    command(0x0F0100B7u, 0xFCu, 1u); // B7h 00010FFCh, 200h bytes: wraps inside the 4 KiB block
    const uint32_t first = arm9.Read32(0x04100010u);
    const uint32_t wrapped = arm9.Read32(0x04100010u);
    t.Expect(first == (image[0x10FFCu] | (image[0x10FFDu] << 8u) | (image[0x10FFEu] << 16u) | (image[0x10FFFu] << 24u)) &&
            wrapped == (image[0x10000u] | (image[0x10001u] << 8u) | (image[0x10002u] << 16u) | (image[0x10003u] << 24u)),
        "a B7h stream wraps to the start of its 4 KiB block");
    for (uint32_t i = 2u; i < 0x80u; ++i) {
        arm9.Read32(0x04100010u); // finish the block so the next command can start
    }
    command(0x000002B7u, 0x00u, 7u); // B7h 02000000h: past the image, mirrors to 0 -> 8000h
    t.Expect(arm9.Read32(0x04100010u) == (image[0x8000u] | (image[0x8001u] << 8u) | (image[0x8002u] << 16u) | (image[0x8003u] << 24u)),
        "addresses past the ROM mirror (then redirect)");
}

void TestIndsBackupMemory(TestContext& t) {
    const auto dir = MakeTempRoot() / "inds-backup";
    std::filesystem::create_directories(dir);
    const auto save = dir / "game.sav";
    std::filesystem::remove(save);
    emugbds::inds::Backup backup;
    std::string error;
    t.Expect(backup.Load(save, error), "a missing save starts a blank chip");
    auto session = [&](std::initializer_list<uint8_t> bytes, size_t read_count = 0u) {
        std::vector<uint8_t> in;
        for (uint8_t byte : bytes) {
            backup.Transfer(byte);
        }
        for (size_t i = 0u; i < read_count; ++i) {
            in.push_back(backup.Transfer(0u));
        }
        backup.Deselect();
        return in;
    };
    t.Expect(session({0x05u}, 1u)[0] == 0x00u, "status of a blank chip: ready");
    t.Expect(session({0x03u, 0x12u, 0x34u}, 2u) == std::vector<uint8_t>{0xFFu, 0xFFu}, "a blank chip reads FFh");
    session({0x06u});
    t.Expect((session({0x05u}, 1u)[0] & 0x02u) != 0u, "WREN sets the write-enable latch");
    // 64K EEPROM page write: command, 2 address bytes, 128 data bytes.
    backup.Transfer(0x02u);
    backup.Transfer(0x01u);
    backup.Transfer(0x00u);
    for (uint32_t i = 0u; i < 128u; ++i) {
        backup.Transfer(static_cast<uint8_t>(i));
    }
    backup.Deselect();
    t.Expect(backup.address_bytes() == 2u, "a 2-byte address and a 128-byte page mean a 64K EEPROM");
    t.Expect(session({0x03u, 0x01u, 0x05u}, 2u) == std::vector<uint8_t>{0x05u, 0x06u}, "EEPROM reads back what was written");
    t.Expect((session({0x05u}, 1u)[0] & 0x02u) == 0u, "a finished write clears the write-enable latch");
    session({0x02u, 0x01u, 0x05u, 0xAAu}); // no WREN: ignored
    t.Expect(session({0x03u, 0x01u, 0x05u}, 1u)[0] == 0x05u, "writes without WREN are ignored");
    session({0x06u});
    session({0x02u, 0x01u, 0x7Fu, 0xA1u, 0xA2u}); // crosses the 128-byte page end at 017Fh
    t.Expect(session({0x03u, 0x01u, 0x7Fu}, 1u)[0] == 0xA1u && session({0x03u, 0x01u, 0x00u}, 1u)[0] == 0xA2u,
        "page writes wrap within the page");
    t.Expect(backup.Flush(error) && std::filesystem::file_size(save) == 0x10000u, "the save file is written at 64K");

    emugbds::inds::Backup reloaded;
    t.Expect(reloaded.Load(save, error) && reloaded.address_bytes() == 2u, "the save file size selects the chip type");
    reloaded.Transfer(0x03u);
    reloaded.Transfer(0x01u);
    reloaded.Transfer(0x7Fu);
    t.Expect(reloaded.Transfer(0u) == 0xA1u, "a reloaded save keeps its contents");
    reloaded.Deselect();

    emugbds::inds::Backup flash;
    t.Expect(flash.Load(dir / "flash.sav", error), "blank FLASH chip");
    flash.Transfer(0xDBu); // page erase is FLASH-only
    flash.Transfer(0u);
    flash.Transfer(0u);
    flash.Transfer(0u);
    flash.Deselect();
    t.Expect(flash.address_bytes() == 3u, "FLASH-only commands select 3-byte FLASH");
    flash.Transfer(0x9Fu);
    t.Expect(flash.Transfer(0u) == 0x20u, "FLASH answers a JEDEC ID");
    flash.Deselect();
}

void TestIndsSound(TestContext& t) {
    emugbds::inds::MemoryBus bus;
    auto& arm7 = bus.arm7();
    auto& sound = bus.io().sound();
    constexpr uint32_t kChannelCnt = 0x04000400u;
    constexpr uint32_t kPcm8 = 0u << 29;
    constexpr uint32_t kPcm16 = 1u << 29;
    constexpr uint32_t kAdpcm = 2u << 29;
    constexpr uint32_t kPsg = 3u << 29;
    constexpr uint32_t kLoop = 1u << 27;
    constexpr uint32_t kOneShot = 2u << 27;
    constexpr uint32_t kStart = 1u << 31;
    constexpr uint32_t kFullVolume = 0x7Fu;
    constexpr uint32_t kPanRight = 0x7Fu << 16;
    constexpr uint32_t kOutputCycles = 1023u; // one 32768 Hz output sample is ~1022.75 ARM7 cycles
    // Left outputs over `count` output samples, with runs of equal values merged.
    const auto run = [&](uint32_t count, bool right = false) {
        sound.ClearSamples();
        sound.Advance(count * kOutputCycles);
        std::vector<int16_t> changes;
        for (size_t i = right ? 1u : 0u; i < sound.samples().size(); i += 2u) {
            if (changes.empty() || changes.back() != sound.samples()[i]) {
                changes.push_back(sound.samples()[i]);
            }
        }
        return changes;
    };
    const auto channel = [&](uint32_t index, uint32_t source, uint16_t timer, uint16_t loop, uint32_t length, uint32_t control) {
        const uint32_t base = kChannelCnt + index * 0x10u;
        arm7.Write32(base + 4u, source);
        arm7.Write32(base + 8u, timer | (static_cast<uint32_t>(loop) << 16u));
        arm7.Write32(base + 0xCu, length);
        arm7.Write32(base, control);
    };
    const auto busy = [&](uint32_t index) { return (arm7.Read32(kChannelCnt + index * 0x10u) & kStart) != 0u; };

    arm7.Write16(0x04000500u, 0x807Fu); // SOUNDCNT: enabled, master volume 127
    arm7.Write16(0x04000504u, 0x200u);  // SOUNDBIAS midpoint
    t.Expect(arm7.Read16(0x04000500u) == 0x807Fu && arm7.Read16(0x04000504u) == 0x200u, "SOUNDCNT and SOUNDBIAS read back");

    // PCM16 0x4000 at full volume, panned left: GBATEK's bit widths give DAC level 0x300, i.e. 0x4000 out.
    for (uint32_t i = 0u; i < 8u; ++i) {
        arm7.Write16(0x02000000u + i * 2u, 0x4000u);
    }
    const uint16_t slow = static_cast<uint16_t>(0x10000u - 2048u); // 4096 ARM7 cycles per channel sample
    channel(0u, 0x02000000u, slow, 0u, 4u, kStart | kOneShot | kPcm16 | kFullVolume);
    t.Expect(arm7.Read32(kChannelCnt + 4u) == 0u, "SOUNDxSAD is write-only");
    t.Expect(busy(0u), "a started channel is busy");
    std::vector<int16_t> left = run(60u);
    t.Expect(left.size() == 3u && left[0] == 0 && left[1] == 0x4000 && left[2] == 0,
        "a one-shot PCM16 sound starts after its delay, plays its samples, then goes silent");
    t.Expect(!busy(0u), "a one-shot sound clears busy at its end");
    channel(0u, 0x02000000u, slow, 0u, 4u, kStart | kOneShot | kPcm16 | kFullVolume | kPanRight);
    t.Expect(run(60u, true) == std::vector<int16_t>({0, 0x4000, 0}) && run(1u) == std::vector<int16_t>({0}),
        "panning fully right moves the sound to the right speaker");

    // PCM8 loop: a byte NN plays as NN00h, and a looping sound stays busy.
    for (uint32_t i = 0u; i < 16u; ++i) {
        arm7.Write8(0x02000100u + i, 0x40u);
    }
    channel(1u, 0x02000100u, slow, 1u, 3u, kStart | kLoop | kPcm8 | kFullVolume);
    left = run(120u);
    t.Expect(left.size() == 2u && left[1] == 0x4000 && busy(1u), "a looping PCM8 sound keeps playing");
    arm7.Write32(kChannelCnt + 0x10u, kLoop | kPcm8 | kFullVolume); // clear start: stop
    t.Expect(!busy(1u) && run(4u) == std::vector<int16_t>({0}), "clearing the start bit stops a channel");

    // IMA-ADPCM: header PCM 4000h at index 88, then nibbles 0 and 8 (step 7FFFh, then 7462h).
    // 4000h + 7FFFh/8 = 4FFFh (DAC 0x200+319), then - 7462h/8 = 4173h (DAC 0x200+261).
    arm7.Write32(0x02000200u, 0x00584000u);
    arm7.Write32(0x02000204u, 0x00000080u);
    for (uint32_t i = 2u; i < 4u; ++i) {
        arm7.Write32(0x02000200u + i * 4u, 0u);
    }
    channel(2u, 0x02000200u, slow, 0u, 4u, kStart | kOneShot | kAdpcm | kFullVolume);
    left = run(80u);
    t.Expect(left.size() >= 3u && left[0] == 0 && left[1] == 319 * 64 && left[2] == 261 * 64,
        "ADPCM decodes with GBATEK's step table and index steps");
    arm7.Write32(kChannelCnt + 0x20u, 0u);

    // PSG 50% duty on channel 8: starts LOW, four LOW then four HIGH steps.
    channel(8u, 0u, slow, 0u, 0u, kStart | kPsg | (3u << 24) | kFullVolume);
    left = run(80u);
    t.Expect(left.size() >= 4u && left[0] == 0 && left[1] == -32768 && left[2] == 32704 && left[3] == -32768,
        "a PSG square wave alternates LOW and HIGH");
    arm7.Write32(kChannelCnt + 0x80u, 0u);
    channel(3u, 0u, slow, 0u, 0u, kStart | kPsg | kFullVolume);
    t.Expect(run(20u) == std::vector<int16_t>({0}), "channels 0-7 have no PSG");
    arm7.Write32(kChannelCnt + 0x30u, 0u);
    channel(14u, 0u, slow, 0u, 0u, kStart | kPsg | kFullVolume);
    left = run(20u);
    t.Expect(left.size() >= 2u && left[1] == -32768, "noise starts from X=7FFFh, whose first output is LOW");
    arm7.Write32(kChannelCnt + 0xE0u, 0u);

    // Capture 0 records the left mixer as PCM16, at channel 1's timer rate.
    channel(0u, 0x02000000u, slow, 1u, 3u, kStart | kLoop | kPcm16 | kFullVolume);
    arm7.Write32(0x04000418u, slow);         // SOUND1TMR doubles as the capture 0 timer
    arm7.Write32(0x04000510u, 0x02001000u);  // SNDCAP0DAD
    arm7.Write32(0x04000514u, 4u);           // SNDCAP0LEN: 4 words
    arm7.Write8(0x04000508u, 0x80u);         // start, left mixer, loop, PCM16
    t.Expect(arm7.Read32(0x04000514u) == 0u && arm7.Read32(0x04000510u) == 0x02001000u,
        "SNDCAPxDAD reads back, SNDCAPxLEN is write-only");
    run(120u);
    bool captured = false;
    for (uint32_t i = 0u; i < 8u; ++i) {
        captured = captured || arm7.Read16(0x02001000u + i * 2u) == 0x4000u;
    }
    t.Expect(captured && (arm7.Read8(0x04000508u) & 0x80u) != 0u, "capture writes the mixer output to memory and keeps looping");
    arm7.Write8(0x04000508u, 0x84u);         // restart as one-shot
    arm7.Write8(0x04000508u, 0x00u);
    arm7.Write8(0x04000508u, 0x84u);
    run(120u);
    t.Expect((arm7.Read8(0x04000508u) & 0x80u) == 0u, "a one-shot capture clears busy when its buffer is full");

    // Capture samples its source when its timer fires, not at the last output sample: a 512-cycle
    // capture of channel 0 (64-cycle period, so its 3-sample start delay ends at cycle 192) records
    // 4000h before the first output mix at cycle ~1023.
    arm7.Write8(0x04000508u, 0x00u);
    arm7.Write32(kChannelCnt, 0u);           // channel 0 silent for the last output mix before the capture
    sound.ClearSamples();
    sound.Advance(0x1000u);                  // line up just after an output sample
    for (uint32_t i = 0u; i < 4u; ++i) {
        arm7.Write32(0x02001000u + i * 4u, 0xFFFFFFFFu);
    }
    channel(0u, 0x02000000u, static_cast<uint16_t>(0x10000u - 32u), 1u, 3u, kStart | kLoop | kPcm16 | kFullVolume);
    arm7.Write32(0x04000418u, 0x10000u - 256u); // capture 0 period: 512 cycles
    arm7.Write8(0x04000508u, 0x82u);         // start, source channel 0, loop, PCM16
    sound.ClearSamples();
    sound.Advance(600u);
    (void)arm7.Read8(0x04000508u);           // a sound register access brings the unit up to date
    t.Expect(sound.samples().size() <= 2u && arm7.Read16(0x02001000u) == 0x4000u,
        "capture records its source at the capture event, before any output mix");
    arm7.Write8(0x04000508u, 0x00u);

    arm7.Write16(0x04000500u, 0x007Fu); // master enable off
    t.Expect(run(10u) == std::vector<int16_t>({0}), "SOUNDCNT master enable off silences the output");
}

void TestIndsGeometryAndWifiStubs(TestContext& t) {
    emugbds::inds::MemoryBus bus;
    auto& arm9 = bus.arm9();
    auto& arm7 = bus.arm7();
    t.Expect((arm9.Read32(0x04000600u) & 0x0E000000u) == 0x06000000u, "GXSTAT: FIFO empty, engine idle");
    arm9.Write32(0x04000600u, 2u << 30); // FIFO IRQ when empty
    t.Expect((arm9.Read32(0x04000214u) & (1u << 21)) != 0u, "the geometry FIFO IRQ follows its (always true) condition");
    arm9.Write32(0x04000214u, 1u << 21);
    t.Expect((arm9.Read32(0x04000214u) & (1u << 21)) != 0u, "acknowledging a level IRQ while its condition holds has no effect");

    t.Expect(arm7.Read16(0x04808000u) == 0x1440u, "W_ID reads the DS wifi chip ID");
    arm7.Write16(0x04808158u, 0x6000u); // BB read register 0
    t.Expect(arm7.Read16(0x0480815Cu) == 0x6Du && arm7.Read16(0x0480815Eu) == 0u, "baseband chip ID, never busy");
    arm7.Write16(0x0480815Au, 0x42u);
    arm7.Write16(0x04808158u, 0x5013u); // BB write register 13h
    arm7.Write16(0x04808158u, 0x6013u);
    t.Expect(arm7.Read16(0x0480815Cu) == 0x42u, "baseband registers read back");
    arm7.Write16(0x04804010u, 0xBEEFu);
    arm7.Write8(0x04804010u, 0x11u);
    t.Expect(arm7.Read16(0x0480C010u) == 0xBEEFu, "wifi RAM mirrors at 480C000h and ignores STRB");
}

void TestIndsMathUnit(TestContext& t) {
    emugbds::inds::MemoryBus bus;
    auto& arm9 = bus.arm9();
    auto result64 = [&](uint32_t address) {
        return static_cast<uint64_t>(arm9.Read32(address)) | (static_cast<uint64_t>(arm9.Read32(address + 4u)) << 32u);
    };
    arm9.Write16(0x04000280u, 0u); // 32/32
    arm9.Write32(0x04000290u, 7u);
    arm9.Write32(0x04000298u, static_cast<uint32_t>(-2));
    t.Expect(result64(0x040002A0u) == static_cast<uint64_t>(-3) && result64(0x040002A8u) == 1u,
        "32-bit signed divide sign-extends quotient and remainder");
    arm9.Write32(0x04000290u, 5u);
    arm9.Write32(0x04000298u, 0u);
    t.Expect((arm9.Read16(0x04000280u) & 0x4000u) != 0u, "divide by zero sets DIVCNT bit 14");
    t.Expect(result64(0x040002A0u) == 0x00000000FFFFFFFFull && result64(0x040002A8u) == 5u,
        "32-bit divide by zero gives -1 with the upper half inverted and REMAIN=NUMER");
    arm9.Write16(0x04000280u, 2u); // 64/64
    arm9.Write32(0x04000290u, 0u);
    arm9.Write32(0x04000294u, 0x10u); // 10_0000_0000h
    arm9.Write32(0x04000298u, 0x100u);
    arm9.Write32(0x0400029Cu, 0u);
    t.Expect(result64(0x040002A0u) == 0x0000000010000000ull && (arm9.Read16(0x04000280u) & 0x4000u) == 0u,
        "64-bit divide uses both halves and clears the DIV0 flag");
    arm9.Write16(0x040002B0u, 1u); // 64-bit sqrt
    arm9.Write32(0x040002B8u, 0u);
    arm9.Write32(0x040002BCu, 0x100u); // 2^40
    t.Expect(arm9.Read32(0x040002B4u) == (1u << 20), "64-bit square root");
    arm9.Write16(0x040002B0u, 0u); // 32-bit mode ignores the upper word
    arm9.Write32(0x040002B8u, 99u);
    t.Expect(arm9.Read32(0x040002B4u) == 9u, "32-bit square root rounds down and ignores the upper word");
}

void TestIndsRtc(TestContext& t) {
    emugbds::inds::MemoryBus bus;
    auto& arm7 = bus.arm7();
    constexpr uint16_t kClockSelectOut = 0x0060u; // SCK and CS driven by the CPU
    auto write_lines = [&](uint16_t lines) { arm7.Write16(0x04000138u, lines); };
    auto send_byte = [&](uint8_t byte) {
        for (int bit = 0; bit < 8; ++bit) {
            const uint16_t data = static_cast<uint16_t>((byte >> bit) & 1u);
            write_lines(static_cast<uint16_t>(kClockSelectOut | 0x10u | 0x4u | data));       // SCK low
            write_lines(static_cast<uint16_t>(kClockSelectOut | 0x10u | 0x4u | 0x2u | data)); // SCK high
        }
    };
    auto receive_byte = [&]() {
        uint8_t byte = 0u;
        for (int bit = 0; bit < 8; ++bit) {
            write_lines(kClockSelectOut | 0x4u);
            write_lines(kClockSelectOut | 0x4u | 0x2u);
            byte = static_cast<uint8_t>(byte | ((arm7.Read16(0x04000138u) & 1u) << bit));
        }
        return byte;
    };
    auto begin = [&](uint8_t command) {
        write_lines(kClockSelectOut | 0x10u | 0x2u); // CS low, SCK high
        write_lines(kClockSelectOut | 0x10u | 0x6u); // CS high
        send_byte(command);
    };
    auto end = [&]() { write_lines(kClockSelectOut | 0x10u | 0x2u); };

    begin(0x86u); // read status 1 (LSB-first command)
    t.Expect(receive_byte() == 0x02u, "RTC status 1 reads 24-hour mode with no power-off flag");
    end();
    begin(0x76u); // write free register
    send_byte(0x5Au);
    end();
    begin(0xF6u);
    t.Expect(receive_byte() == 0x5Au, "RTC free register reads back what was written");
    end();
    begin(0x6Fu); // MSB-first form of F6h
    t.Expect(receive_byte() == 0x5Au, "RTC accepts MSB-first commands");
    end();
    begin(0xA6u); // read date and time
    const uint8_t year = receive_byte();
    const uint8_t month = receive_byte();
    const uint8_t day = receive_byte();
    end();
    auto valid_bcd = [](uint8_t value, uint8_t low, uint8_t high) {
        return (value & 0x0Fu) <= 9u && value >= low && value <= high;
    };
    t.Expect(valid_bcd(year, 0x00u, 0x99u) && valid_bcd(month, 0x01u, 0x12u) && valid_bcd(day, 0x01u, 0x31u),
        "RTC date reads as valid BCD");
}

// Builds a direct-boot ROM: ARM9 code at ROM 400h -> 02000000h, ARM7 code at ROM 800h -> 02380000h.
std::vector<uint8_t> BuildDualCpuNdsRom(const std::vector<uint32_t>& arm9, const std::vector<uint32_t>& arm7) {
    std::vector<uint8_t> rom = BuildSyntheticNdsRom();
    rom.resize(0x1000u, 0u);
    WriteLe32(rom, 0x20, 0x00000400u);
    WriteLe32(rom, 0x2C, 0x00000200u);
    WriteLe32(rom, 0x30, 0x00000800u);
    WriteLe32(rom, 0x3C, 0x00000200u);
    for (size_t i = 0; i < arm9.size(); ++i) {
        WriteLe32(rom, 0x400u + i * 4u, arm9[i]);
    }
    for (size_t i = 0; i < arm7.size(); ++i) {
        WriteLe32(rom, 0x800u + i * 4u, arm7[i]);
    }
    return rom;
}

bool BootSyntheticDs(TestContext& t, emugbds::inds::Core& core, const std::filesystem::path& rom_path) {
    std::string error;
    const bool ok = core.Configure(std::nullopt, std::nullopt, std::nullopt, emugbds::core::DsBootMode::Direct, false, error) &&
        core.Initialize(error) && core.LoadPrimaryRom(rom_path, error);
    t.Expect(ok, "synthetic DS ROM direct-boots");
    return ok;
}

void TestIndsIrqWakesHaltedArm9(TestContext& t) {
    const auto dir = MakeTempRoot() / "inds-irq";
    std::filesystem::create_directories(dir);
    const auto nds = dir / "irq.nds";
    const uint32_t spin = EncodeBranchRelativeWords(-2);
    // Handler (copied to ITCM 18h): acknowledge V-Blank, mark r6, return.
    std::vector<uint32_t> arm9(0x60u, 0u);
    auto at = [&](uint32_t offset, uint32_t word) { arm9[offset / 4u] = word; };
    at(0x00u, EncodeLdrLiteralAt(0u, 0x00u, 0x100u));  // r0 = handler source
    at(0x04u, EncodeMovImm(1u, 0x18u));                // r1 = IRQ vector (low vectors)
    at(0x08u, EncodeLdmiaWriteback(0u, 0x003Cu));      // copy 8 words: r2-r5 twice
    at(0x0Cu, EncodeStmiaWriteback(1u, 0x003Cu));
    at(0x10u, EncodeLdmiaWriteback(0u, 0x003Cu));
    at(0x14u, EncodeStmiaWriteback(1u, 0x003Cu));
    at(0x18u, 0xEE112F10u);                            // MRC p15, 0, r2, c1, c0, 0
    at(0x1Cu, 0xE3C22A02u);                            // BIC r2, r2, #2000h (low vectors)
    at(0x20u, 0xEE012F10u);                            // MCR p15, 0, r2, c1, c0, 0
    at(0x24u, EncodeLdrLiteralAt(0u, 0x24u, 0x104u));  // r0 = DISPSTAT
    at(0x28u, EncodeMovImm(1u, 8u));
    at(0x2Cu, EncodeStrhImm(1u, 0u, 0u));              // V-Blank IRQ enable
    at(0x30u, EncodeLdrLiteralAt(0u, 0x30u, 0x108u));  // r0 = IME
    at(0x34u, EncodeMovImm(1u, 1u));
    at(0x38u, EncodeStrReg(1u, 0u, 0u));               // IME = 1
    at(0x3Cu, EncodeStrReg(1u, 0u, 8u));               // IE = V-Blank
    at(0x40u, 0xE321F01Fu);                            // MSR CPSR_c, #1Fh: IRQs on
    at(0x44u, 0xEE070F90u);                            // MCR p15, 0, r0, c7, c0, 4: wait for interrupt
    at(0x48u, EncodeMovImm(7u, 0x55u));                // runs after the IRQ returns
    at(0x4Cu, spin);
    // Handler words at 100h-ish source; literals after them.
    at(0x80u, EncodeLdrLiteralAt(0u, 0x00u, 0x14u));   // (relative to 18h) r0 = IF
    at(0x84u, EncodeMovImm(1u, 1u));
    at(0x88u, EncodeStrReg(1u, 0u, 0u));               // acknowledge V-Blank
    at(0x8Cu, EncodeMovImm(6u, 0x99u));
    at(0x90u, EncodeSubImmSetFlags(15u, 14u, 4u));     // SUBS pc, lr, #4
    at(0x94u, 0x04000214u);
    at(0x100u, 0x02000080u);
    at(0x104u, 0x04000004u);
    at(0x108u, 0x04000208u);
    arm9.resize(0x110u / 4u);
    WriteFile(nds, BuildDualCpuNdsRom(arm9, {spin}));

    emugbds::inds::Core core;
    if (!BootSyntheticDs(t, core, nds)) {
        return;
    }
    std::string error;
    t.Expect(core.RunFrame(error), "inds IRQ core runs a frame");
    t.Expect(core.arm9().r[6] == 0x99u, "V-Blank IRQ enters the ARM9 handler through the vector");
    t.Expect(core.arm9().r[7] == 0x55u && !core.arm9_halted(), "the IRQ wakes the halted ARM9 and returns to its code");
    t.Expect((core.arm9().cpsr & 0x1Fu) == 0x1Fu, "the handler returns to System mode");
}

void TestIndsHaltcntGbaModeTransition(TestContext& t) {
    const auto dir = MakeTempRoot() / "inds-gba-mode";
    std::filesystem::create_directories(dir);
    const auto nds = dir / "gba_mode.nds";
    const uint32_t spin = EncodeBranchRelativeWords(-2);
    WriteFile(nds, BuildDualCpuNdsRom({spin}, {
        EncodeLdrLiteralAt(0u, 0x00u, 0x20u),  // r0 = POSTFLG/HALTCNT
        EncodeLdrLiteralAt(1u, 0x04u, 0x24u),  // r1 = HALTCNT mode 1 in the upper byte
        EncodeStrhImm(1u, 0u, 0u),
        EncodeMovImm(5u, 0x77u), // Must not run: the DS stops at the GBA-mode request.
        spin, 0u, 0u, 0u,
        0x04000300u, 0x00004000u,
    }));

    emugbds::inds::Core core;
    if (!BootSyntheticDs(t, core, nds)) {
        return;
    }
    std::string error;
    t.Expect(core.RunFrame(error), "inds GBA-mode core runs a frame");
    const auto transition = core.PollTransition();
    t.Expect(transition.has_value() && transition->to_mode == emugbds::core::EmuMode::GBA,
        "ARM7 HALTCNT GBA mode requests the DS -> GBA transition");
    t.Expect(transition.has_value() && transition->reason.find("HALTCNT") != std::string::npos,
        "the transition names its HALTCNT cause");
    t.Expect(core.arm7().r[5] == 0u, "the ARM7 stops at the instruction that requested GBA mode");
}

void TestIndsDirectBootRunsBothCpus(TestContext& t) {
    const auto dir = MakeTempRoot() / "inds-dual-cpu";
    std::filesystem::create_directories(dir);
    const auto nds = dir / "dual.nds";

    std::vector<uint8_t> rom = BuildSyntheticNdsRom();
    rom.resize(0x1000u, 0u);
    WriteLe32(rom, 0x20, 0x00000400u); // ARM9 binary at ROM 400h -> 02000000h
    WriteLe32(rom, 0x2C, 0x00000100u);
    WriteLe32(rom, 0x30, 0x00000800u); // ARM7 binary at ROM 800h -> 02380000h
    WriteLe32(rom, 0x3C, 0x00000100u);
    auto arm9_word = [&](uint32_t offset, uint32_t word) { WriteLe32(rom, 0x400u + offset, word); };
    auto arm7_word = [&](uint32_t offset, uint32_t word) { WriteLe32(rom, 0x800u + offset, word); };
    auto bne = [](int32_t relative_words) { return (EncodeBranchRelativeWords(relative_words) & 0x0FFFFFFFu) | 0x10000000u; };

    // ARM9: read CP15's ID and the header copy, then wait for the ARM7's word in main RAM.
    arm9_word(0x00u, 0xEE103F10u);                          // MRC p15, 0, r3, c0, c0, 0
    arm9_word(0x04u, EncodeLdrLiteralAt(4u, 0x04u, 0x40u)); // r4 = 027FFE0Ch
    arm9_word(0x08u, EncodeLdrReg(4u, 4u, 0u));             // r4 = game code
    arm9_word(0x0Cu, EncodeLdrLiteralAt(1u, 0x0Cu, 0x44u)); // r1 = 02100000h
    arm9_word(0x10u, EncodeLdrReg(2u, 1u, 0u));             // loop: r2 = [r1]
    arm9_word(0x14u, EncodeSubImmSetFlags(5u, 2u, 0u));
    arm9_word(0x18u, (bne(-4) & 0x0FFFFFFFu) | 0x00000000u); // BEQ loop
    arm9_word(0x1Cu, EncodeBranchRelativeWords(-2));
    arm9_word(0x40u, 0x027FFE0Cu);
    arm9_word(0x44u, 0x02100000u);
    // ARM7: publish a value for the ARM9.
    arm7_word(0x00u, EncodeMovImm(0u, 0x22u));
    arm7_word(0x04u, EncodeLdrLiteralAt(1u, 0x04u, 0x40u));
    arm7_word(0x08u, EncodeStrReg(0u, 1u, 0u));
    arm7_word(0x0Cu, EncodeBranchRelativeWords(-2));
    arm7_word(0x40u, 0x02100000u);
    WriteFile(nds, rom);

    emugbds::inds::Core core;
    std::string error;
    t.Expect(core.Configure(std::nullopt, std::nullopt, std::nullopt, emugbds::core::DsBootMode::Direct, false, error),
        "inds dual-CPU core configures");
    t.Expect(core.Initialize(error) && core.LoadPrimaryRom(nds, error), "inds dual-CPU core direct-boots the ROM");
    t.Expect((core.arm9().cpsr & 0x1Fu) == 0x1Fu && core.arm9().r[13] != 0u, "inds direct boot enters System mode with a stack");
    const auto& cp15 = core.memory().cp15();
    const uint32_t sp = core.arm9().r[13];
    t.Expect(sp - cp15.dtcm_base() < cp15.dtcm_size() && sp >= cp15.itcm_size(),
        "inds direct boot puts the ARM9 stack in DTCM, outside the ITCM window");
    t.Expect(core.RunFrame(error), "inds dual-CPU core runs a frame");
    t.Expect(core.arm9().r[3] == 0x41059461u, "inds ARM9 reads CP15 through MRC");
    t.Expect(core.arm9().r[4] == 0x4442474Eu, "inds direct boot copies the header to 027FFE00h");
    t.Expect(core.arm7().r[0] == 0x22u, "inds ARM7 runs its own binary");
    t.Expect(core.arm9().r[2] == 0x22u, "inds ARM9 sees the ARM7's main RAM write");
    t.Expect(core.arm9_cpu().unknown_arm_instruction_count() == 0u && core.arm7_cpu().unknown_arm_instruction_count() == 0u,
        "inds dual-CPU programs use only supported instructions");
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

// With both BIOS images and a firmware image, ds-firmware runs the BIOS from reset instead of
// simulating the Slot-2 pick; the ARM7 BIOS here writes HALTCNT GBA mode (or just loops).
void TestIndsCoreRealFirmwareBoot(TestContext& t) {
    const auto dir = MakeTempRoot() / "inds-core-real-firmware";
    std::filesystem::create_directories(dir);
    const auto slot2 = dir / "test.gba";
    WriteFile(slot2, BuildSyntheticGbaRom());
    std::vector<uint8_t> bios9(0x1000u, 0u);
    WriteWordLe(bios9, 0x0u, EncodeBranchRelativeWords(-2)); // FFFF0000h: B .
    WriteFile(dir / "bios9.bin", bios9);
    std::vector<uint8_t> loop7(0x4000u, 0u);
    WriteWordLe(loop7, 0x0u, EncodeBranchRelativeWords(-2));
    WriteFile(dir / "loop7.bin", loop7);
    std::vector<uint8_t> gba7(0x4000u, 0u);
    WriteWordLe(gba7, 0x00u, 0xE3A00404u); // MOV r0, #04000000h
    WriteWordLe(gba7, 0x04u, 0xE2800C03u); // ADD r0, r0, #300h
    WriteWordLe(gba7, 0x08u, 0xE3A01040u); // MOV r1, #40h (GBA mode)
    WriteWordLe(gba7, 0x0Cu, 0xE5C01001u); // STRB r1, [r0, #1] -> HALTCNT
    WriteWordLe(gba7, 0x10u, EncodeBranchRelativeWords(-2));
    WriteFile(dir / "gba7.bin", gba7);
    WriteFile(dir / "firmware.bin", BuildBytes(0x40000u, 0xF0));

    for (const char* bios7 : {"loop7.bin", "gba7.bin"}) {
        emugbds::inds::Core core;
        std::string error;
        const bool setup = core.Configure(dir / bios7, dir / "bios9.bin", dir / "firmware.bin",
                               emugbds::core::DsBootMode::DsFirmware, false, error) &&
            core.Initialize(error) && core.LoadSlot2Rom(slot2, error) && core.LoadPrimaryRom({}, error);
        t.Expect(setup, "inds core sets up a real firmware boot");
        std::optional<emugbds::core::TransitionEvent> transition;
        for (int frame = 0; frame < 3 && setup && !transition.has_value(); ++frame) {
            t.Expect(core.RunFrame(error), "inds firmware boot runs a frame");
            transition = core.PollTransition();
        }
        if (std::string(bios7) == "loop7.bin") {
            t.Expect(!transition.has_value(), "a real firmware boot never fakes the Slot-2 pick");
            t.Expect(core.arm9().r[15] - 0xFFFF0000u < 0x10u, "the ARM9 starts at its BIOS reset vector");
        } else {
            t.Expect(transition.has_value() && transition->reason.find("HALTCNT") != std::string::npos,
                "the BIOS writing HALTCNT GBA mode hands over to the GBA core");
        }
    }
}

void TestIndsKeysAndTouch(TestContext& t) {
    const auto dir = MakeTempRoot() / "inds-touch";
    std::filesystem::create_directories(dir);
    std::vector<uint8_t> image(0x40000u, 0u);
    // Newer user settings copy (update counter 1 at 3FF70h vs 0): ADC (100,200)-(3100,2200) at pixels (10,10)-(210,110).
    const size_t user = 0x3FF00u;
    image[0x3FE70u] = 0u;
    image[user + 0x70u] = 1u;
    const uint8_t cal[12] = {100, 0, 200, 0, 10, 10, 0x1C, 0x0C, 0x98, 0x08, 210, 110};
    std::copy(std::begin(cal), std::end(cal), image.begin() + static_cast<std::ptrdiff_t>(user + 0x58u));
    WriteFile(dir / "firmware.bin", image);

    emugbds::inds::Core core;
    std::string error;
    t.Expect(core.Configure(std::nullopt, std::nullopt, dir / "firmware.bin", emugbds::core::DsBootMode::Direct, false, error) &&
            core.Initialize(error),
        "inds core loads a firmware image for touch calibration");
    auto& arm7 = const_cast<emugbds::inds::MemoryBus&>(core.memory()).arm7();
    using emugbds::input::GbaKey;
    using emugbds::input::GbaKeyMask;
    core.SetKeys(GbaKeyMask(GbaKey::A) | GbaKeyMask(GbaKey::X));
    t.Expect(arm7.Read16(0x04000130u) == (0x03FFu & ~1u), "KEYINPUT shows A pressed (0 = pressed)");
    t.Expect((arm7.Read16(0x04000136u) & 0x43u) == 0x42u, "EXTKEYIN shows X pressed, Y released, pen up");

    core.SetTouch(true, 110u, 60u); // midway between the calibration points
    t.Expect((arm7.Read16(0x04000136u) & 0x40u) == 0u, "EXTKEYIN shows the pen down");
    auto convert = [&](uint8_t control) {
        auto exchange = [&](uint16_t spicnt, uint8_t out) {
            arm7.Write16(0x040001C0u, spicnt);
            arm7.Write16(0x040001C2u, out);
            return static_cast<uint8_t>(arm7.Read16(0x040001C2u));
        };
        constexpr uint16_t kTouchHold = 0x8000u | 0x0200u | 0x0800u;
        exchange(kTouchHold, control);
        const uint8_t high = exchange(kTouchHold, 0u);
        const uint8_t low = exchange(0x8000u | 0x0200u, 0u);
        return static_cast<uint16_t>((high << 5u) | (low >> 3u));
    };
    t.Expect(convert(0xD1u) == 1600u, "touchscreen X conversion inverts the firmware calibration");
    t.Expect(convert(0x91u) == 1200u, "touchscreen Y conversion inverts the firmware calibration");
    core.SetTouch(false, 0u, 0u);
    t.Expect((arm7.Read16(0x04000136u) & 0x40u) != 0u && convert(0xD1u) == 0u, "lifting the pen reads as up and 0");
}

void TestIndsEngine2dRendering(TestContext& t) {
    emugbds::inds::MemoryBus bus;
    auto& arm9 = bus.arm9();
    std::array<uint32_t, 256> line{};
    auto& engine = bus.engine(0);
    engine.RenderLine(0u, true, line.data());
    t.Expect(line[0] == 0xFFFFFFFFu, "display mode 0 shows a white line");

    arm9.Write8(0x04000240u, 0x81u);                 // VRAM A: engine A BG at 6000000h
    arm9.Write8(0x04000241u, 0x82u);                 // VRAM B: engine A OBJ at 6400000h
    arm9.Write16(0x05000000u, 0x0000u);              // backdrop black
    arm9.Write16(0x05000002u, 0x001Fu);              // BG palette 1: red
    arm9.Write16(0x05000202u, 0x7C00u);              // OBJ palette 1: blue
    arm9.Write16(0x04000008u, 0x0100u);              // BG0CNT: char base 0, screen base 2 KiB, 16 colours
    arm9.Write16(0x06000800u, 0x0001u);              // map entry (0,0) = tile 1
    for (uint32_t i = 0u; i < 32u; ++i) {
        arm9.Write16(0x06000020u + i * 2u, 0x1111u); // tile 1: all colour 1
        arm9.Write16(0x06400020u + i * 2u, 0x1111u); // OBJ tile 1
    }
    arm9.Write32(0x04000000u, 0x00011110u);          // graphics display, BG0 + OBJ, 1D OBJ mapping
    arm9.Write16(0x07000000u, 0x0000u);              // OBJ 0: y=0, 8x8
    arm9.Write16(0x07000002u, 0x0004u);              // x=4
    arm9.Write16(0x07000004u, 0x0001u);              // tile 1, priority 0
    engine.RenderLine(0u, true, line.data());
    t.Expect(line[0] == 0xFF0000FFu, "a text BG tile draws its palette colour (red)");
    t.Expect(line[4] == 0xFFFF0000u, "an OBJ draws over a BG of equal priority (blue)");
    t.Expect(line[12] == 0xFF000000u, "beyond the tile and OBJ the backdrop shows");
    engine.set_hidden_layers(0x10u); // debug: hide OBJ
    engine.RenderLine(0u, true, line.data());
    t.Expect(line[4] == 0xFF0000FFu, "hiding OBJ shows the BG under it");
    engine.set_hidden_layers(0x01u); // hide BG0
    engine.RenderLine(0u, true, line.data());
    t.Expect(line[0] == 0xFF000000u && line[4] == 0xFFFF0000u, "hiding BG0 leaves the OBJ over the backdrop");
    engine.set_hidden_layers(0u);
    for (uint32_t i = 0u; i < 32u; ++i) {
        arm9.Write16(0x06400040u + i * 2u, 0x0101u); // 2D 256-colour OBJ data at tile 2 (64 bytes): colour 1
    }
    arm9.Write32(0x04000000u, 0x00011100u);          // 2D OBJ mapping
    arm9.Write16(0x07000000u, 0x2000u);              // 256 colours
    arm9.Write16(0x07000004u, 0x0003u);              // odd tile 3: bit 0 ignored -> tile 2
    engine.RenderLine(0u, true, line.data());
    t.Expect(line[4] == 0xFFFF0000u, "a 256-colour 2D OBJ ignores tile number bit 0");
    arm9.Write32(0x04000000u, 0x00011110u);
    arm9.Write16(0x07000000u, 0x0000u);
    arm9.Write16(0x07000004u, 0x0001u);
    arm9.Write16(0x0400006Cu, 0x8010u);              // master brightness down, factor 16
    engine.RenderLine(0u, true, line.data());
    t.Expect(line[0] == 0xFF000000u, "master brightness down 16 makes black");
    arm9.Write16(0x0400006Cu, 0u);

    arm9.Write8(0x04000242u, 0x80u);                 // VRAM C: LCDC
    arm9.Write16(0x06840000u + (2u * 256u + 3u) * 2u, 0x03E0u); // pixel (3, 2): green
    arm9.Write32(0x04000000u, 0x00020000u | (2u << 18)); // VRAM display of bank C
    engine.RenderLine(2u, true, line.data());
    t.Expect(line[3] == 0xFF00FF00u, "VRAM display mode shows the LCDC bank as a bitmap");
    engine.RenderLine(2u, false, line.data());
    t.Expect(line[3] == 0xFFFFFFFFu, "a powered-off engine shows white");
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
    auto& memory = const_cast<emugbds::inds::MemoryBus&>(core.memory());
    t.Expect(memory.arm9().Read32(0x027FF800u) == memory.io().card().ChipId() &&
            memory.arm9().Read16(0x027FFC40u) == 1u,
        "direct boot leaves the chip ID and boot indicator where the BIOS would");

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

void TestFrontendSettings(TestContext& t) {
    using emugbds::frontend::DsLayout;
    using emugbds::frontend::Settings;
    const auto dir = MakeTempRoot() / "frontend-settings";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto path = dir / "emugbds.ini";
    std::string error;

    Settings missing;
    t.Expect(missing.Load(path, error), "a missing settings file loads");
    t.Expect(missing.gba_scale == 3 && missing.ds_scale == 2 && missing.keys[0] == "Z" && missing.keys[3] == "Return",
        "a missing settings file keeps the defaults");

    Settings settings;
    for (int i = 0; i < 10; ++i) {
        settings.AddRecentRom("rom" + std::to_string(i) + ".gba");
    }
    settings.AddRecentRom("rom5.gba");
    t.Expect(settings.recent_roms.size() == Settings::kMaxRecentRoms && settings.recent_roms[0] == "rom5.gba" &&
        settings.recent_roms[1] == "rom9.gba" && std::count(settings.recent_roms.begin(), settings.recent_roms.end(),
        std::string("rom5.gba")) == 1, "recent ROMs are most recent first, without duplicates, and capped");

    settings.gba_bios = "D:/bios/gba bios.bin";
    settings.nds_firmware = "fw.bin";
    settings.last_directory = "D:/roms";
    settings.ds_scale = 4;
    settings.ds_layout = DsLayout::Horizontal;
    settings.fast_forward_speed = 0;
    settings.gba_via_ds_firmware = true;
    settings.keys[3] = "Space";
    t.Expect(settings.Save(path, error), "settings save");
    t.Expect(!std::filesystem::exists(dir / "emugbds.ini.tmp"), "saving leaves no temporary file");
    Settings reloaded;
    t.Expect(reloaded.Load(path, error), "saved settings load");
    t.Expect(reloaded.gba_bios == settings.gba_bios && reloaded.nds_firmware == "fw.bin" &&
        reloaded.nds_bios7.empty() && reloaded.last_directory == "D:/roms" &&
        reloaded.recent_roms == settings.recent_roms && reloaded.ds_scale == 4 &&
        reloaded.ds_layout == DsLayout::Horizontal && reloaded.fast_forward_speed == 0 &&
        reloaded.gba_via_ds_firmware && reloaded.keys == settings.keys, "every setting survives a save and load");

    {
        std::ofstream edited(path, std::ios::trunc);
        edited << "gba_scale=99\nds_scale=x\nds_layout=diagonal\nfast_forward_speed=-1\nkey.A=\n"
               << "not a setting\n# key.B=W\nkey.B=Q\r\nkey.Nope=Z\n";
    }
    Settings hand_edited;
    t.Expect(hand_edited.Load(path, error), "a hand-edited settings file loads");
    t.Expect(hand_edited.gba_scale == 3 && hand_edited.ds_scale == 2 && hand_edited.ds_layout == DsLayout::Vertical &&
        hand_edited.fast_forward_speed == 4 && hand_edited.keys[0] == "Z",
        "out-of-range, malformed, and empty values keep the defaults");
    t.Expect(hand_edited.keys[1] == "Q", "a CRLF line still sets its key");
    std::filesystem::remove_all(dir);
}

int RunUnitTests() {
    struct NamedTest {
        const char* name;
        void (*fn)(TestContext&);
    };

    const std::array<NamedTest, 66> tests = {{
        {"frontend_settings", &TestFrontendSettings},
        {"arm_v5_clz_and_saturation", &TestArmV5ClzAndSaturation},
        {"arm_v5_halfword_multiplies", &TestArmV5HalfwordMultiplies},
        {"arm_v5_branches_and_interworking", &TestArmV5BranchesAndInterworking},
        {"arm_memory_additions", &TestArmMemoryAdditions},
        {"arm_v5_coprocessor_transfers", &TestArmV5CoprocessorTransfers},
        {"ingba_memory_bus", &TestIngbaMemoryBus},
        {"ingba_backup", &TestIngbaBackup},
        {"ingba_core_run_frame", &TestIngbaCoreRunFrame},
        {"ingba_cpu_fiq_banking", &TestIngbaCpuFiqBanking},
        {"ingba_fiq_user_bank_transfers", &TestIngbaFiqUserBankTransfers},
        {"ingba_irq_wakes_halt", &TestIngbaIrqWakesHalt},
        {"ingba_halt_wakes_with_masked_irq", &TestIngbaHaltWakesWithMaskedIrq},
        {"ingba_apu_fifo_mixing", &TestIngbaApuFifoMixing},
        {"ingba_sound_dma_refills_fifo", &TestIngbaSoundDmaRefillsFifo},
        {"ingba_dma_latches_count_on_enable", &TestIngbaDmaLatchesCountOnEnable},
        {"ingba_timer_wakes_halt_promptly", &TestIngbaTimerWakesHaltPromptly},
        {"ingba_vcount_target_write_mid_line", &TestIngbaVCountTargetWriteMidLine},
        {"ingba_blend_changes_per_scanline", &TestIngbaBlendChangesPerScanline},
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
        {"ingba_thumb_bl_suffix_uses_lr", &TestIngbaThumbBlSuffixUsesLr},
        {"ingba_load_rotation_and_ldm_base", &TestIngbaLoadRotationAndLdmBase},
        {"ingba_thumb_register_shift_by_zero", &TestIngbaThumbRegisterShiftByZero},
        {"ingba_video_mode3_render", &TestIngbaVideoMode3Render},
        {"ingba_video_mode4_render", &TestIngbaVideoMode4Render},
        {"ingba_video_obj_render", &TestIngbaVideoObjRender},
        {"ingba_video_mode2_render", &TestIngbaVideoMode2Render},
        {"inds_memory_bus", &TestIndsMemoryBus},
        {"inds_cp15_and_tcm", &TestIndsCp15AndTcm},
        {"inds_wramcnt", &TestIndsWramcnt},
        {"inds_direct_boot_runs_both_cpus", &TestIndsDirectBootRunsBothCpus},
        {"inds_timers", &TestIndsTimers},
        {"inds_ipc", &TestIndsIpc},
        {"inds_io_interrupts_and_dma", &TestIndsIoInterruptsAndDma},
        {"inds_irq_wakes_halted_arm9", &TestIndsIrqWakesHaltedArm9},
        {"inds_empty_card_slot", &TestIndsEmptyCardSlot},
        {"inds_spi_devices", &TestIndsSpiDevices},
        {"inds_vram_banking", &TestIndsVramBanking},
        {"inds_cartridge_reads", &TestIndsCartridgeReads},
        {"inds_backup_memory", &TestIndsBackupMemory},
        {"inds_geometry_and_wifi_stubs", &TestIndsGeometryAndWifiStubs},
        {"inds_sound", &TestIndsSound},
        {"inds_math_unit", &TestIndsMathUnit},
        {"inds_rtc", &TestIndsRtc},
        {"inds_haltcnt_gba_mode_transition", &TestIndsHaltcntGbaModeTransition},
        {"inds_core_firmware_slot2_transition", &TestIndsCoreFirmwareSlot2Transition},
        {"inds_core_real_firmware_boot", &TestIndsCoreRealFirmwareBoot},
        {"inds_keys_and_touch", &TestIndsKeysAndTouch},
        {"inds_engine2d_rendering", &TestIndsEngine2dRendering},
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
