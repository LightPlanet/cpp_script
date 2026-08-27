# cpp_script
Single-header library for writing scripts with C++20.
### Core features
1. Self-rebuild if sources has been changed
2. Argument table for args -> variable projection
### Example
```cpp
// File .build.cpp

#include "cpp_script.hpp"

using script::arg_string;
using script::arg_path;
using script::arg_bool;

const script::args_table ArgsTable{
    arg_string{ "-comp=",   "Compiler for script and project", "clang++" },
    arg_string{ "-std=",    "C++ version", "std=c++20" },
    arg_path  { "-boost=",  "Boost dir", "${libs}/boost" },
    arg_path  { "-output=", "Output dir", "bin" },
    arg_bool  { "-run",     "Run binary after script is finished" },
    arg_bool  { "-no-comp", "Skip project compilation" }
};

int main(int ac, char** av) {
    namespace fs = script::fs;

    const script::args_parser args{ ac, av };
    const auto[
        compiler, std_ver,
        boost_dir, output_dir,
        want_run, no_compiling
    ] = ArgsTable.extract(args);
    
    if (script::try_rebuild_itself(args, { compiler, std_ver }, ".build.cpp"))
        return 0;
    if (args.has("help")) {
        ArgsTable.show_help();
        return 0;
    }

    const fs::path bin_file = output_dir / "MyApp.out";
    if (!no_compiling) {
        if (!fs::exists(output_dir))
            fs::create_directories(output_dir);

        script::command compile{ compiler, std_ver, "-o", bin_file };
        const auto source_files = script::get_files_by_ext("./src", ".cpp");
        for (auto& src : source_files)
            compile.append(src);
        compile.append("I" + boost_dir.string());
        compile.run();
    }
    if (want_run) {
        script::command run{ bin_file };
        run.run();
    }
}
```
