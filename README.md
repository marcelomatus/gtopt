

# Gtopt

a new C++ generation and transmission planning project.

## Features

- Optimizes

## Usage
1. [Build the standalone target](building-the-standalone-target)
2. Run the binary as it follows:
   ```bash
   ./build/standalone/gtopt input_file
   ```
### Options Reference
| Short Flag | Long Flag | Argument | Description |
| :--- | :--- | :--- | :--- |
| `-h` | `--help` | | Describes arguments |
| `-v` | `--verbose` | | Activates maximum verbosity |
| `-q` | `--quiet` | `[=arg]` | Do not log in the stdout |
| `-V` | `--version` | | Shows program version |
| `-s` | `--system-file` | `arg` | Name of the system file |
| `-l` | `--lp-file` | `arg` | Name of the lp file to save |
| `-j` | `--json-file` | `arg` | Name of the json file to save |
| `-D` | `--input-directory` | `arg` | Input directory |
| `-F` | `--input-format` | `arg` | Input format |
| `-d` | `--output-directory` | `arg` | Output directory |
| `-f` | `--output-format` | `arg` | Output format `[parquet, csv]` |
| `-C` | `--compression-format` | `arg` | Compression format in parquet `[uncompressed, gzip, zstd, lzo]` |
| `-b` | `--use-single-bus` | `[=arg]` | Use single bus mode |
| `-k` | `--use-kirchhoff` | `[=arg]` | Use kirchhoff mode |
| `-n` | `--use-lp-names` | `[=arg]` | Use real col/row names in the lp file |
| `-e` | `--matrix-eps` | `arg` | Eps value to define A matrix non-zero values |
| `-c` | `--just-create` | `[=arg]` | Just create the problem, then exit |
| `-p` | `--fast-parsing` | `[=arg]` | Use fast (non-strict) json parsing |

## Building from Source
### Dependencies (WIP)
#### Boost
See https://www.boost.org/doc/user-guide/getting-started.html
#### Arrow
See https://arrow.apache.org/install/

### Building the standalone target

Use the following command to build and run the executable target.

```bash
cmake -S standalone -B build/standalone
cmake --build build/standalone
./build/standalone/gtopt --help
```

### Building and run test suite

Use the following commands from the project's root directory to run the test suite.

```bash
cmake -S test -B build/test
cmake --build build/test
CTEST_OUTPUT_ON_FAILURE=1 cmake --build build/test --target test

# or simply call the executable:
./build/test/GreeterTests
```

To collect code coverage information, run CMake with the `-DENABLE_TEST_COVERAGE=1` option.

### Run clang-format

Use the following commands from the project's root directory to check and fix C++ and CMake source style.
This requires _clang-format_, _cmake-format_ and _pyyaml_ to be installed on the current system.

```bash
cmake -S test -B build/test

# view changes
cmake --build build/test --target format

# apply changes
cmake --build build/test --target fix-format
```

See [Format.cmake](https://github.com/TheLartians/Format.cmake) for details.
These dependencies can be easily installed using pip.

```bash
pip install clang-format==14.0.6 cmake_format==0.6.11 pyyaml
```

### Build the documentation

The documentation is automatically built and [published](https://thelartians.github.io/ModernCppStarter) whenever a [GitHub Release](https://help.github.com/en/github/administering-a-repository/managing-releases-in-a-repository) is created.
To manually build documentation, call the following command.

```bash
cmake -S documentation -B build/doc
cmake --build build/doc --target GenerateDocs
# view the docs
open build/doc/doxygen/html/index.html
```

To build the documentation locally, you will need Doxygen, jinja2 and Pygments installed on your system.

### Build everything at once

The project also includes an `all` directory that allows building all targets at the same time.
This is useful during development, as it exposes all subprojects to your IDE and avoids redundant builds of the library.

```bash
cmake -S all -B build
cmake --build build

# run tests
./build/test/GreeterTests
# format code
cmake --build build --target fix-format
# run standalone
./build/standalone/Greeter --help
# build docs
cmake --build build --target GenerateDocs
```

### Additional tools

The test and standalone subprojects include the [tools.cmake](cmake/tools.cmake) file which is used to import additional tools on-demand through CMake configuration arguments.
The following are currently supported.

#### Sanitizers

Sanitizers can be enabled by configuring CMake with `-DUSE_SANITIZER=<Address | Memory | MemoryWithOrigins | Undefined | Thread | Leak | 'Address;Undefined'>`.

#### Static Analyzers

Static Analyzers can be enabled by setting `-DUSE_STATIC_ANALYZER=<clang-tidy | iwyu | cppcheck>`, or a combination of those in quotation marks, separated by semicolons.
By default, analyzers will automatically find configuration files such as `.clang-format`.
Additional arguments can be passed to the analyzers by setting the `CLANG_TIDY_ARGS`, `IWYU_ARGS` or `CPPCHECK_ARGS` variables.

#### Ccache
Ccache can be enabled by configuring with `-DUSE_CCACHE=<ON | OFF>`.
