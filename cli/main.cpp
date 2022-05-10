#include <MC3D/Interface/MCGenerator.hpp>
#include <MC3D/Interface/Reader.hpp>
#include <MC3D/Interface/Writer.hpp>

#include <MC3D/Algorithm/MCBuilder.hpp>
#include <MC3D/Algorithm/MotorcycleSpawner.hpp>
#include <MC3D/Algorithm/MotorcycleTracer.hpp>
#include <MC3D/Algorithm/SingularityInitializer.hpp>

#include <CLI/CLI.hpp>

#include <QGP3D/MCQuantizer.hpp>
#include <QGP3D/ConstraintExtractor.hpp>
#include <QGP3D/ConstraintWriter.hpp>

#include <iomanip>

#include <string>

using namespace qgp3d;

#define ASSERT_SUCCESS(stage, call)                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        LOG(INFO) << stage << "...";                                                                                   \
        if (auto _ERROR_CODE_ = call; _ERROR_CODE_ != 0)                                                               \
        {                                                                                                              \
            LOG(ERROR) << stage << " failed with error code " << _ERROR_CODE_ << ", aborting...";                      \
            return (_ERROR_CODE_);                                                                                     \
        }                                                                                                              \
        LOG(INFO) << stage << " was successful!";                                                                      \
    } while (0)

int main(int argc, char** argv)
{
    // Manage cli options
    CLI::App app{"QGP3D"};
    std::string inputFile = "";
    std::string wallsFile = "";
    bool simulateBC = false;
    bool inputHasMCwalls = false;
    bool splitSelfadjacent = false;
    bool reduceSingularWalls = false;
    bool exactOutput = false;
    bool forceSanitization = false;

    double scaling = 0.0;
    std::string constraintFile = "";

    app.add_option("--input", inputFile, "Specify the input mesh & seamless parametrization file.")->required();
    app.add_flag("--input-has-walls",
                 inputHasMCwalls,
                 "Use, if the input already contains precomputed MC walls. Only use this, if you are sure the input is "
                 "numerically sane!");
    app.add_flag("--force-sanitization",
                 forceSanitization,
                 "Whether input sanitization should be forced even for exact rational input");
    app.add_option("--output-walls",
                   wallsFile,
                   "Specify an output file to write the refined"
                   " mesh & parametrization & MC walls to.");
    app.add_flag("--output-exact, !--param-output-double",
                 exactOutput,
                 "Whether the parametrization should be output in rational numbers"
                 " (not conforming to standard .hexex format, but numerically safe!) or doubles (numerically unsafe)");
    app.add_flag("--bc, !--mc", simulateBC, "Whether BC or MC is to be computed");
    app.add_flag("--split-selfadjacent, !--allow-selfadjacent",
                 splitSelfadjacent,
                 "Whether selfadjacent blocks should be split");
    app.add_flag("--reduce-singularity-walls, !--keep-singularity-walls",
                 reduceSingularWalls,
                 "Whether walls at singularities may be removed");
    app.add_option("--output-constraint-file",
                   constraintFile,
                   "Set this string to generate a quantization constraint file (optional)");

    // Parse cli options
    try
    {
        app.parse(argc, argv);
    }
    catch (const CLI::ParseError& e)
    {
        return app.exit(e);
    }

    // Create base meshes and add property wrapper
    TetMesh meshRaw;
    MCMesh mcMeshRaw;
    TetMeshProps meshProps(meshRaw, mcMeshRaw);

    Reader reader(meshProps, inputFile, forceSanitization);
    if (inputHasMCwalls)
        ASSERT_SUCCESS("Reading precomputed MC walls", reader.readSeamlessParamWithWalls());
    else
        ASSERT_SUCCESS("Reading seamless map", reader.readSeamlessParam());

    MCGenerator mcgen(meshProps);
    if (!inputHasMCwalls)
    {
        // For default usage, the interface is simple to use and requires no property management
        ASSERT_SUCCESS("Tracing and connecting the raw MC",
                       mcgen.traceMC(true, splitSelfadjacent, simulateBC, !constraintFile.empty()));
        if (!simulateBC)
            ASSERT_SUCCESS("Reducing the raw MC", mcgen.reduceMC(!reduceSingularWalls, splitSelfadjacent));
    }
    else
    {
        LOG(INFO) << "Connecting a precomputed MC";

        // For advanced usage, some property management is required.
        // Required/Generated properties are documented for each callable function
        meshProps.allocate<CHILD_CELLS>({});
        meshProps.allocate<CHILD_EDGES>({});
        meshProps.allocate<CHILD_FACES>({});
        meshProps.allocate<IS_ORIGINAL>(false); // Default value = false (for future added elements)
        for (auto f : meshRaw.faces())
            meshProps.set<IS_ORIGINAL>(f, true); // Only current faces are original

        // For advanced usage, the library provides specialized classes for each algorithmic step
        SingularityInitializer init(meshProps);
        ASSERT_SUCCESS("Determining transitions", init.initTransitions());
        ASSERT_SUCCESS("Determining singularities", init.initSingularities());

        if (!constraintFile.empty())
        {
            meshProps.allocate<IS_ORIGINAL_VTX>(false);
            for (auto v : meshRaw.vertices())
                meshProps.set<IS_ORIGINAL_VTX>(v, true);
            meshProps.allocate<CHART_ORIG>();
            for (auto tet : meshRaw.cells())
                meshProps.set<CHART_ORIG>(tet, meshProps.ref<CHART>(tet));
            meshProps.allocate<TRANSITION_ORIG>();
            for (auto f : meshRaw.faces())
                meshProps.set<TRANSITION_ORIG>(f, meshProps.ref<TRANSITION>(f));
        }

        MCBuilder builder(meshProps);
        ASSERT_SUCCESS("Discovering block structure", builder.discoverBlocks());

        MotorcycleQueue mQ;
        MotorcycleSpawner spawner(meshProps, mQ);
        MotorcycleTracer tracer(meshProps, mQ, simulateBC);

        for (int n = builder.nToroidalBlocks(); n > 0; n = builder.nToroidalBlocks())
        {
            LOG(INFO) << "Splitting toroidal blocks. " << n << " remaining";

            ASSERT_SUCCESS("Spawning torus splitting motorcycles", spawner.spawnTorusSplitMotorcycle());
            ASSERT_SUCCESS("Tracing torus splitting motorcycles", tracer.traceAllMotorcycles());

            auto newWalls = tracer.getNewWalls();
            auto anyHf = meshRaw.halfface_handle(newWalls.front(), 0);
            ASSERT_SUCCESS("Updating split block", builder.updateSingleBlock(meshRaw.incident_cell(anyHf)));
            tracer.clearNewWalls();
        }

        for (int n = builder.nSelfadjacentBlocks(); splitSelfadjacent && n > 0; n = builder.nSelfadjacentBlocks())
        {
            LOG(INFO) << "Splitting selfadjacent blocks. " << n << " remaining";

            ASSERT_SUCCESS("Spawning selfadjacency splitting motorcycles", spawner.spawnSelfadjacencySplitMotorcycle());
            ASSERT_SUCCESS("Tracing selfadjacency splitting motorcycles", tracer.traceAllMotorcycles());

            auto newWalls = tracer.getNewWalls();
            auto anyHf = meshRaw.halfface_handle(newWalls.front(), 0);
            auto anyHfOpp = meshRaw.halfface_handle(newWalls.front(), 1);
            ASSERT_SUCCESS("Updating split block", builder.updateSingleBlock(meshRaw.incident_cell(anyHf)));
            ASSERT_SUCCESS("Updating split block", builder.updateSingleBlock(meshRaw.incident_cell(anyHfOpp)));
            tracer.clearNewWalls();
        }

        ASSERT_SUCCESS("Connecting the MC", builder.connectMCMesh(true, splitSelfadjacent));

        if (!simulateBC)
            ASSERT_SUCCESS("Reducing the raw MC", mcgen.reduceMC(!reduceSingularWalls, splitSelfadjacent));
    }

    if (!wallsFile.empty())
    {
        ASSERT_SUCCESS("Writing walls", Writer(meshProps, wallsFile, exactOutput).writeSeamlessParamAndWalls());
    }

    if (!constraintFile.empty())
    {
        ASSERT_SUCCESS("Quantization", MCQuantizer(meshProps).quantizeArcLengths(scaling, true, true));
        ASSERT_SUCCESS("Writing constraints", ConstraintWriter(meshProps, constraintFile).writeConstraintPaths());
    }

    return 0;
}
