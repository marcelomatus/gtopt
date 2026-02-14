# Documentation Structure Review and Recommendations

This document provides a comprehensive review of the gtopt documentation structure and suggests improvements for better organization, clarity, and maintainability.

## Current Documentation Structure

### Root Level Documentation
- **README.md** - Main project overview and quick start guide
- **BUILDING.md** - Detailed build instructions (newly created)
- **USAGE.md** - Comprehensive usage guide with examples
- **CLAUDE.md** - Developer guidance for AI assistants

### Service-Specific Documentation
- **webservice/README.md** - Web service overview
- **webservice/INSTALL.md** - Detailed installation and deployment guide for web service
- **guiservice/README.md** - GUI service overview
- **guiservice/INSTALL.md** - Detailed installation and deployment guide for GUI service
- **guiservice/INPUT_DATA.md** - Comprehensive input data structure reference

### Technical Documentation
- **test/BENCHMARK_RESULTS.md** - Performance benchmarks

## Strengths of Current Structure

1. **Clear Separation of Concerns**: Build instructions, usage, and service-specific documentation are well separated.
2. **Comprehensive Coverage**: Both web service and GUI service have dedicated installation guides.
3. **Progressive Disclosure**: README provides quick start, while detailed guides are available for deep dives.
4. **Service Independence**: Each service has its own README and INSTALL guide, making them discoverable and self-contained.

## Issues and Redundancies Identified

### 1. Redundant "Running the Sample Case" Content
**Issue**: The "Running the Sample Case" section appears in both README.md (lines 124-169) and USAGE.md (lines 207-272) with similar content.

**Recommendation**: 
- Keep a minimal example in README.md (just the command and expected output status)
- Move all detailed explanations to USAGE.md
- Add a clear reference from README.md to USAGE.md for more details

### 2. Unclear Documentation Discovery Path
**Issue**: Users may not know which document to read first or what each document contains.

**Recommendation**: Add a "Documentation Guide" section to README.md that clearly explains the purpose and audience of each documentation file.

### 3. INPUT_DATA.md Location
**Issue**: `guiservice/INPUT_DATA.md` contains general gtopt input data structure documentation, but it's located in the GUI service directory, making it less discoverable for users who only use the CLI tool.

**Recommendation**: 
- Move `INPUT_DATA.md` to the root level as it's a general reference
- Keep a reference/link in `guiservice/README.md`
- Reference it from USAGE.md as well

### 4. Overlap Between README.md Options Reference and USAGE.md
**Issue**: The command-line options table appears in both README.md (lines 98-119) and USAGE.md (lines 48-72) with identical content.

**Recommendation**:
- Keep a condensed version in README.md (just the most common options)
- Keep the complete reference in USAGE.md
- Add a clear reference: "See USAGE.md for complete options reference"

### 5. Missing API Documentation
**Issue**: While webservice and guiservice have API endpoint tables, there's no comprehensive API reference document.

**Recommendation**:
- Consider creating an `API.md` file that consolidates API documentation for both services
- Or keep service-specific APIs in their respective directories but add OpenAPI/Swagger specs

### 6. Build Verification Steps
**Issue**: BUILDING.md lacks a section on verifying the build with actual test runs.

**Status**: BUILDING.md includes test sections, but could be enhanced with troubleshooting for common test failures.

## Suggested Documentation Structure

### Recommended Organization

```
gtopt/
├── README.md                    # Project overview, quick start, documentation guide
├── BUILDING.md                  # Build instructions (all platforms)
├── USAGE.md                     # CLI usage reference and examples
├── INPUT_DATA.md                # Input data structure reference (moved from guiservice/)
├── CONTRIBUTING.md              # Contribution guidelines (consider adding)
├── CHANGELOG.md                 # Version history (consider adding)
├── CLAUDE.md                    # AI assistant guidance
│
├── docs/                        # Optional: consolidated documentation directory
│   ├── architecture.md          # System architecture overview
│   ├── api/                     # API specifications
│   │   ├── webservice.md
│   │   └── guiservice.md
│   └── tutorials/               # Step-by-step tutorials
│       ├── basic-case.md
│       ├── multi-bus.md
│       └── hydro-thermal.md
│
├── webservice/
│   ├── README.md                # Quick overview and getting started
│   ├── INSTALL.md               # Installation and deployment
│   └── API.md                   # Detailed API reference (optional)
│
├── guiservice/
│   ├── README.md                # Quick overview and getting started
│   ├── INSTALL.md               # Installation and deployment
│   └── API.md                   # Detailed API reference (optional)
│
└── test/
    └── BENCHMARK_RESULTS.md     # Performance benchmarks
```

## Specific Recommendations

### 1. README.md Improvements

**Current Issues:**
- Too much detail in some sections (options table, sample case output)
- No clear navigation guide to other documentation

**Recommended Changes:**
```markdown
## Documentation Guide

- **[BUILDING.md](BUILDING.md)** - Detailed build instructions for all platforms, dependencies, and troubleshooting
- **[USAGE.md](USAGE.md)** - Complete command-line reference, examples, and advanced usage
- **[INPUT_DATA.md](INPUT_DATA.md)** - Input data structure and file format reference
- **[webservice/INSTALL.md](webservice/INSTALL.md)** - Web service installation and deployment
- **[guiservice/INSTALL.md](guiservice/INSTALL.md)** - GUI service installation and deployment
```

**Streamline the "Usage" section:**
- Keep only 3-4 most common examples
- Remove the full options table (keep link to USAGE.md)

**Streamline "Running the Sample Case":**
- Keep only the basic command and one-line expected output
- Remove detailed output structure (refer to USAGE.md)

### 2. USAGE.md Enhancements

**Add to the beginning:**
```markdown
> **Note**: This guide assumes gtopt is already installed. If you need build instructions, see [BUILDING.md](../BUILDING.md).
```

**Add cross-references:**
- Link to INPUT_DATA.md for detailed file format specifications
- Link to BUILDING.md when discussing different build types

### 3. Create INPUT_DATA.md at Root Level

**Rationale:**
- Input data structure is relevant to all users (CLI, webservice, GUI)
- Current location in guiservice/ makes it hard to find for CLI users
- It's a reference document, not a service-specific guide

**Migration Steps:**
1. Move `guiservice/INPUT_DATA.md` to root as `INPUT_DATA.md`
2. Update `guiservice/README.md` to reference `../INPUT_DATA.md`
3. Add reference from `USAGE.md` to `INPUT_DATA.md`
4. Update any internal links

### 4. BUILDING.md Enhancements

**Already Implemented:**
- Comprehensive dependency installation for multiple platforms
- Multiple build types explained
- Troubleshooting section
- Testing instructions

**Consider Adding:**
- Docker build instructions (if applicable)
- Cross-compilation instructions
- Custom solver configuration details

### 5. Service Documentation Consistency

**Ensure both webservice and guiservice have:**
- README.md with quick overview and getting started (√)
- INSTALL.md with detailed deployment instructions (√)
- Clear API reference section (√ - in INSTALL.md)
- Troubleshooting section (√)

**Standardize structure:**
Both service INSTALL.md files should follow the same section order:
1. Prerequisites
2. Quick Start
3. Dependencies Installation
4. Configuration
5. Development Setup
6. Production Deployment
7. Systemd Service Setup
8. Reverse Proxy Setup
9. Testing
10. API Reference
11. Troubleshooting

**Current Status:** Both files already follow similar structure ✓

### 6. Consider Adding Missing Documentation

**CONTRIBUTING.md:**
- Code style guidelines (some exist in CLAUDE.md)
- Pull request process
- Testing requirements
- Documentation standards

**CHANGELOG.md:**
- Version history
- Breaking changes
- Migration guides between versions

**ARCHITECTURE.md:**
- System architecture overview
- Component interactions
- Design decisions
- Extension points

## Specific Edits for README.md

### Simplify Usage Section

**Current** (lines 74-96): Shows 7 examples
**Recommended**: Keep only 3 most common examples

```markdown
## Usage

Run gtopt on a system configuration file:

```bash
gtopt system_config.json
```

Common options:

```bash
# Output results to a specific directory
gtopt system_c0.json --output-directory results/

# Single-bus mode (ignore network topology)
gtopt system_c0.json --use-single-bus

# Enable DC power flow (Kirchhoff's laws)
gtopt system_c0.json --use-kirchhoff
```

For complete command-line reference, advanced examples, and detailed usage instructions, see **[USAGE.md](USAGE.md)**.
```

### Simplify Options Reference

**Current** (lines 98-119): Full options table with 19 rows
**Recommended**: Remove the table entirely or keep only 5 most common options

```markdown
### Common Options

| Flag | Description |
| ---- | ----------- |
| `-h, --help` | Show help message |
| `-V, --version` | Show version |
| `-d, --output-directory` | Directory for output files |
| `-b, --use-single-bus` | Use single-bus mode |
| `-k, --use-kirchhoff` | Use DC power flow mode |

For the complete options reference, see **[USAGE.md](USAGE.md#command-line-reference)**.
```

### Simplify Running the Sample Case

**Current** (lines 124-169): Detailed output structure and multiple examples
**Recommended**: Basic example only

```markdown
## Running the Sample Case

The repository includes a sample case in `cases/c0/`:

```bash
cd cases/c0
gtopt system_c0.json
```

The solver produces output files organized by component type in the `output/` directory. A status of `0` in `solution.csv` indicates an optimal solution was found.

For detailed output file descriptions and advanced examples, see **[USAGE.md](USAGE.md#running-the-sample-case)**.
```

## Implementation Priority

### High Priority (Immediate)
1. ✅ Create BUILDING.md with comprehensive build instructions
2. ✅ Update README.md to reference BUILDING.md
3. ✅ Update README.md table of contents
4. Simplify README.md usage examples (remove redundancy)
5. Move INPUT_DATA.md to root level

### Medium Priority (Near Term)
1. Add "Documentation Guide" section to README.md
2. Add cross-references between documentation files
3. Standardize INSTALL.md structure across services
4. Create CONTRIBUTING.md

### Low Priority (Long Term)
1. Create CHANGELOG.md
2. Create ARCHITECTURE.md
3. Create API specification files (OpenAPI/Swagger)
4. Create tutorial documentation in docs/tutorials/
5. Consider consolidating all docs in a docs/ directory

## Conclusion

The current documentation is comprehensive and well-organized. The main improvements needed are:

1. **Reducing redundancy** between README.md and USAGE.md
2. **Improving discoverability** with a clear documentation guide
3. **Relocating INPUT_DATA.md** to make it more accessible
4. **Adding navigation aids** with cross-references between documents

These changes will make the documentation easier to navigate while maintaining comprehensive coverage of all features and use cases.
