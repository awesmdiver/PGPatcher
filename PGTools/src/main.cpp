#include "PGD3D.hpp"
#include "PGDirectory.hpp"
#include "PGGlobals.hpp"
#include "PGModManager.hpp"
#include "PGPatcher.hpp"
#include "patchers/PatcherMeshGlobalParticleLightsToLP.hpp"
#include "patchers/PatcherMeshPostFixSSS.hpp"
#include "patchers/PatcherMeshPostHairFlowMap.hpp"
#include "patchers/PatcherMeshPostRestoreDefaultShaders.hpp"
#include "patchers/PatcherMeshPreFixMeshLighting.hpp"
#include "patchers/PatcherMeshPreFixTextureSlotCount.hpp"
#include "patchers/PatcherMeshShaderComplexMaterial.hpp"
#include "patchers/PatcherMeshShaderTransformParallaxToCM.hpp"
#include "patchers/PatcherMeshShaderTruePBR.hpp"
#include "patchers/PatcherMeshShaderVanillaParallax.hpp"
#include "patchers/PatcherTextureGlobalConvertToHDR.hpp"
#include "patchers/PatcherTextureHookConvertToCM.hpp"
#include "patchers/PatcherTextureHookFixSSS.hpp"
#include "patchers/base/PatcherUtil.hpp"
#include "util/ExceptionHandler.hpp"
#include "util/FileUtil.hpp"
#include "util/StringUtil.hpp"

#include <CLI/CLI.hpp>
#include <cpptrace/from_current.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/common.h>
#include <spdlog/spdlog.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <windows.h>

using namespace std;

namespace {
auto getExecutablePath() -> filesystem::path
{
    array<wchar_t, MAX_PATH> buffer {};
    if (GetModuleFileNameW(nullptr, buffer.data(), MAX_PATH) == 0) {
        cerr << "Error getting executable path: " << GetLastError() << "\n";
        exit(1);
    }

    filesystem::path outPath = filesystem::path(buffer.data());

    if (filesystem::exists(outPath)) {
        return outPath;
    }

    cerr << "Error getting executable path: path does not exist\n";
    exit(1);

    return {};
}

void configureDotnetLibDirectory(const filesystem::path& exeDir)
{
    const auto libDir = exeDir / "dotnetlib";
    if (!filesystem::exists(libDir)) {
        return;
    }

    if (SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS) == 0) {
        cerr << "Failed to configure DLL search directories.\n";
        exit(1);
    }

    if (AddDllDirectory(libDir.c_str()) == nullptr) {
        cerr << "Failed to add dotnetlib directory to DLL search path.\n";
        exit(1);
    }
}

struct PGToolsCLIArgs {
    int verbosity = 0;
    bool multithreading = true;
    bool shortcut = false;

    struct Patch {
        CLI::App* subCommand = nullptr;
        unordered_set<string> patchers;
        filesystem::path source = ".";
        filesystem::path output = "ParallaxGen_Output";
        bool mapTexturesFromMeshes = false;
        bool highMem = false;
    } Patch;

    // docs/plans/2026-08-19-pgpatcher-load-order-tool.md's own "conflicts" subcommand, for
    // vortex-collection-tools. Runs the SAME populate-file-map -> map-files -> patch-meshes pipeline
    // `patch` does, just in dry-run mode and without ever touching textures -- see this
    // subcommand's own block in mainRunner for why. Same `patcher` list shape as Patch above, but
    // `source`/`output` are named options here rather than positionals -- see that block for why.
    struct Conflicts {
        CLI::App* subCommand = nullptr;
        unordered_set<string> patchers;
        filesystem::path source = ".";
        filesystem::path output = "ParallaxGen_Output";
        filesystem::path cfgDir;
        filesystem::path jsonOutput;
    } Conflicts;
};

void mainRunner(PGToolsCLIArgs& args)
{
    // Welcome Message
    spdlog::info("Welcome to PGTools version {}!", PG_FULL_VERSION);

    // Get EXE path
    const auto exePath = getExecutablePath().parent_path();

#if defined(PG_PRERELEASE) && (PG_PRERELEASE > 0)
    // Post test message for test builds
    spdlog::warn("This is an EXPERIMENTAL pre-release build of PGTools");
#endif

    ExceptionHandler::setMainThread();

    // Check if patch subcommand was used
    if (args.Patch.subCommand->parsed()) {
        // Get current time to compare later
        auto startTime = chrono::high_resolution_clock::now();
        long long timeTaken = 0;

        args.Patch.source = filesystem::absolute(args.Patch.source);
        args.Patch.output = filesystem::absolute(args.Patch.output);

        auto pgd = PGDirectory(args.Patch.source, args.Patch.output);
        PGGlobals::setPGD(&pgd);
        auto pgd3D = PGD3D(exePath / "cshaders");
        PGGlobals::setPGD3D(&pgd3D);

        // Check if GPU needs to be initialized
        if (!pgd3D.initGPU()) {
            spdlog::critical("Failed to initialize GPU. Exiting.");
        }

        if (!pgd3D.initShaders()) {
            spdlog::critical("Failed to initialize internal shaders. Exiting.");
        }

        // Create output directory
        try {
            filesystem::create_directories(args.Patch.output);
        } catch (const filesystem::filesystem_error& e) {
            spdlog::critical("Failed to create output directory: {}", e.what());
            exit(1);
        }

        // If output dir is the same as data dir meshes might get overwritten
        if (filesystem::equivalent(args.Patch.output, pgd.getDataPath())) {
            spdlog::critical("Output directory cannot be the same directory as your data folder. "
                             "Exiting.");
            exit(1);
        }

        // delete existing output
        PGPatcher::deleteOutputDir();

        // Init file map
        pgd.populateFileMap(false);

        // Map files
        pgd.mapFiles({}, {}, {}, {}, args.multithreading);

        // Split patchers into names and options
        unordered_map<string, unordered_map<string, string>> patcherDefs;
        for (const auto& patcher : args.Patch.patchers) {
            auto openBracket = patcher.find('[');
            auto closeBracket = patcher.find(']');
            if (openBracket == string::npos || closeBracket == string::npos) {
                patcherDefs[patcher] = {};
                continue;
            }

            // Get substring between brackets
            auto options = patcher.substr(openBracket + 1, closeBracket - openBracket - 1);
            // Split options by | into unordered set
            unordered_map<string, string> optionSet;
            for (const auto& option : options | views::split('|')) {
                // check if = in option string
                const auto optionStr = string(option.begin(), option.end());
                const auto eqPos = optionStr.find('=');
                if (eqPos != string::npos) {
                    optionSet[optionStr.substr(0, eqPos)] = optionStr.substr(eqPos + 1);
                    continue;
                }

                optionSet[optionStr] = "";
            }

            // Add to set
            patcherDefs[patcher.substr(0, openBracket)] = optionSet;
        }

        // Create patcher factory
        PatcherUtil::PatcherMeshSet meshPatchers;
        if (patcherDefs.contains("fixmeshlighting")) {
            meshPatchers.prePatchers.emplace_back(PatcherMeshPreFixMeshLighting::getFactory());
        }
        if (patcherDefs.contains("fixtextureslotcount")) {
            meshPatchers.prePatchers.emplace_back(PatcherMeshPreFixTextureSlotCount::getFactory());
        }
        if (patcherDefs.contains("parallax")) {
            meshPatchers.shaderPatchers.emplace(PatcherMeshShaderVanillaParallax::getShaderType(),
                                                PatcherMeshShaderVanillaParallax::getFactory());
        }
        if (patcherDefs.contains("complexmaterial")) {
            meshPatchers.shaderPatchers.emplace(PatcherMeshShaderComplexMaterial::getShaderType(),
                                                PatcherMeshShaderComplexMaterial::getFactory());
            PatcherMeshShaderComplexMaterial::loadOptions(patcherDefs["complexmaterial"]);
        }
        if (patcherDefs.contains("truepbr")) {
            meshPatchers.shaderPatchers.emplace(PatcherMeshShaderTruePBR::getShaderType(),
                                                PatcherMeshShaderTruePBR::getFactory());
            PatcherMeshShaderTruePBR::loadStatics(pgd.getPBRJSONs());
            PatcherMeshShaderTruePBR::loadOptions(patcherDefs["truepbr"]);
        }
        if (patcherDefs.contains("parallaxtocm")) {
            meshPatchers.shaderTransformPatchers[PatcherMeshShaderTransformParallaxToCM::getFromShader()]
                = {PatcherMeshShaderTransformParallaxToCM::getToShader(),
                   PatcherMeshShaderTransformParallaxToCM::getFactory()};

            PatcherTextureHookConvertToCM::initShader();
        }
        if (patcherDefs.contains("particlelightstolp")) {
            meshPatchers.globalPatchers.emplace_back(PatcherMeshGlobalParticleLightsToLP::getFactory());
        }

        if (patcherDefs.contains("restoredefaultshaders")) {
            meshPatchers.postPatchers.emplace_back(PatcherMeshPostRestoreDefaultShaders::getFactory());
        }
        if (patcherDefs.contains("fixsss")) {
            meshPatchers.postPatchers.emplace_back(PatcherMeshPostFixSSS::getFactory());

            PatcherTextureHookFixSSS::initShader();
        }
        if (patcherDefs.contains("hairflowmap")) {
            meshPatchers.postPatchers.emplace_back(PatcherMeshPostHairFlowMap::getFactory());
        }

        PatcherUtil::PatcherTextureSet texPatchers;
        if (patcherDefs.contains("converttohdr")) {
            PatcherTextureGlobalConvertToHDR::initShader();

            texPatchers.globalPatchers.emplace_back(PatcherTextureGlobalConvertToHDR::getFactory());
            PatcherTextureGlobalConvertToHDR::loadOptions(patcherDefs["converttohdr"]);
        }

        PGPatcher::loadPatchers(meshPatchers, texPatchers);
        PGPatcher::patchMeshes(args.multithreading, true);
        PGPatcher::patchTextures(args.multithreading);

        // Finalize step
        if (patcherDefs.contains("particlelightstolp")) {
            PatcherMeshGlobalParticleLightsToLP::finalize();
        }

        // Check if dynamic cubemap file is needed
        if (args.Patch.patchers.contains("complexmaterial")
            && !patcherDefs["complexmaterial"].contains("disable_dyncubemap")) {
            // Install default cubemap file if needed
            static const filesystem::path dynCubeMapPath = "textures/cubemaps/dynamic1pxcubemap_black.dds";

            spdlog::info("Installing default dynamic cubemap file");

            // Create Directory
            const filesystem::path outputCubemapPath = args.Patch.output / dynCubeMapPath.parent_path();
            filesystem::create_directories(outputCubemapPath);

            const filesystem::path assetPath = filesystem::path(exePath) / "assets/dynamic1pxcubemap_black_ENB.dds";
            const filesystem::path outputPath = filesystem::path(args.Patch.output) / dynCubeMapPath;

            // Move File
            filesystem::copy_file(assetPath, outputPath, filesystem::copy_options::overwrite_existing);
        }

        const auto endTime = chrono::high_resolution_clock::now();
        timeTaken += chrono::duration_cast<chrono::seconds>(endTime - startTime).count();

        spdlog::info("PGPatcher took {} seconds to complete", timeTaken);
    }

    // Check if conflicts subcommand was used (docs/plans/2026-08-19-pgpatcher-load-order-tool.md,
    // vortex-collection-tools). Runs the SAME populate-file-map -> map-files -> patch-meshes
    // pipeline `patch` above does -- PGModManager's conflicts/shaders/isNew state is a genuine
    // byproduct of the real shader-matching pass (getMatches, PGPatcher.cpp), not a cheap file-path
    // diff, so there's no shortcut around running it for real. Two deliberate differences from
    // `patch`: (1) patchMeshes runs with dryRun=true (PGPatcher.hpp/.cpp) so no patched mesh ever
    // gets written to disk -- matching data is fully populated well before any write would happen;
    // (2) patchTextures is never called at all -- confirmed by reading patchDDS's full body that it
    // never touches PGModManager/getMatches, so it contributes nothing here and is the single most
    // expensive part of a real patch run (full DDS decode/GPU passes/re-encode).
    if (args.Conflicts.subCommand->parsed()) {
        args.Conflicts.source = filesystem::absolute(args.Conflicts.source);
        args.Conflicts.output = filesystem::absolute(args.Conflicts.output);

        auto pgd = PGDirectory(args.Conflicts.source, args.Conflicts.output);
        PGGlobals::setPGD(&pgd);
        auto pgd3D = PGD3D(exePath / "cshaders");
        PGGlobals::setPGD3D(&pgd3D);

        // GPU/shader init is needed for MESH patching too, not just textures -- confirmed by
        // reading `patch`'s own setup above, which does this before ever touching the file map.
        if (!pgd3D.initGPU()) {
            spdlog::critical("Failed to initialize GPU. Exiting.");
            exit(1);
        }
        if (!pgd3D.initShaders()) {
            spdlog::critical("Failed to initialize internal shaders. Exiting.");
            exit(1);
        }

        // create_directories() before equivalent() -- filesystem::equivalent() THROWS if either
        // path doesn't exist yet (confirmed: not just returns false), so the output dir must exist
        // on disk before this check is safe to run, same reason `patch` above creates it first.
        // Creating an empty directory is harmless even though nothing ever gets written inside it.
        filesystem::create_directories(args.Conflicts.output);

        if (filesystem::equivalent(args.Conflicts.output, pgd.getDataPath())) {
            spdlog::critical("Output directory cannot be the same directory as your data folder. Exiting.");
            exit(1);
        }

        // Deliberately NO deleteOutputDir() call, unlike `patch` above -- this subcommand never
        // writes to output at all (dryRun=true skips the one real disk-write site entirely), so it
        // must absolutely never delete a real prior patch run's output just to compute conflicts.

        // Mod manager setup -- Vortex only for now. The director's own real install (this
        // subcommand's own test target) is Vortex; MO2 support (PGModManager::populateModFileMapMO2)
        // is a real, cheap follow-up but wasn't wired up here since there's no real MO2 install to
        // verify it against, and this task's own instruction is to test against real data before
        // committing, not ship an unverified code path.
        auto pgmm = PGModManager(PGModManager::ModManagerType::VORTEX);
        PGGlobals::setPGMM(&pgmm);

        nlohmann::json modJSON;
        const auto modListFile = args.Conflicts.cfgDir / "modrules.json";
        if (FileUtil::getJSON(modListFile, modJSON)) {
            pgmm.loadJSON(modJSON);
        } else {
            spdlog::warn("No existing modrules.json found at {} -- every mod will read as new.", modListFile.string());
        }

        pgmm.populateModFileMapVortex(args.Conflicts.source);

        // Init file map (same `false` -- exclude BSAs -- as `patch` above; matching that
        // subcommand's own established default rather than introducing a new CLI flag for it)
        pgd.populateFileMap(false);

        // Map files
        pgd.mapFiles({}, {}, {}, {}, args.multithreading);

        // Register only the requested shader (+ shader-transform) patchers. Deliberately simpler
        // than `patch`'s own patcherDefs mechanism -- no per-patcher bracket-syntax options parsing,
        // since conflict detection only needs each patcher ACTIVE (so it contributes matches), not
        // its output tuned; every patcher below loads with its own default options.
        PatcherUtil::PatcherMeshSet meshPatchers;
        if (args.Conflicts.patchers.contains("parallax")) {
            meshPatchers.shaderPatchers.emplace(PatcherMeshShaderVanillaParallax::getShaderType(),
                                                PatcherMeshShaderVanillaParallax::getFactory());
        }
        if (args.Conflicts.patchers.contains("complexmaterial")) {
            meshPatchers.shaderPatchers.emplace(PatcherMeshShaderComplexMaterial::getShaderType(),
                                                PatcherMeshShaderComplexMaterial::getFactory());
            unordered_map<string, string> complexMaterialOptions;
            PatcherMeshShaderComplexMaterial::loadOptions(complexMaterialOptions);
        }
        if (args.Conflicts.patchers.contains("truepbr")) {
            meshPatchers.shaderPatchers.emplace(PatcherMeshShaderTruePBR::getShaderType(),
                                                PatcherMeshShaderTruePBR::getFactory());
            PatcherMeshShaderTruePBR::loadStatics(pgd.getPBRJSONs());
            unordered_map<string, string> truePBROptions;
            PatcherMeshShaderTruePBR::loadOptions(truePBROptions);
        }
        if (args.Conflicts.patchers.contains("parallaxtocm")) {
            meshPatchers.shaderTransformPatchers[PatcherMeshShaderTransformParallaxToCM::getFromShader()]
                = {PatcherMeshShaderTransformParallaxToCM::getToShader(),
                   PatcherMeshShaderTransformParallaxToCM::getFactory()};
            PatcherTextureHookConvertToCM::initShader();
        }

        // Empty -- patchTextures is never called, so no texture patchers are ever needed.
        const PatcherUtil::PatcherTextureSet texPatchers;
        PGPatcher::loadPatchers(meshPatchers, texPatchers);

        PGPatcher::patchMeshes(args.multithreading, true, {}, false, false, {}, true);

        // Serialize PGModManager's full mod list -- the NEW contract this subcommand exists to
        // provide (getJSON() on PGModManager is the modrules.json SAVE format -- priority/enabled/
        // meshesignored only -- used elsewhere for a different real purpose, deliberately not reused
        // here).
        auto result = nlohmann::json::array();
        for (const auto& mod : pgmm.getModsByPriority()) {
            auto modJson = nlohmann::json::object();
            modJson["name"] = StringUtil::utf16toUTF8(mod->name);
            modJson["isNew"] = mod->isNew;
            modJson["priority"] = mod->priority;
            modJson["enabled"] = mod->isEnabled;
            modJson["areMeshesIgnored"] = mod->areMeshesIgnored;
            modJson["hasMeshes"] = mod->hasMeshes;

            auto shaders = nlohmann::json::array();
            for (const auto& shader : mod->shaders) {
                shaders.push_back(PGEnums::getStrFromShader(shader));
            }
            modJson["shaders"] = shaders;

            auto conflicts = nlohmann::json::array();
            for (const auto& conflictMod : mod->conflicts) {
                conflicts.push_back(StringUtil::utf16toUTF8(conflictMod->name));
            }
            modJson["conflicts"] = conflicts;

            result.push_back(modJson);
        }

        if (!args.Conflicts.jsonOutput.empty()) {
            ofstream out(args.Conflicts.jsonOutput);
            out << result.dump(2);
            spdlog::info("Wrote conflicts JSON ({} mods) to {}", result.size(), args.Conflicts.jsonOutput.string());
        } else {
            cout << result.dump(2) << "\n";
        }
    }
}

void addArguments(CLI::App& app,
                  PGToolsCLIArgs& args)
{
    // Logging
    app.add_flag("-v",
                 args.verbosity,
                 "Verbosity level -v for DEBUG data or -vv for TRACE data "
                 "(warning: TRACE data is very verbose)");
    app.add_flag("--no-multithreading", args.multithreading, "Disable multithreading");
    app.add_flag("--shortcut",
                 args.shortcut,
                 "Keep pgtools running at the end (useful if you are running not in a terminal directly)");

    args.Patch.subCommand = app.add_subcommand("patch", "Patch meshes");
    args.Patch.subCommand->add_option("patcher", args.Patch.patchers, "List of patchers to use")
        ->required()
        ->delimiter(',');
    args.Patch.subCommand->add_option("source", args.Patch.source, "Source directory")->default_str("");
    args.Patch.subCommand->add_option("output", args.Patch.output, "Output directory")
        ->default_str("ParallaxGen_Output");
    args.Patch.subCommand->add_flag("--high-mem", args.Patch.highMem, "High memory usage mode (default: false)");

    args.Conflicts.subCommand = app.add_subcommand(
        "conflicts",
        "Compute mod conflict/new-mod status (PGModManager's own state) without writing any patched output");
    // NOTE: source/output are named options here, NOT positionals like `patch`'s own "source
    // output" pair above -- confirmed live that CLI11 does not reliably reserve trailing positional
    // slots after this project's own delimited-unordered_set "patcher..." positional (the SAME
    // shape `patch` uses): `pgtools patch truepbr C:/temp/src` leaves `source` at its "." default
    // rather than binding "C:/temp/src", reproduced with `patch` itself, not just this new
    // subcommand -- an apparent pre-existing bug in how `patch`'s own CLI is invoked, out of scope
    // to fix here. Named options sidestep the ambiguity entirely rather than inheriting it.
    args.Conflicts.subCommand
        ->add_option("patcher", args.Conflicts.patchers, "List of shader patchers to consider for conflicts")
        ->required()
        ->delimiter(',');
    args.Conflicts.subCommand
        ->add_option("--source", args.Conflicts.source, "Source directory (Vortex deployment / game Data folder)")
        ->required();
    args.Conflicts.subCommand
        ->add_option("--output",
                     args.Conflicts.output,
                     "Output directory (never written to by this subcommand, but still required by the underlying "
                     "directory scanner -- must not be the same directory as --source)")
        ->default_str("ParallaxGen_Output");
    args.Conflicts.subCommand
        ->add_option(
            "--cfg-dir", args.Conflicts.cfgDir, "Directory containing modrules.json (PGPatcher's own cfg folder)")
        ->required();
    args.Conflicts.subCommand->add_option(
        "--json-output", args.Conflicts.jsonOutput, "Write the resulting JSON to this file instead of stdout");
}
}

auto main(int argC,
          char** argV) -> int
{
// Block until enter only in debug mode
#ifdef _DEBUG
    cout << "Press ENTER to start (DEBUG mode)...";
    cin.get();
#endif

    SetConsoleOutputCP(CP_UTF8);

    const auto exePath = getExecutablePath().parent_path();
    configureDotnetLibDirectory(exePath);

    // CLI Arguments
    PGToolsCLIArgs args;
    CLI::App app {"PGTools: A collection of tools for ParallaxGen"};
    addArguments(app, args);

    // Parse CLI Arguments (this is what exits on any validation issues)
    CLI11_PARSE(app, argC, argV);

    // Initialize Logger
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

    // Set logging mode
    if (args.verbosity >= 1) {
        spdlog::set_level(spdlog::level::debug);
        spdlog::debug("DEBUG logging enabled");
    }

    if (args.verbosity >= 2) {
        spdlog::set_level(spdlog::level::trace);
        spdlog::trace("TRACE logging enabled");
    }

    // Main Runner (Catches all exceptions)
    CPPTRACE_TRY { mainRunner(args); }
    CPPTRACE_CATCH(const exception& e)
    {
        ExceptionHandler::setException(e, cpptrace::from_current_exception().to_string());
    }

    int returnCode = 0;
    if (ExceptionHandler::hasException()) {
        ExceptionHandler::throwExceptionOnMainThread();
        returnCode = 1;
    }

    if (args.shortcut) {
        cout << "Press ENTER to exit...";
        cin.get();
    }

    return returnCode;
}
