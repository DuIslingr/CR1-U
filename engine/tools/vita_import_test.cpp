// Synthetic exports only. These tests need no game assets or display server.
#include "platform/vita_import_ui.hpp"
#include <iostream>

using namespace vitaimport;
void check(bool good, const char *message) {
    if (!good)
        throw Error(message);
}
template <class F> void rejects(F f) {
    bool failed = false;
    try {
        f();
    } catch (const std::exception &) {
        failed = true;
    }
    check(failed, "Invalid input was accepted");
}
void put(Bytes &b, size_t p, uint32_t n, int bytes = 4) {
    for (int k = 0; k < bytes; ++k)
        b.at(p + size_t(k)) = uint8_t(n >> (8 * k));
}
void saveBinary(const fs::path &p, Bytes b) {
    put(b, 2, 0x3f6, 2);
    put(b, 0, crc(b), 2);
    write(p, std::string(reinterpret_cast<const char *>(b.data()), b.size()));
}
std::string content(const fs::path &p) {
    const Bytes b = read(p);
    return {b.begin(), b.end()};
}
// a converted save without its `saved` line: each prepare stamps it with the second it ran in
std::string unstamped(std::string s) {
    const size_t at = s.find("\nsaved ");
    return at == std::string::npos ? s : s.erase(at, s.find('\n', at + 1) - at);
}
std::string zip(const std::string &s) {
    std::string out(compressBound(uLong(s.size())), '\0');
    uLongf n = uLongf(out.size());
    check(compress(reinterpret_cast<Bytef *>(out.data()), &n, reinterpret_cast<const Bytef *>(s.data()),
                   uLong(s.size())) == Z_OK,
          "compress");
    out.resize(n);
    return out;
}
Bytes encoded(const std::string &json, bool compressed) {
    std::string body;
    const std::string key = "mediavision";
    for (unsigned char c : json)
        for (int n : {int(c >> 4), int(c & 15)})
            body += char(((n ^ key[body.size() % key.size()]) - 1) & 255);
    if (compressed) {
        const std::string z = zip(body);
        Bytes prefix(20);
        std::memcpy(prefix.data(), "^q7@#=0TmLx3MC5$", 16);
        put(prefix, 16, uint32_t(z.size()));
        body = std::string(prefix.begin(), prefix.end()) + z;
    }
    Bytes out(4);
    put(out, 0, uint32_t(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}
int main() {
    const fs::path root = fs::temp_directory_path() / ("vita-import-test-" + unique());
    try {
        fs::create_directories(root);
        for (const auto &title : {"PCSG00497", "PCSG00499", "PCSG00498", "PCSG00457"})
            fs::create_directory(root / title);
        Bytes first(0xc24), omega(0x1558), second(0x910), sys(0x98);
        for (int c = 0; c < 8; ++c)
            put(first, 0x320 + size_t(c) * 0x4c + 0x48, 100, 2);
        first[0xc14] = 1;
        put(first, 4, 36);
        put(first, 8, 195165);
        for (int c = 0; c < 10; ++c)
            put(omega, 0x4a4 + size_t(c) * 0x4c + 0x48, 1, 2);
        omega[0x1546] = omega[0x1547] = omega[0x1548] = 1;
        omega[0x24 + 0x1004 / 8] |= 1 << (0x1004 % 8);
        saveBinary(root / "PCSG00497/SAVEDATA0001.bin", first);
        saveBinary(root / "PCSG00499/SAVEDATA0011.bin", omega);
        write(root / "PCSG00499/SAVEDATA0012.bin", "broken suspend");
        put(second, 0x5f0, 1);
        put(second, 0x5f4, 2);
        put(sys, 0x80, 2);
        saveBinary(root / "PCSG00498/SAVEDATA0020.bin", sys);
        saveBinary(root / "PCSG00498/SAVEDATA0021.bin", second);
        const std::string json =
            R"({"flags":[5],"vipTime":1,"vipTime":2,"name":"\u65e5\u672c\u8a9e \ud83d\ude00","savedata":{"main_parties":[1,2,3,4,5],"field":{"nDungeonNum":9,"nAreaNum":1,"vPosition":[1,2,3],"qRotation":[0,0,0,1]}},"profile":{"teamLevel":139}})";
        for (bool compressed : {false, true}) {
            Bytes b = encoded(json, compressed);
            b.insert(b.end(), {'o', 'l', 'd'}); // stale tail from the original writer is outside its payload
            check(cr3Document(b) == json, "CR3 encoding or payload boundary");
            check(JsonParser(cr3Document(b)).parse()["vipTime"].n() == 2, "Original duplicate JSON fields");
            b.resize(12);
            rejects([&] { cr3Document(b); });
        }
        const Bytes third = encoded(json, true);
        write(root / "PCSG00457/SAVEDATA0031.bin", std::string(third.begin(), third.end()));
        check(prepare(root.u8string(), "cr1").files.at("save1.dat").find("play 129795.165") != std::string::npos,
              "Completed hours lost");
        auto op = prepare(root.u8string(), "omega");
        check(op.slots == 2 && op.warnings.size() == 1, "Optional bad suspend must be skipped");
        check(op.files.at("save1.dat").find("noencounters 1 0 0 0") != std::string::npos, "Omega encounter switch");
        check(op.files.at("save1.dat").find("scenario_genes 1") != std::string::npos, "Omega local genes");
        auto sp = prepare((root / "PCSG00498").u8string(), "cr2");
        check(sp.files.at("system.txt").find("options 2 0 0 0 0") != std::string::npos, "CR2 Vita system options");
        check(sp.files.at("cont.txt") == sp.files.at("save1.txt"), "Manual fallback without suspend");
        check(prepare(root.u8string(), "cr3").files.at("save1.txt").find("area d0502") != std::string::npos,
              "CR3 dungeon mapping");
        for (const std::string game : {"cr1", "omega", "cr2", "cr3"}) {
            const fs::path dest = root / fs::u8path(game + "-profile-\xc3\xa9");
            fs::create_directory(dest);
            const auto plan = prepare(root.u8string(), game);
            for (const auto &[name, data] : plan.files) {
                (void)data;
                write(dest / name, "previous " + name);
            }
            write(dest / "settings.ini", "keep settings");
            Session ui;
            ui.game = game;
            ui.prepareFolder(root.u8string());
            check(ui.state == Session::Confirm && ui.focus == 1, "Confirmation must default to No");
            ui.answer(false, dest.u8string());
            check(!ui.active(), "Cancel must close confirmation");
            for (const auto &[name, data] : plan.files) {
                (void)data;
                check(content(dest / name) == "previous " + name, "Cancel modified saves");
            }
            ui.prepareFolder(root.u8string());
            ui.answer(true, dest.u8string());
            check(ui.state == Session::Result && ui.succeeded, "Import result");
            fs::path backup;
            for (const auto &f : fs::directory_iterator(dest))
                if (f.path().filename().u8string().find("vita-backup-") == 0)
                    backup = f.path();
            check(!backup.empty(), "Missing backup");
            for (const auto &[name, data] : plan.files) {
                check(unstamped(content(dest / name)) == unstamped(data), "Incorrect installed save");
                check(content(backup / name) == "previous " + name, "Backup lost old save");
            }
            check(content(dest / "settings.ini") == "keep settings", "Unrelated profile file changed");
            ui.answer(false, dest.u8string());
            check(!ui.active(), "Result acknowledgement");
            ui.prepareFolder((root / "missing").u8string());
            check(ui.state == Session::Result && !ui.succeeded, "Invalid folder must give an error");
            std::cout << game << ": confirmation, cancel, import, backup, result, invalid folder passed\n";
        }
        // A destination conflict fails before the commit and preserves existing saves.
        const fs::path dest = root / "rollback";
        fs::create_directory(dest);
        write(dest / "save1.txt", "old");
        Plan bad;
        bad.files = {{"save1.txt", "new"}, {"save2.txt", "new2"}};
        fs::create_directory(dest / "save2.txt");
        rejects([&] { install(bad, dest.u8string()); });
        check(content(dest / "save1.txt") == "old", "Failed installation changed saves");
        bad.files = {{"../outside", "invalid"}};
        rejects([&] { install(bad, dest.u8string()); });
        check(!fs::exists(root / "outside"), "Output path escaped profile");
        Bytes corrupt = read(root / "PCSG00497/SAVEDATA0001.bin");
        corrupt[100] ^= 1;
        write(root / "PCSG00497/SAVEDATA0001.bin", std::string(corrupt.begin(), corrupt.end()));
        rejects([&] { prepare(root.u8string(), "cr1"); });
        rejects([&] { JsonParser("{\"x\":\"\\ud800\"}").parse(); });
        rejects([&] { inflate("corrupt"); });
        check(read(root / "PCSG00457/SAVEDATA0031.bin") == third, "Source save changed");
        fs::remove_all(root);
        std::cout << "Native Vita import checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << " (fixtures: " << root.u8string() << ")\n";
        fs::remove_all(root);
        return 1;
    }
}
