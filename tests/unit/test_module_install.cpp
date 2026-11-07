// Install from .zip: good packages install, and malicious zips write nothing.
#include <doctest/doctest.h>

#include <dsptb/core/module_install.hpp>

#include <miniz.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace dsptb;
namespace fs = std::filesystem;

namespace {

const char* const kManifest = R"({"schema":1,"id":"example.zipped","name":"Zipped","version":"1.2.3","abi":1,
  "dll":"zipped.dll","modules":[{"id":"example.zipped","kind":"processor"}]})";

struct Entry {
    std::string name, content;
};

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeAll(const fs::path& p, const std::string& s) {
    std::ofstream out(p, std::ios::binary);
    out.write(s.data(), static_cast<std::streamsize>(s.size()));
}

// Calls fn(offset) for each central directory record of a zip held in memory.
template <class Fn> void forEachCentralRecord(std::string& zip, Fn fn) {
    const char sig[4] = {'P', 'K', 1, 2};
    for (size_t at = zip.find(sig, 0, 4); at != std::string::npos; at = zip.find(sig, at + 4, 4))
        fn(at);
}

class Fixture {
public:
    Fixture() {
        std::error_code ec;
        root = fs::temp_directory_path() / "dsptb-install-test";
        fs::remove_all(root, ec);
        modules = root / "data" / "modules";
        fs::create_directories(modules);
        zip = root / "pkg.zip";
    }
    ~Fixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // miniz's writer refuses some bad names, so tests write a same-length placeholder and swap the bytes
    // after.
    void makeZip(const std::vector<Entry>& entries) {
        const std::string path = zip.string();
        mz_zip_archive z{};
        REQUIRE(mz_zip_writer_init_file(&z, path.c_str(), 0));
        for (const Entry& e : entries)
            REQUIRE(
                mz_zip_writer_add_mem(&z, e.name.c_str(), e.content.data(), e.content.size(), MZ_BEST_SPEED));
        REQUIRE(mz_zip_writer_finalize_archive(&z));
        REQUIRE(mz_zip_writer_end(&z));
    }

    void patchNames(const std::string& from, const std::string& to) {
        REQUIRE(from.size() == to.size());
        std::string bytes = readAll(zip);
        for (size_t at = bytes.find(from); at != std::string::npos; at = bytes.find(from, at + 1))
            bytes.replace(at, from.size(), to);
        writeAll(zip, bytes);
    }

    ModuleInstallResult install(bool known = false) {
        return installModuleZip(zip, modules, [known](const std::string&) { return known; });
    }

    // Every failure must leave the modules folder empty, and nothing beside it.
    void checkNothingWritten() {
        CHECK(fs::is_empty(modules));
        size_t files = 0;
        for (const auto& e : fs::recursive_directory_iterator(root))
            if (e.is_regular_file())
                ++files;
        CHECK(files == 1); // the zip itself
    }

    fs::path root, modules, zip;
};

std::vector<Entry> goodPackage(const std::string& prefix = "") {
    return {{prefix + "module.json", kManifest},
            {prefix + "zipped.dll", "not really a dll"},
            {prefix + "assets/data.bin", "data"}};
}

} // namespace

TEST_CASE("module install: a package at the zip root installs") {
    Fixture f;
    f.makeZip(goodPackage());
    const ModuleInstallResult r = f.install();
    INFO(r.error);
    REQUIRE(r.ok);
    CHECK_FALSE(r.needsRestart);
    CHECK(r.manifest.id == "example.zipped");
    CHECK(fs::exists(f.modules / "example.zipped" / "zipped.dll"));
    CHECK(fs::exists(f.modules / "example.zipped" / "assets" / "data.bin"));
    CHECK_FALSE(fs::exists(f.modules / ".installing")); // nothing left to scan or clean up
}

TEST_CASE("module install: a folder that can't be written to gives a reason") {
    Fixture f;
    f.makeZip(goodPackage());
    writeAll(f.root / "afile", "a file, so nothing can be created under it");
    f.modules = f.root / "afile" / "modules";
    const ModuleInstallResult r = f.install();
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("Can't write to") != std::string::npos);
}

TEST_CASE("module install: a package one folder down installs") {
    Fixture f;
    f.makeZip(goodPackage("example.zipped/"));
    const ModuleInstallResult r = f.install();
    INFO(r.error);
    REQUIRE(r.ok);
    CHECK(fs::exists(f.modules / "example.zipped" / "module.json"));
    CHECK_FALSE(fs::exists(f.modules / "example.zipped" / "example.zipped"));
}

TEST_CASE("module install: a real module installed from a zip passes validation") {
    Fixture f;
    const fs::path built = fs::path(DSPTB_MODULES_DIR) / "org.dsptb.gain";
    f.makeZip({{"org.dsptb.gain/module.json", readAll(built / "module.json")},
               {"org.dsptb.gain/gain.dll", readAll(built / "gain.dll")}});
    const ModuleInstallResult r = f.install();
    INFO(r.error);
    REQUIRE(r.ok);
    // In another process, because a loaded DLL can't be deleted with the temp folder. The outer quotes are
    // for cmd.
    const std::string command = std::string("\"\"") + DSPTB_CLI_PATH + "\" validate \"" +
                                (f.modules / "org.dsptb.gain").string() + "\"\"";
    CHECK(std::system(command.c_str()) == 0);
}

TEST_CASE("module install: a known module is staged and swapped in at the next launch") {
    Fixture f;
    fs::create_directories(f.modules / "example.zipped");
    writeAll(f.modules / "example.zipped" / "zipped.dll", "old");
    f.makeZip(goodPackage());
    const ModuleInstallResult r = f.install(true);
    INFO(r.error);
    REQUIRE(r.ok);
    CHECK(r.needsRestart);
    CHECK(readAll(f.modules / "example.zipped" / "zipped.dll") == "old"); // a loaded DLL stays untouched
    CHECK(readAll(f.modules / ".pending" / "example.zipped" / "zipped.dll") == "not really a dll");

    CHECK(applyPendingModuleInstalls(f.modules) == 1);
    CHECK(readAll(f.modules / "example.zipped" / "zipped.dll") == "not really a dll");
    CHECK_FALSE(fs::exists(f.modules / ".pending"));
    CHECK_FALSE(fs::exists(f.modules / "example.zipped.old"));
    CHECK(applyPendingModuleInstalls(f.modules) == 0);
}

TEST_CASE("module install: an unloaded module is replaced straight away, with no leftover files") {
    Fixture f;
    fs::create_directories(f.modules / "example.zipped");
    writeAll(f.modules / "example.zipped" / "stale.dll", "old");
    f.makeZip(goodPackage());
    REQUIRE(f.install().ok);
    CHECK_FALSE(fs::exists(f.modules / "example.zipped" / "stale.dll"));
    CHECK(fs::exists(f.modules / "example.zipped" / "zipped.dll"));
}

TEST_CASE("module install: malicious paths are refused and write nothing") {
    struct Case {
        const char* placeholder; // what miniz's writer accepts
        const char* actual;      // what the zip ends up saying
    };
    const Case cases[] = {
        {"XX/evil.dll", "../evil.dll"},
        {"a/XX/XX/evil.dll", "a/../../evil.dll"},
        {"a\\XX\\evil.dll", "a\\..\\evil.dll"}, // backslashes count as separators
        {"XXetc/evil.dll", "/\\etc/evil.dll"},
        {"Xetc/evil.dll", "/etc/evil.dll"},
        {"XX/evil.dll", "C:/evil.dll"},
        {"zipped.dllXevil", "zipped.dll:evil"}, // an NTFS alternate data stream
        {"a/XXX/evil.dll", "a/.. /evil.dll"},   // Windows strips the trailing space, leaving ".."
        {"a/XXX/evil.dll", "a/.../evil.dll"},
        {"XXX.dll", "CON.dll"}, // opens a device instead of a file
        {"XXX", "NUL"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.actual);
        Fixture f;
        std::vector<Entry> entries = goodPackage();
        entries.push_back({c.placeholder, "evil"});
        f.makeZip(entries);
        f.patchNames(c.placeholder, c.actual);
        const ModuleInstallResult r = f.install();
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.error.empty());
        f.checkNothingWritten();
        CHECK_FALSE(fs::exists(f.root / "data" / "evil.dll"));
    }
}

TEST_CASE("module install: symbolic links are refused") {
    for (const uint32_t attrs : {0120777u << 16, 0x400u}) { // a Unix symlink, a Windows reparse point
        CAPTURE(attrs);
        Fixture f;
        std::vector<Entry> entries = goodPackage();
        entries.push_back({"link", "../../outside"});
        f.makeZip(entries);
        std::string bytes = readAll(f.zip);
        forEachCentralRecord(bytes, [&](size_t at) {
            const uint16_t nameLen =
                static_cast<uint8_t>(bytes[at + 28]) | (static_cast<uint8_t>(bytes[at + 29]) << 8);
            if (bytes.compare(at + 46, nameLen, "link") == 0)
                std::memcpy(&bytes[at + 38], &attrs, sizeof(attrs)); // external file attributes
        });
        writeAll(f.zip, bytes);
        const ModuleInstallResult r = f.install();
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("symbolic link") != std::string::npos);
        f.checkNothingWritten();
    }
}

TEST_CASE("module install: a declared size over the limit is refused before unpacking") {
    Fixture f;
    f.makeZip(goodPackage());
    std::string bytes = readAll(f.zip);
    const uint32_t huge = 0xF0000000u;
    forEachCentralRecord(bytes, [&](size_t at) { std::memcpy(&bytes[at + 24], &huge, sizeof(huge)); });
    writeAll(f.zip, bytes);
    CHECK_FALSE(f.install().ok);
    f.checkNothingWritten();
}

TEST_CASE("module install: a false uncompressed size fails and leaves no files") {
    Fixture f;
    f.makeZip({{"module.json", kManifest}, {"zipped.dll", std::string(4096, 'x')}});
    std::string bytes = readAll(f.zip);
    const uint32_t small = 10u;
    forEachCentralRecord(bytes, [&](size_t at) {
        const uint16_t nameLen =
            static_cast<uint8_t>(bytes[at + 28]) | (static_cast<uint8_t>(bytes[at + 29]) << 8);
        if (bytes.compare(at + 46, nameLen, "zipped.dll") == 0)
            std::memcpy(&bytes[at + 24], &small, sizeof(small));
    });
    writeAll(f.zip, bytes);
    CHECK_FALSE(f.install().ok);
    f.checkNothingWritten();
}

TEST_CASE("module install: unusable packages are refused") {
    SUBCASE("no manifest") {
        Fixture f;
        f.makeZip({{"zipped.dll", "x"}});
        CHECK(f.install().error.find("no module.json") != std::string::npos);
        f.checkNothingWritten();
    }
    SUBCASE("a manifest too deep doesn't count") {
        Fixture f;
        f.makeZip({{"a/b/module.json", kManifest}, {"a/b/zipped.dll", "x"}});
        CHECK_FALSE(f.install().ok);
        f.checkNothingWritten();
    }
    SUBCASE("two manifests") {
        Fixture f;
        f.makeZip({{"a/module.json", kManifest}, {"b/module.json", kManifest}, {"a/zipped.dll", "x"}});
        CHECK(f.install().error.find("more than one") != std::string::npos);
        f.checkNothingWritten();
    }
    SUBCASE("duplicate names differing only by case") {
        Fixture f;
        f.makeZip({{"module.json", kManifest}, {"zipped.dll", "a"}, {"ZIPPED.dll", "b"}});
        CHECK(f.install().error.find("duplicate") != std::string::npos);
        f.checkNothingWritten();
    }
    SUBCASE("broken manifest") {
        Fixture f;
        f.makeZip({{"module.json", "{not json"}, {"zipped.dll", "x"}});
        CHECK(f.install().error.find("Invalid module.json") != std::string::npos);
        f.checkNothingWritten();
    }
    SUBCASE("the DLL is missing") {
        Fixture f;
        f.makeZip({{"module.json", kManifest}});
        CHECK(f.install().error.find("DLL") != std::string::npos);
        f.checkNothingWritten();
    }
    SUBCASE("a different ABI") {
        Fixture f;
        std::string manifest = kManifest;
        manifest.replace(manifest.find("\"abi\":1"), 7, "\"abi\":9");
        f.makeZip({{"module.json", manifest}, {"zipped.dll", "x"}});
        CHECK(f.install().error.find("ABI") != std::string::npos);
        f.checkNothingWritten();
    }
    SUBCASE("a newer app required") {
        Fixture f;
        std::string manifest = kManifest;
        manifest.insert(manifest.find("\"dll\""), "\"minHostVersion\":\"99.0.0\",");
        f.makeZip({{"module.json", manifest}, {"zipped.dll", "x"}});
        CHECK(f.install().error.find("99.0.0") != std::string::npos);
        f.checkNothingWritten();
    }
    SUBCASE("a module id that is a device name") {
        Fixture f;
        std::string manifest = kManifest;
        for (size_t at = manifest.find("example.zipped"); at != std::string::npos;
             at = manifest.find("example.zipped"))
            manifest.replace(at, 14, "con");
        f.makeZip({{"module.json", manifest}, {"zipped.dll", "x"}});
        CHECK_FALSE(f.install().ok);
        f.checkNothingWritten();
    }
    SUBCASE("not a zip") {
        Fixture f;
        writeAll(f.zip, "this is not a zip file");
        CHECK(f.install().error.find("valid zip") != std::string::npos);
        f.checkNothingWritten();
    }
    SUBCASE("a truncated zip") {
        Fixture f;
        f.makeZip(goodPackage());
        std::string bytes = readAll(f.zip);
        writeAll(f.zip, bytes.substr(0, bytes.size() / 2));
        CHECK_FALSE(f.install().ok);
        f.checkNothingWritten();
    }
}
