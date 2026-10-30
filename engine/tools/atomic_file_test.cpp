#include "platform/atomic_file.hpp"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#endif

int main() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / ("cr-storage-test-" + std::to_string(SDL_GetTicksNS()));
    if (!fs::create_directory(root)) return 1;
    const auto path = root / fs::u8path("save-\xc3\xa9.txt");
    auto read = [](const fs::path& p) { std::ifstream in(p, std::ios::binary); return std::string(std::istreambuf_iterator<char>(in), {}); };
    int problems = 0;
    auto check = [&](bool ok, const char* what) { if (!ok) { ++problems; std::fprintf(stderr,"atomic save FAILED: %s\n",what); } };
    std::string error;
    check(crstorage::replaceFile(path.u8string(),"old save",&error),"create complete save (UTF-8 path)");
    check(crstorage::replaceFile(path.u8string(),"new complete save",&error) && read(path)=="new complete save","replace existing save");
    const auto directory = root / "blocked";
    fs::create_directory(directory);
    { std::ofstream f(directory/"sentinel"); f << "keep"; }
    check(!crstorage::replaceFile(directory.u8string(),"replacement",&error) && !error.empty() && read(directory/"sentinel")=="keep",
          "failed rename preserves destination");
    check(!crstorage::replaceFile((root/"missing/save.txt").u8string(),"replacement",&error),"missing parent fails");
#ifndef _WIN32
    struct rlimit original{};
    if (getrlimit(RLIMIT_FSIZE,&original)==0) {
        auto previous = std::signal(SIGXFSZ,SIG_IGN);
        struct rlimit limited = original;
        limited.rlim_cur = 64;
        const bool set = setrlimit(RLIMIT_FSIZE,&limited)==0;
        bool failed = false;
        if (set) failed = !crstorage::replaceFile(path.u8string(),std::string(131072,'x'),&error);
        const bool restored = setrlimit(RLIMIT_FSIZE,&original)==0;
        std::signal(SIGXFSZ,previous);
        check(set && restored && failed && read(path)=="new complete save","short write preserves the previous save");
    } else check(false,"query short-write limit");
#endif
    check(crstorage::replaceFile(path.u8string(),"",&error) && read(path).empty(),"intentional empty continue file is supported");
    for (const auto& e : fs::directory_iterator(root))
        check(e.path().filename().u8string().rfind(".cr-save-",0)!=0,"staging directory cleaned after success and failure");
    fs::remove(path);
    fs::remove(directory/"sentinel"); fs::remove(directory); fs::remove(root);
    std::printf("atomic save: %d problems\n",problems);
    return problems ? 1 : 0;
}
