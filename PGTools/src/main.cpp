#include "PGD3D.hpp"
#include "PGDirectory.hpp"
#include "PGGlobals.hpp"
#include "PGModManager.hpp"
#include "PGPatcher.hpp"
#include "PGPlugin.hpp"
#include "common/BethesdaGame.hpp"
#include "patchers/PatcherMeshGlobalParticleLightsToLP.hpp"
#include "patchers/PatcherMeshPostFixSSS.hpp"
#include "patchers/PatcherMeshPostHairFlowMap.hpp"
#include "patchers/PatcherMeshPostRestoreDefaultShaders.hpp"
#include "patchers/PatcherMeshPreFixMeshLighting.hpp"
#include "patchers/PatcherMeshPreFixTextureSlotCount.hpp"
#include "patchers/PatcherMeshShaderComplexMaterial.hpp"
#include "patchers/PatcherMeshShaderDefault.hpp"
#include "patchers/PatcherMeshShaderTransformParallaxToCM.hpp"
#include "patchers/PatcherMeshShaderTruePBR.hpp"
#include "patchers/PatcherMeshShaderVanillaParallax.hpp"
#include "patchers/PatcherTextureGlobalConvertToHDR.hpp"
#include "patchers/PatcherTextureHookConvertToCM.hpp"
#include "patchers/PatcherTextureHookFixSSS.hpp"
#include "patchers/base/PatcherUtil.hpp"
#include "pgutil/PGMeshPermutationTracker.hpp"
#include "util/ExceptionHandler.hpp"
#include "util/FileUtil.hpp"
#include "util/StringUtil.hpp"

#include <CLI/CLI.hpp>
#include <cpptrace/from_current.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
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
    // See PGMeshPermutationTracker::setRelaxWeightValidation's own comment for the full story --
    // this is a pure log-severity toggle for the real, confirmed false positives in the _0/_1
    // weight-variant consistency check (upstream hakasapl/PGPatcher#729). Off by default, matching
    // the real GUI's own current (strict, error-level) behavior exactly -- never changes which
    // meshes get patched or what output gets written either way.
    bool relaxWeightValidation = false;

    struct Patch {
        CLI::App* subCommand = nullptr;
        unordered_set<string> patchers;
        filesystem::path source = ".";
        filesystem::path output = "ParallaxGen_Output";
        filesystem::path cfgDir; // optional -- see readProcessingSettings's own comment for why
        bool mapTexturesFromMeshes = false;
        bool highMem = false;
        // Matches the real GUI's own --consider-allmeshes flag (PGPatcher/src/main.cpp:918) and its
        // own default (false, main.cpp:77). Previously this subcommand always passed forceBasePatch=
        // true to patchMeshes() regardless -- meshes with zero real plugin reference (no ARMO/WEAP/
        // etc. record anywhere uses them) got force-patched using their own baked-in textures instead
        // of being skipped like the GUI's own default does. Confirmed live (2026-08-19/20): this was
        // the source of a real ~10,500-file overshoot vs. the director's own fresh GUI run. Left
        // default-off so this subcommand matches the GUI's own conservative default unless a user
        // opts in.
        bool considerAllMeshes = false;
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
        // Same reasoning as Patch.considerAllMeshes above -- default-off, matching the real GUI's own
        // default, so this dry-run scan reflects what a default-mode `patch` run would actually do.
        bool considerAllMeshes = false;
    } Conflicts;
};

// PGPatcher's real GUI never scans every mesh it finds -- it passes a real mesh blocklist/allowlist/
// vanillaBSAList to mapFiles() (PGPatcher/src/main.cpp:575), all sourced from settings.json's own
// `params.processing.*` fields (PGConfig.cpp's own load logic, confirmed by reading it directly --
// this mirrors that parsing exactly, key names included). Neither `conflicts` nor `patch` ever read
// this before -- both called mapFiles({}, {}, {}, {}, ...), completely unfiltered. Confirmed live
// against the director's real ~2000-mod install (2026-08-19): his real blocklist (default patterns
// plus his own added `*\actors\*`/`*loadscreen*`) is why his real GUI's own Mesh Patcher phase
// processes 108,705 meshes while our own unfiltered scan was processing 141,910 -- a ~33,000-mesh
// gap, `*\actors\*` alone accounting for most of it (66,295 loose files in his own Data/meshes/actors
// folder). This isn't just a cosmetic count mismatch: those extra ~33,000 meshes are exactly the
// kind of content (actor/character meshes, LOD, markers, cameras) the real GUI has never needed to
// handle correctly in its own real (non-dry-run) save pipeline, since it always filters them out
// first -- a real, live suspect for the `patch` subcommand's own confirmed indefinite hang (see that
// subcommand's own comment on its still-BSA-excluded PGDirectory construction).
struct PGProcessingSettings {
    vector<wstring> blockList;
    vector<wstring> allowList;
    vector<pair<wstring, PGEnums::TextureType>> textureMaps;
    vector<wstring> vanillaBSAList;
    // Defaults to the same real default the GUI itself falls back to (PGConfig::getDefaultParams)
    // when settings.json has no explicit `allowedmodelrecordtypes` key -- matches real GUI behavior
    // for a mod-manager-fresh settings.json, not just an empty set.
    unordered_set<PGPlugin::ModelRecordType> allowedModelRecTypes = PGPlugin::getDefaultRecTypeSet();
    // Read for the same reason as everything else in this struct: so PGTools' own logging level
    // reflects the user's real settings.json (params.processing.enabledebuglogging/
    // enabletracelogging) rather than requiring a separate -v/-vv CLI flag every time, matching how
    // the real GUI's own initLogger() is driven entirely by these two settings.json fields.
    bool enableDebugLogging = false;
    bool enableTraceLogging = false;
};

auto readProcessingSettings(const filesystem::path& cfgDir) -> PGProcessingSettings
{
    PGProcessingSettings out;
    if (cfgDir.empty()) {
        return out; // no cfg dir given -- empty lists, i.e. this subcommand's own prior behavior
    }

    nlohmann::json settingsJSON;
    if (!FileUtil::getJSON(cfgDir / "settings.json", settingsJSON)) {
        spdlog::warn("No settings.json found at {} -- mesh blocklist/allowlist/vanillaBSAList will be "
                     "empty, unlike the real GUI.",
                     (cfgDir / "settings.json").string());
        return out;
    }

    if (!settingsJSON.contains("params") || !settingsJSON["params"].contains("processing")) {
        return out;
    }
    const auto& proc = settingsJSON["params"]["processing"];

    if (proc.contains("blocklist")) {
        for (const auto& item : proc["blocklist"]) {
            out.blockList.push_back(StringUtil::utf8toUTF16(item.get<string>()));
        }
    }
    if (proc.contains("allowlist")) {
        for (const auto& item : proc["allowlist"]) {
            out.allowList.push_back(StringUtil::utf8toUTF16(item.get<string>()));
        }
    }
    if (proc.contains("texturemaps")) {
        for (const auto& item : proc["texturemaps"].items()) {
            out.textureMaps.emplace_back(StringUtil::utf8toUTF16(item.key()),
                                         PGEnums::getTexTypeFromStr(item.value().get<string>()));
        }
    }
    if (proc.contains("vanillabsalist")) {
        for (const auto& item : proc["vanillabsalist"]) {
            out.vanillaBSAList.push_back(StringUtil::utf8toUTF16(item.get<string>()));
        }
    }
    // Matches PGConfig.cpp's own load logic exactly: only replace the default set if the key is
    // actually present (clear first, same as the GUI's own loader) -- an absent key keeps the
    // struct's own default (PGPlugin::getDefaultRecTypeSet()), not an empty set.
    if (proc.contains("allowedmodelrecordtypes")) {
        out.allowedModelRecTypes.clear();
        for (const auto& item : proc["allowedmodelrecordtypes"]) {
            out.allowedModelRecTypes.insert(PGPlugin::getRecTypeFromString(item.get<string>()));
        }
    }
    if (proc.contains("enabledebuglogging")) {
        out.enableDebugLogging = proc["enabledebuglogging"].get<bool>();
    }
    if (proc.contains("enabletracelogging")) {
        out.enableTraceLogging = proc["enabletracelogging"].get<bool>();
    }

    return out;
}

// Same values as the real GUI's own initLogger (PGPatcher/src/main.cpp:68-69) -- not shared/
// importable from a common header (checked PGLib first, confirmed GUI-local), so duplicated here
// rather than introducing new cross-target coupling for two constants.
constexpr unsigned MAX_LOG_SIZE = 10490000; // 10 MB
constexpr unsigned MAX_LOG_FILES = 1000;

// Mirrors the real GUI's own initLogger (PGPatcher/src/main.cpp:178-233) as closely as makes sense
// for a CLI tool -- same rotating file sink, same size/count limits, same log-folder cleanup, same
// line format, same debug/trace level semantics (including the asymmetric console-vs-file clamp
// under trace). PGTools previously only ever configured spdlog's own default logger in place
// (console-only, implicit) -- no file sink, no rotation, no stale-log cleanup -- so a real PGTools
// run left no record on disk at all once the terminal scrolled past it, unlike every real GUI run.
void initLogger(const filesystem::path& logpath, bool enableDebug, bool enableTrace)
{
    // delete old logs -- same "PGPatcher*.log" match as the GUI's own cleanup, so a PGTools run and
    // a GUI run pointed at the same log folder don't leave each other's stale files behind either.
    if (filesystem::exists(logpath.parent_path())) {
        try {
            for (const auto& entry : filesystem::directory_iterator(logpath.parent_path())) {
                if (entry.is_regular_file() && entry.path().extension() == ".log"
                    && entry.path().filename().wstring().starts_with(L"PGPatcher")) {
                    filesystem::remove(entry.path());
                }
            }
        } catch (const filesystem::filesystem_error& e) {
            cerr << "Failed to delete old logs: " << e.what() << "\n";
        }
    }

    vector<spdlog::sink_ptr> sinks;
    auto consoleSink = make_shared<spdlog::sinks::stdout_color_sink_mt>();
    sinks.push_back(consoleSink);

    auto fileSink = make_shared<spdlog::sinks::rotating_file_sink_mt>(logpath.wstring(), MAX_LOG_SIZE, MAX_LOG_FILES);
    sinks.push_back(fileSink);

    auto logger = make_shared<spdlog::logger>("PGTools", sinks.begin(), sinks.end());

    spdlog::register_logger(logger);
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::info);
    spdlog::flush_on(spdlog::level::info);

    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

    if (enableDebug) {
        spdlog::set_level(spdlog::level::debug);
        spdlog::flush_on(spdlog::level::debug);
        spdlog::debug("DEBUG logging enabled");
    }
    if (enableTrace) {
        spdlog::set_level(spdlog::level::trace);
        // Same asymmetric clamp as the GUI: trace-level spam stays in the file only, console
        // output never exceeds debug even when the file sink is capturing full trace detail.
        consoleSink->set_level(spdlog::level::debug);
        spdlog::flush_on(spdlog::level::trace);
        spdlog::trace("TRACE logging enabled");
    }
}

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

    PGMeshPermutationTracker::setRelaxWeightValidation(args.relaxWeightValidation);

    // Check if patch subcommand was used
    if (args.Patch.subCommand->parsed()) {
        // Get current time to compare later
        auto startTime = chrono::high_resolution_clock::now();
        long long timeTaken = 0;

        args.Patch.source = filesystem::absolute(args.Patch.source);
        args.Patch.output = filesystem::absolute(args.Patch.output);

        // BSA-inclusive scanning RE-ENABLED here after a real investigation (see this repo's own
        // docs -- vortex-collection-tools/docs/reference-pgpatcher-internals.md has the full story).
        // This was reverted earlier in the same session after live testing found what looked like an
        // indefinite hang under multithreading. Root-caused with a debugger: the actual hang was
        // TaskPoolRunner::runTasks() looping forever after an exception (a plain std::filesystem
        // "path too long" error, caused by this session's own overlong scratch-folder test output
        // path, nothing BSA-related) because stop()-ing the pool doesn't unstick tasks that never
        // started. That bug is now fixed at the source (TaskPoolRunner.cpp) -- any future exception,
        // from any cause, fails fast and cleanly instead of hanging, so it's safe to restore BSA
        // inclusion here too. Confirmed live: the real GUI's own build (PGPatcher/src/main.cpp:437)
        // includes BSA content too, and a real ~2000-mod `patch` run with this restored matched the
        // GUI's own file count far more closely than the BSA-excluded version ever could.
        auto bg = BethesdaGame(BethesdaGame::GameType::SKYRIM_SE, args.Patch.source.parent_path());
        auto pgd = PGDirectory(&bg, args.Patch.output);
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

        // Plugin initialization -- REQUIRED for mesh-use detection to work AT ALL. Confirmed missing
        // from this subcommand entirely until now, and confirmed live (2026-08-20) via a full traced
        // run: with this missing, PGPlugin::getModelUses() (called from PGDirectory::mapFiles, which
        // populates nifCache.meshUses -- PGPatcher.cpp:427/434) returns EMPTY for every single mesh in
        // the game, because the plugin/ESP model-record cache PGPlugin::populateObjs() builds was
        // simply never populated. That live run: 108,705 meshes scanned, 108,705 hit "no plugin uses"
        // -- 100%, not a subset. This is *why* forceBasePatch was hardcoded true in the first place
        // (without it, `patch` would silently write ZERO patched meshes) -- not because PGTools was
        // being "more thorough" than the GUI's own conservative default, but because it was blind to
        // plugin data altogether and needed forceBasePatch as a workaround. It also means every mesh
        // that DOES have real plugin uses was still being patched using only its own base/default
        // textures (an empty `alternateTextures` map, same as the forced-dummy path) instead of
        // whatever a plugin's own AlternateTextures override actually specifies -- a much bigger,
        // more systemic explanation for real content mismatches than the single-mesh mod-priority
        // finding below. Mirrors the real GUI's own unconditional setup exactly (PGPatcher/src/
        // main.cpp:370-380, called before its own mod-manager init, same ordering used here).
        PGPlugin::initialize(bg, exePath);
        PGPlugin::populateObjs(args.Patch.output / "PGPatcher.esp");

        // Mod manager setup -- REQUIRED for `patch` to make the same mod-priority-aware conflict
        // resolution decisions the real GUI always makes (PGPatcher/src/main.cpp:820-821 -- this is
        // unconditional there, every real run, not something the GUI can skip). Confirmed missing
        // from this subcommand ENTIRELY until now: PGGlobals::isPGMMSet() was false for the whole
        // `patch` pipeline, so every ShaderPatcherMatch's `mod` field (PGPatcher.cpp's getMatches,
        // ~line 794 -- only ever set `if (PGGlobals::isPGMMSet())`) stayed null. sortMatches()
        // (PGPatcher.cpp:193) checks `mod->priority` to pick the winning match on a shader conflict,
        // but with every match's mod null, it fell straight through to its own final tiebreak --
        // alphabetical by JSON config path -- silently ignoring mod priority for the ENTIRE real
        // build pipeline, every single conflict, the whole time. Confirmed as the real, definitive
        // root cause of a live traced example (chickencarcass.nif, 2026-08-19/20): the CLI picked
        // "Faultier's PBR Skyrim AIO 4k" (priority 1215, path starts with "faultier's...") over
        // "Chicken and Chicks PBR" (priority 1258 -- the correct, higher-priority winner, path starts
        // with "pbrchicken...") purely because of alphabetical ordering, not mod priority at all.
        // Mirrors `conflicts`' own identical setup below -- `patch` just never had it.
        auto pgmm = PGModManager(PGModManager::ModManagerType::VORTEX);
        PGGlobals::setPGMM(&pgmm);

        nlohmann::json patchModJSON;
        if (!args.Patch.cfgDir.empty()) {
            const auto modListFile = args.Patch.cfgDir / "modrules.json";
            if (FileUtil::getJSON(modListFile, patchModJSON)) {
                pgmm.loadJSON(patchModJSON);
            } else {
                spdlog::warn("No existing modrules.json found at {} -- every mod will read as new "
                             "(default priority), so shader conflicts will not reflect real mod priority.",
                             modListFile.string());
            }
        } else {
            spdlog::warn("No --cfg-dir given -- mod priority is unavailable, so shader conflicts will "
                         "fall back to alphabetical tiebreaking instead of respecting mod priority.");
        }

        pgmm.populateModFileMapVortex(args.Patch.source);

        // Init file map -- BSA-inclusive now too, matching the real GUI. See the comment on this
        // subcommand's own PGDirectory construction above for the full story on why this was
        // reverted and then safely restored.
        pgd.populateFileMap(true);

        // Map files -- now applies the real mesh blocklist/allowlist/vanillaBSAList from
        // settings.json (readProcessingSettings's own comment has the full story). This was
        // completely unfiltered before (every {} below), meaning meshes/actors, meshes/lod,
        // meshes/markers etc. all got processed by the real save pipeline that BSA-inclusion alone
        // stalled -- a live suspect for that hang, independent of BSAs entirely.
        const auto processingSettings = readProcessingSettings(args.Patch.cfgDir);
        pgd.mapFiles(processingSettings.blockList,
                    processingSettings.allowList,
                    processingSettings.textureMaps,
                    processingSettings.vanillaBSAList,
                    args.multithreading);

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

        // Create patcher factory -- the real activation decisions (which patchers/pre-patchers/
        // post-patchers get built into this set, and under what conditions) now live in ONE shared
        // place, PGPatcher::buildStandardMeshPatcherSet() (PGLib/src/PGPatcher.cpp), consumed by both
        // this subcommand and the real GUI's own equivalent block. This is a direct structural fix
        // for the literal failure mode all five of tonight's real parity bugs shared: two hand-
        // maintained copies of the same "if (x) { emplace(...) }" logic, with this subcommand's own
        // copy silently missing pieces the GUI's had all along. See that function's own doc comment
        // (PGLib/include/PGPatcher.hpp) for exactly what it does and does NOT cover -- it only
        // decides set membership, not loadOptions()/loadStatics()/shader-hook initialization, which
        // stay each caller's own responsibility below, completely unchanged from before this refactor.
        PGPatcher::ActivePatcherRequest activePatchers;
        activePatchers.fixMeshLighting = patcherDefs.contains("fixmeshlighting");
        activePatchers.parallax = patcherDefs.contains("parallax");
        activePatchers.complexMaterial = patcherDefs.contains("complexmaterial");
        activePatchers.truePBR = patcherDefs.contains("truepbr");
        activePatchers.parallaxToCM = patcherDefs.contains("parallaxtocm");
        activePatchers.restoreDefaultShaders = patcherDefs.contains("restoredefaultshaders");
        activePatchers.fixSSS = patcherDefs.contains("fixsss");
        activePatchers.hairFlowMap = patcherDefs.contains("hairflowmap");
        // fixtextureslotcount kept as its own explicit token for backward compatibility (existing
        // callers may already pass it directly) -- the shared function's own condition already covers
        // the GUI's real auto-activation case (parallax/complexMaterial/truePBR active), so this only
        // matters for a caller wanting it WITHOUT any of those three, which the shared function alone
        // wouldn't trigger.
        auto meshPatchers = PGPatcher::buildStandardMeshPatcherSet(activePatchers);
        // Only add it again if the shared function's own condition (parallax/complexMaterial/
        // truePBR) didn't already add it -- checking the source booleans directly, not the resulting
        // vector's emptiness, since fixMeshLighting can independently populate prePatchers too and
        // would make an empty()-check wrongly skip this when it shouldn't (or double-add when it's
        // already there -- either way, checking the real source condition is the only correct test).
        if (patcherDefs.contains("fixtextureslotcount")
            && !(activePatchers.parallax || activePatchers.complexMaterial || activePatchers.truePBR)) {
            meshPatchers.prePatchers.emplace_back(PatcherMeshPreFixTextureSlotCount::getFactory());
        }

        // Caller-specific option-loading and shader-hook initialization -- deliberately NOT part of
        // the shared function above (see its own doc comment for why). Unchanged from before this
        // refactor, still gated on the exact same conditions.
        if (patcherDefs.contains("complexmaterial")) {
            PatcherMeshShaderComplexMaterial::loadOptions(patcherDefs["complexmaterial"]);
        }
        if (patcherDefs.contains("truepbr")) {
            PatcherMeshShaderTruePBR::loadStatics(pgd.getPBRJSONs());
            PatcherMeshShaderTruePBR::loadOptions(patcherDefs["truepbr"]);
        }
        if (patcherDefs.contains("parallaxtocm")) {
            PatcherTextureHookConvertToCM::initShader();
        }
        if (patcherDefs.contains("fixsss")) {
            PatcherTextureHookFixSSS::initShader();
        }

        // PGTools-only capability -- no GUI equivalent at all (confirmed via a full grep of
        // PGPatcher/src/main.cpp, zero references), so it stays here rather than in the shared
        // function, same as before this refactor.
        if (patcherDefs.contains("particlelightstolp")) {
            meshPatchers.globalPatchers.emplace_back(PatcherMeshGlobalParticleLightsToLP::getFactory());
        }

        PatcherUtil::PatcherTextureSet texPatchers;
        if (patcherDefs.contains("converttohdr")) {
            PatcherTextureGlobalConvertToHDR::initShader();

            texPatchers.globalPatchers.emplace_back(PatcherTextureGlobalConvertToHDR::getFactory());
            PatcherTextureGlobalConvertToHDR::loadOptions(patcherDefs["converttohdr"]);
        }

        PGPatcher::loadPatchers(meshPatchers, texPatchers);
        // checkAllowedRecTypes=true + the real allowedModelRecTypes now, matching the real GUI's own
        // call exactly (PGPatcher/src/main.cpp:633-638) -- confirmed by reading it directly that the
        // GUI hardcodes true here, while this subcommand previously left it at its own false default,
        // meaning the allowedmodelrecordtypes setting.json field was silently ignored entirely.
        PGPatcher::patchMeshes(
            args.multithreading, args.Patch.considerAllMeshes, processingSettings.allowedModelRecTypes, true);
        PGPatcher::patchTextures(args.multithreading);

        // Saving plugins / diff JSON -- REQUIRED, confirmed missing entirely until now. This
        // subcommand never called PGPlugin::savePlugin() or saved ParallaxGen_Diff.json at all, so a
        // real `patch` run's output directory was always missing PGPatcher.esp/PG_1.esp/PG_2.esp
        // (whatever new plugin records any patcher created, e.g. new AlternateTextures entries, were
        // computed in memory and then silently discarded) and ParallaxGen_Diff.json (the per-mesh
        // CRC32 diff log). Mirrors the real GUI's own finalization exactly (PGPatcher/src/main.cpp:
        // 661-726) -- wait for the async file-saver queue to finish, bail if nothing was generated,
        // then save plugins before saving the diff log. PGTools has no --esm-all/--no-esm flags of
        // its own, so this always uses the GUI's own default (ESMMode::PGPATCHER_ONLY).
        if (PGGlobals::getFileSaver().isWorking()) {
            spdlog::info("Waiting for files to finish saving...");
            PGGlobals::getFileSaver().waitForCompletion();
        }
        if (PGPatcher::isOutputEmpty()) {
            spdlog::warn("Output directory is empty. No files were generated.");
        } else {
            spdlog::info("Saving Plugins");
            PGPlugin::savePlugin(args.Patch.output, PGPlugin::ESMMode::PGPATCHER_ONLY);
        }

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

        // Save diff json -- see the "Saving plugins / diff JSON" comment above for why this is here
        // at all. Matches the GUI's own ordering (diff JSON saved last, after plugins and the
        // cubemap asset deploy).
        const auto diffJSON = PGPatcher::getDiffJSON();
        if (!diffJSON.empty()) {
            const filesystem::path diffJSONPath = args.Patch.output / "ParallaxGen_Diff.json";
            FileUtil::saveJSON(diffJSONPath, diffJSON, true);
            pgd.addGeneratedFile("ParallaxGen_Diff.json");
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

        // Same real-BethesdaGame requirement as `patch` above -- see that block's own comment for
        // the full reasoning (BSA inclusion needs a real m_bg, the raw-path constructor always left
        // it null).
        auto bg = BethesdaGame(BethesdaGame::GameType::SKYRIM_SE, args.Conflicts.source.parent_path());
        auto pgd = PGDirectory(&bg, args.Conflicts.output);
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

        // Plugin initialization -- same real gap as `patch`'s own identical block above (see that
        // comment for the full story): without this, every mesh reads as having zero plugin uses,
        // so this subcommand's own conflict/shader data would never reflect real plugin-driven
        // AlternateTextures overrides, just each mesh's own base textures. Mirrors the real GUI's own
        // unconditional setup exactly (PGPatcher/src/main.cpp:370-380).
        PGPlugin::initialize(bg, exePath);
        PGPlugin::populateObjs(args.Conflicts.output / "PGPatcher.esp");

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

        // Init file map -- includes BSA-packed files now, matching the real GUI's own behavior
        // (`patch` above now does too). Every mod whose shader config (e.g. a TruePBR
        // pbrnifpatcher/*.json) ships packed inside a BSA rather than as loose files was previously
        // invisible to conflict detection entirely, and now isn't.
        pgd.populateFileMap(true);

        // Map files -- now applies the real mesh blocklist/allowlist/vanillaBSAList too, matching
        // the real GUI exactly (readProcessingSettings's own comment has the full story). Confirmed
        // live this was the source of a real mesh-count mismatch against the director's own real
        // PGPatcher run (141,910 unfiltered vs. his real 108,705 after his own blocklist, mostly his
        // added `*\actors\*` entry) -- this was never about BSAs at all, a separate pre-existing gap.
        const auto processingSettings = readProcessingSettings(args.Conflicts.cfgDir);
        pgd.mapFiles(processingSettings.blockList,
                    processingSettings.allowList,
                    processingSettings.textureMaps,
                    processingSettings.vanillaBSAList,
                    args.multithreading);

        // Register only the requested shader (+ shader-transform) patchers. Deliberately simpler
        // than `patch`'s own patcherDefs mechanism -- no per-patcher bracket-syntax options parsing,
        // since conflict detection only needs each patcher ACTIVE (so it contributes matches), not
        // its output tuned; every patcher below loads with its own default options.
        //
        // Same shared PGPatcher::buildStandardMeshPatcherSet() `patch` above now uses -- see that
        // function's own doc comment (PGLib/include/PGPatcher.hpp) for what it covers. This
        // subcommand never activates fixMeshLighting/restoreDefaultShaders/fixSSS/hairFlowMap (no
        // pre-existing behavior to preserve there -- conflict detection never needed post-patchers,
        // dry-run purpose predates this refactor), so those stay false/default here, unchanged from
        // before.
        PGPatcher::ActivePatcherRequest activePatchers;
        activePatchers.parallax = args.Conflicts.patchers.contains("parallax");
        activePatchers.complexMaterial = args.Conflicts.patchers.contains("complexmaterial");
        activePatchers.truePBR = args.Conflicts.patchers.contains("truepbr");
        activePatchers.parallaxToCM = args.Conflicts.patchers.contains("parallaxtocm");
        auto meshPatchers = PGPatcher::buildStandardMeshPatcherSet(activePatchers);

        // Caller-specific option-loading and shader-hook initialization -- same reasoning as `patch`
        // above, unchanged from before this refactor.
        if (args.Conflicts.patchers.contains("complexmaterial")) {
            unordered_map<string, string> complexMaterialOptions;
            PatcherMeshShaderComplexMaterial::loadOptions(complexMaterialOptions);
        }
        if (args.Conflicts.patchers.contains("truepbr")) {
            PatcherMeshShaderTruePBR::loadStatics(pgd.getPBRJSONs());
            unordered_map<string, string> truePBROptions;
            PatcherMeshShaderTruePBR::loadOptions(truePBROptions);
        }
        if (args.Conflicts.patchers.contains("parallaxtocm")) {
            PatcherTextureHookConvertToCM::initShader();
        }

        // Empty -- patchTextures is never called, so no texture patchers are ever needed.
        const PatcherUtil::PatcherTextureSet texPatchers;
        PGPatcher::loadPatchers(meshPatchers, texPatchers);

        // checkAllowedRecTypes=true + the real allowedModelRecTypes, matching the real GUI's own call
        // (see `patch` subcommand's own identical comment above) -- previously always false/empty
        // here too, silently ignoring the allowedmodelrecordtypes setting.json field.
        PGPatcher::patchMeshes(args.multithreading,
                               args.Conflicts.considerAllMeshes,
                               processingSettings.allowedModelRecTypes,
                               true,
                               false,
                               {},
                               true);

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
    // Real, separate, pre-existing bug found live (2026-08-19): CLI11's add_flag on a bool just sets
    // the bound variable to true whenever the flag is present -- it does not infer "disable" from the
    // flag's own name. Since args.multithreading already defaults to true, passing --no-multithreading
    // was a complete no-op the entire time this flag has existed -- confirmed via a live debugger
    // stack trace showing the "single-threaded" repro was still running the full 22-thread
    // TaskPoolRunner path. The `{false}` suffix is CLI11's own real syntax for "set to this value when
    // the flag is passed" -- this is what actually wires --no-multithreading to its own stated meaning.
    app.add_flag("--no-multithreading{false}", args.multithreading, "Disable multithreading");
    app.add_flag("--shortcut",
                 args.shortcut,
                 "Keep pgtools running at the end (useful if you are running not in a terminal directly)");
    app.add_flag("--relax-weight-validation",
                 args.relaxWeightValidation,
                 "Log _0/_1 weight-variant mesh mismatches as warnings instead of errors -- does not "
                 "change what gets patched, only how these known-false-positive-prone checks are "
                 "logged (see hakasapl/PGPatcher#729)");

    args.Patch.subCommand = app.add_subcommand("patch", "Patch meshes");
    args.Patch.subCommand->add_option("patcher", args.Patch.patchers, "List of patchers to use")
        ->required()
        ->delimiter(',');
    // `--source`/`--output` named options, REPLACING the positional `source`/`output` this
    // subcommand used to declare -- confirmed live (docs/plans/2026-08-19-pgpatcher-load-order-
    // tool.md, vortex-collection-tools) that CLI11 does not reliably bind those positionals once the
    // preceding delimited-unordered_set "patcher..." positional is present: `pgtools patch truepbr
    // C:/temp/src` silently left source at its "." default rather than binding the path given, so
    // in practice the positional form never worked with real values anyway -- only the zero-extra-
    // args invocation (`patch truepbr`, both defaults) ever actually worked. Named options sidestep
    // the ambiguity entirely, same fix already proven on the `conflicts` subcommand. (A prior attempt
    // at declaring BOTH a `--output` named option AND a positional also named "output" side by side
    // crashed the built exe outright, even on plain `--help` -- reverted; don't reintroduce that
    // combination.)
    args.Patch.subCommand->add_option("--source", args.Patch.source, "Source directory")->default_str(".");
    args.Patch.subCommand->add_option("--output", args.Patch.output, "Output directory")
        ->default_str("ParallaxGen_Output");
    // Optional, not required -- unlike `conflicts`, `patch` predates this whole item and existing
    // callers (vortex-collection-tools' own /build route, before this change) don't pass it. Missing
    // this just means the real mesh blocklist/allowlist/vanillaBSAList never get read, matching this
    // subcommand's own prior behavior exactly -- never a hard failure for omitting it.
    args.Patch.subCommand->add_option(
        "--cfg-dir",
        args.Patch.cfgDir,
        "Directory containing settings.json (PGPatcher's own cfg folder) -- optional, used to read the real mesh "
        "blocklist/allowlist/vanillaBSAList so this matches what the real GUI actually scans");
    args.Patch.subCommand->add_flag("--high-mem", args.Patch.highMem, "High memory usage mode (default: false)");
    // Same flag name/help text as the real GUI's own option (PGPatcher/src/main.cpp:918) -- default
    // off, matching the GUI's own default (considerAllMeshes=false, main.cpp:77).
    args.Patch.subCommand->add_flag("--consider-allmeshes",
                                    args.Patch.considerAllMeshes,
                                    "Consider all meshes, even those not in plugins, for patching");

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
    args.Conflicts.subCommand->add_flag("--consider-allmeshes",
                                        args.Conflicts.considerAllMeshes,
                                        "Consider all meshes, even those not in plugins, for patching");
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

    // Initialize Logger -- mirrors the real GUI's own initLogger (see that function's own comment
    // above for the full story). Reuses readProcessingSettings() (already reads this same
    // settings.json for the mesh blocklist/allowlist/etc elsewhere in this file) rather than
    // hand-rolling a second JSON parse just for these two fields. Whichever subcommand was actually
    // invoked determines which --cfg-dir to read; if neither subcommand has a cfg-dir (or none was
    // given at all), readProcessingSettings() already degrades gracefully to false/false, same as
    // every other field it reads.
    filesystem::path logCfgDir;
    if (args.Patch.subCommand->parsed()) {
        logCfgDir = args.Patch.cfgDir;
    } else if (args.Conflicts.subCommand->parsed()) {
        logCfgDir = args.Conflicts.cfgDir;
    }
    const auto logProcessingSettings = readProcessingSettings(logCfgDir);

    // Combined with the existing -v/-vv CLI flags rather than replacing them -- either one turning
    // on debug/trace is enough, so a caller can still force verbosity without needing settings.json,
    // while a real settings.json with logging already enabled elevates PGTools' own log to match
    // without requiring the caller to also pass -v/-vv.
    const bool enableDebug = args.verbosity >= 1 || logProcessingSettings.enableDebugLogging;
    const bool enableTrace = args.verbosity >= 2 || logProcessingSettings.enableTraceLogging;

    initLogger(exePath / "log" / "PGPatcher.log", enableDebug, enableTrace);

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
