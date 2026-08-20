#pragma once

#include "PGDirectory.hpp"
#include "PGPlugin.hpp"
#include "patchers/base/PatcherUtil.hpp"
#include "pgutil/PGEnums.hpp"
#include "pgutil/PGMeshPermutationTracker.hpp"
#include "pgutil/PGTypes.hpp"
#include "util/TaskTracker.hpp"

#include "Geometry.hpp"
#include "NifFile.hpp"
#include "util/TaskQueue.hpp"
#include <DirectXTex.h>
#include <boost/container_hash/hash.hpp>
#include <boost/functional/hash.hpp>
#include <miniz.h>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class PGPatcher {
public:
    // Mesh Patch Tracking structures (for meta info displayed to user later)
    struct MatchMeta {
        std::shared_ptr<PGModManager::Mod> mod;
        PGEnums::ShapeShader shader {};
        PGEnums::ShapeShader shaderTransformTo {};
        std::filesystem::path matchedPath;
    };
    struct MeshShapeMeta {
        uint32_t blockID;
        std::string shapeName;
        std::vector<std::string> prePatchersApplied;
        std::vector<std::string> postPatchersApplied;
        std::unordered_map<PGMeshPermutationTracker::FormKey,
                           std::vector<MatchMeta>,
                           PGMeshPermutationTracker::FormKeyHash>
            matches;
    };
    struct MeshMeta {
        std::vector<std::string> globalPatchersApplied;
        std::vector<PGMeshPermutationTracker::FormKey> formKeys;
        std::map<size_t, MeshShapeMeta> shapeMeta;
    };

    using MeshPatchInfo = std::map<std::filesystem::path, MeshMeta>;

    /**
     * @struct ActivePatcherRequest
     * @brief Which real, GUI-and-CLI-shared mesh patchers/pre-patchers/post-patchers should be active
     * for a run -- the single source of truth consumed by buildStandardMeshPatcherSet() below.
     *
     * Deliberately covers only the patchers BOTH the real GUI (PGPatcher/src/main.cpp) and PGTools
     * (PGTools/src/main.cpp) actually expose -- confirmed by reading both files' own patcher-
     * activation logic in full (2026-08-20). PGTools-only capabilities with no GUI equivalent at all
     * (ParticleLightsToLP, ConvertToHDR -- confirmed via a full grep of PGPatcher/src/main.cpp,
     * zero references to either) are deliberately NOT included here; they stay PGTools-only, built
     * directly in that file, same as today.
     */
    struct ActivePatcherRequest {
        bool fixMeshLighting = false;
        bool parallax = false;
        bool complexMaterial = false;
        bool truePBR = false;
        bool parallaxToCM = false;
        bool restoreDefaultShaders = false;
        bool fixSSS = false;
        bool hairFlowMap = false;
    };

private:
    // Registered Patchers
    static PatcherUtil::PatcherTextureSet s_texPatchers;
    static PatcherUtil::PatcherMeshSet s_meshPatchers;

    static std::shared_mutex s_diffJSONMutex;
    static nlohmann::json s_diffJSON;

    static inline MeshPatchInfo s_meshPatchInfo;
    static inline std::shared_mutex s_meshPatchInfoMutex;

public:
    /**
     * @brief Builds the standard (GUI-and-CLI-shared) mesh patcher set from a real ActivePatcherRequest.
     *
     * Single source of truth for "which patchers get activated, and under what conditions" -- pulled
     * out of PGPatcher/src/main.cpp and PGTools/src/main.cpp (2026-08-20) after all five real bugs
     * found during a parity investigation shared the exact same shape: the two files each hand-
     * maintained their own copy of this same decision, and PGTools' own copy was silently missing
     * pieces the GUI's had all along, five separate times. Both frontends now call this one function
     * instead.
     *
     * Deliberately narrow in scope: this ONLY decides which patcher/pre-patcher/post-patcher/shader-
     * transform-patcher factories go into the returned set -- it does NOT call any patcher's own
     * loadOptions()/loadStatics(), and does NOT call the shader-hook initShader() functions
     * (PatcherTextureHookConvertToCM::initShader()/PatcherTextureHookFixSSS::initShader()) that
     * parallaxToCM/fixSSS also need. Those stay each caller's own responsibility, exactly as today --
     * the GUI and PGTools use genuinely different option sources (GUI: raw booleans from its own
     * params/args; PGTools: bracket-parsed CLI option maps) and different error-handling around the
     * shader-hook calls (GUI: hard-fails the whole run on init failure; PGTools: doesn't check the
     * return value) -- folding either into this shared function would either lose caller-specific
     * behavior or require inventing a new one, and a real behavior change here was explicitly out of
     * scope for this refactor.
     *
     * @param req which patchers should be active for this run.
     */
    static auto buildStandardMeshPatcherSet(const ActivePatcherRequest& req) -> PatcherUtil::PatcherMeshSet;

    /**
     * @brief Allows patchers to be registered and used in the patching process.
     *
     * @param meshPatchers mesh patchers
     * @param texPatchers texture patchers
     */
    static void loadPatchers(const PatcherUtil::PatcherMeshSet& meshPatchers,
                             const PatcherUtil::PatcherTextureSet& texPatchers);

    /**
     * @brief Run mesh patcher
     *
     * @param multiThread whether to use multithreading
     * @param excludeFacegens whether to skip patching facegen meshes
     * @param dryRun when true, runs the full matching pass (so PGModManager's conflict/shader data
     * gets populated exactly as a real run would) but skips writing any patched mesh to disk. Used
     * by the conflicts-only CLI path, which only needs PGModManager's resulting state, not real
     * output.
     */
    static void patchMeshes(const bool& multiThread = true,
                            const bool& forceBasePatch = false,
                            const std::unordered_set<PGPlugin::ModelRecordType>& allowedModelRecTypes = {},
                            const bool& checkAllowedRecTypes = false,
                            const bool& excludeFacegens = false,
                            const std::function<void(size_t,
                                                     size_t)>& progressCallback = {},
                            const bool& dryRun = false);

    /**
     * @brief Run texture patcher
     *
     * @param multiThread whether to use multithreading
     */
    static void patchTextures(const bool& multiThread = true,
                              const std::function<void(size_t,
                                                       size_t)>& progressCallback = {});

    /**
     * @brief Get the Patch Meta object
     *
     * @return std::map<std::filesystem::path, MeshMeta>
     */
    static auto getPatchMeta() -> MeshPatchInfo;

    /**
     * @brief Sort matches according to a provided mod priority list.
     *
     * Matches are ordered by mod priority first, then by shader quality, with stable ordering within ties.
     */
    static void sortMatches(std::vector<PatcherUtil::ShaderPatcherMatch>& matches);

    /**
     * @brief Sort mesh patch metadata matches according to a provided mod priority list.
     *
     * Matches are ordered by mod priority first, then by shader quality, with stable ordering within ties.
     */
    static void sortMatches(std::vector<MatchMeta>& matches,
                            const std::vector<std::shared_ptr<PGModManager::Mod>>& modPriorityList);

    /**
     * @brief Check whether any mesh patch metadata has been collected.
     *
     * @return true if patch metadata exists
     * @return false if no patch metadata exists
     */
    static auto hasConflictData() -> bool;

    /**
     * @brief Reset transient data collected during patching.
     *
     * Clears in-memory conflict metadata and diff JSON so subsequent patch runs
     * start from a clean state.
     */
    static void resetRunState();

    /**
     * @brief Delets output directory in a smart way
     *
     * @param preOutput whether this is being called pre or post output
     */
    static void deleteOutputDir(const bool& preOutput = true);

    /**
     * @brief Check if the output directory is empty
     *
     * @return true if the output directory is empty
     * @return false if the output directory is not empty
     */
    static auto isOutputEmpty() -> bool;

    static auto getDiffJSON() -> nlohmann::json;

private:
    // NIF Runners

    /**
     * @brief Patch a single NIF file
     *
     * @param nifPath relative path to the NIF file
     * @return TaskTracker::Result result of the patching process
     */
    static auto patchNIF(const std::filesystem::path& nifPath,
                         TaskQueue& setModelUsesQueue,
                         const bool& forceBasePatch = false,
                         const std::unordered_set<PGPlugin::ModelRecordType>& allowedModelRecTypes = {},
                         const bool& checkAllowedRecTypes = false,
                         const bool& excludeFacegens = false,
                         const bool& dryRun = false) -> TaskTracker::Result;

    // NIF Helpers

    /**
     * @brief Process a single NIF file
     *
     * @param nifPath relative path to the NIF file
     * @param nifBytes NIF file bytes (used for CRC calculation)
     * @param[in,out] nifCache NIF cache JSON object to populate
     * @param[out] createdNIFs map of created NIFs
     * @param[out] nifModified whether the NIF file was modified
     * @param forceShaders optional map of shape shaders to force (used for duplicate meshes in recursion)
     * @return true if the NIF file was processed successfully
     * @return false if the NIF file was not processed successfully
     */
    static auto processNIF(const std::filesystem::path& nifPath,
                           nifly::NifFile* nif,
                           MeshMeta& meshMeta,
                           bool singlepassMATO,
                           const PGMeshPermutationTracker::FormKey& formKey,
                           const PGPlugin::ModelRecordType& modelRecordType,
                           std::unordered_map<unsigned int,
                                              PGTypes::TextureSet>& alternateTextures,
                           std::unordered_set<unsigned int>& nonAltTexShapes) -> bool;

    /**
     * @brief Process a single NIF shape
     *
     * @param nifPath relative path to the NIF file
     * @param nif loaded NIF file object
     * @param nifShape NIF shape object (pointer) to process
     * @param[out] shapeCache NIF shape cache JSON object to populate
     * @param canApply map of shape shaders that can be applied to this shape
     * @param patchers patcher objects created from registered patchers
     * @param[out] shaderApplied shader that was applied to this shape
     * @param forceShader optional shape shader to force (used for duplicate meshes in recursion)
     * @return true if the NIF shape was processed successfully
     * @return false if the NIF shape was not processed successfully
     */
    static auto processNIFShape(const std::filesystem::path& nifPath,
                                nifly::NifFile* nif,
                                nifly::NiShape* nifShape,
                                MeshShapeMeta& meshShapeMeta,
                                const PatcherUtil::PatcherMeshObjectSet& patchers,
                                bool singlepassMATO,
                                const PGMeshPermutationTracker::FormKey& formKey,
                                const PGPlugin::ModelRecordType& modelRecordType,
                                PGTypes::TextureSet* alternateTexture = nullptr) -> bool;

    static auto getMatches(const PGTypes::TextureSet& slots,
                           const PatcherUtil::PatcherMeshObjectSet& patchers,
                           bool singlepassMATO,
                           const PGPlugin::ModelRecordType& modelRecordType,
                           const PatcherUtil::PatcherMeshObjectSet* patcherObjects = nullptr,
                           nifly::NiShape* shape = nullptr) -> std::vector<PatcherUtil::ShaderPatcherMatch>;

    /**
     * @brief Helper method to run a transform if needed on a match
     *
     * @param Match Match to run transform
     * @param Patchers Patcher set to use
     * @return ShaderPatcherMatch Transformed match
     */
    static auto applyTransformIfNeeded(PatcherUtil::ShaderPatcherMatch& match,
                                       const PatcherUtil::PatcherMeshObjectSet& patchers) -> bool;

    static auto createNIFPatcherObjects(const std::filesystem::path& nifPath,
                                        nifly::NifFile* nif) -> PatcherUtil::PatcherMeshObjectSet;

    // DDS Runners
    static auto patchDDS(const std::filesystem::path& ddsPath) -> TaskTracker::Result;

    static auto createDDSPatcherObjects(const std::filesystem::path& ddsPath,
                                        DirectX::ScratchImage* dds) -> PatcherUtil::PatcherTextureObjectSet;
};
